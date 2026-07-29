// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/backends/dinit.hpp"

#include <dinit-client.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ovf::exec::backends {
namespace {

using dinit_cptypes::handle_t;
using dinit_cptypes::srvname_len_t;

constexpr std::uint16_t kMinimumProtocol = 5U;
constexpr std::uint16_t kMaximumProtocol = 6U;
constexpr auto kStatusPollInterval = std::chrono::milliseconds(5);

class Connection final {
public:
  static Result<Connection> Open(const std::string& path, Deadline deadline) noexcept {
    try {
      const int descriptor = connect_to_daemon(path.c_str());
      Connection connection(descriptor, 0U, {});
      auto configured = connection.SetDeadline(deadline);
      if (!configured) {
        return configured.error();
      }
      connection.protocol_ = check_protocol_version(kMinimumProtocol, kMaximumProtocol,
                                                    connection.buffer_, descriptor);
      return connection;
    } catch (control_sock_conn_err& error) {
      return MakeError(ErrorCode::backend_unavailable, "cannot connect to dinit control socket",
                       static_cast<std::uint64_t>(error.get_err()));
    } catch (const cp_old_client_exception&) {
      return MakeError(ErrorCode::unsupported, "dinit requires a newer control client");
    } catch (const cp_old_server_exception&) {
      return MakeError(ErrorCode::unsupported, "dinit control protocol is too old");
    } catch (const cp_read_exception& error) {
      return MakeError(ErrorCode::communication_error, "dinit protocol negotiation read failed",
                       static_cast<std::uint64_t>(error.errcode));
    } catch (const cp_write_exception& error) {
      return MakeError(ErrorCode::communication_error, "dinit protocol negotiation write failed",
                       static_cast<std::uint64_t>(error.errcode));
    } catch (...) {
      return MakeError(ErrorCode::backend_error, "dinit protocol negotiation failed");
    }
  }

  ~Connection() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  Connection(Connection&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)), protocol_(other.protocol_),
        buffer_(std::move(other.buffer_)) {}
  Connection& operator=(Connection&&) = delete;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int descriptor() const noexcept { return descriptor_; }
  std::uint16_t protocol() const noexcept { return protocol_; }
  cpbuffer_t& buffer() noexcept { return buffer_; }

  Result<void> SetDeadline(Deadline deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
      return MakeError(ErrorCode::deadline_exceeded, "dinit operation deadline expired");
    }
    const auto remaining = deadline - now;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(remaining - seconds);
    const struct timeval timeout{static_cast<time_t>(seconds.count()),
                                 static_cast<suseconds_t>(microseconds.count())};
    if (::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(descriptor_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
      return MakeError(ErrorCode::communication_error,
                       "cannot configure dinit control socket deadline", errno);
    }
    return {};
  }

private:
  Connection(int descriptor, std::uint16_t protocol, cpbuffer_t buffer)
      : descriptor_(descriptor), protocol_(protocol), buffer_(std::move(buffer)) {}

  int descriptor_;
  std::uint16_t protocol_;
  cpbuffer_t buffer_;
};

struct LoadedService final {
  handle_t handle{};
  service_state_t state{service_state_t::STOPPED};
};

Result<LoadedService> Load(Connection& connection, std::string_view service, bool find_only,
                           Deadline deadline) noexcept {
  if (service.empty() || service.size() > std::numeric_limits<srvname_len_t>::max()) {
    return MakeError(ErrorCode::invalid_argument, "invalid dinit service name length");
  }
  auto configured = connection.SetDeadline(deadline);
  if (!configured) {
    return configured.error();
  }
  try {
    const auto length = static_cast<srvname_len_t>(service.size());
    std::vector<char> request(1U + sizeof(length) + service.size());
    request[0] = static_cast<char>(find_only ? cp_cmd::FINDSERVICE : cp_cmd::LOADSERVICE);
    std::memcpy(request.data() + 1, &length, sizeof(length));
    std::memcpy(request.data() + 1 + sizeof(length), service.data(), service.size());
    write_all_x(connection.descriptor(), request);
    wait_for_reply(connection.buffer(), connection.descriptor());
    const auto reply = static_cast<cp_rply>(connection.buffer()[0]);
    if (reply == cp_rply::NOSERVICE) {
      connection.buffer().consume(1);
      return MakeError(ErrorCode::not_found, "dinit service is not loaded or defined");
    }
    if (reply != cp_rply::SERVICERECORD) {
      return MakeError(ErrorCode::backend_error, "unexpected dinit load-service response",
                       static_cast<std::uint64_t>(reply));
    }
    constexpr auto response_size = 3U + sizeof(handle_t);
    fill_buffer_to(connection.buffer(), connection.descriptor(), response_size);
    LoadedService loaded;
    loaded.state = static_cast<service_state_t>(connection.buffer()[1]);
    connection.buffer().extract(reinterpret_cast<char*>(&loaded.handle), 2, sizeof(loaded.handle));
    connection.buffer().consume(response_size);
    return loaded;
  } catch (const cp_read_exception& error) {
    return MakeError(
        error.errcode == EAGAIN || error.errcode == EWOULDBLOCK ? ErrorCode::deadline_exceeded
                                                                : ErrorCode::communication_error,
        "dinit load-service response failed", static_cast<std::uint64_t>(error.errcode));
  } catch (const cp_write_exception& error) {
    return MakeError(ErrorCode::communication_error, "dinit load-service request failed",
                     static_cast<std::uint64_t>(error.errcode));
  } catch (...) {
    return MakeError(ErrorCode::backend_error, "dinit load-service operation failed");
  }
}

