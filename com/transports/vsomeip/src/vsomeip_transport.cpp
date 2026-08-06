// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip.h"
#include "ovf/com/transports/vsomeip_mapping.hpp"

#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
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
  ovf_com_subscription_state_callback_v1 state_callback{};
  void* state_user{};
  ovf_com_subscription_state_v1 state{OVF_COM_SUBSCRIPTION_REQUESTED};
  ovf_com_status_v1 reason{OVF_COM_STATUS_OK};
  std::atomic_bool active{true};
  std::recursive_mutex callback_gate;
};
struct Watch {
  native::Mapping mapping{};
  ovf_com_uuid_v1 service_id{};
  ovf_com_discovery_callback_v1 callback{};
  void* user{};
  std::recursive_mutex callback_gate;
  bool active{true};
};
struct RequestHandler {
  ovf_com_request_callback_v1 callback{};
  void* user{};
  std::recursive_mutex callback_gate;
  bool active{true};
};
struct Pending {
  ovf_com_handle_v1 endpoint{};
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
struct ServiceKey {
  vsomeip::service_t service{};
  vsomeip::instance_t instance{};
  vsomeip::major_version_t major{};
  vsomeip::minor_version_t minor{};
  friend auto operator==(ServiceKey const&, ServiceKey const&) -> bool = default;
  friend auto operator<(ServiceKey const& lhs, ServiceKey const& rhs) -> bool {
    return std::tie(lhs.service, lhs.instance, lhs.major, lhs.minor) <
           std::tie(rhs.service, rhs.instance, rhs.major, rhs.minor);
  }
};
struct MessageKey {
  vsomeip::service_t service{};
  vsomeip::instance_t instance{};
  vsomeip::method_t element{};
  friend auto operator<(MessageKey const& lhs, MessageKey const& rhs) -> bool {
    return std::tie(lhs.service, lhs.instance, lhs.element) <
           std::tie(rhs.service, rhs.instance, rhs.element);
  }
};
struct EventKey {
  ServiceKey service;
  vsomeip::event_t event{};
  vsomeip::eventgroup_t event_group{};
  bool reliable{};
  native::ElementKind kind{};
  friend auto operator<(EventKey const& lhs, EventKey const& rhs) -> bool {
    return std::tie(lhs.service, lhs.event, lhs.event_group, lhs.reliable, lhs.kind) <
           std::tie(rhs.service, rhs.event, rhs.event_group, rhs.reliable, rhs.kind);
  }
};
struct GroupKey {
  ServiceKey service;
  vsomeip::eventgroup_t event_group{};
  friend auto operator<(GroupKey const& lhs, GroupKey const& rhs) -> bool {
    return std::tie(lhs.service, lhs.event_group) < std::tie(rhs.service, rhs.event_group);
  }
};

struct Transport {
  ovf_com_transport_v1 api{};
  ovf_com_host_api_v1 const* host{};
  std::shared_ptr<vsomeip::application> application;
  std::thread runner;
  std::thread deadline_worker;
  std::mutex mutex;
  std::mutex diagnostic_mutex;
  std::condition_variable changed;
  bool running{};
  bool registered{};
  bool started_once{};
  ovf_com_health_state_v1 health{OVF_COM_HEALTH_STOPPED};
  std::uint64_t health_sequence{};
  ovf_com_health_callback_v1 health_callback{};
  void* health_user{};
  ovf_com_diagnostic_callback_v1 diagnostic_callback{};
  void* diagnostic_user{};
  ovf_com_handle_v1 next_handle{1};
  std::uint64_t sequence{};
  std::uint32_t max_endpoints{128};
  std::uint32_t max_operations{128};
  std::map<ovf_com_handle_v1, Endpoint> endpoints;
  std::map<ovf_com_handle_v1, std::shared_ptr<Subscription>> subscriptions;
  std::map<ovf_com_handle_v1, std::shared_ptr<Watch>> watches;
  std::map<ovf_com_handle_v1, Pending> pending;
  std::map<Correlation, ovf_com_handle_v1> correlations;
  std::map<ovf_com_handle_v1, Incoming> incoming;
  std::map<ovf_com_handle_v1, std::shared_ptr<RequestHandler>> handlers;
  std::map<ServiceKey, std::size_t> offered_services;
  std::map<ServiceKey, std::size_t> requested_services;
  std::map<ServiceKey, std::size_t> availability_handlers;
  std::map<MessageKey, std::size_t> message_handlers;
  std::map<EventKey, std::size_t> offered_events;
  std::map<EventKey, std::size_t> requested_events;
  std::map<GroupKey, std::size_t> subscribed_groups;
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
  if (status != OVF_COM_STATUS_OK) {
    release(stored);
    ovf_com_diagnostic_callback_v1 callback{};
    void* user{};
    {
      std::lock_guard lock(self.diagnostic_mutex);
      callback = self.diagnostic_callback;
      user = self.diagnostic_user;
    }
    if (callback) {
      constexpr char message[] = "host executor rejected callback";
      ovf_com_diagnostic_v1 diagnostic{sizeof(diagnostic),
                                       status,
                                       OVF_COM_DIAGNOSTIC_PROVIDER,
                                       0,
                                       0,
                                       0,
                                       {message, sizeof(message) - 1U}};
      callback(user, &diagnostic);
    }
  }
  return status;
}
auto Parse(ovf_com_string_view_v1 text, native::Mapping& mapping) -> bool {
  std::string error;
  return native::ParseMapping(View(text), mapping, error);
}
auto Service(native::Mapping const& mapping) -> ServiceKey {
  return {mapping.service, mapping.instance, mapping.major_version, mapping.minor_version};
}
auto Message(native::Mapping const& mapping) -> MessageKey {
  return {mapping.service, mapping.instance, mapping.element};
}
auto Event(native::Mapping const& mapping) -> EventKey {
  return {Service(mapping), mapping.element, mapping.event_group, mapping.reliable, mapping.kind};
}
auto Group(native::Mapping const& mapping) -> GroupKey {
  return {Service(mapping), mapping.event_group};
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
  switch (code) {
  case vsomeip::return_code_e::E_OK:
    return OVF_COM_STATUS_OK;
  case vsomeip::return_code_e::E_NOT_OK:
    return OVF_COM_STATUS_APPLICATION_ERROR;
  case vsomeip::return_code_e::E_UNKNOWN_SERVICE:
  case vsomeip::return_code_e::E_UNKNOWN_METHOD:
  case vsomeip::return_code_e::E_NOT_REACHABLE:
    return OVF_COM_STATUS_NOT_FOUND;
  case vsomeip::return_code_e::E_NOT_READY:
    return OVF_COM_STATUS_INVALID_STATE;
  case vsomeip::return_code_e::E_TIMEOUT:
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  default:
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  }
}
auto ReturnCode(ovf_com_status_v1 status) -> vsomeip::return_code_e {
  switch (status) {
  case OVF_COM_STATUS_OK:
    return vsomeip::return_code_e::E_OK;
  case OVF_COM_STATUS_APPLICATION_ERROR:
    return vsomeip::return_code_e::E_NOT_OK;
  case OVF_COM_STATUS_NOT_FOUND:
  case OVF_COM_STATUS_UNSUPPORTED:
    return vsomeip::return_code_e::E_UNKNOWN_METHOD;
  case OVF_COM_STATUS_INVALID_ARGUMENT:
    return vsomeip::return_code_e::E_MALFORMED_MESSAGE;
  case OVF_COM_STATUS_DEADLINE_EXCEEDED:
    return vsomeip::return_code_e::E_TIMEOUT;
  case OVF_COM_STATUS_INVALID_STATE:
  case OVF_COM_STATUS_RESOURCE_EXHAUSTED:
  case OVF_COM_STATUS_SHUTTING_DOWN:
    return vsomeip::return_code_e::E_NOT_READY;
  default:
    return vsomeip::return_code_e::E_NOT_REACHABLE;
  }
}
auto Respond(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_status_v1, ovf_com_bytes_view_v1)
    -> ovf_com_status_v1;

void DeadlineLoop(Transport* self) {
  std::unique_lock lock(self->mutex);
  while (self->running) {
    std::uint64_t next_deadline{std::numeric_limits<std::uint64_t>::max()};
    for (auto const& [_, pending] : self->pending)
      if (pending.deadline != 0)
        next_deadline = std::min(next_deadline, pending.deadline);
    if (next_deadline == std::numeric_limits<std::uint64_t>::max()) {
      self->changed.wait(lock);
      continue;
    }
    auto now = self->host->monotonic_time_ns(self->host->user_data);
    if (now < next_deadline) {
      auto remaining =
          std::min(next_deadline - now,
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
      self->changed.wait_for(lock, std::chrono::nanoseconds(remaining));
      continue;
    }
    std::vector<std::pair<ovf_com_handle_v1, Pending>> expired;
    for (auto item = self->pending.begin(); item != self->pending.end();) {
      if (item->second.deadline != 0 && item->second.deadline <= now) {
        expired.push_back(*item);
        auto operation = item->first;
        item = self->pending.erase(item);
        for (auto correlation = self->correlations.begin();
             correlation != self->correlations.end();) {
          if (correlation->second == operation)
            correlation = self->correlations.erase(correlation);
          else
            ++correlation;
        }
      } else {
        ++item;
      }
    }
    lock.unlock();
    for (auto const& [operation, pending] : expired)
      (void)Dispatch(*self, [operation, pending] {
        pending.callback(pending.user, operation, OVF_COM_STATUS_DEADLINE_EXCEEDED, {nullptr, 0});
      });
    lock.lock();
  }
}

void OnMessage(Transport* self, std::shared_ptr<vsomeip::message> const& message) {
  std::vector<std::shared_ptr<Subscription>> subscribers;
  Pending completion{};
  ovf_com_handle_v1 operation{};
  std::shared_ptr<RequestHandler> handler;
  ovf_com_handle_v1 request{};
  std::optional<vsomeip::return_code_e> request_error;
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
      self->changed.notify_all();
    } else if (message->get_message_type() == vsomeip::message_type_e::MT_REQUEST ||
               message->get_message_type() == vsomeip::message_type_e::MT_REQUEST_NO_RETURN) {
      request_error = vsomeip::return_code_e::E_UNKNOWN_METHOD;
      for (auto const& [endpoint_handle, endpoint] : self->endpoints) {
        auto found = self->handlers.find(endpoint_handle);
        if (found != self->handlers.end() && endpoint.mapping.service == message->get_service() &&
            endpoint.mapping.instance == message->get_instance() &&
            endpoint.mapping.element == message->get_method()) {
          handler = found->second;
          if (self->incoming.size() >= self->max_operations) {
            request_error = vsomeip::return_code_e::E_NOT_READY;
          } else {
            request = self->next_handle++;
            self->incoming.emplace(request, Incoming{message, endpoint_handle});
            request_error.reset();
          }
          break;
        }
      }
    } else {
      for (auto const& [_, subscription] : self->subscriptions) {
        auto endpoint = self->endpoints.find(subscription->endpoint);
        if (endpoint != self->endpoints.end() &&
            endpoint->second.mapping.service == message->get_service() &&
            endpoint->second.mapping.instance == message->get_instance() &&
            endpoint->second.mapping.element == message->get_method())
          subscribers.push_back(subscription);
      }
    }
  }
  if (request_error) {
    if (message->get_message_type() != vsomeip::message_type_e::MT_REQUEST_NO_RETURN) {
      auto response = vsomeip::runtime::get()->create_response(message);
      response->set_return_code(*request_error);
      self->application->send(response);
    }
    return;
  }
  if (completion.callback) {
    auto bytes = Payload(message);
    (void)Dispatch(*self, [completion, operation, result = Status(message->get_return_code()),
                           copy = Copy(bytes)] {
      completion.callback(completion.user, operation, result, {copy.data(), copy.size()});
    });
  } else if (handler) {
    auto bytes = Payload(message);
    auto dispatch_status = Dispatch(*self, [handler, request, copy = Copy(bytes)] {
      std::lock_guard callback_lock(handler->callback_gate);
      if (handler->active)
        handler->callback(handler->user, request, {copy.data(), copy.size()}, 0);
    });
    if (dispatch_status != OVF_COM_STATUS_OK)
      (void)Respond(&self->api, request, dispatch_status, {});
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
        std::lock_guard callback_lock(subscriber->callback_gate);
        if (!subscriber->active.load(std::memory_order_acquire))
          return;
        ovf_com_sample_v1 sample{
            sizeof(sample), {copy->data(), copy->size()}, OVF_COM_INVALID_HANDLE_V1, sequence, 0};
        subscriber->callback(subscriber->user, &sample);
      });
    }
  }
}

