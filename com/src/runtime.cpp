// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/runtime.hpp"
#include "ovf/com/provider_binding.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ovf::com {
namespace {

RuntimeError ToRuntimeError(ovf_com_status_v1 status) {
  switch (status) {
  case OVF_COM_STATUS_OK:
    return RuntimeError::none;
  case OVF_COM_STATUS_INVALID_ARGUMENT:
    return RuntimeError::invalid_argument;
  case OVF_COM_STATUS_INCOMPATIBLE_ABI:
    return RuntimeError::incompatible_abi;
  case OVF_COM_STATUS_ALREADY_EXISTS:
    return RuntimeError::duplicate_transport;
  case OVF_COM_STATUS_INVALID_STATE:
    return RuntimeError::invalid_state;
  case OVF_COM_STATUS_UNSUPPORTED:
    return RuntimeError::unsupported;
  case OVF_COM_STATUS_RESOURCE_EXHAUSTED:
    return RuntimeError::resource_exhausted;
  case OVF_COM_STATUS_NOT_FOUND:
    return RuntimeError::not_found;
  case OVF_COM_STATUS_CANCELLED:
    return RuntimeError::cancelled;
  case OVF_COM_STATUS_DEADLINE_EXCEEDED:
    return RuntimeError::deadline_exceeded;
  case OVF_COM_STATUS_SHUTTING_DOWN:
    return RuntimeError::shutting_down;
  case OVF_COM_STATUS_APPLICATION_ERROR:
    return RuntimeError::transport_error;
  case OVF_COM_STATUS_TRANSPORT_ERROR:
  default:
    return RuntimeError::transport_error;
  }
}

LogLevel ToLogLevel(ovf_com_log_level_v1 level) {
  switch (level) {
  case OVF_COM_LOG_DEBUG:
    return LogLevel::debug;
  case OVF_COM_LOG_WARNING:
    return LogLevel::warning;
  case OVF_COM_LOG_ERROR:
    return LogLevel::error;
  case OVF_COM_LOG_INFO:
  default:
    return LogLevel::info;
  }
}

std::string_view AsStringView(ovf_com_string_view_v1 value) {
  return {value.data == nullptr ? "" : value.data, value.data == nullptr ? 0U : value.size};
}

} // namespace

class Runtime::Impl {
public:
  struct Transport {
    const ovf_com_transport_factory_v1* factory;
    ovf_com_transport_v1* instance;

    ~Transport() {
      if (instance != nullptr) {
        factory->destroy(instance);
      }
    }

    Transport(const ovf_com_transport_factory_v1* new_factory, ovf_com_transport_v1* new_instance)
        : factory(new_factory), instance(new_instance) {}

    Transport(Transport&& other) noexcept
        : factory(std::exchange(other.factory, nullptr)),
          instance(std::exchange(other.instance, nullptr)) {}

    Transport& operator=(Transport&&) = delete;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
  };

  explicit Impl(RuntimeConfig new_config) : config(std::move(new_config)) {
    host.struct_size = sizeof(host);
    host.user_data = this;
    host.log = [](void* user_data, ovf_com_log_level_v1 level, ovf_com_string_view_v1 message) {
      auto& self = *static_cast<Impl*>(user_data);
      if (self.config.logger) {
        self.config.logger(ToLogLevel(level), AsStringView(message));
      }
    };
    host.dispatch = [](void* user_data, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release,
                       void* task_user_data) {
      if (task == nullptr || release == nullptr) {
        return OVF_COM_STATUS_INVALID_ARGUMENT;
      }
      auto& self = *static_cast<Impl*>(user_data);
      auto work = [task, release, task_user_data] {
        task(task_user_data);
        release(task_user_data);
      };
      if (self.config.dispatcher) {
        return self.config.dispatcher(std::move(work)) ? OVF_COM_STATUS_OK
                                                       : OVF_COM_STATUS_RESOURCE_EXHAUSTED;
      }
      work();
      return OVF_COM_STATUS_OK;
    };
    host.monotonic_time_ns = [](void*) {
      return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
    };
  }

  ~Impl() {
    transports.clear();
#if defined(__unix__) || defined(__APPLE__)
    for (auto library = libraries.rbegin(); library != libraries.rend(); ++library) {
      dlclose(*library);
    }
#endif
  }

  RuntimeConfig config;
  ovf_com_host_api_v1 host{};
  std::vector<Transport> transports;
  std::vector<void*> libraries;
  bool running{false};
};

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Runtime::~Runtime() { Stop(); }

Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

