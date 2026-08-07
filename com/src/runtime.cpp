// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/runtime.hpp"
#include "ovf/com/provider_binding.hpp"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ovf::com {
namespace {

Error ToRuntimeError(ovf_com_status_v1 status) {
  switch (status) {
  case OVF_COM_STATUS_OK:
    return Error::none;
  case OVF_COM_STATUS_INVALID_ARGUMENT:
    return Error::invalid_argument;
  case OVF_COM_STATUS_INCOMPATIBLE_ABI:
    return Error::incompatible_abi;
  case OVF_COM_STATUS_ALREADY_EXISTS:
    return Error::duplicate_transport;
  case OVF_COM_STATUS_INVALID_STATE:
    return Error::invalid_state;
  case OVF_COM_STATUS_UNSUPPORTED:
    return Error::unsupported;
  case OVF_COM_STATUS_RESOURCE_EXHAUSTED:
    return Error::resource_exhausted;
  case OVF_COM_STATUS_NOT_FOUND:
    return Error::not_found;
  case OVF_COM_STATUS_CANCELLED:
    return Error::cancelled;
  case OVF_COM_STATUS_DEADLINE_EXCEEDED:
    return Error::deadline_exceeded;
  case OVF_COM_STATUS_SHUTTING_DOWN:
    return Error::shutting_down;
  case OVF_COM_STATUS_APPLICATION_ERROR:
    return Error::provider_failure;
  case OVF_COM_STATUS_TRANSPORT_ERROR:
  default:
    return Error::provider_failure;
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

auto Hex(char value) -> std::optional<std::uint8_t> {
  if (value >= '0' && value <= '9')
    return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F')
    return static_cast<std::uint8_t>(value - 'A' + 10);
  return std::nullopt;
}

auto ParseUuid(Json::Value const& value) -> std::optional<Uuid> {
  if (!value.isString())
    return std::nullopt;
  auto const text = value.asString();
  if (text.size() != 36U || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    return std::nullopt;
  Uuid result{};
  std::size_t output{};
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '-') {
      ++index;
      continue;
    }
    if (index + 1U >= text.size() || output >= result.bytes.size())
      return std::nullopt;
    auto high = Hex(text[index]);
    auto low = Hex(text[index + 1U]);
    if (!high || !low)
      return std::nullopt;
    result.bytes[output++] = static_cast<std::uint8_t>((*high << 4U) | *low);
    index += 2U;
  }
  return output == result.bytes.size() ? std::optional<Uuid>{result} : std::nullopt;
}

auto ToHealthState(ovf_com_health_state_v1 state) -> TransportHealthState {
  switch (state) {
  case OVF_COM_HEALTH_INITIALIZING:
    return TransportHealthState::initializing;
  case OVF_COM_HEALTH_READY:
    return TransportHealthState::ready;
  case OVF_COM_HEALTH_DEGRADED:
    return TransportHealthState::degraded;
  case OVF_COM_HEALTH_FAILED:
    return TransportHealthState::failed;
  case OVF_COM_HEALTH_STOPPED:
  default:
    return TransportHealthState::stopped;
  }
}

auto ToDiagnosticOperation(ovf_com_diagnostic_operation_v1 operation) -> DiagnosticOperation {
  switch (operation) {
  case OVF_COM_DIAGNOSTIC_DISCOVERY:
    return DiagnosticOperation::discovery;
  case OVF_COM_DIAGNOSTIC_ENDPOINT:
    return DiagnosticOperation::endpoint;
  case OVF_COM_DIAGNOSTIC_SUBSCRIPTION:
    return DiagnosticOperation::subscription;
  case OVF_COM_DIAGNOSTIC_PUBLISH:
    return DiagnosticOperation::publish;
  case OVF_COM_DIAGNOSTIC_REQUEST:
    return DiagnosticOperation::request;
  case OVF_COM_DIAGNOSTIC_RESPONSE:
    return DiagnosticOperation::response;
  case OVF_COM_DIAGNOSTIC_PROVIDER:
  default:
    return DiagnosticOperation::provider;
  }
}

struct ConfiguredRoute {
  std::string instance;
  RouteBinding binding;
};

auto ParseDeployment(std::string const& path, std::vector<TransportRegistration>& transports,
                     std::vector<ConfiguredRoute>& routes) -> Error {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return Error::not_found;
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["rejectDupKeys"] = true;
  builder["strictRoot"] = true;
  Json::Value root;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &root, &errors) || !root.isObject() ||
      root["runtimeDeploymentVersion"].asUInt64() != 1U || !root["transports"].isArray() ||
      !root["routes"].isArray())
    return Error::invalid_argument;
  for (auto const& transport : root["transports"]) {
    if (!transport.isObject() || !transport["provider"].isString() ||
        !transport["configuration"].isString() || !transport["maxEndpoints"].isUInt() ||
        !transport["maxOutstandingOperations"].isUInt() ||
        !transport["startTimeoutMs"].isUInt64() || !transport["stopTimeoutMs"].isUInt64() ||
        transport["startTimeoutMs"].asUInt64() == 0U || transport["stopTimeoutMs"].asUInt64() == 0U)
      return Error::invalid_argument;
    transports.push_back(
        {transport["provider"].asString(),
         {transport["configuration"].asString(), transport["maxEndpoints"].asUInt(),
          transport["maxOutstandingOperations"].asUInt(),
          std::chrono::milliseconds(transport["startTimeoutMs"].asUInt64()),
          std::chrono::milliseconds(transport["stopTimeoutMs"].asUInt64())}});
  }
  for (auto const& route : root["routes"]) {
    auto service = ParseUuid(route["serviceId"]);
    auto instance = ParseUuid(route["instanceId"]);
    if (!route.isObject() || !service || !instance || !route["instance"].isString() ||
        !route["provider"].isString() || !route["nativeService"].isString() ||
        !route["elements"].isArray() || !route["maxPayloadSize"].isUInt64() ||
        !route["historyDepth"].isUInt() || !route["priority"].isInt())
      return Error::invalid_argument;
    RouteBinding binding{*service,
                         *instance,
                         1U,
                         route["maxPayloadSize"].asUInt64(),
                         route["historyDepth"].asUInt(),
                         {},
                         route["provider"].asString(),
                         route["nativeService"].asString(),
                         route["priority"].asInt()};
    for (auto const& element : route["elements"]) {
      auto id = ParseUuid(element["id"]);
      if (!element.isObject() || !id || !element["event"].isString() ||
          !element["method"].isString() ||
          (!element["get"].isNull() && !element["get"].isString()) ||
          (!element["set"].isNull() && !element["set"].isString()))
        return Error::invalid_argument;
      binding.native_elements.push_back({*id, element["event"].asString(),
                                         element["method"].asString(), element["get"].asString(),
                                         element["set"].asString()});
    }
    routes.push_back({route["instance"].asString(), std::move(binding)});
  }
  return Error::none;
}

} // namespace