auto Start(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  ovf_com_health_callback_v1 callback{};
  void* user{};
  ovf_com_diagnostic_callback_v1 diagnostic_callback{};
  void* diagnostic_user{};
  ovf_com_health_v1 health{};
  bool initialized{};
  {
    std::lock_guard lock(self.mutex);
    if (self.running || self.started_once)
      return OVF_COM_STATUS_INVALID_STATE;
    self.health = OVF_COM_HEALTH_INITIALIZING;
    health = {sizeof(health),
              self.health,
              ++self.health_sequence,
              {sizeof(ovf_com_diagnostic_v1),
               OVF_COM_STATUS_OK,
               OVF_COM_DIAGNOSTIC_PROVIDER,
               0,
               0,
               0,
               {nullptr, 0}}};
    callback = self.health_callback;
    user = self.health_user;
    initialized = self.application->init();
    if (!initialized) {
      self.health = OVF_COM_HEALTH_FAILED;
      health.state = self.health;
      health.sequence = ++self.health_sequence;
      health.diagnostic.status = OVF_COM_STATUS_TRANSPORT_ERROR;
    } else {
      self.running = true;
      self.started_once = true;
    }
  }
  {
    std::lock_guard lock(self.diagnostic_mutex);
    diagnostic_callback = self.diagnostic_callback;
    diagnostic_user = self.diagnostic_user;
  }
  if (callback)
    callback(user, &health);
  if (!initialized && diagnostic_callback)
    diagnostic_callback(diagnostic_user, &health.diagnostic);
  if (!initialized)
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  self.application->register_state_handler([&self](vsomeip::state_type_e state) {
    struct SubscriptionUpdate {
      ovf_com_handle_v1 handle{};
      std::shared_ptr<Subscription> subscription;
      ovf_com_subscription_state_v1 state{};
      ovf_com_status_v1 reason{};
    };
    std::vector<SubscriptionUpdate> subscriptions;
    ovf_com_health_callback_v1 notify{};
    void* notify_user{};
    ovf_com_diagnostic_callback_v1 diagnostic_callback{};
    void* diagnostic_user{};
    ovf_com_health_v1 update{};
    {
      std::lock_guard lock(self.mutex);
      self.registered = state == vsomeip::state_type_e::ST_REGISTERED;
      self.health = self.registered
                        ? OVF_COM_HEALTH_READY
                        : (self.running ? OVF_COM_HEALTH_DEGRADED : OVF_COM_HEALTH_STOPPED);
      update = {sizeof(update),
                self.health,
                ++self.health_sequence,
                {sizeof(ovf_com_diagnostic_v1),
                 self.registered ? OVF_COM_STATUS_OK : OVF_COM_STATUS_TRANSPORT_ERROR,
                 OVF_COM_DIAGNOSTIC_PROVIDER,
                 static_cast<std::int64_t>(state),
                 0,
                 0,
                 {nullptr, 0}}};
      notify = self.health_callback;
      notify_user = self.health_user;
      for (auto& [handle, subscription] : self.subscriptions) {
        auto next =
            self.registered ? OVF_COM_SUBSCRIPTION_REQUESTED : OVF_COM_SUBSCRIPTION_SUSPENDED;
        auto reason = self.registered ? OVF_COM_STATUS_OK : OVF_COM_STATUS_TRANSPORT_ERROR;
        if (subscription->state != next) {
          subscription->state = next;
          subscription->reason = reason;
          subscriptions.push_back({handle, subscription, next, reason});
        }
      }
    }
    {
      std::lock_guard lock(self.diagnostic_mutex);
      diagnostic_callback = self.diagnostic_callback;
      diagnostic_user = self.diagnostic_user;
    }
    if (notify)
      notify(notify_user, &update);
    if (diagnostic_callback && update.diagnostic.status != OVF_COM_STATUS_OK)
      diagnostic_callback(diagnostic_user, &update.diagnostic);
    for (auto const& item : subscriptions) {
      std::lock_guard callback_lock(item.subscription->callback_gate);
      if (item.subscription->active.load(std::memory_order_acquire) &&
          item.subscription->state_callback)
        item.subscription->state_callback(item.subscription->state_user, item.handle, item.state,
                                          item.reason);
    }
  });
  self.runner = std::thread([&self] { self.application->start(); });
  self.deadline_worker = std::thread([&self] { DeadlineLoop(&self); });
  return OVF_COM_STATUS_OK;
}
auto Stop(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::vector<std::pair<ovf_com_handle_v1, Pending>> pending;
  std::vector<std::pair<ovf_com_handle_v1, std::shared_ptr<Subscription>>> subscriptions;
  std::vector<std::shared_ptr<Watch>> watches;
  std::vector<std::shared_ptr<RequestHandler>> handlers;
  ovf_com_health_callback_v1 callback{};
  void* user{};
  ovf_com_health_v1 health{};
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_OK;
    self.running = false;
    self.changed.notify_all();
    pending.assign(self.pending.begin(), self.pending.end());
    self.pending.clear();
    self.correlations.clear();
    self.incoming.clear();
    for (auto& [handle, subscription] : self.subscriptions)
      subscriptions.emplace_back(handle, subscription);
    for (auto& [_, watch] : self.watches)
      watches.push_back(watch);
    for (auto& [_, handler] : self.handlers)
      handlers.push_back(handler);
    self.offered_services.clear();
    self.requested_services.clear();
    self.availability_handlers.clear();
    self.message_handlers.clear();
    self.offered_events.clear();
    self.requested_events.clear();
    self.subscribed_groups.clear();
    self.health = OVF_COM_HEALTH_STOPPED;
    health = {sizeof(health),
              self.health,
              ++self.health_sequence,
              {sizeof(ovf_com_diagnostic_v1),
               OVF_COM_STATUS_OK,
               OVF_COM_DIAGNOSTIC_PROVIDER,
               0,
               0,
               0,
               {nullptr, 0}}};
    callback = self.health_callback;
    user = self.health_user;
  }
  self.application->clear_all_handler();
  self.application->stop();
  if (self.runner.joinable())
    self.runner.join();
  if (self.deadline_worker.joinable())
    self.deadline_worker.join();
  for (auto const& [handle, subscription] : subscriptions) {
    std::lock_guard callback_lock(subscription->callback_gate);
    if (subscription->active.load(std::memory_order_acquire) && subscription->state_callback)
      subscription->state_callback(subscription->state_user, handle, OVF_COM_SUBSCRIPTION_WITHDRAWN,
                                   OVF_COM_STATUS_SHUTTING_DOWN);
    subscription->active.store(false, std::memory_order_release);
  }
  for (auto const& watch : watches) {
    std::lock_guard callback_lock(watch->callback_gate);
    watch->active = false;
  }
  for (auto const& handler : handlers) {
    std::lock_guard callback_lock(handler->callback_gate);
    handler->active = false;
  }
  for (auto const& [operation, item] : pending)
    (void)Dispatch(self, [operation, item] {
      item.callback(item.user, operation, OVF_COM_STATUS_SHUTTING_DOWN, {nullptr, 0});
    });
  if (callback)
    callback(user, &health);
  return OVF_COM_STATUS_OK;
}

