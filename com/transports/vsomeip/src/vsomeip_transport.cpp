// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip.h"
#include "ovf/com/transports/vsomeip_mapping.hpp"

#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace native = ovf::com::transports::vsomeip;

struct Endpoint {
  ovf_com_endpoint_descriptor_v1 descriptor{};
  native::Mapping mapping{};
};
struct Subscription {
  ovf_com_handle_v1 endpoint{};
  ovf_com_sample_callback_v1 callback{};
  void* user{};
};
struct Watch {
  native::Mapping mapping{};
  ovf_com_uuid_v1 service_id{};
  ovf_com_discovery_callback_v1 callback{};
  void* user{};
};
struct Pending {
  ovf_com_completion_callback_v1 callback{};
  void* user{};
  std::uint64_t deadline{};
};
struct Incoming {
  std::shared_ptr<vsomeip::message> message;
  ovf_com_handle_v1 endpoint{};
};
struct Correlation {
  vsomeip::client_t client{};
  vsomeip::session_t session{};
  friend auto operator<(Correlation const& lhs, Correlation const& rhs) -> bool {
    return std::tie(lhs.client, lhs.session) < std::tie(rhs.client, rhs.session);
  }
};

struct Transport {
  ovf_com_transport_v1 api{};
  ovf_com_host_api_v1 const* host{};
  std::shared_ptr<vsomeip::application> application;
  std::thread runner;
  std::mutex mutex;
  bool running{};
  bool registered{};
  ovf_com_handle_v1 next_handle{1};
  std::uint64_t sequence{};
  std::uint32_t max_endpoints{128};
  std::uint32_t max_operations{128};
  std::map<ovf_com_handle_v1, Endpoint> endpoints;
  std::map<ovf_com_handle_v1, Subscription> subscriptions;
  std::map<ovf_com_handle_v1, Watch> watches;
  std::map<ovf_com_handle_v1, Pending> pending;
  std::map<Correlation, ovf_com_handle_v1> correlations;
  std::map<ovf_com_handle_v1, Incoming> incoming;
  std::map<ovf_com_handle_v1, std::pair<ovf_com_request_callback_v1, void*>> handlers;
};

constexpr char kName[] = "vsomeip";

auto Self(ovf_com_transport_v1* api) -> Transport& {
  return *static_cast<Transport*>(api->implementation);
}
auto View(ovf_com_string_view_v1 value) -> std::string_view {
  return {value.data == nullptr ? "" : value.data, value.data == nullptr ? 0U : value.size};
}
template <class Task> auto Dispatch(Transport& self, Task task) -> ovf_com_status_v1 {
  auto* stored = new (std::nothrow) Task(std::move(task));
  if (!stored)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto run = [](void* value) { (*static_cast<Task*>(value))(); };
  auto release = [](void* value) { delete static_cast<Task*>(value); };
  auto status = self.host->dispatch(self.host->user_data, run, release, stored);
  if (status != OVF_COM_STATUS_OK)
    release(stored);
  return status;
}
auto Parse(ovf_com_string_view_v1 text, native::Mapping& mapping) -> bool {
  std::string error;
  return native::ParseMapping(View(text), mapping, error);
}
auto Payload(std::shared_ptr<vsomeip::message> const& message) -> ovf_com_bytes_view_v1 {
  auto payload = message->get_payload();
  return payload ? ovf_com_bytes_view_v1{payload->get_data(), payload->get_length()}
                 : ovf_com_bytes_view_v1{nullptr, 0};
}
auto Copy(ovf_com_bytes_view_v1 bytes) -> std::vector<std::uint8_t> {
  if (bytes.size == 0)
    return {};
  return {bytes.data, bytes.data + bytes.size};
}
auto NativePayload(ovf_com_bytes_view_v1 bytes) -> std::shared_ptr<vsomeip::payload> {
  auto payload = vsomeip::runtime::get()->create_payload();
  payload->set_data(bytes.data, static_cast<vsomeip::length_t>(bytes.size));
  return payload;
}
auto Status(vsomeip::return_code_e code) -> ovf_com_status_v1 {
  return code == vsomeip::return_code_e::E_OK ? OVF_COM_STATUS_OK
                                              : OVF_COM_STATUS_APPLICATION_ERROR;
}

