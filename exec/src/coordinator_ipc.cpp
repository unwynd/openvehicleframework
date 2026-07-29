// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/coordinator_client.hpp"
#include "ovf/exec/internal/coordinator_server.hpp"
#include "ovf/exec/internal/coordinator_service.hpp"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace ovf::exec::detail {
namespace {

constexpr std::uint32_t kProtocolVersion = 1U;
constexpr auto kEventPollInterval = std::chrono::milliseconds{50};
constexpr std::size_t kClientMaximumMessageSize = 16U * 1024U * 1024U;

bool IsUnsignedInteger(const Json::Value& value) noexcept {
  return value.isIntegral() && (!value.isInt64() || value.asInt64() >= 0);
}

bool IsSignedInteger(const Json::Value& value) noexcept { return value.isIntegral(); }

bool Terminal(TransitionPhase phase) noexcept {
  return phase == TransitionPhase::succeeded || phase == TransitionPhase::rejected ||
         phase == TransitionPhase::failed || phase == TransitionPhase::cancelled ||
         phase == TransitionPhase::superseded || phase == TransitionPhase::deadline_exceeded ||
         phase == TransitionPhase::recovery_failed;
}

Json::Value ErrorValue(const Error& error) {
  Json::Value value;
  value["code"] = static_cast<Json::UInt>(error.code);
  value["message"] = error.message;
  value["supportData"] = Json::UInt64{error.support_data};
  return value;
}

Error DecodeError(const Json::Value& value) {
  if (!value.isObject() || !IsUnsignedInteger(value["code"]) || !value["message"].isString() ||
      !IsUnsignedInteger(value["supportData"]) ||
      value["code"].asUInt64() > static_cast<unsigned>(ErrorCode::internal_error)) {
    return MakeError(ErrorCode::communication_error, "coordinator returned an invalid error");
  }
  return {static_cast<ErrorCode>(value["code"].asUInt()), value["message"].asString(),
          value["supportData"].asUInt64()};
}

Json::Value TransitionValue(const ovf::exec::TransitionSnapshot& transition) {
  Json::Value value;
  value["id"] = Json::UInt64{transition.id.value()};
  value["domain"] = Json::UInt64{transition.domain.value()};
  value["sourceMode"] = Json::UInt64{transition.source_mode.value()};
  value["targetMode"] = Json::UInt64{transition.target_mode.value()};
  value["phase"] = static_cast<Json::UInt>(transition.phase);
  value["updatedNs"] = Json::Int64{
      std::chrono::duration_cast<std::chrono::nanoseconds>(transition.updated_at.time_since_epoch())
          .count()};
  if (transition.failure) {
    Json::Value failure;
    failure["error"] = ErrorValue(transition.failure->error);
    if (transition.failure->application) {
      failure["application"] = Json::UInt64{transition.failure->application->value()};
    }
    failure["exitCode"] = transition.failure->exit_code;
    failure["signal"] = transition.failure->signal;
    failure["backendCode"] = Json::UInt64{transition.failure->backend_code};
    failure["backendMessage"] = transition.failure->backend_message;
    failure["recoveryAction"] = static_cast<Json::UInt>(transition.failure->recovery_action);
    failure["recoveryAttempted"] = transition.failure->recovery_attempted;
    failure["recoverySucceeded"] = transition.failure->recovery_succeeded;
    if (transition.failure->recovery_error) {
      failure["recoveryError"] = ErrorValue(*transition.failure->recovery_error);
    }
    if (transition.failure->recovered_mode) {
      failure["recoveredMode"] = Json::UInt64{transition.failure->recovered_mode->value()};
    }
    failure["recoveryStoppedApplications"] = Json::Value{Json::arrayValue};
    for (const auto application : transition.failure->recovery_stopped_applications) {
      failure["recoveryStoppedApplications"].append(Json::UInt64{application.value()});
    }
    failure["recoveryStartedApplications"] = Json::Value{Json::arrayValue};
    for (const auto application : transition.failure->recovery_started_applications) {
      failure["recoveryStartedApplications"].append(Json::UInt64{application.value()});
    }
    value["failure"] = std::move(failure);
  }
  return value;
}

Result<ovf::exec::TransitionSnapshot> DecodeTransition(const Json::Value& value) {
  if (!value.isObject() || !IsUnsignedInteger(value["id"]) || !IsUnsignedInteger(value["domain"]) ||
      !IsUnsignedInteger(value["sourceMode"]) || !IsUnsignedInteger(value["targetMode"]) ||
      !IsUnsignedInteger(value["phase"]) || !IsSignedInteger(value["updatedNs"]) ||
      value["phase"].asUInt64() > static_cast<unsigned>(TransitionPhase::recovering)) {
    return MakeError(ErrorCode::communication_error, "coordinator returned an invalid transition");
  }
  ovf::exec::TransitionSnapshot result{
      TransitionId{value["id"].asUInt64()},
      DomainId{value["domain"].asUInt64()},
      ModeId{value["sourceMode"].asUInt64()},
      ModeId{value["targetMode"].asUInt64()},
      static_cast<TransitionPhase>(value["phase"].asUInt()),
      std::chrono::steady_clock::time_point{std::chrono::nanoseconds{value["updatedNs"].asInt64()}},
      std::nullopt,
  };
  if (value.isMember("failure")) {
    const auto& failure = value["failure"];
    if (!failure.isObject() || !IsSignedInteger(failure["exitCode"]) ||
        !IsSignedInteger(failure["signal"]) || !IsUnsignedInteger(failure["backendCode"]) ||
        !failure["backendMessage"].isString() || !IsUnsignedInteger(failure["recoveryAction"]) ||
        failure["recoveryAction"].asUInt64() >
            static_cast<unsigned>(FailureAction::request_system_recovery) ||
        !failure["recoveryAttempted"].isBool() || !failure["recoverySucceeded"].isBool() ||
        !failure["recoveryStoppedApplications"].isArray() ||
        !failure["recoveryStartedApplications"].isArray()) {
      return MakeError(ErrorCode::communication_error,
                       "coordinator returned invalid failure evidence");
    }
    TransitionFailureSnapshot decoded{
        DecodeError(failure["error"]),
        std::nullopt,
        failure["exitCode"].asInt(),
        failure["signal"].asInt(),
        failure["backendCode"].asUInt64(),
        failure["backendMessage"].asString(),
        static_cast<FailureAction>(failure["recoveryAction"].asUInt()),
        failure["recoveryAttempted"].asBool(),
        failure["recoverySucceeded"].asBool(),
        std::nullopt,
        std::nullopt,
        {},
        {},
    };
    if (failure.isMember("application")) {
      if (!IsUnsignedInteger(failure["application"])) {
        return MakeError(ErrorCode::communication_error,
                         "coordinator returned invalid failure application");
      }
      decoded.application = ApplicationId{failure["application"].asUInt64()};
    }
    if (failure.isMember("recoveryError")) {
      const auto recovery_error = DecodeError(failure["recoveryError"]);
      if (!recovery_error) {
        return MakeError(ErrorCode::communication_error,
                         "coordinator returned invalid recovery error");
      }
      decoded.recovery_error = recovery_error;
    }
    if (failure.isMember("recoveredMode")) {
      if (!IsUnsignedInteger(failure["recoveredMode"])) {
        return MakeError(ErrorCode::communication_error,
                         "coordinator returned invalid recovered mode");
      }
      decoded.recovered_mode = ModeId{failure["recoveredMode"].asUInt64()};
    }
    try {
      for (const auto& application : failure["recoveryStoppedApplications"]) {
        if (!IsUnsignedInteger(application) || application.asUInt64() == 0U) {
          return MakeError(ErrorCode::communication_error,
                           "coordinator returned invalid recovery stop evidence");
        }
        decoded.recovery_stopped_applications.emplace_back(application.asUInt64());
      }
      for (const auto& application : failure["recoveryStartedApplications"]) {
        if (!IsUnsignedInteger(application) || application.asUInt64() == 0U) {
          return MakeError(ErrorCode::communication_error,
                           "coordinator returned invalid recovery start evidence");
        }
        decoded.recovery_started_applications.emplace_back(application.asUInt64());
      }
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted,
                       "cannot allocate recovery application evidence");
    }
    result.failure = std::move(decoded);
  }
  return result;
}