auto GetHealth(ovf_com_transport_v1* api, ovf_com_health_v1* out) -> ovf_com_status_v1 {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  *out = {sizeof(*out),
          self.health,
          self.health_sequence,
          {sizeof(ovf_com_diagnostic_v1),
           self.health == OVF_COM_HEALTH_FAILED || self.health == OVF_COM_HEALTH_DEGRADED
               ? OVF_COM_STATUS_TRANSPORT_ERROR
               : OVF_COM_STATUS_OK,
           OVF_COM_DIAGNOSTIC_PROVIDER,
           0,
           0,
           0,
           {nullptr, 0}}};
  return OVF_COM_STATUS_OK;
}

auto SetHealthHandler(ovf_com_transport_v1* api, ovf_com_health_callback_v1 callback, void* user)
    -> ovf_com_status_v1 {
  auto& self = Self(api);
  ovf_com_health_v1 health{};
  {
    std::lock_guard lock(self.mutex);
    self.health_callback = callback;
    self.health_user = user;
    health = {sizeof(health),
              self.health,
              self.health_sequence,
              {sizeof(ovf_com_diagnostic_v1),
               OVF_COM_STATUS_OK,
               OVF_COM_DIAGNOSTIC_PROVIDER,
               0,
               0,
               0,
               {nullptr, 0}}};
  }
  if (callback)
    callback(user, &health);
  return OVF_COM_STATUS_OK;
}