void OnMessage(Transport* self, std::shared_ptr<vsomeip::message> const& message) {
  std::vector<Subscription> subscribers;
  Pending completion{};
  ovf_com_handle_v1 operation{};
  std::pair<ovf_com_request_callback_v1, void*> handler{};
  ovf_com_handle_v1 request{};
  {
    std::lock_guard lock(self->mutex);
    if (!self->running)
      return;
    if (message->get_message_type() == vsomeip::message_type_e::MT_RESPONSE ||
        message->get_message_type() == vsomeip::message_type_e::MT_ERROR) {
      auto found = self->correlations.find({message->get_client(), message->get_session()});
      if (found == self->correlations.end())
        return;
      operation = found->second;
      auto pending = self->pending.find(operation);
      if (pending == self->pending.end())
        return;
      completion = pending->second;
      self->pending.erase(pending);
      self->correlations.erase(found);
    } else if (message->get_message_type() == vsomeip::message_type_e::MT_REQUEST ||
               message->get_message_type() == vsomeip::message_type_e::MT_REQUEST_NO_RETURN) {
      for (auto const& [endpoint_handle, endpoint] : self->endpoints) {
        auto found = self->handlers.find(endpoint_handle);
        if (found != self->handlers.end() && endpoint.mapping.service == message->get_service() &&
            endpoint.mapping.instance == message->get_instance() &&
            endpoint.mapping.element == message->get_method()) {
          handler = found->second;
          request = self->next_handle++;
          self->incoming.emplace(request, Incoming{message, endpoint_handle});
          break;
        }
      }
    } else {
      for (auto const& [_, subscription] : self->subscriptions) {
        auto endpoint = self->endpoints.find(subscription.endpoint);
        if (endpoint != self->endpoints.end() &&
            endpoint->second.mapping.service == message->get_service() &&
            endpoint->second.mapping.instance == message->get_instance() &&
            endpoint->second.mapping.element == message->get_method())
          subscribers.push_back(subscription);
      }
    }
  }
  if (completion.callback) {
    auto bytes = Payload(message);
    (void)Dispatch(*self, [completion, operation, result = Status(message->get_return_code()),
                           copy = Copy(bytes)] {
      completion.callback(completion.user, operation, result, {copy.data(), copy.size()});
    });
  } else if (handler.first) {
    auto bytes = Payload(message);
    auto deadline = self->host->monotonic_time_ns(self->host->user_data) + UINT64_C(30000000000);
    (void)Dispatch(*self, [handler, request, deadline, copy = Copy(bytes)] {
      handler.first(handler.second, request, {copy.data(), copy.size()}, deadline);
    });
  } else {
    auto bytes = Payload(message);
    auto copy = std::make_shared<std::vector<std::uint8_t>>(Copy(bytes));
    std::uint64_t sequence{};
    {
      std::lock_guard lock(self->mutex);
      sequence = ++self->sequence;
    }
    for (auto subscriber : subscribers) {
      (void)Dispatch(*self, [subscriber, copy, sequence] {
        ovf_com_sample_v1 sample{
            sizeof(sample), {copy->data(), copy->size()}, OVF_COM_INVALID_HANDLE_V1, sequence, 0};
        subscriber.callback(subscriber.user, &sample);
      });
    }
  }
}