Json::Value DomainValue(const ExecutionDomain& domain) {
  Json::Value value;
  value["id"] = Json::UInt64{domain.id.value()};
  value["name"] = domain.name;
  value["committedMode"] = Json::UInt64{domain.committed_mode.value()};
  value["status"] = static_cast<Json::UInt>(domain.status);
  if (domain.target_mode) {
    value["targetMode"] = Json::UInt64{domain.target_mode->value()};
  }
  if (domain.active_transition) {
    value["activeTransition"] = Json::UInt64{domain.active_transition->value()};
  }
  return value;
}

Result<ExecutionDomain> DecodeDomain(const Json::Value& value) {
  if (!value.isObject() || !IsUnsignedInteger(value["id"]) || !value["name"].isString() ||
      !IsUnsignedInteger(value["committedMode"]) || !IsUnsignedInteger(value["status"]) ||
      value["status"].asUInt64() > static_cast<unsigned>(DomainStatus::recovering)) {
    return MakeError(ErrorCode::communication_error, "coordinator returned an invalid domain");
  }
  ExecutionDomain result;
  result.id = DomainId{value["id"].asUInt64()};
  result.name = value["name"].asString();
  result.committed_mode = ModeId{value["committedMode"].asUInt64()};
  result.status = static_cast<DomainStatus>(value["status"].asUInt());
  if (value.isMember("targetMode")) {
    if (!IsUnsignedInteger(value["targetMode"])) {
      return MakeError(ErrorCode::communication_error,
                       "coordinator returned an invalid target mode");
    }
    result.target_mode = ModeId{value["targetMode"].asUInt64()};
  }
  if (value.isMember("activeTransition")) {
    if (!IsUnsignedInteger(value["activeTransition"])) {
      return MakeError(ErrorCode::communication_error,
                       "coordinator returned an invalid active transition");
    }
    result.active_transition = TransitionId{value["activeTransition"].asUInt64()};
  }
  return result;
}

