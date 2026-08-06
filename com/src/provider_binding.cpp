// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/provider_binding.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <tuple>

namespace ovf::com {
namespace {

auto AbiUuid(Uuid const& value) -> ovf_com_uuid_v1 {
  ovf_com_uuid_v1 result{};
  std::copy(value.bytes.begin(), value.bytes.end(), result.bytes);
  return result;
}

auto DeadlineNs(std::chrono::steady_clock::time_point value) -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}

template <class Member>
auto HasExtension(ovf_com_transport_v1 const& provider, std::size_t offset,
                  Member ovf_com_transport_v1::* member) -> bool {
  return provider.struct_size >= offset + sizeof(provider.*member) && provider.*member != nullptr;
}

auto HasCapability(ovf_com_transport_v1& provider, std::uint64_t capability) -> bool {
  ovf_com_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  return provider.get_capabilities(&provider, &capabilities) == OVF_COM_STATUS_OK &&
         (capabilities.feature_bits & capability) == capability;
}

auto HasRequestLoans(ovf_com_transport_v1& provider) -> bool {
  return HasCapability(provider, OVF_COM_CAP_REQUEST_LOANS) &&
         HasExtension(provider, offsetof(ovf_com_transport_v1, request_loan_acquire),
                      &ovf_com_transport_v1::request_loan_acquire) &&
         HasExtension(provider, offsetof(ovf_com_transport_v1, request_loan_send),
                      &ovf_com_transport_v1::request_loan_send);
}

auto HasResponseLoans(ovf_com_transport_v1& provider) -> bool {
  return HasCapability(provider, OVF_COM_CAP_RESPONSE_LOANS) &&
         HasExtension(provider, offsetof(ovf_com_transport_v1, response_loan_acquire),
                      &ovf_com_transport_v1::response_loan_acquire) &&
         HasExtension(provider, offsetof(ovf_com_transport_v1, response_loan_send),
                      &ovf_com_transport_v1::response_loan_send);
}

auto HasSubscriptionState(ovf_com_transport_v1& provider) -> bool {
  return HasCapability(provider, OVF_COM_CAP_SUBSCRIPTION_STATE) &&
         HasExtension(provider, offsetof(ovf_com_transport_v1, subscription_set_state_handler),
                      &ovf_com_transport_v1::subscription_set_state_handler);
}

auto MapStatus(ovf_com_status_v1 status) -> CommunicationError {
  switch (status) {
  case OVF_COM_STATUS_NOT_FOUND:
    return CommunicationError::unavailable;
  case OVF_COM_STATUS_INCOMPATIBLE_ABI:
  case OVF_COM_STATUS_UNSUPPORTED:
    return CommunicationError::incompatible;
  case OVF_COM_STATUS_RESOURCE_EXHAUSTED:
    return CommunicationError::resource_exhausted;
  case OVF_COM_STATUS_DEADLINE_EXCEEDED:
    return CommunicationError::deadline_exceeded;
  case OVF_COM_STATUS_CANCELLED:
    return CommunicationError::cancelled;
  case OVF_COM_STATUS_SHUTTING_DOWN:
    return CommunicationError::shutting_down;
  default:
    return CommunicationError::provider_failure;
  }
}

auto AbiStatus(CommunicationError error) -> ovf_com_status_v1 {
  switch (error) {
  case CommunicationError::unavailable:
    return OVF_COM_STATUS_NOT_FOUND;
  case CommunicationError::incompatible:
    return OVF_COM_STATUS_UNSUPPORTED;
  case CommunicationError::resource_exhausted:
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  case CommunicationError::deadline_exceeded:
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  case CommunicationError::cancelled:
    return OVF_COM_STATUS_CANCELLED;
  case CommunicationError::shutting_down:
    return OVF_COM_STATUS_SHUTTING_DOWN;
  default:
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  }
}

class OperationState final : public RawOperation,
                             public std::enable_shared_from_this<OperationState> {
public:
  OperationState(ovf_com_transport_v1& provider, ovf_com_handle_v1 endpoint)
      : provider_(&provider), endpoint_(endpoint) {}
  ~OperationState() override {
    if (provider_ && operation_ != OVF_COM_INVALID_HANDLE_V1)
      provider_->cancel(provider_, operation_);
    if (provider_ && endpoint_ != OVF_COM_INVALID_HANDLE_V1)
      provider_->endpoint_destroy(provider_, endpoint_);
  }

  auto retain() -> void { self_ = shared_from_this(); }
  auto accepted(ovf_com_handle_v1 operation) -> void { operation_ = operation; }
  auto fail(ovf_com_status_v1 status) -> void {
    std::lock_guard lock(mutex_);
    result_ = {{}, true, false, {}};
    result_.error = MapStatus(status);
    complete_ = true;
    self_.reset();
    condition_.notify_all();
  }
  static void Complete(void* user, ovf_com_handle_v1, ovf_com_status_v1 status,
                       ovf_com_bytes_view_v1 payload) {
    static_cast<OperationState*>(user)->complete(status, payload);
  }
  auto wait(std::chrono::steady_clock::time_point deadline) -> RawResult override {
    std::unique_lock lock(mutex_);
    if (!complete_ && !condition_.wait_until(lock, deadline, [this] { return complete_; })) {
      complete_ = true;
      result_ = {CommunicationError::deadline_exceeded, true, false, {}};
      auto operation = operation_;
      lock.unlock();
      if (operation != OVF_COM_INVALID_HANDLE_V1)
        (void)provider_->cancel(provider_, operation);
      lock.lock();
    }
    return result_;
  }
  auto cancel() noexcept -> void override {
    ovf_com_handle_v1 operation{};
    {
      std::lock_guard lock(mutex_);
      if (complete_)
        return;
      operation = operation_;
    }
    if (operation != OVF_COM_INVALID_HANDLE_V1)
      (void)provider_->cancel(provider_, operation);
  }

private:
  auto complete(ovf_com_status_v1 status, ovf_com_bytes_view_v1 payload) -> void {
    std::shared_ptr<OperationState> keep_alive;
    {
      std::lock_guard lock(mutex_);
      keep_alive = self_;
      if (!complete_) {
        complete_ = true;
        result_.application_error = status == OVF_COM_STATUS_APPLICATION_ERROR;
        result_.has_error = status != OVF_COM_STATUS_OK && !result_.application_error;
        result_.error = MapStatus(status);
        if (payload.size) {
          auto begin = reinterpret_cast<std::byte const*>(payload.data);
          result_.payload.assign(begin, begin + payload.size);
        }
      }
      self_.reset();
    }
    condition_.notify_all();
  }

  ovf_com_transport_v1* provider_;
  ovf_com_handle_v1 endpoint_{};
  ovf_com_handle_v1 operation_{};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool complete_{};
  RawResult result_{};
  std::shared_ptr<OperationState> self_;
};

class ProviderSubscriptionState final : public RawSubscription {
public:
  ProviderSubscriptionState(ovf_com_transport_v1& provider, ovf_com_handle_v1 endpoint)
      : provider_(&provider), endpoint_(endpoint) {}
  ~ProviderSubscriptionState() override { close(); }
  auto start() -> ovf_com_status_v1 {
    auto status = provider_->subscribe(provider_, endpoint_, &OnSample, this, &subscription_);
    if (status != OVF_COM_STATUS_OK)
      return status;
    if (HasSubscriptionState(*provider_)) {
      status = provider_->subscription_set_state_handler(provider_, subscription_, &OnState, this);
      if (status != OVF_COM_STATUS_OK)
        return status;
    } else {
      on_state(OVF_COM_SUBSCRIPTION_ACTIVE, OVF_COM_STATUS_OK);
    }
    return OVF_COM_STATUS_OK;
  }
  auto set_callback(Callback callback) -> void override {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
  }
  auto set_state_callback(StateCallback callback) -> void override {
    std::optional<std::pair<ovf_com_subscription_state_v1, ovf_com_status_v1>> state;
    {
      std::lock_guard lock(mutex_);
      state_callback_ = callback;
      state = state_;
    }
    if (callback && state)
      callback(ToState(state->first), state->second == OVF_COM_STATUS_OK
                                          ? std::optional<CommunicationError>{}
                                          : MapStatus(state->second));
  }
  auto close() noexcept -> void override {
    ovf_com_handle_v1 subscription{}, endpoint{};
    StateCallback state_callback;
    {
      std::lock_guard lock(mutex_);
      subscription = std::exchange(subscription_, OVF_COM_INVALID_HANDLE_V1);
      endpoint = std::exchange(endpoint_, OVF_COM_INVALID_HANDLE_V1);
      callback_ = {};
      state_callback = std::move(state_callback_);
      state_callback_ = {};
    }
    if (subscription && state_callback)
      state_callback(ovf::com::SubscriptionState::withdrawn, {});
    if (subscription)
      (void)provider_->unsubscribe(provider_, subscription);
    if (endpoint)
      (void)provider_->endpoint_destroy(provider_, endpoint);
  }

private:
  static void OnSample(void* user, ovf_com_sample_v1 const* sample) {
    static_cast<ProviderSubscriptionState*>(user)->on_sample(sample);
  }
  static void OnState(void* user, ovf_com_handle_v1, ovf_com_subscription_state_v1 state,
                      ovf_com_status_v1 reason) {
    static_cast<ProviderSubscriptionState*>(user)->on_state(state, reason);
  }
  static auto ToState(ovf_com_subscription_state_v1 state) -> ovf::com::SubscriptionState {
    switch (state) {
    case OVF_COM_SUBSCRIPTION_REQUESTED:
      return ovf::com::SubscriptionState::requested;
    case OVF_COM_SUBSCRIPTION_ACTIVE:
      return ovf::com::SubscriptionState::active;
    case OVF_COM_SUBSCRIPTION_REJECTED:
      return ovf::com::SubscriptionState::rejected;
    case OVF_COM_SUBSCRIPTION_SUSPENDED:
      return ovf::com::SubscriptionState::suspended;
    case OVF_COM_SUBSCRIPTION_WITHDRAWN:
      return ovf::com::SubscriptionState::withdrawn;
    }
    return ovf::com::SubscriptionState::rejected;
  }
  auto on_state(ovf_com_subscription_state_v1 state, ovf_com_status_v1 reason) -> void {
    StateCallback callback;
    {
      std::lock_guard lock(mutex_);
      state_ = std::pair{state, reason};
      callback = state_callback_;
    }
    if (callback)
      callback(ToState(state), reason == OVF_COM_STATUS_OK ? std::optional<CommunicationError>{}
                                                           : MapStatus(reason));
  }
  auto on_sample(ovf_com_sample_v1 const* sample) -> void {
    Callback callback;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
    }
    if (callback && sample)
      callback({reinterpret_cast<std::byte const*>(sample->payload.data), sample->payload.size});
    if (sample && sample->provider_loan != OVF_COM_INVALID_HANDLE_V1) {
      (void)provider_->loan_release(provider_, sample->provider_loan);
    }
  }
  ovf_com_transport_v1* provider_;
  ovf_com_handle_v1 endpoint_{};
  ovf_com_handle_v1 subscription_{};
  std::mutex mutex_;
  Callback callback_;
  StateCallback state_callback_;
  std::optional<std::pair<ovf_com_subscription_state_v1, ovf_com_status_v1>> state_;
};