class Runtime::Impl {
public:
  struct HealthState {
    Impl* owner{};
    mutable std::mutex mutex;
    std::condition_variable changed;
    TransportHealth value;
  };

  struct Transport {
    const ovf_com_transport_factory_v1* factory;
    ovf_com_transport_v1* instance;
    TransportConfig config;
    std::shared_ptr<HealthState> health;
    bool health_supported{};

    ~Transport() {
      if (instance != nullptr) {
        factory->destroy(instance);
      }
    }

    Transport(const ovf_com_transport_factory_v1* new_factory, ovf_com_transport_v1* new_instance,
              TransportConfig new_config, std::shared_ptr<HealthState> new_health,
              bool new_health_supported)
        : factory(new_factory), instance(new_instance), config(std::move(new_config)),
          health(std::move(new_health)), health_supported(new_health_supported) {}

    Transport(Transport&& other) noexcept
        : factory(std::exchange(other.factory, nullptr)),
          instance(std::exchange(other.instance, nullptr)), config(std::move(other.config)),
          health(std::move(other.health)), health_supported(other.health_supported) {}

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

  static void OnProviderHealth(void* user, ovf_com_health_v1 const* update) {
    if (!user || !update || update->struct_size < sizeof(*update))
      return;
    auto& state = *static_cast<HealthState*>(user);
    std::function<void(TransportHealth const&)> callback;
    TransportHealth snapshot;
    {
      std::lock_guard lock(state.mutex);
      state.value.state = ToHealthState(update->state);
      state.value.sequence = update->sequence;
      state.value.diagnostic = {state.value.provider,
                                ToRuntimeError(update->diagnostic.status),
                                ToDiagnosticOperation(update->diagnostic.operation),
                                update->diagnostic.native_code,
                                update->diagnostic.endpoint,
                                update->diagnostic.operation_handle,
                                std::string(AsStringView(update->diagnostic.message))};
      snapshot = state.value;
    }
    state.changed.notify_all();
    {
      std::lock_guard lock(state.owner->health_callback_mutex);
      callback = state.owner->health_callback;
    }
    if (callback)
      callback(snapshot);
  }