Json::Value SnapshotValue(const SystemSnapshot& snapshot) {
  Json::Value value;
  value["generation"] = Json::UInt64{snapshot.model_generation.value};
  value["revision"] = Json::UInt64{snapshot.revision};
  value["recovering"] = snapshot.recovering;
  value["domains"] = Json::Value{Json::arrayValue};
  value["applications"] = Json::Value{Json::arrayValue};
  value["transitions"] = Json::Value{Json::arrayValue};
  for (const auto& domain : snapshot.domains) {
    value["domains"].append(DomainValue(domain));
  }
  for (const auto& application : snapshot.applications) {
    Json::Value item;
    item["id"] = Json::UInt64{application.id.value()};
    item["name"] = application.name;
    item["state"] = static_cast<Json::UInt>(application.state);
    value["applications"].append(std::move(item));
  }
  for (const auto& transition : snapshot.transitions) {
    value["transitions"].append(TransitionValue(transition));
  }
  return value;
}

Result<SystemSnapshot> DecodeSnapshot(const Json::Value& value) {
  if (!value.isObject() || !IsUnsignedInteger(value["generation"]) ||
      !IsUnsignedInteger(value["revision"]) || !value["recovering"].isBool() ||
      !value["domains"].isArray() || !value["applications"].isArray() ||
      !value["transitions"].isArray()) {
    return MakeError(ErrorCode::communication_error, "coordinator returned an invalid snapshot");
  }
  SystemSnapshot result;
  result.model_generation = {value["generation"].asUInt64()};
  result.revision = value["revision"].asUInt64();
  result.recovering = value["recovering"].asBool();
  try {
    for (const auto& item : value["domains"]) {
      auto domain = DecodeDomain(item);
      if (!domain) {
        return domain.error();
      }
      result.domains.push_back(std::move(domain).value());
    }
    for (const auto& item : value["applications"]) {
      if (!IsUnsignedInteger(item["id"]) || !item["name"].isString() ||
          !IsUnsignedInteger(item["state"]) ||
          item["state"].asUInt64() > static_cast<unsigned>(ApplicationState::unavailable)) {
        return MakeError(ErrorCode::communication_error,
                         "coordinator returned an invalid application");
      }
      result.applications.push_back({ApplicationId{item["id"].asUInt64()}, item["name"].asString(),
                                     static_cast<ApplicationState>(item["state"].asUInt())});
    }
    for (const auto& item : value["transitions"]) {
      auto transition = DecodeTransition(item);
      if (!transition) {
        return transition.error();
      }
      result.transitions.push_back(std::move(transition).value());
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate coordinator snapshot");
  }
  return result;
}

Json::Value EventValue(const CoordinatorEvent& event) {
  Json::Value value;
  value["kind"] = static_cast<Json::UInt>(event.kind);
  value["revision"] = Json::UInt64{event.revision};
  if (event.transition) {
    value["transition"] = Json::UInt64{event.transition->value()};
  }
  if (event.domain) {
    value["domain"] = Json::UInt64{event.domain->value()};
  }
  return value;
}

Result<CoordinatorEvent> DecodeEvent(const Json::Value& value) {
  if (!value.isObject() || !IsUnsignedInteger(value["kind"]) ||
      !IsUnsignedInteger(value["revision"]) ||
      value["kind"].asUInt64() > static_cast<unsigned>(CoordinatorEventKind::recovery_changed)) {
    return MakeError(ErrorCode::communication_error, "coordinator returned an invalid event");
  }
  CoordinatorEvent event{static_cast<CoordinatorEventKind>(value["kind"].asUInt()),
                         value["revision"].asUInt64(), std::nullopt, std::nullopt};
  if (value.isMember("transition")) {
    if (!IsUnsignedInteger(value["transition"])) {
      return MakeError(ErrorCode::communication_error,
                       "coordinator returned an invalid event transition");
    }
    event.transition = TransitionId{value["transition"].asUInt64()};
  }
  if (value.isMember("domain")) {
    if (!IsUnsignedInteger(value["domain"])) {
      return MakeError(ErrorCode::communication_error,
                       "coordinator returned an invalid event domain");
    }
    event.domain = DomainId{value["domain"].asUInt64()};
  }
  return event;
}

std::string Encode(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value) + "\n";
}

Result<Json::Value> Decode(std::string_view content) {
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["rejectDupKeys"] = true;
  builder["strictRoot"] = true;
  std::istringstream input(std::string{content});
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors) || !value.isObject()) {
    return MakeError(ErrorCode::communication_error, "coordinator message is invalid");
  }
  return value;
}