auto Descriptor(RouteBinding const& route, ElementDescriptor const& element,
                ovf_com_endpoint_kind_v1 kind) -> ovf_com_endpoint_descriptor_v1 {
  auto same_id = [&element](RouteBinding::NativeElementMapping const& mapping) {
    return mapping.element_id.bytes == element.id.bytes;
  };
  auto mapping = std::find_if(route.native_elements.begin(), route.native_elements.end(), same_id);
  std::string_view native;
  if (mapping != route.native_elements.end()) {
    switch (element.operation) {
    case ElementDescriptor::Operation::event:
    case ElementDescriptor::Operation::field_notify:
      native = mapping->event;
      break;
    case ElementDescriptor::Operation::field_get:
      native = mapping->field_get;
      break;
    case ElementDescriptor::Operation::field_set:
      native = mapping->field_set;
      break;
    case ElementDescriptor::Operation::method:
      native = mapping->method;
      break;
    }
  }
  auto operation = [value = element.operation] {
    switch (value) {
    case ElementDescriptor::Operation::event:
      return OVF_COM_OPERATION_EVENT;
    case ElementDescriptor::Operation::method:
      return OVF_COM_OPERATION_METHOD;
    case ElementDescriptor::Operation::field_get:
      return OVF_COM_OPERATION_FIELD_GET;
    case ElementDescriptor::Operation::field_set:
      return OVF_COM_OPERATION_FIELD_SET;
    case ElementDescriptor::Operation::field_notify:
      return OVF_COM_OPERATION_FIELD_NOTIFY;
    }
    return OVF_COM_OPERATION_UNSPECIFIED;
  }();
  return {sizeof(ovf_com_endpoint_descriptor_v1),
          kind,
          AbiUuid(route.service_id),
          AbiUuid(route.instance_id),
          AbiUuid(element.id),
          route.route_epoch,
          route.max_payload_size,
          route.history_depth,
          static_cast<std::uint64_t>(
              kind == OVF_COM_ENDPOINT_EVENT_SUBSCRIBER || kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER
                  ? OVF_COM_CAP_EVENTS | OVF_COM_CAP_ORDERED
              : kind == OVF_COM_ENDPOINT_METHOD_CLIENT
                  ? OVF_COM_CAP_METHODS | OVF_COM_CAP_DEADLINES | OVF_COM_CAP_CANCELLATION
                  : OVF_COM_CAP_METHODS),
          {native.data(), native.size()},
          operation};
}
} // namespace

