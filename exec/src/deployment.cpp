// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/deployment.hpp"

#include <json/json.h>

#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace ovf::exec::detail {
namespace {

constexpr std::size_t kMaximumDeploymentSize = 16U * 1024U * 1024U;

Result<std::string> Read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return MakeError(ErrorCode::configuration_error, "cannot open deployment artifact");
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumDeploymentSize) {
    return MakeError(ErrorCode::resource_exhausted, "deployment artifact exceeds size limit");
  }
  input.seekg(0, std::ios::beg);
  std::string content(static_cast<std::size_t>(size), '\0');
  if (!content.empty()) {
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
  }
  if (!input) {
    return MakeError(ErrorCode::configuration_error, "cannot read deployment artifact");
  }
  return content;
}

Result<Json::Value> Parse(std::string_view content) {
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["rejectDupKeys"] = true;
  builder["strictRoot"] = true;
  std::istringstream input(std::string{content});
  Json::Value root;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &root, &errors) || !root.isObject()) {
    return MakeError(ErrorCode::configuration_error, "deployment JSON is invalid");
  }
  return root;
}

Result<std::uint64_t> Unsigned(const Json::Value& value, std::string_view field,
                               bool allow_zero = false) {
  if (!value.isUInt64() || (!allow_zero && value.asUInt64() == 0U)) {
    return MakeError(ErrorCode::configuration_error,
                     std::string{field} + " must be a positive integer");
  }
  return value.asUInt64();
}

Result<std::chrono::milliseconds> Milliseconds(const Json::Value& value, std::string_view field,
                                               bool allow_zero = false) {
  auto parsed = Unsigned(value, field, allow_zero);
  if (!parsed ||
      parsed.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return parsed ? Result<std::chrono::milliseconds>{MakeError(ErrorCode::configuration_error,
                                                                std::string{field} +
                                                                    " exceeds the runtime range")}
                  : Result<std::chrono::milliseconds>{parsed.error()};
  }
  return std::chrono::milliseconds{static_cast<std::int64_t>(parsed.value())};
}

Result<std::string> String(const Json::Value& value, std::string_view field) {
  if (!value.isString() || value.asString().empty()) {
    return MakeError(ErrorCode::configuration_error,
                     std::string{field} + " must be a non-empty string");
  }
  return value.asString();
}