  static void OnProviderDiagnostic(void* user, ovf_com_diagnostic_v1 const* update) {
    if (!user || !update || update->struct_size < sizeof(*update))
      return;
    auto& state = *static_cast<HealthState*>(user);
    CommunicationDiagnostic diagnostic{state.value.provider,
                                       ToRuntimeError(update->status),
                                       ToDiagnosticOperation(update->operation),
                                       update->native_code,
                                       update->endpoint,
                                       update->operation_handle,
                                       std::string(AsStringView(update->message))};
    std::function<void(CommunicationDiagnostic const&)> callback;
    {
      std::lock_guard lock(state.owner->diagnostic_callback_mutex);
      callback = state.owner->diagnostic_callback;
    }
    if (callback)
      callback(diagnostic);
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
  std::vector<ConfiguredRoute> routes;
  std::vector<void*> libraries;
  mutable std::mutex health_callback_mutex;
  std::function<void(TransportHealth const&)> health_callback;
  mutable std::mutex diagnostic_callback_mutex;
  std::function<void(CommunicationDiagnostic const&)> diagnostic_callback;
  bool running{false};
};

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Runtime::~Runtime() { Stop(); }

Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

ApplicationRuntime::ApplicationRuntime(RuntimeConfig config,
                                       std::vector<TransportRegistration> transports)
    : runtime_(std::move(config)) {
  std::vector<std::string> loaded;
  for (auto& transport : transports) {
    if (std::find(loaded.begin(), loaded.end(), transport.provider) != loaded.end())
      continue;
    error_ = runtime_.LoadTransport(transport.provider, std::move(transport.config));
    if (error_ != Error::none)
      return;
    loaded.push_back(std::move(transport.provider));
  }
  error_ = runtime_.Start();
}

ApplicationRuntime::ApplicationRuntime(RuntimeConfig config, DeploymentConfig deployment)
    : runtime_(std::move(config)) {
  error_ = runtime_.ConfigureDeployment(deployment);
  if (error_ == Error::none)
    error_ = runtime_.Start();
}

Error Runtime::ConfigureDeployment(DeploymentConfig const& deployment) {
  if (impl_->running || deployment.path.empty())
    return Error::invalid_state;
  std::vector<TransportRegistration> transports;
  std::vector<ConfiguredRoute> routes;
  auto parsed = ParseDeployment(deployment.path, transports, routes);
  if (parsed != Error::none)
    return parsed;
  for (auto& transport : transports) {
    auto loaded = LoadTransport(transport.provider, std::move(transport.config));
    if (loaded != Error::none)
      return loaded;
  }
  impl_->routes = std::move(routes);
  return Error::none;
}

Error Runtime::AddTransport(const ovf_com_transport_factory_v1& factory, TransportConfig config) {
  if (impl_->running) {
    return Error::invalid_state;
  }
  if (factory.struct_size < sizeof(ovf_com_transport_factory_v1) ||
      factory.abi_version != OVF_COM_TRANSPORT_ABI_VERSION_1 || factory.create == nullptr ||
      factory.destroy == nullptr || factory.name.data == nullptr || factory.name.size == 0U) {
    return Error::incompatible_abi;
  }

  const auto name = AsStringView(factory.name);
  const auto duplicate =
      std::any_of(impl_->transports.begin(), impl_->transports.end(), [&](const auto& transport) {
        return AsStringView(transport.factory->name) == name;
      });
  if (duplicate) {
    return Error::duplicate_transport;
  }

  const ovf_com_transport_config_v1 abi_config{
      sizeof(ovf_com_transport_config_v1),
      {impl_->config.instance_name.data(), impl_->config.instance_name.size()},
      {config.configuration.data(), config.configuration.size()},
      config.max_endpoints,
      config.max_outstanding_operations,
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(config.start_timeout).count()),
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(config.stop_timeout).count())};
  ovf_com_transport_v1* instance = nullptr;
  const auto result = factory.create(&impl_->host, &abi_config, &instance);
  if (result != OVF_COM_STATUS_OK) {
    return ToRuntimeError(result);
  }
  if (instance == nullptr || instance->struct_size < OVF_COM_TRANSPORT_V1_BASE_SIZE ||
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
    return Error::incompatible_abi;
  }