struct DiscoveryWatch::Impl {
  struct Registration {
    Impl* owner{};
    std::size_t candidate{};
    ovf_com_transport_v1* provider{};
    ovf_com_handle_v1 handle{};
  };

  mutable std::mutex mutex;
  std::vector<RouteBinding> candidates;
  std::vector<Registration> registrations;
  std::vector<std::size_t> counts;
  Callback callback;
  bool closed{};

  static void OnDiscovery(void* user, ovf_com_discovery_entry_v1 const* entry) {
    auto& registration = *static_cast<Registration*>(user);
    if (!entry || entry->struct_size < sizeof(*entry))
      return;
    auto const& candidate = registration.owner->candidates[registration.candidate];
    auto const instance_is_unspecified =
        std::all_of(std::begin(entry->instance_id.bytes), std::end(entry->instance_id.bytes),
                    [](auto byte) { return byte == 0U; });
    if (!instance_is_unspecified &&
        !std::equal(candidate.instance_id.bytes.begin(), candidate.instance_id.bytes.end(),
                    std::begin(entry->instance_id.bytes)))
      return;
    if (entry->route_epoch != 0U && entry->route_epoch != candidate.route_epoch)
      return;
    registration.owner->update(registration.candidate, entry->available != 0U);
  }

  auto update(std::size_t index, bool available) -> void {
    Callback notify;
    std::vector<ServiceRoute> snapshot;
    {
      std::lock_guard lock(mutex);
      if (closed || index >= counts.size())
        return;
      if (available) {
        ++counts[index];
      } else if (counts[index] != 0U) {
        --counts[index];
      }
      notify = callback;
      if (notify)
        snapshot = routes_locked();
    }
    if (notify)
      notify(snapshot);
  }