RuntimeError Runtime::AddTransport(const ovf_com_transport_factory_v1& factory,
                                   TransportConfig config) {
  if (impl_->running) {
    return RuntimeError::invalid_state;
  }
  if (factory.struct_size < sizeof(ovf_com_transport_factory_v1) ||
      factory.abi_version != OVF_COM_TRANSPORT_ABI_VERSION_1 || factory.create == nullptr ||
      factory.destroy == nullptr || factory.name.data == nullptr || factory.name.size == 0U) {
    return RuntimeError::incompatible_abi;
  }

  const auto name = AsStringView(factory.name);
  const auto duplicate =
      std::any_of(impl_->transports.begin(), impl_->transports.end(), [&](const auto& transport) {
        return AsStringView(transport.factory->name) == name;
      });
  if (duplicate) {
    return RuntimeError::duplicate_transport;
  }

  const ovf_com_transport_config_v1 abi_config{
      sizeof(ovf_com_transport_config_v1),
      {impl_->config.instance_name.data(), impl_->config.instance_name.size()},
      {config.configuration.data(), config.configuration.size()},
      config.max_endpoints,
      config.max_outstanding_operations};
  ovf_com_transport_v1* instance = nullptr;
  const auto result = factory.create(&impl_->host, &abi_config, &instance);
  if (result != OVF_COM_STATUS_OK) {
    return ToRuntimeError(result);
  }
  if (instance == nullptr || instance->struct_size < sizeof(ovf_com_transport_v1) ||
      instance->abi_version != OVF_COM_TRANSPORT_ABI_VERSION_1 || instance->start == nullptr ||
      instance->stop == nullptr || instance->get_capabilities == nullptr ||
      instance->watch_start == nullptr || instance->watch_stop == nullptr ||
      instance->endpoint_create == nullptr || instance->endpoint_destroy == nullptr ||
      instance->subscribe == nullptr || instance->unsubscribe == nullptr ||
      instance->publish == nullptr || instance->publish_iov == nullptr ||
      instance->loan_acquire == nullptr || instance->loan_publish == nullptr ||
      instance->loan_release == nullptr || instance->request == nullptr ||
      instance->cancel == nullptr || instance->set_request_handler == nullptr ||
      instance->respond == nullptr) {
    if (instance != nullptr) {
      factory.destroy(instance);
    }
    return RuntimeError::incompatible_abi;
  }

  ovf_com_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  const auto capability_result = instance->get_capabilities(instance, &capabilities);
  if (capability_result != OVF_COM_STATUS_OK || capabilities.struct_size < sizeof(capabilities) ||
      capabilities.max_endpoints < config.max_endpoints ||
      capabilities.max_outstanding_operations < config.max_outstanding_operations) {
    factory.destroy(instance);
    return capability_result == OVF_COM_STATUS_OK ? RuntimeError::resource_exhausted
                                                  : ToRuntimeError(capability_result);
  }

  impl_->transports.emplace_back(&factory, instance);
  return RuntimeError{};
}

RuntimeError Runtime::LoadTransport(std::string_view provider, TransportConfig config) {
  if (provider.empty() || provider.find_first_not_of(
                              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                              std::string_view::npos) {
    return RuntimeError::invalid_argument;
  }
#if defined(__unix__) || defined(__APPLE__)
#if defined(__APPLE__)
  constexpr std::string_view suffix = ".dylib";
#else
  constexpr std::string_view suffix = ".so";
#endif
  const auto filename = "libovf_com_provider_" + std::string(provider) + std::string(suffix);
  std::vector<std::string> candidates;
  if (const auto* search_path = std::getenv("OVF_COM_PROVIDER_PATH"); search_path != nullptr) {
    std::string paths(search_path);
    std::size_t offset = 0;
    while (offset <= paths.size()) {
      const auto end = paths.find(':', offset);
      auto directory = paths.substr(offset, end - offset);
      candidates.emplace_back(directory.empty() ? filename : directory + "/" + filename);
      if (end == std::string::npos) {
        break;
      }
      offset = end + 1;
    }
  }
  candidates.emplace_back(filename);

  void* library = nullptr;
  for (const auto& candidate : candidates) {
    library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr) {
      break;
    }
  }
  if (library == nullptr) {
    return RuntimeError::not_found;
  }
  using Query = const ovf_com_transport_factory_v1* (*)();
  const auto query = reinterpret_cast<Query>(dlsym(library, "ovf_com_transport_query_v1"));
  if (query == nullptr) {
    dlclose(library);
    return RuntimeError::incompatible_abi;
  }
  const auto* factory = query();
  if (factory == nullptr) {
    dlclose(library);
    return RuntimeError::incompatible_abi;
  }
  const auto result = AddTransport(*factory, std::move(config));
  if (result != RuntimeError::none) {
    dlclose(library);
    return result;
  }
  impl_->libraries.push_back(library);
  return RuntimeError::none;
#else
  static_cast<void>(config);
  return RuntimeError::unsupported;
#endif
}

RuntimeError Runtime::Start() {
  if (impl_->running) {
    return RuntimeError::invalid_state;
  }

  std::size_t started = 0;
  for (auto& transport : impl_->transports) {
    const auto result = transport.instance->start(transport.instance);
    if (result != OVF_COM_STATUS_OK) {
      while (started > 0U) {
        --started;
        static_cast<void>(
            impl_->transports[started].instance->stop(impl_->transports[started].instance));
      }
      return ToRuntimeError(result);
    }
    ++started;
  }
  impl_->running = true;
  return RuntimeError{};
}

void Runtime::Stop() noexcept {
  if (!impl_ || !impl_->running) {
    return;
  }
  for (auto it = impl_->transports.rbegin(); it != impl_->transports.rend(); ++it) {
    static_cast<void>(it->instance->stop(it->instance));
  }
  impl_->running = false;
}

bool Runtime::IsRunning() const noexcept { return impl_->running; }

std::vector<std::string> Runtime::TransportNames() const {
  std::vector<std::string> names;
  names.reserve(impl_->transports.size());
  for (const auto& transport : impl_->transports) {
    names.emplace_back(AsStringView(transport.factory->name));
  }
  return names;
}

ovf_com_transport_v1* detail::RuntimeAccess::find(Runtime& runtime,
                                                  std::string_view name) noexcept {
  if (!runtime.impl_ || !runtime.impl_->running) {
    return nullptr;
  }
  const auto transport = std::find_if(
      runtime.impl_->transports.begin(), runtime.impl_->transports.end(),
      [name](const auto& candidate) { return AsStringView(candidate.factory->name) == name; });
  return transport == runtime.impl_->transports.end() ? nullptr : transport->instance;
}

} // namespace ovf::com