  ovf_com_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  const auto capability_result = instance->get_capabilities(instance, &capabilities);
  if (capability_result != OVF_COM_STATUS_OK || capabilities.struct_size < sizeof(capabilities) ||
      capabilities.max_endpoints < config.max_endpoints ||
      capabilities.max_outstanding_operations < config.max_outstanding_operations) {
    factory.destroy(instance);
    return capability_result == OVF_COM_STATUS_OK ? Error::resource_exhausted
                                                  : ToRuntimeError(capability_result);
  }
  const auto has_health =
      (capabilities.feature_bits & OVF_COM_CAP_HEALTH) != 0U &&
      instance->struct_size >= offsetof(ovf_com_transport_v1, set_diagnostic_handler) +
                                   sizeof(instance->set_diagnostic_handler) &&
      instance->get_health && instance->set_health_handler;
  const auto has_diagnostics =
      (capabilities.feature_bits & OVF_COM_CAP_DIAGNOSTICS) != 0U &&
      instance->struct_size >= offsetof(ovf_com_transport_v1, set_diagnostic_handler) +
                                   sizeof(instance->set_diagnostic_handler) &&
      instance->set_diagnostic_handler;
  auto health = std::make_shared<Impl::HealthState>();
  health->owner = impl_.get();
  health->value.provider = std::string(name);
  if (has_health) {
    auto health_result =
        instance->set_health_handler(instance, &Impl::OnProviderHealth, health.get());
    if (health_result != OVF_COM_STATUS_OK) {
      factory.destroy(instance);
      return ToRuntimeError(health_result);
    }
  }
  if (has_diagnostics) {
    auto diagnostic_result =
        instance->set_diagnostic_handler(instance, &Impl::OnProviderDiagnostic, health.get());
    if (diagnostic_result != OVF_COM_STATUS_OK) {
      factory.destroy(instance);
      return ToRuntimeError(diagnostic_result);
    }
  }
  impl_->transports.emplace_back(&factory, instance, config, std::move(health), has_health);
  return Error{};
}

Error Runtime::LoadTransport(std::string_view provider, TransportConfig config) {
  if (provider.empty() || provider.find_first_not_of(
                              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                              std::string_view::npos) {
    return Error::invalid_argument;
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
    return Error::not_found;
  }
  using Query = const ovf_com_transport_factory_v1* (*)();
  const auto query = reinterpret_cast<Query>(dlsym(library, "ovf_com_transport_query_v1"));
  if (query == nullptr) {
    dlclose(library);
    return Error::incompatible_abi;
  }
  const auto* factory = query();
  if (factory == nullptr) {
    dlclose(library);
    return Error::incompatible_abi;
  }
  const auto result = AddTransport(*factory, std::move(config));
  if (result != Error::none) {
    dlclose(library);
    return result;
  }
  impl_->libraries.push_back(library);
  return Error::none;
#else
  static_cast<void>(config);
  return Error::unsupported;
#endif
}

Error Runtime::Start() {
  if (impl_->running) {
    return Error::invalid_state;
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
    if (transport.health_supported) {
      std::unique_lock lock(transport.health->mutex);
      const auto ready =
          transport.health->changed.wait_for(lock, transport.config.start_timeout, [&] {
            return transport.health->value.state != TransportHealthState::initializing;
          });
      if (!ready || transport.health->value.state != TransportHealthState::ready) {
        lock.unlock();
        (void)transport.instance->stop(transport.instance);
        while (started > 0U) {
          --started;
          (void)impl_->transports[started].instance->stop(impl_->transports[started].instance);
        }
        return ready ? Error::provider_failure : Error::deadline_exceeded;
      }
    }
    ++started;
  }
  impl_->running = true;
  return Error{};
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

std::vector<TransportHealth> Runtime::Health() const {
  std::vector<TransportHealth> result;
  result.reserve(impl_->transports.size());
  for (auto const& transport : impl_->transports) {
    std::lock_guard lock(transport.health->mutex);
    result.push_back(transport.health->value);
  }
  return result;
}

void Runtime::OnHealth(std::function<void(TransportHealth const&)> callback) {
  {
    std::lock_guard lock(impl_->health_callback_mutex);
    impl_->health_callback = callback;
  }
  if (callback)
    for (auto const& health : Health())
      callback(health);
}

void Runtime::OnDiagnostic(std::function<void(CommunicationDiagnostic const&)> callback) {
  std::lock_guard lock(impl_->diagnostic_callback_mutex);
  impl_->diagnostic_callback = std::move(callback);
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

auto detail::RuntimeAccess::routes(Runtime& runtime, RouteSelector const& selector)
    -> std::vector<RouteBinding> {
  if (!runtime.impl_ || !runtime.impl_->running)
    return {};
  std::vector<RouteBinding> result;
  for (auto const& route : runtime.impl_->routes) {
    if (route.binding.service_id.bytes == selector.service_id.bytes &&
        (selector.instance.empty() || selector.instance == route.instance)) {
      result.push_back(route.binding);
    }
  }
  return result;
}

} // namespace ovf::com