  auto routes_locked() const -> std::vector<ServiceRoute> {
    std::vector<ServiceRoute> result;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (counts[index] != 0U)
        result.push_back({candidates[index]});
    }
    std::sort(result.begin(), result.end(), [](auto const& left, auto const& right) {
      return std::tie(left.binding.priority, left.binding.provider, left.binding.instance_id.bytes,
                      left.binding.route_epoch) <
             std::tie(right.binding.priority, right.binding.provider,
                      right.binding.instance_id.bytes, right.binding.route_epoch);
    });
    return result;
  }
};

DiscoveryWatch::DiscoveryWatch(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DiscoveryWatch::~DiscoveryWatch() { close(); }

auto DiscoveryWatch::routes() const -> std::vector<ServiceRoute> {
  if (!impl_)
    return {};
  std::lock_guard lock(impl_->mutex);
  return impl_->routes_locked();
}

auto DiscoveryWatch::select() const -> std::optional<ServiceRoute> {
  auto available = routes();
  if (available.empty())
    return std::nullopt;
  return available.front();
}

auto DiscoveryWatch::on_change(Callback callback) -> void {
  if (!impl_)
    return;
  Callback notify;
  std::vector<ServiceRoute> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed)
      return;
    impl_->callback = std::move(callback);
    notify = impl_->callback;
    if (notify)
      snapshot = impl_->routes_locked();
  }
  if (notify)
    notify(snapshot);
}