template <typename IdentifierType>
Result<std::vector<IdentifierType>> Identifiers(const Json::Value& value, std::string_view field) {
  if (!value.isArray()) {
    return MakeError(ErrorCode::configuration_error, std::string{field} + " must be an array");
  }
  std::vector<IdentifierType> result;
  try {
    result.reserve(value.size());
    for (const auto& entry : value) {
      auto id = Unsigned(entry, field);
      if (!id) {
        return id.error();
      }
      result.emplace_back(id.value());
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate deployment identifiers");
  }
  return result;
}

Result<ReadinessPolicy> Readiness(const Json::Value& value) {
  if (value == "required") {
    return ReadinessPolicy::required;
  }
  if (value == "process_started") {
    return ReadinessPolicy::process_started;
  }
  return MakeError(ErrorCode::configuration_error, "unknown readiness policy");
}

Result<ReplacementPolicy> Replacement(const Json::Value& value) {
  if (value == "reject_while_busy") {
    return ReplacementPolicy::reject_while_busy;
  }
  if (value == "queue") {
    return ReplacementPolicy::queue;
  }
  if (value == "supersede_if_safe") {
    return ReplacementPolicy::supersede_if_safe;
  }
  return MakeError(ErrorCode::configuration_error, "unknown replacement policy");
}

Result<FailureAction> Failure(const Json::Value& value) {
  if (value == "hold_observed_configuration") {
    return FailureAction::hold_observed_configuration;
  }
  if (value == "enter_fallback_mode") {
    return FailureAction::enter_fallback_mode;
  }
  if (value == "stop_domain") {
    return FailureAction::stop_domain;
  }
  if (value == "request_system_recovery") {
    return FailureAction::request_system_recovery;
  }
  return MakeError(ErrorCode::configuration_error, "unknown recovery policy");
}

Result<ConstraintKind> Constraint(const Json::Value& value) {
  if (value == "requires_mode") {
    return ConstraintKind::requires_mode;
  }
  if (value == "excludes_mode") {
    return ConstraintKind::excludes_mode;
  }
  return MakeError(ErrorCode::configuration_error, "unknown mode constraint");
}

Result<ApplicationDefinition> Application(const Json::Value& value) {
  auto id = Unsigned(value["id"], "application.id");
  auto name = String(value["name"], "application.name");
  auto readiness = Readiness(value["readiness"]);
  auto start = Milliseconds(value["startTimeoutMs"], "application.startTimeoutMs");
  auto stop = Milliseconds(value["stopTimeoutMs"], "application.stopTimeoutMs");
  auto attempts = Unsigned(value["retry"]["maxAttempts"], "application.retry.maxAttempts");
  auto delay = Milliseconds(value["retry"]["delayMs"], "application.retry.delayMs", true);
  auto dependencies = Identifiers<ApplicationId>(value["dependencies"], "application.dependencies");
  auto resources =
      Identifiers<ResourceId>(value["exclusiveResources"], "application.exclusiveResources");
  if (!id || !name || !readiness || !start || !stop || !attempts || !delay || !dependencies ||
      !resources || attempts.value() > std::numeric_limits<std::uint32_t>::max()) {
    const Error* error = nullptr;
    if (!id) {
      error = &id.error();
    } else if (!name) {
      error = &name.error();
    } else if (!readiness) {
      error = &readiness.error();
    } else if (!start) {
      error = &start.error();
    } else if (!stop) {
      error = &stop.error();
    } else if (!attempts) {
      error = &attempts.error();
    } else if (!delay) {
      error = &delay.error();
    } else if (!dependencies) {
      error = &dependencies.error();
    } else if (!resources) {
      error = &resources.error();
    }
    return error == nullptr ? Result<ApplicationDefinition>{MakeError(
                                  ErrorCode::configuration_error, "retry count is too large")}
                            : Result<ApplicationDefinition>{*error};
  }
  return ApplicationDefinition{
      ApplicationId{id.value()},
      std::move(name).value(),
      readiness.value(),
      start.value(),
      stop.value(),
      {static_cast<std::uint32_t>(attempts.value()), delay.value()},
      std::move(dependencies).value(),
      std::move(resources).value(),
  };
}

Result<ModeDefinition> Mode(const Json::Value& value) {
  auto id = Unsigned(value["id"], "mode.id");
  auto name = String(value["name"], "mode.name");
  auto applications = Identifiers<ApplicationId>(value["applications"], "mode.applications");
  if (!id || !name || !applications || !value["constraints"].isArray()) {
    return MakeError(ErrorCode::configuration_error, "mode definition is invalid");
  }
  std::vector<ModeConstraint> constraints;
  try {
    constraints.reserve(value["constraints"].size());
    for (const auto& entry : value["constraints"]) {
      auto kind = Constraint(entry["kind"]);
      auto domain = Unsigned(entry["other"]["domain"], "constraint.other.domain");
      auto mode = Unsigned(entry["other"]["mode"], "constraint.other.mode");
      if (!kind || !domain || !mode) {
        return MakeError(ErrorCode::configuration_error, "mode constraint is invalid");
      }
      constraints.push_back({kind.value(), {DomainId{domain.value()}, ModeId{mode.value()}}});
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate mode constraints");
  }
  return ModeDefinition{ModeId{id.value()}, std::move(name).value(),
                        std::move(applications).value(), std::move(constraints)};
}

Result<DomainDefinition> Domain(const Json::Value& value) {
  auto id = Unsigned(value["id"], "domain.id");
  auto name = String(value["name"], "domain.name");
  auto initial = Unsigned(value["initialMode"], "domain.initialMode");
  auto replacement = Replacement(value["replacement"]);
  auto action = Failure(value["recovery"]["action"]);
  auto deadline = Milliseconds(value["recovery"]["deadlineMs"], "domain.recovery.deadlineMs");
  if (!id || !name || !initial || !replacement || !action || !deadline ||
      !value["modes"].isArray()) {
    return MakeError(ErrorCode::configuration_error, "domain definition is invalid");
  }
  ModeId fallback;
  if (value["recovery"].isMember("fallbackMode")) {
    auto parsed = Unsigned(value["recovery"]["fallbackMode"], "domain.recovery.fallbackMode");
    if (!parsed) {
      return parsed.error();
    }
    fallback = ModeId{parsed.value()};
  }
  std::vector<ModeDefinition> modes;
  try {
    modes.reserve(value["modes"].size());
    for (const auto& entry : value["modes"]) {
      auto mode = Mode(entry);
      if (!mode) {
        return mode.error();
      }
      modes.push_back(std::move(mode).value());
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate domain modes");
  }
  return DomainDefinition{
      DomainId{id.value()},
      std::move(name).value(),
      ModeId{initial.value()},
      replacement.value(),
      {action.value(), fallback, deadline.value()},
      std::move(modes),
  };
}

Result<ValidatedModel> Execution(const Json::Value& root) {
  if (root["deploymentVersion"] != 1 || !root["applications"].isArray() ||
      !root["domains"].isArray()) {
    return MakeError(ErrorCode::configuration_error, "execution model version is unsupported");
  }
  auto generation = Unsigned(root["generation"], "generation");
  if (!generation) {
    return generation.error();
  }
  ExecutionModel model;
  model.generation = {generation.value()};
  try {
    model.applications.reserve(root["applications"].size());
    for (const auto& entry : root["applications"]) {
      auto application = Application(entry);
      if (!application) {
        return application.error();
      }
      model.applications.push_back(std::move(application).value());
    }
    model.domains.reserve(root["domains"].size());
    for (const auto& entry : root["domains"]) {
      auto domain = Domain(entry);
      if (!domain) {
        return domain.error();
      }
      model.domains.push_back(std::move(domain).value());
    }
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate execution model");
  }
  return ValidateModel(std::move(model));
}

} // namespace

Result<RuntimeDeployment> LoadRuntimeDeployment(const std::string& execution_model_path,
                                                const std::string& backend_configuration_path) {
  auto model_content = Read(execution_model_path);
  auto backend_content = Read(backend_configuration_path);
  if (!model_content) {
    return model_content.error();
  }
  if (!backend_content) {
    return backend_content.error();
  }
  auto root = Parse(model_content.value());
  auto backend = Parse(backend_content.value());
  if (!root) {
    return root.error();
  }
  if (!backend) {
    return backend.error();
  }
  auto model = Execution(root.value());
  auto backend_kind = String(backend.value()["kind"], "backend.kind");
  auto journal_path =
      String(root.value()["platform"]["persistence"]["journal"], "persistence.journal");
  auto maximum_record = Unsigned(root.value()["platform"]["persistence"]["maximumRecordSize"],
                                 "persistence.maximumRecordSize");
  const auto& synchronize = root.value()["platform"]["persistence"]["synchronize"];
  auto endpoint = String(root.value()["platform"]["coordinator"]["socket"], "coordinator.socket");
  auto queue = Unsigned(root.value()["platform"]["coordinator"]["queueCapacity"],
                        "coordinator.queueCapacity");
  auto workers =
      Unsigned(root.value()["platform"]["coordinator"]["workerCount"], "coordinator.workerCount");
  if (!model || !backend_kind || !journal_path || !maximum_record || !synchronize.isBool() ||
      !endpoint || !queue || !workers ||
      maximum_record.value() > std::numeric_limits<std::size_t>::max() ||
      queue.value() > std::numeric_limits<std::size_t>::max() ||
      workers.value() > std::numeric_limits<std::size_t>::max()) {
    return MakeError(ErrorCode::configuration_error, "runtime platform configuration is invalid");
  }
  return RuntimeDeployment{
      std::move(model).value(),
      std::move(backend_kind).value(),
      std::move(backend_content).value(),
      {std::move(journal_path).value(), static_cast<std::size_t>(maximum_record.value()),
       synchronize.asBool()},
      {std::move(endpoint).value(),
       {static_cast<std::size_t>(queue.value()), static_cast<std::size_t>(workers.value())}},
  };
}

} // namespace ovf::exec::detail