auto Start(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  {
    std::lock_guard lock(self.mutex);
    if (self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    if (!self.application->init())
      return OVF_COM_STATUS_TRANSPORT_ERROR;
    self.running = true;
  }
  self.application->register_state_handler([&self](vsomeip::state_type_e state) {
    std::lock_guard lock(self.mutex);
    self.registered = state == vsomeip::state_type_e::ST_REGISTERED;
  });
  self.runner = std::thread([&self] { self.application->start(); });
  return OVF_COM_STATUS_OK;
}
auto Stop(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::vector<std::pair<ovf_com_handle_v1, Pending>> pending;
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_OK;
    self.running = false;
    pending.assign(self.pending.begin(), self.pending.end());
    self.pending.clear();
    self.correlations.clear();
    self.incoming.clear();
  }
  self.application->clear_all_handler();
  self.application->stop();
  if (self.runner.joinable())
    self.runner.join();
  for (auto const& [operation, item] : pending)
    (void)Dispatch(self, [operation, item] {
      item.callback(item.user, operation, OVF_COM_STATUS_SHUTTING_DOWN, {nullptr, 0});
    });
  return OVF_COM_STATUS_OK;
}
auto Capabilities(ovf_com_transport_v1* api, ovf_com_capabilities_v1* out) -> ovf_com_status_v1 {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  *out = {sizeof(*out),
          OVF_COM_CAP_DISCOVERY | OVF_COM_CAP_EVENTS | OVF_COM_CAP_METHODS | OVF_COM_CAP_RELIABLE |
              OVF_COM_CAP_ORDERED | OVF_COM_CAP_DEADLINES | OVF_COM_CAP_CANCELLATION,
          OVF_COM_ISOLATION_PROCESS_SINGLETON,
          self.max_endpoints,
          self.max_endpoints,
          self.max_operations,
          1,
          0xffffffffU,
          0,
          1};
  return OVF_COM_STATUS_OK;
}
auto WatchStart(ovf_com_transport_v1* api, ovf_com_discovery_filter_v1 const* filter,
                ovf_com_discovery_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!filter || filter->struct_size < sizeof(*filter) || !callback || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  native::Mapping mapping{};
  if (!Parse(filter->native_mapping, mapping))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto handle = self.next_handle++;
  self.watches.emplace(handle, Watch{mapping, filter->service_id, callback, user});
  self.application->register_availability_handler(
      mapping.service, mapping.instance,
      [&self, handle](vsomeip::service_t, vsomeip::instance_t, bool available) {
        Watch watch{};
        {
          std::lock_guard guard(self.mutex);
          auto found = self.watches.find(handle);
          if (found == self.watches.end())
            return;
          watch = found->second;
        }
        ovf_com_discovery_entry_v1 entry{
            sizeof(entry), watch.service_id, {}, handle, 0, static_cast<std::uint8_t>(available)};
        (void)Dispatch(self, [watch, entry] { watch.callback(watch.user, &entry); });
      });
  self.application->request_service(mapping.service, mapping.instance, mapping.major_version,
                                    mapping.minor_version);
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto WatchStop(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.watches.find(handle);
  if (found == self.watches.end())
    return OVF_COM_STATUS_NOT_FOUND;
  self.application->unregister_availability_handler(found->second.mapping.service,
                                                    found->second.mapping.instance);
  self.application->release_service(found->second.mapping.service, found->second.mapping.instance);
  self.watches.erase(found);
  return OVF_COM_STATUS_OK;
}
auto EndpointCreate(ovf_com_transport_v1* api, ovf_com_endpoint_descriptor_v1 const* descriptor,
                    ovf_com_handle_v1* out) -> ovf_com_status_v1 {
  if (!descriptor || descriptor->struct_size < sizeof(*descriptor) || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  native::Mapping mapping{};
  if (!Parse(descriptor->native_mapping, mapping))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  if (self.endpoints.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto handle = self.next_handle++;
  const bool server = descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                      descriptor->kind == OVF_COM_ENDPOINT_METHOD_SERVER;
  const bool service_already_offered =
      std::any_of(self.endpoints.begin(), self.endpoints.end(), [&mapping](auto const& item) {
        auto const& endpoint = item.second;
        const bool offered = endpoint.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                             endpoint.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER;
        return offered && endpoint.mapping.service == mapping.service &&
               endpoint.mapping.instance == mapping.instance &&
               endpoint.mapping.major_version == mapping.major_version &&
               endpoint.mapping.minor_version == mapping.minor_version;
      });
  self.endpoints.emplace(handle, Endpoint{*descriptor, mapping});
  if (server) {
    if (!service_already_offered)
      self.application->offer_service(mapping.service, mapping.instance, mapping.major_version,
                                      mapping.minor_version);
    if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER) {
      self.application->offer_event(mapping.service, mapping.instance, mapping.element,
                                    {mapping.event_group}, vsomeip::event_type_e::ET_EVENT,
                                    std::chrono::milliseconds::zero(), false, true, nullptr,
                                    mapping.reliable ? vsomeip::reliability_type_e::RT_RELIABLE
                                                     : vsomeip::reliability_type_e::RT_UNRELIABLE);
    }
  } else {
    self.application->request_service(mapping.service, mapping.instance, mapping.major_version,
                                      mapping.minor_version);
  }
  self.application->register_message_handler(
      mapping.service, mapping.instance, mapping.element,
      [&self](std::shared_ptr<vsomeip::message> const& message) { OnMessage(&self, message); });
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto EndpointDestroy(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(handle);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  auto const mapping = found->second.mapping;
  const bool server = found->second.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                      found->second.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER;
  const bool last_server =
      server && std::none_of(self.endpoints.begin(), self.endpoints.end(), [&](auto const& item) {
        if (item.first == handle)
          return false;
        auto const& endpoint = item.second;
        const bool offered = endpoint.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                             endpoint.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER;
        return offered && endpoint.mapping.service == mapping.service &&
               endpoint.mapping.instance == mapping.instance &&
               endpoint.mapping.major_version == mapping.major_version &&
               endpoint.mapping.minor_version == mapping.minor_version;
      });
  self.application->unregister_message_handler(mapping.service, mapping.instance, mapping.element);
  if (found->second.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER)
    self.application->stop_offer_event(mapping.service, mapping.instance, mapping.element);
  if (last_server)
    self.application->stop_offer_service(mapping.service, mapping.instance, mapping.major_version,
                                         mapping.minor_version);
  else if (!server)
    self.application->release_service(mapping.service, mapping.instance);
  self.handlers.erase(handle);
  self.endpoints.erase(found);
  return OVF_COM_STATUS_OK;
}
auto Subscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
               ovf_com_sample_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!callback || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_SUBSCRIBER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto handle = self.next_handle++;
  self.subscriptions.emplace(handle, Subscription{endpoint, callback, user});
  auto const& mapping = found->second.mapping;
  self.application->request_event(mapping.service, mapping.instance, mapping.element,
                                  {mapping.event_group}, vsomeip::event_type_e::ET_EVENT,
                                  mapping.reliable ? vsomeip::reliability_type_e::RT_RELIABLE
                                                   : vsomeip::reliability_type_e::RT_UNRELIABLE);
  self.application->subscribe(mapping.service, mapping.instance, mapping.event_group,
                              mapping.major_version);
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto Unsubscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.subscriptions.find(handle);
  if (found == self.subscriptions.end())
    return OVF_COM_STATUS_NOT_FOUND;
  auto endpoint = self.endpoints.find(found->second.endpoint);
  if (endpoint != self.endpoints.end()) {
    auto const& mapping = endpoint->second.mapping;
    self.application->unsubscribe(mapping.service, mapping.instance, mapping.event_group);
    self.application->release_event(mapping.service, mapping.instance, mapping.element);
  }
  self.subscriptions.erase(found);
  return OVF_COM_STATUS_OK;
}
auto Publish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint, ovf_com_bytes_view_v1 payload)
    -> ovf_com_status_v1 {
  if (payload.size && !payload.data)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_PUBLISHER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto const& mapping = found->second.mapping;
  self.application->notify(mapping.service, mapping.instance, mapping.element,
                           NativePayload(payload));
  return OVF_COM_STATUS_OK;
}
auto PublishIov(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                ovf_com_iovec_v1 const* segments, size_t count) -> ovf_com_status_v1 {
  if (!segments || count != 1)
    return count ? OVF_COM_STATUS_UNSUPPORTED : OVF_COM_STATUS_INVALID_ARGUMENT;
  return Publish(api, endpoint, {segments->data, segments->size});
}
auto UnsupportedLoan(ovf_com_transport_v1*, ovf_com_handle_v1, size_t, ovf_com_loan_v1*)
    -> ovf_com_status_v1 {
  return OVF_COM_STATUS_UNSUPPORTED;
}
auto UnsupportedLoanPublish(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_handle_v1, size_t)
    -> ovf_com_status_v1 {
  return OVF_COM_STATUS_UNSUPPORTED;
}
auto UnsupportedLoanRelease(ovf_com_transport_v1*, ovf_com_handle_v1) -> ovf_com_status_v1 {
  return OVF_COM_STATUS_UNSUPPORTED;
}
auto Request(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint, ovf_com_bytes_view_v1 payload,
             std::uint64_t deadline, ovf_com_completion_callback_v1 callback, void* user,
             ovf_com_handle_v1* out) -> ovf_com_status_v1 {
  if (!callback || !out || (payload.size && !payload.data))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  if (deadline <= self.host->monotonic_time_ns(self.host->user_data))
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (self.pending.size() >= self.max_operations)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto const& mapping = found->second.mapping;
  auto message = vsomeip::runtime::get()->create_request(mapping.reliable);
  message->set_service(mapping.service);
  message->set_instance(mapping.instance);
  message->set_method(mapping.element);
  message->set_interface_version(mapping.major_version);
  message->set_payload(NativePayload(payload));
  auto operation = self.next_handle++;
  self.pending.emplace(operation, Pending{callback, user, deadline});
  self.application->send(message);
  self.correlations.emplace(Correlation{message->get_client(), message->get_session()}, operation);
  *out = operation;
  return OVF_COM_STATUS_OK;
}
auto Cancel(ovf_com_transport_v1* api, ovf_com_handle_v1 operation) -> ovf_com_status_v1 {
  auto& self = Self(api);
  Pending pending{};
  {
    std::lock_guard lock(self.mutex);
    auto found = self.pending.find(operation);
    if (found == self.pending.end())
      return OVF_COM_STATUS_NOT_FOUND;
    pending = found->second;
    self.pending.erase(found);
    for (auto item = self.correlations.begin(); item != self.correlations.end(); ++item)
      if (item->second == operation) {
        self.correlations.erase(item);
        break;
      }
  }
  return Dispatch(self, [pending, operation] {
    pending.callback(pending.user, operation, OVF_COM_STATUS_CANCELLED, {nullptr, 0});
  });
}
auto SetHandler(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                ovf_com_request_callback_v1 callback, void* user) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_METHOD_SERVER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (callback)
    self.handlers[endpoint] = {callback, user};
  else
    self.handlers.erase(endpoint);
  return OVF_COM_STATUS_OK;
}
auto Respond(ovf_com_transport_v1* api, ovf_com_handle_v1 request, ovf_com_status_v1 status,
             ovf_com_bytes_view_v1 payload) -> ovf_com_status_v1 {
  if (payload.size && !payload.data)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::shared_ptr<vsomeip::message> incoming;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.incoming.find(request);
    if (found == self.incoming.end())
      return OVF_COM_STATUS_NOT_FOUND;
    incoming = found->second.message;
    self.incoming.erase(found);
  }
  auto response = vsomeip::runtime::get()->create_response(incoming);
  response->set_return_code(status == OVF_COM_STATUS_OK ? vsomeip::return_code_e::E_OK
                                                        : vsomeip::return_code_e::E_NOT_OK);
  response->set_payload(NativePayload(payload));
  self.application->send(response);
  return OVF_COM_STATUS_OK;
}