auto DiscoveryWatch::close() noexcept -> void {
  if (!impl_)
    return;
  std::vector<Impl::Registration> registrations;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed)
      return;
    impl_->closed = true;
    impl_->callback = {};
    registrations = impl_->registrations;
    impl_->registrations.clear();
  }
  for (auto& registration : registrations) {
    if (registration.provider && registration.handle != OVF_COM_INVALID_HANDLE_V1) {
      (void)registration.provider->watch_stop(registration.provider, registration.handle);
    }
  }
}

auto Discover(Runtime& runtime, std::vector<RouteBinding> candidates)
    -> std::shared_ptr<DiscoveryWatch> {
  auto impl = std::make_unique<DiscoveryWatch::Impl>();
  impl->candidates = std::move(candidates);
  impl->counts.resize(impl->candidates.size());
  impl->registrations.resize(impl->candidates.size());
  for (std::size_t index = 0; index < impl->candidates.size(); ++index) {
    auto& candidate = impl->candidates[index];
    auto* provider = detail::RuntimeAccess::find(runtime, candidate.provider);
    if (!provider)
      return {};
    auto& registration = impl->registrations[index];
    registration = {impl.get(), index, provider, OVF_COM_INVALID_HANDLE_V1};
    ovf_com_discovery_filter_v1 filter{
        sizeof(filter),
        AbiUuid(candidate.service_id),
        {candidate.native_service.data(), candidate.native_service.size()}};
    if (provider->watch_start(provider, &filter, &DiscoveryWatch::Impl::OnDiscovery, &registration,
                              &registration.handle) != OVF_COM_STATUS_OK) {
      for (std::size_t previous = 0; previous < index; ++previous) {
        auto& active = impl->registrations[previous];
        (void)active.provider->watch_stop(active.provider, active.handle);
      }
      return {};
    }
  }
  return std::shared_ptr<DiscoveryWatch>(new DiscoveryWatch(std::move(impl)));
}

ProviderClientBinding::ProviderClientBinding(ovf_com_transport_v1& provider,
                                             RouteBinding route) noexcept
    : provider_(&provider), route_(route) {}
ProviderClientBinding::~ProviderClientBinding() = default;

auto ProviderClientBinding::invoke(ElementDescriptor const& element,
                                   std::span<const std::byte> payload, CallOptions options)
    -> std::shared_ptr<RawOperation> {
  auto descriptor = Descriptor(route_, element, OVF_COM_ENDPOINT_METHOD_CLIENT);
  ovf_com_handle_v1 endpoint{};
  auto status = provider_->endpoint_create(provider_, &descriptor, &endpoint);
  auto state = std::make_shared<OperationState>(*provider_, endpoint);
  if (status != OVF_COM_STATUS_OK) {
    state->fail(status);
    return state;
  }
  state->retain();
  ovf_com_handle_v1 operation{};
  status = provider_->request(
      provider_, endpoint, {reinterpret_cast<std::uint8_t const*>(payload.data()), payload.size()},
      DeadlineNs(options.deadline), &OperationState::Complete, state.get(), &operation);
  if (status == OVF_COM_STATUS_OK)
    state->accepted(operation);
  else
    state->fail(status);
  return state;
}