detail::BackendEvidence Evidence(service_state_t state, stopped_reason_t reason = {}, int code = 0,
                                 int signal = 0) {
  ApplicationState mapped{ApplicationState::unknown};
  switch (state) {
  case service_state_t::STARTING:
    mapped = ApplicationState::starting;
    break;
  case service_state_t::STARTED:
    mapped = ApplicationState::ready;
    break;
  case service_state_t::STOPPING:
    mapped = ApplicationState::stopping;
    break;
  case service_state_t::STOPPED:
    mapped = reason == stopped_reason_t::FAILED || reason == stopped_reason_t::EXECFAILED ||
                     reason == stopped_reason_t::TIMEDOUT || reason == stopped_reason_t::DEPFAILED
                 ? ApplicationState::failed
                 : ApplicationState::stopped;
    break;
  }
  return {mapped, code, signal, static_cast<std::uint64_t>(reason), {}};
}

Result<detail::BackendEvidence> Status(Connection& connection, handle_t handle,
                                       Deadline deadline) noexcept {
  auto configured = connection.SetDeadline(deadline);
  if (!configured) {
    return configured.error();
  }
  try {
    const char command = static_cast<char>(connection.protocol() >= 6U ? cp_cmd::SERVICESTATUS6
                                                                       : cp_cmd::SERVICESTATUS5);
    const auto request = membuf().append(command).append(handle);
    write_all_x(connection.descriptor(), request);
    wait_for_reply(connection.buffer(), connection.descriptor());
    if (static_cast<cp_rply>(connection.buffer()[0]) != cp_rply::SERVICESTATUS) {
      return MakeError(ErrorCode::backend_error, "unexpected dinit status response");
    }
    connection.buffer().consume(1);
    const auto status_size =
        connection.protocol() >= 6U ? STATUS_BUFFER6_SIZE : STATUS_BUFFER5_SIZE;
    fill_buffer_to(connection.buffer(), connection.descriptor(), status_size + 1U);
    connection.buffer().consume(1);
    const auto state = static_cast<service_state_t>(connection.buffer()[0]);
    const auto flags = static_cast<unsigned char>(connection.buffer()[2]);
    const auto reason = static_cast<stopped_reason_t>(connection.buffer()[3]);
    int process_code{};
    int process_status{};
    if ((flags & 16U) == 0U) {
      connection.buffer().extract(reinterpret_cast<char*>(&process_code), 6, sizeof(process_code));
      connection.buffer().extract(reinterpret_cast<char*>(&process_status),
                                  6 + sizeof(process_code), sizeof(process_status));
    }
    connection.buffer().consume(status_size);
    const int exit_code = process_code == CLD_EXITED ? process_status : 0;
    const int terminating_signal =
        process_code != 0 && process_code != CLD_EXITED ? process_status : 0;
    auto evidence = Evidence(state, reason, exit_code, terminating_signal);
    evidence.native_code =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(process_code)) << 32U) |
        static_cast<std::uint32_t>(reason);
    return evidence;
  } catch (const cp_read_exception& error) {
    return MakeError(error.errcode == EAGAIN || error.errcode == EWOULDBLOCK
                         ? ErrorCode::deadline_exceeded
                         : ErrorCode::communication_error,
                     "dinit status response failed", static_cast<std::uint64_t>(error.errcode));
  } catch (const cp_write_exception& error) {
    return MakeError(ErrorCode::communication_error, "dinit status request failed",
                     static_cast<std::uint64_t>(error.errcode));
  } catch (...) {
    return MakeError(ErrorCode::backend_error, "dinit status operation failed");
  }
}

