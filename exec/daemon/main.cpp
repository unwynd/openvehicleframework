// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/artifact_integrity.hpp"
#include "ovf/exec/internal/backend_binding.hpp"
#include "ovf/exec/internal/backend_loader.hpp"
#include "ovf/exec/internal/coordinator_server.hpp"
#include "ovf/exec/internal/coordinator_service.hpp"
#include "ovf/exec/internal/deployment.hpp"
#include "ovf/exec/internal/engine.hpp"
#include "ovf/exec/internal/file_journal.hpp"

#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <pthread.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

struct Arguments final {
  RuntimeArtifactPaths artifacts;
};

Result<Arguments> ParseArguments(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (index + 1 >= argc) {
      return MakeError(ErrorCode::invalid_argument, "every daemon option requires a value");
    }
    std::string* target{};
    if (option == "--model") {
      target = &result.artifacts.execution_model;
    } else if (option == "--backend") {
      target = &result.artifacts.backend_configuration;
    } else if (option == "--services") {
      target = &result.artifacts.services_directory;
    } else if (option == "--manifest") {
      target = &result.artifacts.manifest;
    } else {
      return MakeError(ErrorCode::invalid_argument,
                       "unknown execution daemon option: " + std::string{option});
    }
    if (!target->empty()) {
      return MakeError(ErrorCode::invalid_argument,
                       "execution daemon option was specified more than once");
    }
    *target = argv[++index];
  }
  if (result.artifacts.execution_model.empty() || result.artifacts.backend_configuration.empty() ||
      result.artifacts.services_directory.empty() || result.artifacts.manifest.empty()) {
    return MakeError(
        ErrorCode::invalid_argument,
        "usage: ovf-execd --model PATH --backend PATH --services PATH --manifest PATH");
  }
  return result;
}

Result<sigset_t> BlockTerminationSignals() {
  sigset_t signals;
  if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTERM) != 0 ||
      sigaddset(&signals, SIGINT) != 0 || ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    return MakeError(ErrorCode::backend_error, "cannot block execution daemon termination signals");
  }
  return signals;
}

int Failure(const Error& error) {
  std::cerr << "ovf-execd: " << error.message;
  if (error.support_data != 0U) {
    std::cerr << " (" << error.support_data << ')';
  }
  std::cerr << '\n';
  return 1;
}

Result<void> Run(const Arguments& arguments, const sigset_t& signals) {
  auto verified = VerifyRuntimeArtifacts(arguments.artifacts);
  if (!verified) {
    return verified.error();
  }
  auto deployment = LoadRuntimeDeployment(arguments.artifacts.execution_model,
                                          arguments.artifacts.backend_configuration);
  if (!deployment) {
    return deployment.error();
  }
  auto library = LoadBackendLibrary(deployment.value().backend_library);
  if (!library) {
    return library.error();
  }
  auto backend =
      BindBackend(library.value().Factory(),
                  {.configuration = deployment.value().backend_configuration,
                   .required_parallel_operations = static_cast<std::uint32_t>(
                       deployment.value().coordinator.limits.worker_count),
                   .logger = [](BackendLogLevel level, std::string_view message) {
                     if (level == BackendLogLevel::warning || level == BackendLogLevel::error) {
                       std::cerr << "ovf-execd backend: " << message << '\n';
                     }
                   }});
  if (!backend) {
    return backend.error();
  }
  const auto& factory = library.value().Factory();
  const std::string_view backend_name{factory.name.data == nullptr ? "" : factory.name.data,
                                      factory.name.data == nullptr ? 0U : factory.name.size};
  if (backend_name != deployment.value().backend_kind) {
    return MakeError(ErrorCode::configuration_error,
                     "loaded backend kind does not match deployment");
  }
  auto journal = OpenFileTransitionJournal(deployment.value().journal);
  if (!journal) {
    return journal.error();
  }
  auto engine = ExecutionEngine::Create(std::move(deployment.value().model),
                                        std::move(backend).value(), std::move(journal).value());
  if (!engine) {
    return engine.error();
  }
  auto coordinator = detail_CoordinatorFactory::Create(std::move(engine).value(),
                                                       {.observe = true, .mutate = true},
                                                       deployment.value().coordinator.limits);
  if (!coordinator) {
    return coordinator.error();
  }
  auto server = StartCoordinatorServer(
      std::move(coordinator).value(),
      {.endpoint = deployment.value().coordinator.socket,
       .observation_uids = deployment.value().coordinator.observation_uids,
       .mutation_uids = deployment.value().coordinator.mutation_uids,
       .connection_capacity = deployment.value().coordinator.connection_capacity,
       .worker_count = deployment.value().coordinator.limits.worker_count,
       .maximum_message_size = deployment.value().coordinator.maximum_message_size});
  if (!server) {
    return server.error();
  }
  int signal{};
  const int waited = ::sigwait(&signals, &signal);
  if (waited != 0) {
    return MakeError(ErrorCode::backend_error, "cannot wait for execution daemon termination",
                     waited);
  }
  return {};
}

} // namespace

int main(int argc, char** argv) {
  auto arguments = ParseArguments(argc, argv);
  if (!arguments) {
    return Failure(arguments.error());
  }
  auto signals = BlockTerminationSignals();
  if (!signals) {
    return Failure(signals.error());
  }
  auto result = Run(arguments.value(), signals.value());
  return result ? 0 : Failure(result.error());
}