auto ProviderClientBinding::invoke_loaned(ElementDescriptor const& element, std::size_t size,
                                          std::function<bool(std::span<std::byte>)> encode,
                                          CallOptions options) -> std::shared_ptr<RawOperation> {
  const bool extension_available = HasRequestLoans(*provider_);
  if (!extension_available) {
    std::vector<std::byte> payload(size);
    if (!encode(payload)) {
      auto state = std::make_shared<OperationState>(*provider_, OVF_COM_INVALID_HANDLE_V1);
      state->fail(OVF_COM_STATUS_TRANSPORT_ERROR);
      return state;
    }
    return invoke(element, payload, options);
  }
  auto descriptor = Descriptor(route_, element, OVF_COM_ENDPOINT_METHOD_CLIENT);
  ovf_com_handle_v1 endpoint{};
  auto status = provider_->endpoint_create(provider_, &descriptor, &endpoint);
  auto state = std::make_shared<OperationState>(*provider_, endpoint);
  if (status != OVF_COM_STATUS_OK) {
    state->fail(status);
    return state;
  }
  ovf_com_loan_v1 loan{sizeof(loan), OVF_COM_INVALID_HANDLE_V1, {nullptr, 0}};
  status = provider_->request_loan_acquire(provider_, endpoint, size, DeadlineNs(options.deadline),
                                           &loan);
  if (status == OVF_COM_STATUS_UNSUPPORTED) {
    std::vector<std::byte> payload(size);
    if (!encode(payload)) {
      state->fail(OVF_COM_STATUS_TRANSPORT_ERROR);
      return state;
    }
    state->retain();
    ovf_com_handle_v1 operation{};
    status = provider_->request(
        provider_, endpoint,
        {reinterpret_cast<std::uint8_t const*>(payload.data()), payload.size()},
        DeadlineNs(options.deadline), &OperationState::Complete, state.get(), &operation);
    if (status == OVF_COM_STATUS_OK)
      state->accepted(operation);
    else
      state->fail(status);
    return state;
  }
  if (status != OVF_COM_STATUS_OK) {
    state->fail(status);
    return state;
  }
  if (!encode(std::span(reinterpret_cast<std::byte*>(loan.bytes.data), loan.bytes.size))) {
    (void)provider_->loan_release(provider_, loan.handle);
    state->fail(OVF_COM_STATUS_TRANSPORT_ERROR);
    return state;
  }
  state->retain();
  ovf_com_handle_v1 operation{};
  status = provider_->request_loan_send(provider_, endpoint, loan.handle, size,
                                        &OperationState::Complete, state.get(), &operation);
  if (status == OVF_COM_STATUS_OK)
    state->accepted(operation);
  else
    state->fail(status);
  return state;
}

auto ProviderClientBinding::subscribe(ElementDescriptor const& element)
    -> std::shared_ptr<RawSubscription> {
  auto descriptor = Descriptor(route_, element, OVF_COM_ENDPOINT_EVENT_SUBSCRIBER);
  ovf_com_handle_v1 endpoint{};
  auto status = provider_->endpoint_create(provider_, &descriptor, &endpoint);
  if (status != OVF_COM_STATUS_OK)
    return {};
  auto state = std::make_shared<ProviderSubscriptionState>(*provider_, endpoint);
  if (state->start() != OVF_COM_STATUS_OK)
    return {};
  return state;
}

auto Connect(Runtime& runtime, ServiceRoute route) -> std::shared_ptr<ClientBinding> {
  auto* provider = detail::RuntimeAccess::find(runtime, route.binding.provider);
  if (!provider)
    return {};
  return std::make_shared<ProviderClientBinding>(*provider, std::move(route.binding));
}

auto FindService(Runtime& runtime, RouteBinding candidate,
                 std::chrono::steady_clock::duration timeout) -> std::shared_ptr<ClientBinding> {
  auto discovery = Discover(runtime, {std::move(candidate)});
  if (!discovery)
    return {};
  std::mutex mutex;
  std::condition_variable condition;
  discovery->on_change([&](std::span<ServiceRoute const> routes) {
    if (!routes.empty())
      condition.notify_all();
  });
  std::optional<ServiceRoute> selected;
  {
    std::unique_lock lock(mutex);
    condition.wait_for(lock, timeout, [&] {
      selected = discovery->select();
      return selected.has_value();
    });
  }
  discovery->close();
  return selected ? Connect(runtime, std::move(*selected)) : nullptr;
}

auto FindService(Runtime& runtime, RouteSelector selector,
                 std::chrono::steady_clock::duration timeout) -> std::shared_ptr<ClientBinding> {
  auto candidates = detail::RuntimeAccess::routes(runtime, selector);
  auto discovery = Discover(runtime, std::move(candidates));
  if (!discovery)
    return {};
  std::mutex mutex;
  std::condition_variable condition;
  discovery->on_change([&](std::span<ServiceRoute const> routes) {
    if (!routes.empty())
      condition.notify_all();
  });
  std::optional<ServiceRoute> selected;
  {
    std::unique_lock lock(mutex);
    condition.wait_for(lock, timeout, [&] {
      selected = discovery->select();
      return selected.has_value();
    });
  }
  discovery->close();
  return selected ? Connect(runtime, std::move(*selected)) : nullptr;
}