Result<void> Command(Connection& connection, handle_t handle, cp_cmd command,
                     Deadline deadline) noexcept {
  auto configured = connection.SetDeadline(deadline);
  if (!configured) {
    return configured.error();
  }
  try {
    const char flags = command == cp_cmd::STOPSERVICE ? 2 : 0;
    const auto request = membuf().append(static_cast<char>(command)).append(flags).append(handle);
    write_all_x(connection.descriptor(), request);
    observed_states_t observed;
    wait_for_reply(connection.buffer(), connection.descriptor(), handle, &observed);
    const auto reply = static_cast<cp_rply>(connection.buffer()[0]);
    connection.buffer().consume(1);
    if (reply == cp_rply::ACK || reply == cp_rply::ALREADYSS) {
      return {};
    }
    if (reply == cp_rply::PINNEDSTARTED || reply == cp_rply::PINNEDSTOPPED ||
        reply == cp_rply::DEPENDENTS) {
      return MakeError(ErrorCode::permission_denied,
                       "dinit service policy rejected lifecycle command",
                       static_cast<std::uint64_t>(reply));
    }
    if (reply == cp_rply::SHUTTINGDOWN) {
      return MakeError(ErrorCode::backend_unavailable, "dinit is shutting down");
    }
    return MakeError(ErrorCode::backend_error, "unexpected dinit lifecycle response",
                     static_cast<std::uint64_t>(reply));
  } catch (const cp_read_exception& error) {
    return MakeError(error.errcode == EAGAIN || error.errcode == EWOULDBLOCK
                         ? ErrorCode::deadline_exceeded
                         : ErrorCode::communication_error,
                     "dinit lifecycle response failed", static_cast<std::uint64_t>(error.errcode));
  } catch (const cp_write_exception& error) {
    return MakeError(ErrorCode::communication_error, "dinit lifecycle request failed",
                     static_cast<std::uint64_t>(error.errcode));
  } catch (...) {
    return MakeError(ErrorCode::backend_error, "dinit lifecycle command failed");
  }
}

class DinitBackend final : public detail::ProcessBackend {
public:
  explicit DinitBackend(DinitConfig config) : config_(std::move(config)) {}

  Result<detail::BackendEvidence> Inspect(ApplicationId application) noexcept override {
    const auto service = Service(application);
    if (!service) {
      return service.error();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto connection = Connection::Open(config_.control_socket, deadline);
    if (!connection) {
      return connection.error();
    }
    auto loaded = Load(connection.value(), service.value(), true, deadline);
    if (!loaded) {
      return loaded.error();
    }
    return Status(connection.value(), loaded.value().handle, deadline);
  }

  Result<detail::BackendEvidence> Start(ApplicationId application,
                                        Deadline deadline) noexcept override {
    return Change(application, cp_cmd::STARTSERVICE, ApplicationState::ready, StopReason::none,
                  deadline);
  }

  Result<detail::BackendEvidence> Stop(ApplicationId application, StopReason reason,
                                       Deadline deadline) noexcept override {
    return Change(application, cp_cmd::STOPSERVICE, ApplicationState::stopped, reason, deadline);
  }

private:
  Result<std::string_view> Service(ApplicationId application) const noexcept {
    const auto found = config_.services.find(application);
    if (found == config_.services.end()) {
      return MakeError(ErrorCode::not_found, "application has no dinit service mapping",
                       application.value());
    }
    return std::string_view(found->second);
  }

  Result<detail::BackendEvidence> Change(ApplicationId application, cp_cmd command,
                                         ApplicationState target, StopReason,
                                         Deadline deadline) noexcept {
    const auto service = Service(application);
    if (!service) {
      return service.error();
    }
    auto connection = Connection::Open(config_.control_socket, deadline);
    if (!connection) {
      return connection.error();
    }
    auto loaded = Load(connection.value(), service.value(), false, deadline);
    if (!loaded) {
      return loaded.error();
    }
    auto issued = Command(connection.value(), loaded.value().handle, command, deadline);
    if (!issued) {
      return issued.error();
    }
    for (;;) {
      auto status = Status(connection.value(), loaded.value().handle, deadline);
      if (!status) {
        return status.error();
      }
      if (status.value().state == target) {
        return status;
      }
      if (status.value().state == ApplicationState::failed) {
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return MakeError(ErrorCode::deadline_exceeded,
                         "dinit service did not reach the requested state");
      }
      std::this_thread::sleep_for(kStatusPollInterval);
    }
  }

  DinitConfig config_;
};

bool ValidServiceName(std::string_view name) {
  return !name.empty() && name.size() <= std::numeric_limits<srvname_len_t>::max() &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '_' || character == '-' ||
                  character == '@' || character == '.';
         });
}

} // namespace

Result<std::unique_ptr<detail::ProcessBackend>> CreateDinitBackend(DinitConfig config) {
  if (config.control_socket.empty() || config.control_socket.front() != '/') {
    return MakeError(ErrorCode::invalid_argument, "dinit control socket path must be absolute");
  }
  if (config.services.empty()) {
    return MakeError(ErrorCode::invalid_argument,
                     "dinit backend needs at least one application mapping");
  }
  for (const auto& [application, service] : config.services) {
    if (!application || !ValidServiceName(service)) {
      return MakeError(ErrorCode::invalid_argument,
                       "invalid application identifier or dinit service name");
    }
  }
  try {
    return std::unique_ptr<detail::ProcessBackend>(new DinitBackend(std::move(config)));
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "dinit backend allocation failed");
  }
}

} // namespace ovf::exec::backends