auto SetDiagnosticHandler(ovf_com_transport_v1* api, ovf_com_diagnostic_callback_v1 callback,
                          void* user) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.diagnostic_mutex);
  self.diagnostic_callback = callback;
  self.diagnostic_user = user;
  return OVF_COM_STATUS_OK;
}
auto Capabilities(ovf_com_transport_v1* api, ovf_com_capabilities_v1* out) -> ovf_com_status_v1 {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  *out = {sizeof(*out),
          OVF_COM_CAP_DISCOVERY | OVF_COM_CAP_EVENTS | OVF_COM_CAP_METHODS | OVF_COM_CAP_RELIABLE |
              OVF_COM_CAP_ORDERED | OVF_COM_CAP_DEADLINES | OVF_COM_CAP_CANCELLATION |
              OVF_COM_CAP_SUBSCRIPTION_STATE | OVF_COM_CAP_HEALTH | OVF_COM_CAP_DIAGNOSTICS,
          OVF_COM_ISOLATION_PROCESS_SINGLETON,
          self.max_endpoints,
          self.max_endpoints,
          self.max_operations,
          1,
          std::numeric_limits<vsomeip::length_t>::max(),
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
  if (self.watches.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  for (auto const& [_, watch] : self.watches)
    if (watch->mapping.service == mapping.service && watch->mapping.instance == mapping.instance &&
        (watch->mapping.major_version != mapping.major_version ||
         watch->mapping.minor_version != mapping.minor_version))
      return OVF_COM_STATUS_INCOMPATIBLE_ABI;
  for (auto const& [_, endpoint] : self.endpoints)
    if (endpoint.mapping.service == mapping.service &&
        endpoint.mapping.instance == mapping.instance &&
        (endpoint.mapping.major_version != mapping.major_version ||
         endpoint.mapping.minor_version != mapping.minor_version))
      return OVF_COM_STATUS_INCOMPATIBLE_ABI;
  auto handle = self.next_handle++;
  auto watch = std::make_shared<Watch>();
  watch->mapping = mapping;
  watch->service_id = filter->service_id;
  watch->callback = callback;
  watch->user = user;
  self.watches.emplace(handle, watch);
  auto service = Service(mapping);
  if (++self.availability_handlers[service] == 1) {
    self.application->register_availability_handler(
        mapping.service, mapping.instance,
        [&self, service](vsomeip::service_t, vsomeip::instance_t, bool available) {
          std::vector<std::pair<ovf_com_handle_v1, std::shared_ptr<Watch>>> watches;
          {
            std::lock_guard guard(self.mutex);
            for (auto const& [watch_handle, watch] : self.watches)
              if (Service(watch->mapping) == service)
                watches.emplace_back(watch_handle, watch);
          }
          for (auto const& [watch_handle, watch] : watches) {
            ovf_com_discovery_entry_v1 entry{sizeof(entry),
                                             watch->service_id,
                                             {},
                                             watch_handle,
                                             0,
                                             static_cast<std::uint8_t>(available)};
            (void)Dispatch(self, [watch, entry] {
              std::lock_guard callback_lock(watch->callback_gate);
              if (watch->active)
                watch->callback(watch->user, &entry);
            });
          }
        },
        mapping.major_version, mapping.minor_version);
  }
  if (++self.requested_services[service] == 1)
    self.application->request_service(mapping.service, mapping.instance, mapping.major_version,
                                      mapping.minor_version);
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto WatchStop(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::shared_ptr<Watch> watch;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.watches.find(handle);
    if (found == self.watches.end())
      return OVF_COM_STATUS_NOT_FOUND;
    watch = found->second;
    auto mapping = watch->mapping;
    auto service = Service(mapping);
    self.watches.erase(found);
    auto availability = self.availability_handlers.find(service);
    if (availability != self.availability_handlers.end() && --availability->second == 0) {
      if (self.running)
        self.application->unregister_availability_handler(
            mapping.service, mapping.instance, mapping.major_version, mapping.minor_version);
      self.availability_handlers.erase(availability);
    }
    auto requested = self.requested_services.find(service);
    if (requested != self.requested_services.end() && --requested->second == 0) {
      if (self.running)
        self.application->release_service(mapping.service, mapping.instance);
      self.requested_services.erase(requested);
    }
  }
  std::lock_guard callback_lock(watch->callback_gate);
  watch->active = false;
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
  auto notification = mapping.kind == native::ElementKind::event ||
                      mapping.kind == native::ElementKind::field_notify;
  auto event_endpoint = descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                        descriptor->kind == OVF_COM_ENDPOINT_EVENT_SUBSCRIBER;
  if (notification != event_endpoint)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  constexpr auto supported = OVF_COM_CAP_DISCOVERY | OVF_COM_CAP_EVENTS | OVF_COM_CAP_METHODS |
                             OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED | OVF_COM_CAP_DEADLINES |
                             OVF_COM_CAP_CANCELLATION;
  if ((descriptor->required_features & ~supported) != 0)
    return OVF_COM_STATUS_UNSUPPORTED;
  if ((descriptor->required_features & (OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED)) != 0 &&
      !mapping.reliable)
    return OVF_COM_STATUS_UNSUPPORTED;
  if (descriptor->max_payload_size > std::numeric_limits<vsomeip::length_t>::max() ||
      descriptor->history_depth > 1)
    return OVF_COM_STATUS_UNSUPPORTED;
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  if (self.endpoints.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  for (auto const& [_, watch] : self.watches)
    if (watch->mapping.service == mapping.service && watch->mapping.instance == mapping.instance &&
        (watch->mapping.major_version != mapping.major_version ||
         watch->mapping.minor_version != mapping.minor_version))
      return OVF_COM_STATUS_INCOMPATIBLE_ABI;
  for (auto const& [_, endpoint] : self.endpoints) {
    auto same_service_instance = endpoint.mapping.service == mapping.service &&
                                 endpoint.mapping.instance == mapping.instance;
    if (same_service_instance && (endpoint.mapping.major_version != mapping.major_version ||
                                  endpoint.mapping.minor_version != mapping.minor_version))
      return OVF_COM_STATUS_INCOMPATIBLE_ABI;
    auto existing_notification = endpoint.mapping.kind == native::ElementKind::event ||
                                 endpoint.mapping.kind == native::ElementKind::field_notify;
    if (same_service_instance && notification && existing_notification &&
        endpoint.mapping.event_group == mapping.event_group &&
        endpoint.mapping.reliable != mapping.reliable)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    if (descriptor->kind == OVF_COM_ENDPOINT_METHOD_SERVER &&
        endpoint.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER &&
        Message(endpoint.mapping).service == Message(mapping).service &&
        Message(endpoint.mapping).instance == Message(mapping).instance &&
        Message(endpoint.mapping).element == Message(mapping).element)
      return OVF_COM_STATUS_ALREADY_EXISTS;
    if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER &&
        endpoint.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER &&
        endpoint.mapping.service == mapping.service &&
        endpoint.mapping.instance == mapping.instance &&
        endpoint.mapping.element == mapping.element)
      return OVF_COM_STATUS_ALREADY_EXISTS;
  }
  auto handle = self.next_handle++;
  const bool server = descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                      descriptor->kind == OVF_COM_ENDPOINT_METHOD_SERVER;
  self.endpoints.emplace(handle, Endpoint{*descriptor, mapping});
  auto service = Service(mapping);
  if (server) {
    if (++self.offered_services[service] == 1)
      self.application->offer_service(mapping.service, mapping.instance, mapping.major_version,
                                      mapping.minor_version);
    if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER) {
      auto event_type = mapping.kind == native::ElementKind::field_notify
                            ? vsomeip::event_type_e::ET_FIELD
                            : vsomeip::event_type_e::ET_EVENT;
      if (++self.offered_events[Event(mapping)] == 1)
        self.application->offer_event(
            mapping.service, mapping.instance, mapping.element, {mapping.event_group}, event_type,
            std::chrono::milliseconds::zero(), false, true, nullptr,
            mapping.reliable ? vsomeip::reliability_type_e::RT_RELIABLE
                             : vsomeip::reliability_type_e::RT_UNRELIABLE);
    }
  } else {
    if (++self.requested_services[service] == 1)
      self.application->request_service(mapping.service, mapping.instance, mapping.major_version,
                                        mapping.minor_version);
  }
  if (++self.message_handlers[Message(mapping)] == 1)
    self.application->register_message_handler(
        mapping.service, mapping.instance, mapping.element,
        [&self](std::shared_ptr<vsomeip::message> const& message) { OnMessage(&self, message); });
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto EndpointDestroy(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::unique_lock lock(self.mutex);
  if (std::any_of(self.subscriptions.begin(), self.subscriptions.end(),
                  [handle](auto const& item) { return item.second->endpoint == handle; }) ||
      std::any_of(self.pending.begin(), self.pending.end(),
                  [handle](auto const& item) { return item.second.endpoint == handle; }) ||
      std::any_of(self.incoming.begin(), self.incoming.end(),
                  [handle](auto const& item) { return item.second.endpoint == handle; }))
    return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(handle);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  auto const mapping = found->second.mapping;
  std::shared_ptr<RequestHandler> handler;
  if (auto item = self.handlers.find(handle); item != self.handlers.end())
    handler = item->second;
  const bool server = found->second.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                      found->second.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER;
  auto message = self.message_handlers.find(Message(mapping));
  if (message != self.message_handlers.end() && --message->second == 0) {
    if (self.running)
      self.application->unregister_message_handler(mapping.service, mapping.instance,
                                                   mapping.element);
    self.message_handlers.erase(message);
  }
  if (found->second.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER) {
    auto event = self.offered_events.find(Event(mapping));
    if (event != self.offered_events.end() && --event->second == 0) {
      if (self.running)
        self.application->stop_offer_event(mapping.service, mapping.instance, mapping.element);
      self.offered_events.erase(event);
    }
  }
  auto service = Service(mapping);
  if (server) {
    auto offered = self.offered_services.find(service);
    if (offered != self.offered_services.end() && --offered->second == 0) {
      if (self.running)
        self.application->stop_offer_service(mapping.service, mapping.instance,
                                             mapping.major_version, mapping.minor_version);
      self.offered_services.erase(offered);
    }
  } else {
    auto requested = self.requested_services.find(service);
    if (requested != self.requested_services.end() && --requested->second == 0) {
      if (self.running)
        self.application->release_service(mapping.service, mapping.instance);
      self.requested_services.erase(requested);
    }
  }
  self.handlers.erase(handle);
  self.endpoints.erase(found);
  lock.unlock();
  if (handler) {
    std::lock_guard callback_lock(handler->callback_gate);
    handler->active = false;
    handler->callback = nullptr;
    handler->user = nullptr;
  }
  return OVF_COM_STATUS_OK;
}
auto Subscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
               ovf_com_sample_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!callback || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_SUBSCRIBER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (self.subscriptions.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto handle = self.next_handle++;
  auto subscription = std::make_shared<Subscription>();
  subscription->endpoint = endpoint;
  subscription->callback = callback;
  subscription->user = user;
  self.subscriptions.emplace(handle, std::move(subscription));
  auto const& mapping = found->second.mapping;
  auto event_type = mapping.kind == native::ElementKind::field_notify
                        ? vsomeip::event_type_e::ET_FIELD
                        : vsomeip::event_type_e::ET_EVENT;
  auto event_key = Event(mapping);
  if (++self.requested_events[event_key] == 1) {
    self.application->request_event(mapping.service, mapping.instance, mapping.element,
                                    {mapping.event_group}, event_type,
                                    mapping.reliable ? vsomeip::reliability_type_e::RT_RELIABLE
                                                     : vsomeip::reliability_type_e::RT_UNRELIABLE);
    self.application->register_subscription_status_handler(
        mapping.service, mapping.instance, mapping.event_group, mapping.element,
        [&self, event_key](vsomeip::service_t, vsomeip::instance_t, vsomeip::eventgroup_t,
                           vsomeip::event_t, std::uint16_t error) {
          struct Notification {
            ovf_com_handle_v1 handle{};
            ovf_com_subscription_state_callback_v1 callback{};
            void* user{};
            ovf_com_subscription_state_v1 state{};
            ovf_com_status_v1 reason{};
            std::shared_ptr<Subscription> subscription;
          };
          std::vector<Notification> notifications;
          {
            std::lock_guard lock(self.mutex);
            for (auto& [subscription_handle, subscription] : self.subscriptions) {
              auto endpoint = self.endpoints.find(subscription->endpoint);
              if (endpoint == self.endpoints.end() ||
                  !(Event(endpoint->second.mapping).service == event_key.service) ||
                  Event(endpoint->second.mapping).event != event_key.event ||
                  Event(endpoint->second.mapping).event_group != event_key.event_group)
                continue;
              subscription->state =
                  error == 0 ? OVF_COM_SUBSCRIPTION_ACTIVE : OVF_COM_SUBSCRIPTION_REJECTED;
              subscription->reason = error == 0 ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
              if (subscription->state_callback &&
                  subscription->active.load(std::memory_order_acquire))
                notifications.push_back({subscription_handle, subscription->state_callback,
                                         subscription->state_user, subscription->state,
                                         subscription->reason, subscription});
            }
          }
          for (auto const& notification : notifications)
            (void)Dispatch(self, [notification] {
              std::lock_guard callback_lock(notification.subscription->callback_gate);
              if (notification.subscription->active.load(std::memory_order_acquire))
                notification.callback(notification.user, notification.handle, notification.state,
                                      notification.reason);
            });
        },
        true);
  }
  if (++self.subscribed_groups[Group(mapping)] == 1)
    self.application->subscribe(mapping.service, mapping.instance, mapping.event_group,
                                mapping.major_version);
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto Unsubscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::shared_ptr<Subscription> subscription;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.subscriptions.find(handle);
    if (found == self.subscriptions.end())
      return OVF_COM_STATUS_NOT_FOUND;
    subscription = found->second;
    auto endpoint = self.endpoints.find(subscription->endpoint);
    if (endpoint != self.endpoints.end()) {
      auto const& mapping = endpoint->second.mapping;
      auto group = self.subscribed_groups.find(Group(mapping));
      if (group != self.subscribed_groups.end() && --group->second == 0) {
        if (self.running)
          self.application->unsubscribe(mapping.service, mapping.instance, mapping.event_group);
        self.subscribed_groups.erase(group);
      }
      auto event = self.requested_events.find(Event(mapping));
      if (event != self.requested_events.end() && --event->second == 0) {
        if (self.running) {
          self.application->unregister_subscription_status_handler(
              mapping.service, mapping.instance, mapping.event_group, mapping.element);
          self.application->release_event(mapping.service, mapping.instance, mapping.element);
        }
        self.requested_events.erase(event);
      }
    }
    self.subscriptions.erase(found);
  }
  std::lock_guard callback_lock(subscription->callback_gate);
  if (subscription->active.load(std::memory_order_acquire) && subscription->state_callback)
    subscription->state_callback(subscription->state_user, handle, OVF_COM_SUBSCRIPTION_WITHDRAWN,
                                 OVF_COM_STATUS_OK);
  subscription->active.store(false, std::memory_order_release);
  return OVF_COM_STATUS_OK;
}
auto SetSubscriptionStateHandler(ovf_com_transport_v1* api, ovf_com_handle_v1 handle,
                                 ovf_com_subscription_state_callback_v1 callback, void* user)
    -> ovf_com_status_v1 {
  if (!callback)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  ovf_com_subscription_state_v1 state{};
  ovf_com_status_v1 reason{};
  std::shared_ptr<Subscription> subscription;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.subscriptions.find(handle);
    if (found == self.subscriptions.end())
      return OVF_COM_STATUS_NOT_FOUND;
    found->second->state_callback = callback;
    found->second->state_user = user;
    state = found->second->state;
    reason = found->second->reason;
    subscription = found->second;
  }
  return Dispatch(self, [callback, user, handle, state, reason, subscription] {
    std::lock_guard callback_lock(subscription->callback_gate);
    if (subscription->active.load(std::memory_order_acquire))
      callback(user, handle, state, reason);
  });
}
auto Publish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint, ovf_com_bytes_view_v1 payload)
    -> ovf_com_status_v1 {
  if (payload.size && !payload.data)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_PUBLISHER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (payload.size > found->second.descriptor.max_payload_size ||
      payload.size > std::numeric_limits<vsomeip::length_t>::max())
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
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
  if (deadline != 0 && deadline <= self.host->monotonic_time_ns(self.host->user_data))
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_METHOD_CLIENT)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (self.pending.size() >= self.max_operations)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  if (payload.size > found->second.descriptor.max_payload_size ||
      payload.size > std::numeric_limits<vsomeip::length_t>::max())
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto const& mapping = found->second.mapping;
  auto message = vsomeip::runtime::get()->create_request(mapping.reliable);
  message->set_service(mapping.service);
  message->set_instance(mapping.instance);
  message->set_method(mapping.element);
  message->set_interface_version(mapping.major_version);
  message->set_payload(NativePayload(payload));
  auto operation = self.next_handle++;
  self.pending.emplace(operation, Pending{endpoint, callback, user, deadline});
  self.application->send(message);
  self.correlations.emplace(Correlation{message->get_client(), message->get_session()}, operation);
  self.changed.notify_all();
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
    self.changed.notify_all();
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
  std::shared_ptr<RequestHandler> removed;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.endpoints.find(endpoint);
    if (found == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    if (found->second.descriptor.kind != OVF_COM_ENDPOINT_METHOD_SERVER)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    if (callback && self.handlers.contains(endpoint))
      return OVF_COM_STATUS_INVALID_STATE;
    if (callback) {
      auto handler = std::make_shared<RequestHandler>();
      handler->callback = callback;
      handler->user = user;
      self.handlers[endpoint] = std::move(handler);
    } else if (auto handler = self.handlers.find(endpoint); handler != self.handlers.end()) {
      removed = handler->second;
      self.handlers.erase(handler);
    }
  }
  if (removed) {
    std::lock_guard callback_lock(removed->callback_gate);
    removed->active = false;
  }
  return OVF_COM_STATUS_OK;
}
auto Respond(ovf_com_transport_v1* api, ovf_com_handle_v1 request, ovf_com_status_v1 status,
             ovf_com_bytes_view_v1 payload) -> ovf_com_status_v1 {
  if ((payload.size && !payload.data) || status > OVF_COM_STATUS_APPLICATION_ERROR ||
      payload.size > std::numeric_limits<vsomeip::length_t>::max())
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::shared_ptr<vsomeip::message> incoming;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.incoming.find(request);
    if (found == self.incoming.end())
      return OVF_COM_STATUS_NOT_FOUND;
    auto endpoint = self.endpoints.find(found->second.endpoint);
    if (endpoint == self.endpoints.end())
      return OVF_COM_STATUS_INVALID_STATE;
    if (payload.size > endpoint->second.descriptor.max_payload_size)
      return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
    incoming = found->second.message;
    self.incoming.erase(found);
  }
  auto response = vsomeip::runtime::get()->create_response(incoming);
  response->set_return_code(ReturnCode(status));
  response->set_payload(NativePayload(payload));
  self.application->send(response);
  return OVF_COM_STATUS_OK;
}

auto Create(ovf_com_host_api_v1 const* host, ovf_com_transport_config_v1 const* config,
            ovf_com_transport_v1** out) -> ovf_com_status_v1 {
  if (!host || host->struct_size < sizeof(*host) || !host->dispatch || !host->monotonic_time_ns ||
      !config || config->struct_size < OVF_COM_TRANSPORT_CONFIG_V1_BASE_SIZE || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto self = std::unique_ptr<Transport>(new (std::nothrow) Transport);
  if (!self)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  self->host = host;
  self->max_endpoints = config->max_endpoints ? config->max_endpoints : 128;
  self->max_operations =
      config->max_outstanding_operations ? config->max_outstanding_operations : 128;
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
               &Respond,
               nullptr,
               nullptr,
               nullptr,
               nullptr,
               &SetSubscriptionStateHandler,
               &GetHealth,
               &SetHealthHandler,
               &SetDiagnosticHandler};
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