struct ProviderServerBinding::Impl {
  struct Method {
    Impl* owner{};
    ovf_com_handle_v1 endpoint{};
    MethodHandler handler;
  };

  ovf_com_transport_v1* provider{};
  RouteBinding route;
  std::mutex mutex;
  std::vector<std::unique_ptr<Method>> methods;
  std::map<std::array<std::uint8_t, 16>, ovf_com_handle_v1> events;
  bool closed{};

  static void OnRequest(void* user, ovf_com_handle_v1 request, ovf_com_bytes_view_v1 payload,
                        std::uint64_t deadline_ns) {
    auto& method = *static_cast<Method*>(user);
    RawServerResult result;
    try {
      result = method.handler(
          {reinterpret_cast<std::byte const*>(payload.data), payload.size},
          std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns)));
    } catch (...) {
      result = {CommunicationError::provider_failure, true, false, {}, 0, {}};
    }
    auto status = result.application_error ? OVF_COM_STATUS_APPLICATION_ERROR
                  : result.has_error       ? AbiStatus(result.error)
                                           : OVF_COM_STATUS_OK;
    if (result.encode_payload && HasResponseLoans(*method.owner->provider)) {
      ovf_com_loan_v1 loan{sizeof(loan), OVF_COM_INVALID_HANDLE_V1, {nullptr, 0}};
      auto loan_status = method.owner->provider->response_loan_acquire(
          method.owner->provider, request, result.encoded_payload_size, status, &loan);
      if (loan_status == OVF_COM_STATUS_OK) {
        auto destination =
            std::span(reinterpret_cast<std::byte*>(loan.bytes.data), loan.bytes.size);
        if (result.encode_payload(destination)) {
          (void)method.owner->provider->response_loan_send(
              method.owner->provider, request, loan.handle, result.encoded_payload_size);
          return;
        }
        (void)method.owner->provider->loan_release(method.owner->provider, loan.handle);
        return;
      }
    }
    if (result.encode_payload) {
      try {
        result.payload.resize(result.encoded_payload_size);
      } catch (...) {
        status = OVF_COM_STATUS_RESOURCE_EXHAUSTED;
        result.payload.clear();
      }
      if (status != OVF_COM_STATUS_RESOURCE_EXHAUSTED && !result.encode_payload(result.payload)) {
        status = OVF_COM_STATUS_TRANSPORT_ERROR;
        result.payload.clear();
      }
    }
    (void)method.owner->provider->respond(
        method.owner->provider, request, status,
        {reinterpret_cast<std::uint8_t const*>(result.payload.data()), result.payload.size()});
  }

  auto close() noexcept -> void {
    std::vector<std::unique_ptr<Method>> old_methods;
    std::map<std::array<std::uint8_t, 16>, ovf_com_handle_v1> old_events;
    {
      std::lock_guard lock(mutex);
      if (closed)
        return;
      closed = true;
      old_methods.swap(methods);
      old_events.swap(events);
    }
    for (auto& method : old_methods) {
      (void)provider->set_request_handler(provider, method->endpoint, nullptr, nullptr);
      (void)provider->endpoint_destroy(provider, method->endpoint);
    }
    for (auto const& [unused, endpoint] : old_events) {
      (void)unused;
      (void)provider->endpoint_destroy(provider, endpoint);
    }
  }
};

ProviderServerBinding::ProviderServerBinding(ovf_com_transport_v1& provider,
                                             RouteBinding route) noexcept
    : impl_(std::make_unique<Impl>()) {
  impl_->provider = &provider;
  impl_->route = std::move(route);
}
ProviderServerBinding::~ProviderServerBinding() { close(); }

auto ProviderServerBinding::add_method(ElementDescriptor const& element, MethodHandler handler)
    -> bool {
  if (!impl_ || !handler)
    return false;
  auto descriptor = Descriptor(impl_->route, element, OVF_COM_ENDPOINT_METHOD_SERVER);
  auto method = std::make_unique<Impl::Method>();
  method->owner = impl_.get();
  method->handler = std::move(handler);
  if (impl_->provider->endpoint_create(impl_->provider, &descriptor, &method->endpoint) !=
      OVF_COM_STATUS_OK)
    return false;
  if (impl_->provider->set_request_handler(impl_->provider, method->endpoint, &Impl::OnRequest,
                                           method.get()) != OVF_COM_STATUS_OK) {
    (void)impl_->provider->endpoint_destroy(impl_->provider, method->endpoint);
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    (void)impl_->provider->set_request_handler(impl_->provider, method->endpoint, nullptr, nullptr);
    (void)impl_->provider->endpoint_destroy(impl_->provider, method->endpoint);
    return false;
  }
  impl_->methods.push_back(std::move(method));
  return true;
}