bool WriteAll(int descriptor, std::string_view content) noexcept {
  std::size_t offset{};
  while (offset < content.size()) {
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = ::send(descriptor, content.data() + offset, content.size() - offset, flags);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

Result<std::string> ReadLine(int descriptor, std::size_t maximum) {
  std::string content;
  try {
    content.reserve(std::min(maximum, std::size_t{4096U}));
    for (;;) {
      char buffer[4096];
      const auto count = ::recv(descriptor, buffer, sizeof(buffer), 0);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return MakeError(ErrorCode::communication_error,
                         "coordinator connection closed before a complete message");
      }
      const auto* newline =
          static_cast<const char*>(std::memchr(buffer, '\n', static_cast<std::size_t>(count)));
      const auto append = newline == nullptr ? static_cast<std::size_t>(count)
                                             : static_cast<std::size_t>(newline - buffer);
      if (append > maximum - content.size()) {
        return MakeError(ErrorCode::resource_exhausted, "coordinator message exceeds limit");
      }
      content.append(buffer, append);
      if (newline != nullptr) {
        return content;
      }
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate coordinator message");
  }
}

class Descriptor final {
public:
  Descriptor() = default;
  explicit Descriptor(int value) : value_(value) {}
  ~Descriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        ::close(value_);
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const noexcept { return value_; }
  int release() noexcept { return std::exchange(value_, -1); }

private:
  int value_{-1};
};

Result<void> DisableSigPipe(int descriptor) {
#if defined(__APPLE__)
  const int enabled = 1;
  if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
    return MakeError(ErrorCode::communication_error,
                     "cannot configure coordinator socket signal handling", errno);
  }
#else
  static_cast<void>(descriptor);
#endif
  return {};
}

Result<Descriptor> Connect(const std::string& endpoint, std::chrono::milliseconds timeout) {
  if (endpoint.empty() || endpoint.front() != '/' ||
      endpoint.size() >= sizeof(sockaddr_un::sun_path) ||
      timeout <= std::chrono::milliseconds::zero()) {
    return MakeError(ErrorCode::invalid_argument, "invalid coordinator endpoint options");
  }
  Descriptor descriptor{::socket(AF_UNIX, SOCK_STREAM, 0)};
  if (descriptor.get() < 0) {
    return MakeError(ErrorCode::communication_error, "cannot create coordinator socket", errno);
  }
  const int descriptor_flags = ::fcntl(descriptor.get(), F_GETFD, 0);
  if (descriptor_flags < 0 ||
      ::fcntl(descriptor.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    return MakeError(ErrorCode::communication_error,
                     "cannot protect coordinator socket across exec", errno);
  }
  auto signal_configured = DisableSigPipe(descriptor.get());
  if (!signal_configured) {
    return signal_configured.error();
  }
  const int flags = ::fcntl(descriptor.get(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    return MakeError(ErrorCode::communication_error, "cannot configure coordinator socket", errno);
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1U);
  if (::connect(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
          0 &&
      errno != EINPROGRESS) {
    return MakeError(ErrorCode::backend_unavailable, "cannot connect to execution coordinator",
                     errno);
  }
  pollfd poll_descriptor{descriptor.get(), POLLOUT, 0};
  const auto milliseconds =
      std::min<std::int64_t>(timeout.count(), std::numeric_limits<int>::max());
  if (::poll(&poll_descriptor, 1, static_cast<int>(milliseconds)) <= 0) {
    return MakeError(ErrorCode::deadline_exceeded, "coordinator connection deadline expired");
  }
  int socket_error{};
  socklen_t length = sizeof(socket_error);
  if (::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
      socket_error != 0) {
    return MakeError(ErrorCode::backend_unavailable, "execution coordinator is unavailable",
                     socket_error == 0 ? errno : socket_error);
  }
  if (::fcntl(descriptor.get(), F_SETFL, flags) != 0) {
    return MakeError(ErrorCode::communication_error, "cannot restore coordinator socket", errno);
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
  const timeval socket_timeout{static_cast<time_t>(seconds.count()),
                               static_cast<suseconds_t>(microseconds.count())};
  if (::setsockopt(descriptor.get(), SOL_SOCKET, SO_RCVTIMEO, &socket_timeout,
                   sizeof(socket_timeout)) != 0 ||
      ::setsockopt(descriptor.get(), SOL_SOCKET, SO_SNDTIMEO, &socket_timeout,
                   sizeof(socket_timeout)) != 0) {
    return MakeError(ErrorCode::communication_error, "cannot set coordinator socket deadline",
                     errno);
  }
  return descriptor;
}

Result<std::uint32_t> PeerUid(int descriptor) {
#if defined(__APPLE__)
  uid_t uid{};
  gid_t gid{};
  if (::getpeereid(descriptor, &uid, &gid) != 0) {
    return MakeError(ErrorCode::permission_denied, "cannot authenticate coordinator peer", errno);
  }
  return static_cast<std::uint32_t>(uid);
#elif defined(__linux__)
  struct ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
    return MakeError(ErrorCode::permission_denied, "cannot authenticate coordinator peer", errno);
  }
  return static_cast<std::uint32_t>(credentials.uid);
#else
  static_cast<void>(descriptor);
  return MakeError(ErrorCode::unsupported, "peer credential authentication is unsupported");
#endif
}

Json::Value Success(Json::Value value = {}) {
  Json::Value response;
  response["ok"] = true;
  response["value"] = std::move(value);
  return response;
}

Json::Value Failure(const Error& error) {
  Json::Value response;
  response["ok"] = false;
  response["error"] = ErrorValue(error);
  return response;
}

template <typename T, typename EncodeValue>
Json::Value Response(Result<T> result, EncodeValue encode) {
  return result ? Success(encode(result.value())) : Failure(result.error());
}

class IpcCoordinatorClient final : public CoordinatorClient {
public:
  IpcCoordinatorClient(std::string endpoint, std::chrono::milliseconds timeout)
      : endpoint_(std::move(endpoint)), timeout_(timeout) {}

  Result<Json::Value> Call(Json::Value request,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{}) const {
    request["version"] = Json::UInt64{kProtocolVersion};
    const auto effective = timeout > std::chrono::milliseconds::zero() ? timeout : timeout_;
    auto connected = Connect(endpoint_, effective);
    if (!connected) {
      return connected.error();
    }
    const auto encoded = Encode(request);
    if (!WriteAll(connected.value().get(), encoded)) {
      return MakeError(ErrorCode::communication_error, "cannot send coordinator request", errno);
    }
    auto content = ReadLine(connected.value().get(), kClientMaximumMessageSize);
    if (!content) {
      return content.error();
    }
    auto response = Decode(content.value());
    if (!response || !response.value()["ok"].isBool()) {
      return response ? Result<Json::Value>{MakeError(ErrorCode::communication_error,
                                                      "coordinator returned an invalid response")}
                      : Result<Json::Value>{response.error()};
    }
    if (!response.value()["ok"].asBool()) {
      return DecodeError(response.value()["error"]);
    }
    return response.value()["value"];
  }

  Result<SystemSnapshot> Snapshot() const override {
    Json::Value request;
    request["operation"] = "snapshot";
    auto response = Call(std::move(request));
    return response ? DecodeSnapshot(response.value()) : Result<SystemSnapshot>{response.error()};
  }

  Result<ExecutionDomain> Domain(DomainId domain) const override {
    Json::Value request;
    request["operation"] = "domain";
    request["domain"] = Json::UInt64{domain.value()};
    auto response = Call(std::move(request));
    return response ? DecodeDomain(response.value()) : Result<ExecutionDomain>{response.error()};
  }

  Result<ExecutionMode> Mode(DomainId domain, ModeId mode) const override {
    Json::Value request;
    request["operation"] = "mode";
    request["domain"] = Json::UInt64{domain.value()};
    request["mode"] = Json::UInt64{mode.value()};
    auto response = Call(std::move(request));
    if (!response) {
      return response.error();
    }
    if (!IsUnsignedInteger(response.value()["domain"]) ||
        !IsUnsignedInteger(response.value()["id"]) || !response.value()["name"].isString()) {
      return MakeError(ErrorCode::communication_error, "coordinator returned an invalid mode");
    }
    return ExecutionMode{DomainId{response.value()["domain"].asUInt64()},
                         ModeId{response.value()["id"].asUInt64()},
                         response.value()["name"].asString()};
  }

  Result<TransitionId> Request(DomainId domain, ModeId mode, TransitionOptions options) override {
    Json::Value request;
    request["operation"] = "request";
    request["domain"] = Json::UInt64{domain.value()};
    request["mode"] = Json::UInt64{mode.value()};
    request["timeoutMs"] = Json::Int64{options.timeout.count()};
    auto response = Call(std::move(request));
    return response && IsUnsignedInteger(response.value())
               ? Result<TransitionId>{TransitionId{response.value().asUInt64()}}
           : response
               ? Result<TransitionId>{MakeError(ErrorCode::communication_error,
                                                "coordinator returned an invalid transition id")}
               : Result<TransitionId>{response.error()};
  }

  Result<void> Cancel(TransitionId transition) override {
    Json::Value request;
    request["operation"] = "cancel";
    request["transition"] = Json::UInt64{transition.value()};
    auto response = Call(std::move(request));
    return response ? Result<void>{} : Result<void>{response.error()};
  }

  Result<ovf::exec::TransitionSnapshot> TransitionState(TransitionId transition) const override {
    Json::Value request;
    request["operation"] = "transition";
    request["transition"] = Json::UInt64{transition.value()};
    auto response = Call(std::move(request));
    return response ? DecodeTransition(response.value())
                    : Result<ovf::exec::TransitionSnapshot>{response.error()};
  }

  Result<ovf::exec::TransitionSnapshot> Wait(TransitionId transition,
                                             Deadline deadline) const override {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
      return MakeError(ErrorCode::deadline_exceeded, "transition wait deadline expired");
    }
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    Json::Value request;
    request["operation"] = "wait";
    request["transition"] = Json::UInt64{transition.value()};
    request["timeoutMs"] = Json::Int64{timeout.count()};
    auto response = Call(std::move(request), timeout + timeout_);
    return response ? DecodeTransition(response.value())
                    : Result<ovf::exec::TransitionSnapshot>{response.error()};
  }

  Result<std::function<void()>> Subscribe(EventFilter filter,
                                          SystemCoordinator::EventHandler handler) override {
    if (!handler) {
      return MakeError(ErrorCode::invalid_argument, "event handler is required");
    }
    auto snapshot = Snapshot();
    if (!snapshot) {
      return snapshot.error();
    }
    struct State final {
      std::atomic_bool stop{};
      std::thread worker;
    };
    std::shared_ptr<State> state;
    try {
      state = std::make_shared<State>();
      const auto endpoint = endpoint_;
      const auto timeout = timeout_;
      auto client = std::make_shared<IpcCoordinatorClient>(endpoint, timeout);
      state->worker = std::thread([state, client, filter = std::move(filter),
                                   handler = std::move(handler),
                                   revision = snapshot.value().revision]() mutable {
        while (!state->stop.load(std::memory_order_acquire)) {
          Json::Value request;
          request["operation"] = "events";
          request["afterRevision"] = Json::UInt64{revision};
          auto response = client->Call(std::move(request));
          if (response && response.value().isArray()) {
            for (const auto& item : response.value()) {
              auto event = DecodeEvent(item);
              if (!event) {
                state->stop.store(true, std::memory_order_release);
                break;
              }
              revision = std::max(revision, event.value().revision);
              const bool domain_matches = !filter.domain || event.value().domain == filter.domain;
              const bool kind_matches =
                  (event.value().kind == CoordinatorEventKind::transition_changed &&
                   filter.transitions) ||
                  (event.value().kind == CoordinatorEventKind::configuration_changed &&
                   filter.configuration) ||
                  (event.value().kind == CoordinatorEventKind::recovery_changed && filter.recovery);
              if (domain_matches && kind_matches) {
                try {
                  handler(event.value());
                } catch (...) {
                }
              }
            }
          }
          std::this_thread::sleep_for(kEventPollInterval);
        }
      });
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted,
                       "cannot create coordinator event subscription");
    }
    return std::function<void()>([state] {
      state->stop.store(true, std::memory_order_release);
      if (state->worker.joinable()) {
        if (state->worker.get_id() == std::this_thread::get_id()) {
          state->worker.detach();
        } else {
          state->worker.join();
        }
      }
    });
  }

private:
  std::string endpoint_;
  std::chrono::milliseconds timeout_;
};

class IpcCoordinatorServer final : public CoordinatorServer {
public:
  IpcCoordinatorServer(SystemCoordinator coordinator, CoordinatorServerOptions options,
                       Descriptor listener)
      : coordinator_(std::move(coordinator)), options_(std::move(options)),
        listener_(std::move(listener)) {}

  ~IpcCoordinatorServer() override {
    stopping_.store(true, std::memory_order_release);
    {
      std::lock_guard lock(mutex_);
      for (const auto descriptor : active_) {
        ::shutdown(descriptor, SHUT_RDWR);
      }
    }
    available_.notify_all();
    if (acceptor_.joinable()) {
      acceptor_.join();
    }
    listener_ = Descriptor{};
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    event_subscription_.Reset();
    ::unlink(options_.endpoint.c_str());
  }

  Result<void> Start() {
    auto subscribed = coordinator_.Subscribe({}, [this](const CoordinatorEvent& event) {
      std::lock_guard lock(event_mutex_);
      events_.push_back(event);
      while (events_.size() > options_.connection_capacity * 16U) {
        events_.pop_front();
      }
    });
    if (!subscribed) {
      return subscribed.error();
    }
    event_subscription_ = std::move(subscribed).value();
    try {
      workers_.reserve(options_.worker_count);
      for (std::size_t index = 0; index < options_.worker_count; ++index) {
        workers_.emplace_back([this] { Worker(); });
      }
      acceptor_ = std::thread([this] { Accept(); });
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted, "cannot create coordinator IPC workers");
    }
    return {};
  }

  std::string Endpoint() const override { return options_.endpoint; }

private:
  struct Work final {
    int descriptor{-1};
    std::uint32_t uid{};
  };

  bool Authorized(std::uint32_t uid, bool mutation) const noexcept {
    const auto& values = mutation ? options_.mutation_uids : options_.observation_uids;
    return std::find(values.begin(), values.end(), uid) != values.end();
  }

  void Accept() {
    while (!stopping_.load(std::memory_order_acquire)) {
      pollfd ready{listener_.get(), POLLIN, 0};
      const int polled = ::poll(&ready, 1, 100);
      if (polled == 0) {
        continue;
      }
      if (polled < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      if ((ready.revents & POLLIN) == 0) {
        return;
      }
      const int descriptor = ::accept(listener_.get(), nullptr, nullptr);
      if (descriptor < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
      if (descriptor_flags < 0 ||
          ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
          !DisableSigPipe(descriptor)) {
        ::close(descriptor);
        continue;
      }
      auto uid = PeerUid(descriptor);
      std::lock_guard lock(mutex_);
      if (!uid || queue_.size() >= options_.connection_capacity) {
        ::close(descriptor);
        continue;
      }
      queue_.push_back({descriptor, uid.value()});
      active_.push_back(descriptor);
      available_.notify_one();
    }
  }

  void Worker() {
    for (;;) {
      Work work;
      {
        std::unique_lock lock(mutex_);
        available_.wait(
            lock, [this] { return stopping_.load(std::memory_order_acquire) || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        work = queue_.front();
        queue_.pop_front();
      }
      Descriptor descriptor{work.descriptor};
      auto request_text = ReadLine(descriptor.get(), options_.maximum_message_size);
      Json::Value response;
      if (!request_text) {
        response = Failure(request_text.error());
      } else {
        auto request = Decode(request_text.value());
        response = request ? Dispatch(request.value(), work.uid) : Failure(request.error());
      }
      static_cast<void>(WriteAll(descriptor.get(), Encode(response)));
      {
        std::lock_guard lock(mutex_);
        std::erase(active_, work.descriptor);
      }
    }
  }

  Json::Value Dispatch(const Json::Value& request, std::uint32_t uid) {
    if (!request["version"].isIntegral() || request["version"].asUInt64() != kProtocolVersion ||
        !request["operation"].isString()) {
      return Failure(MakeError(ErrorCode::communication_error,
                               "coordinator protocol version is incompatible"));
    }
    const auto operation = request["operation"].asString();
    const bool mutation = operation == "request" || operation == "cancel";
    if (!Authorized(uid, mutation)) {
      return Failure(MakeError(ErrorCode::permission_denied,
                               mutation ? "coordinator mutation is not authorized"
                                        : "coordinator observation is not authorized"));
    }
    if (operation == "hello") {
      Json::Value value;
      value["protocol"] = Json::UInt64{kProtocolVersion};
      value["mutation"] = Authorized(uid, true);
      return Success(std::move(value));
    }
    if (operation == "snapshot") {
      return Response(coordinator_.GetSnapshot(), SnapshotValue);
    }
    if (operation == "domain") {
      if (!IsUnsignedInteger(request["domain"])) {
        return Failure(MakeError(ErrorCode::invalid_argument, "domain id is required"));
      }
      return Response(coordinator_.ResolveDomain(DomainId{request["domain"].asUInt64()}),
                      DomainValue);
    }
    if (operation == "mode") {
      if (!IsUnsignedInteger(request["domain"]) || !IsUnsignedInteger(request["mode"])) {
        return Failure(MakeError(ErrorCode::invalid_argument, "domain and mode ids are required"));
      }
      auto mode = coordinator_.ResolveMode(DomainId{request["domain"].asUInt64()},
                                           ModeId{request["mode"].asUInt64()});
      return Response(std::move(mode), [](const ExecutionMode& value) {
        Json::Value result;
        result["domain"] = Json::UInt64{value.domain.value()};
        result["id"] = Json::UInt64{value.id.value()};
        result["name"] = value.name;
        return result;
      });
    }
    if (operation == "request") {
      if (!IsUnsignedInteger(request["domain"]) || !IsUnsignedInteger(request["mode"])) {
        return Failure(MakeError(ErrorCode::invalid_argument, "domain and mode ids are required"));
      }
      if (!IsSignedInteger(request["timeoutMs"]) || request["timeoutMs"].asInt64() <= 0) {
        return Failure(
            MakeError(ErrorCode::invalid_argument, "transition timeout must be positive"));
      }
      auto transition = coordinator_.RequestMode(
          DomainId{request["domain"].asUInt64()}, ModeId{request["mode"].asUInt64()},
          {std::chrono::milliseconds{request["timeoutMs"].asInt64()}});
      return transition ? Success(Json::Value{Json::UInt64{transition.value().Id().value()}})
                        : Failure(transition.error());
    }
    if (operation == "cancel") {
      if (!IsUnsignedInteger(request["transition"])) {
        return Failure(MakeError(ErrorCode::invalid_argument, "transition id is required"));
      }
      auto result = coordinator_.Cancel(TransitionId{request["transition"].asUInt64()});
      return result ? Success() : Failure(result.error());
    }
    if (operation == "transition" || operation == "wait") {
      if (!IsUnsignedInteger(request["transition"]) ||
          (operation == "wait" &&
           (!IsSignedInteger(request["timeoutMs"]) || request["timeoutMs"].asInt64() <= 0))) {
        return Failure(
            MakeError(ErrorCode::invalid_argument, "valid transition id and timeout are required"));
      }
      const auto id = TransitionId{request["transition"].asUInt64()};
      auto snapshot = coordinator_.GetSnapshot();
      if (!snapshot) {
        return Failure(snapshot.error());
      }
      const auto found =
          std::find_if(snapshot.value().transitions.begin(), snapshot.value().transitions.end(),
                       [id](const auto& transition) { return transition.id == id; });
      if (found == snapshot.value().transitions.end()) {
        return Failure(MakeError(ErrorCode::not_found, "transition is not known"));
      }
      if (operation == "transition" || Terminal(found->phase)) {
        return Success(TransitionValue(*found));
      }
      const auto timeout = request["timeoutMs"].asInt64();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout};
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        auto current = coordinator_.GetSnapshot();
        if (!current) {
          return Failure(current.error());
        }
        const auto item =
            std::find_if(current.value().transitions.begin(), current.value().transitions.end(),
                         [id](const auto& value) { return value.id == id; });
        if (item == current.value().transitions.end()) {
          return Failure(MakeError(ErrorCode::not_found, "transition is not known"));
        }
        if (Terminal(item->phase)) {
          return Success(TransitionValue(*item));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return Failure(
              MakeError(ErrorCode::deadline_exceeded, "transition wait deadline expired"));
        }
      }
    }
    if (operation == "events") {
      if (!IsUnsignedInteger(request["afterRevision"])) {
        return Failure(MakeError(ErrorCode::invalid_argument, "event revision is required"));
      }
      const auto revision = request["afterRevision"].asUInt64();
      Json::Value result{Json::arrayValue};
      std::lock_guard lock(event_mutex_);
      if (!events_.empty() && revision + 1U < events_.front().revision) {
        return Failure(
            MakeError(ErrorCode::resource_exhausted, "coordinator event history was overrun"));
      }
      for (const auto& event : events_) {
        if (event.revision > revision) {
          result.append(EventValue(event));
        }
      }
      return Success(std::move(result));
    }
    return Failure(MakeError(ErrorCode::unsupported, "unknown coordinator operation"));
  }

  SystemCoordinator coordinator_;
  CoordinatorServerOptions options_;
  Descriptor listener_;
  EventSubscription event_subscription_;
  std::atomic_bool stopping_{};
  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<Work> queue_;
  std::vector<int> active_;
  std::mutex event_mutex_;
  std::deque<CoordinatorEvent> events_;
};

} // namespace

Result<std::shared_ptr<CoordinatorClient>>
ConnectCoordinatorIpc(const CoordinatorOptions& options) {
  if (options.endpoint.empty()) {
    return MakeError(ErrorCode::invalid_argument, "coordinator endpoint is required");
  }
  try {
    auto client = std::make_shared<IpcCoordinatorClient>(options.endpoint, options.connect_timeout);
    Json::Value request;
    request["operation"] = "hello";
    auto hello = client->Call(std::move(request));
    if (!hello) {
      return hello.error();
    }
    if (!hello.value()["protocol"].isIntegral() ||
        hello.value()["protocol"].asUInt() != kProtocolVersion) {
      return MakeError(ErrorCode::incompatible_abi,
                       "coordinator protocol version is incompatible: " +
                           hello.value().toStyledString(),
                       hello.value()["protocol"].asUInt());
    }
    return std::shared_ptr<CoordinatorClient>{std::move(client)};
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate coordinator IPC client");
  }
}

Result<std::unique_ptr<CoordinatorServer>>
StartCoordinatorServer(SystemCoordinator coordinator, CoordinatorServerOptions options) {
  if (options.endpoint.empty() || options.endpoint.front() != '/' ||
      options.endpoint.size() >= sizeof(sockaddr_un::sun_path) ||
      options.observation_uids.empty() || options.connection_capacity == 0U ||
      options.worker_count == 0U || options.worker_count > options.connection_capacity ||
      options.maximum_message_size < 4096U) {
    return MakeError(ErrorCode::invalid_argument, "invalid coordinator server options");
  }
  for (const auto uid : options.mutation_uids) {
    if (std::find(options.observation_uids.begin(), options.observation_uids.end(), uid) ==
        options.observation_uids.end()) {
      return MakeError(ErrorCode::invalid_argument,
                       "mutation UID must also have observation authority");
    }
  }
  Descriptor listener{::socket(AF_UNIX, SOCK_STREAM, 0)};
  if (listener.get() < 0) {
    return MakeError(ErrorCode::communication_error, "cannot create coordinator listener", errno);
  }
  const int listener_flags = ::fcntl(listener.get(), F_GETFD, 0);
  if (listener_flags < 0 || ::fcntl(listener.get(), F_SETFD, listener_flags | FD_CLOEXEC) != 0) {
    return MakeError(ErrorCode::communication_error,
                     "cannot protect coordinator listener across exec", errno);
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, options.endpoint.c_str(), options.endpoint.size() + 1U);
  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return MakeError(errno == EADDRINUSE ? ErrorCode::busy : ErrorCode::communication_error,
                     "cannot bind coordinator endpoint", errno);
  }
  if (::chmod(options.endpoint.c_str(), 0660) != 0 ||
      ::listen(listener.get(), static_cast<int>(options.connection_capacity)) != 0) {
    const auto error = errno;
    ::unlink(options.endpoint.c_str());
    return MakeError(ErrorCode::communication_error, "cannot activate coordinator endpoint", error);
  }
  const auto endpoint = options.endpoint;
  try {
    auto server = std::make_unique<IpcCoordinatorServer>(std::move(coordinator), std::move(options),
                                                         std::move(listener));
    auto started = server->Start();
    if (!started) {
      return started.error();
    }
    return std::unique_ptr<CoordinatorServer>{std::move(server)};
  } catch (...) {
    ::unlink(endpoint.c_str());
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate coordinator server");
  }
}

} // namespace ovf::exec::detail