auto Create(ovf_com_host_api_v1 const* host, ovf_com_transport_config_v1 const* config,
            ovf_com_transport_v1** out) -> ovf_com_status_v1 {
  if (!host || host->struct_size < sizeof(*host) || !host->dispatch || !host->monotonic_time_ns ||
      !config || config->struct_size < sizeof(*config) || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto self = std::unique_ptr<Transport>(new (std::nothrow) Transport);
  if (!self)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  self->host = host;
  self->max_endpoints = config->max_endpoints;
  self->max_operations = config->max_outstanding_operations;
  self->application =
      vsomeip::runtime::get()->create_application(std::string(View(config->instance_name)));
  if (!self->application)
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  self->api = {sizeof(self->api),
               OVF_COM_TRANSPORT_ABI_VERSION_1,
               self.get(),
               {kName, sizeof(kName) - 1},
               &Start,
               &Stop,
               &Capabilities,
               &WatchStart,
               &WatchStop,
               &EndpointCreate,
               &EndpointDestroy,
               &Subscribe,
               &Unsubscribe,
               &Publish,
               &PublishIov,
               &UnsupportedLoan,
               &UnsupportedLoanPublish,
               &UnsupportedLoanRelease,
               &Request,
               &Cancel,
               &SetHandler,
               &Respond};
  *out = &self.release()->api;
  return OVF_COM_STATUS_OK;
}
void Destroy(ovf_com_transport_v1* api) {
  if (!api)
    return;
  auto* self = static_cast<Transport*>(api->implementation);
  (void)Stop(api);
  delete self;
}
const ovf_com_transport_factory_v1 kFactory{sizeof(kFactory),
                                            OVF_COM_TRANSPORT_ABI_VERSION_1,
                                            {kName, sizeof(kName) - 1},
                                            &Create,
                                            &Destroy};
} // namespace

extern "C" const ovf_com_transport_factory_v1* ovf_com_vsomeip_transport_query_v1(void) {
  return &kFactory;
}