auto ProviderServerBinding::add_event(ElementDescriptor const& element) -> bool {
  if (!impl_)
    return false;
  auto descriptor = Descriptor(impl_->route, element, OVF_COM_ENDPOINT_EVENT_PUBLISHER);
  ovf_com_handle_v1 endpoint{};
  if (impl_->provider->endpoint_create(impl_->provider, &descriptor, &endpoint) !=
      OVF_COM_STATUS_OK)
    return false;
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed || impl_->events.contains(element.id.bytes)) {
    (void)impl_->provider->endpoint_destroy(impl_->provider, endpoint);
    return false;
  }
  impl_->events.emplace(element.id.bytes, endpoint);
  return true;
}

auto ProviderServerBinding::publish(ElementDescriptor const& element,
                                    std::span<const std::byte> payload)
    -> std::optional<CommunicationError> {
  if (!impl_)
    return CommunicationError::shutting_down;
  ovf_com_handle_v1 endpoint{};
  {
    std::lock_guard lock(impl_->mutex);
    auto found = impl_->events.find(element.id.bytes);
    if (impl_->closed || found == impl_->events.end())
      return CommunicationError::unavailable;
    endpoint = found->second;
  }
  auto status = impl_->provider->publish(
      impl_->provider, endpoint,
      {reinterpret_cast<std::uint8_t const*>(payload.data()), payload.size()});
  return status == OVF_COM_STATUS_OK ? std::optional<CommunicationError>{}
                                     : std::optional<CommunicationError>{MapStatus(status)};
}

auto ProviderServerBinding::publish_loaned(ElementDescriptor const& element, std::size_t size,
                                           std::function<bool(std::span<std::byte>)> encode)
    -> std::optional<CommunicationError> {
  if (!impl_)
    return CommunicationError::shutting_down;
  ovf_com_handle_v1 endpoint{};
  {
    std::lock_guard lock(impl_->mutex);
    auto found = impl_->events.find(element.id.bytes);
    if (impl_->closed || found == impl_->events.end())
      return CommunicationError::unavailable;
    endpoint = found->second;
  }
  ovf_com_loan_v1 loan{sizeof(loan), OVF_COM_INVALID_HANDLE_V1, {nullptr, 0}};
  auto status = impl_->provider->loan_acquire(impl_->provider, endpoint, size, &loan);
  if (status == OVF_COM_STATUS_UNSUPPORTED) {
    std::vector<std::byte> payload(size);
    if (!encode(payload))
      return CommunicationError::provider_failure;
    return publish(element, payload);
  }
  if (status != OVF_COM_STATUS_OK)
    return MapStatus(status);
  auto destination = std::span(reinterpret_cast<std::byte*>(loan.bytes.data), loan.bytes.size);
  if (!encode(destination)) {
    (void)impl_->provider->loan_release(impl_->provider, loan.handle);
    return CommunicationError::provider_failure;
  }
  status = impl_->provider->loan_publish(impl_->provider, endpoint, loan.handle, size);
  if (status != OVF_COM_STATUS_OK) {
    (void)impl_->provider->loan_release(impl_->provider, loan.handle);
    return MapStatus(status);
  }
  return {};
}

auto ProviderServerBinding::close() noexcept -> void {
  if (impl_)
    impl_->close();
}

auto Offer(Runtime& runtime, RouteBinding route) -> std::shared_ptr<ServerBinding> {
  auto* provider = detail::RuntimeAccess::find(runtime, route.provider);
  if (!provider)
    return {};
  return std::make_shared<ProviderServerBinding>(*provider, std::move(route));
}

auto Offer(Runtime& runtime, RouteSelector selector) -> std::shared_ptr<ServerBinding> {
  auto routes = detail::RuntimeAccess::routes(runtime, selector);
  if (routes.size() != 1U)
    return {};
  return Offer(runtime, std::move(routes.front()));
}
} // namespace ovf::com
