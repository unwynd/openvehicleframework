// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/inproc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Endpoint {
  ovf_com_endpoint_descriptor_v1 descriptor{};
};
struct Subscription {
  ovf_com_handle_v1 endpoint{};
  ovf_com_sample_callback_v1 callback{};
  void* user{};
};
struct Watch {
  ovf_com_uuid_v1 service{};
  ovf_com_discovery_callback_v1 callback{};
  void* user{};
};
struct Server {
  ovf_com_request_callback_v1 callback{};
  void* user{};
};
struct Pending {
  ovf_com_completion_callback_v1 callback{};
  void* user{};
  bool terminal{};
};

struct InprocTransport {
  ovf_com_transport_v1 api{};
  const ovf_com_host_api_v1* host{};
  std::mutex mutex;
  bool running{false};
  ovf_com_handle_v1 next_handle{1};
  std::uint64_t sequence{};
  std::uint32_t max_endpoints{256};
  std::uint32_t max_operations{256};
  std::unordered_map<ovf_com_handle_v1, Endpoint> endpoints;
  std::unordered_map<ovf_com_handle_v1, Subscription> subscriptions;
  std::unordered_map<ovf_com_handle_v1, Watch> watches;
  std::unordered_map<ovf_com_handle_v1, std::vector<std::uint8_t>> loans;
  std::unordered_map<ovf_com_handle_v1, Server> servers;
  std::unordered_map<ovf_com_handle_v1, Pending> pending;
};

constexpr char name[] = "inproc";

bool Equal(ovf_com_uuid_v1 const& lhs, ovf_com_uuid_v1 const& rhs) {
  return std::memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) == 0;
}

bool SameElement(Endpoint const& lhs, Endpoint const& rhs) {
  return Equal(lhs.descriptor.service_id, rhs.descriptor.service_id) &&
         Equal(lhs.descriptor.instance_id, rhs.descriptor.instance_id) &&
         Equal(lhs.descriptor.element_id, rhs.descriptor.element_id);
}

template <class Task> ovf_com_status_v1 Dispatch(InprocTransport& self, Task task) {
  auto* stored = new (std::nothrow) Task(std::move(task));
  if (stored == nullptr)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto run = [](void* value) { (*static_cast<Task*>(value))(); };
  auto release = [](void* value) { delete static_cast<Task*>(value); };
  auto result = self.host->dispatch(self.host->user_data, run, release, stored);
  if (result != OVF_COM_STATUS_OK)
    release(stored);
  return result;
}

void Log(InprocTransport& self, const char* message) {
  if (self.host->log)
    self.host->log(self.host->user_data, OVF_COM_LOG_INFO,
                   {message, std::char_traits<char>::length(message)});
}

InprocTransport& Self(ovf_com_transport_v1* api) {
  return *static_cast<InprocTransport*>(api->implementation);
}

ovf_com_status_v1 Start(ovf_com_transport_v1* api) {
  auto& self = Self(api);
  {
    std::lock_guard lock(self.mutex);
    if (self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    self.running = true;
  }
  Log(self, "inproc transport started");
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 Stop(ovf_com_transport_v1* api) {
  auto& self = Self(api);
  std::vector<std::pair<ovf_com_handle_v1, Pending>> pending;
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_OK;
    self.running = false;
    for (auto const& item : self.pending)
      pending.push_back(item);
    self.pending.clear();
    self.loans.clear();
  }
  for (auto const& [operation, completion] : pending) {
    (void)Dispatch(self, [operation, completion] {
      completion.callback(completion.user, operation, OVF_COM_STATUS_SHUTTING_DOWN, {nullptr, 0});
    });
  }
  Log(self, "inproc transport stopped");
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 Capabilities(ovf_com_transport_v1* api, ovf_com_capabilities_v1* out) {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  *out = {sizeof(*out),
          OVF_COM_CAP_DISCOVERY | OVF_COM_CAP_EVENTS | OVF_COM_CAP_METHODS | OVF_COM_CAP_LOANS |
              OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED | OVF_COM_CAP_DEADLINES |
              OVF_COM_CAP_CANCELLATION,
          OVF_COM_ISOLATION_INDEPENDENT,
          self.max_endpoints,
          self.max_endpoints,
          self.max_operations,
          1,
          UINT64_C(16) * 1024 * 1024,
          UINT64_C(16) * 1024 * 1024,
          4096};
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 WatchStart(ovf_com_transport_v1* api, const ovf_com_discovery_filter_v1* filter,
                             ovf_com_discovery_callback_v1 callback, void* user,
                             ovf_com_handle_v1* out) {
  if (!filter || filter->struct_size < sizeof(*filter) || !callback || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::vector<Endpoint> available;
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    auto handle = self.next_handle++;
    self.watches.emplace(handle, Watch{filter->service_id, callback, user});
    *out = handle;
    for (auto const& [unused, endpoint] : self.endpoints) {
      (void)unused;
      if ((endpoint.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
           endpoint.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER) &&
          Equal(endpoint.descriptor.service_id, filter->service_id)) {
        available.push_back(endpoint);
      }
    }
  }
  for (auto const& endpoint : available) {
    auto entry = ovf_com_discovery_entry_v1{
        sizeof(ovf_com_discovery_entry_v1), endpoint.descriptor.service_id,
        endpoint.descriptor.instance_id,    OVF_COM_INVALID_HANDLE_V1,
        endpoint.descriptor.route_epoch,    1};
    (void)Dispatch(self, [callback, user, entry] { callback(user, &entry); });
  }
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 WatchStop(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  return self.watches.erase(handle) ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
}

ovf_com_status_v1 EndpointCreate(ovf_com_transport_v1* api,
                                 const ovf_com_endpoint_descriptor_v1* descriptor,
                                 ovf_com_handle_v1* out) {
  if (!descriptor || descriptor->struct_size < sizeof(*descriptor) || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::vector<Watch> notify;
  ovf_com_handle_v1 handle{};
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    if (self.endpoints.size() >= self.max_endpoints)
      return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
    if (descriptor->kind < OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
        descriptor->kind > OVF_COM_ENDPOINT_METHOD_SERVER)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    if (descriptor->max_payload_size > UINT64_C(16) * 1024 * 1024 ||
        descriptor->history_depth > 4096)
      return OVF_COM_STATUS_UNSUPPORTED;
    constexpr auto supported = OVF_COM_CAP_DISCOVERY | OVF_COM_CAP_EVENTS | OVF_COM_CAP_METHODS |
                               OVF_COM_CAP_LOANS | OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED |
                               OVF_COM_CAP_DEADLINES | OVF_COM_CAP_CANCELLATION;
    if ((descriptor->required_features & ~supported) != 0)
      return OVF_COM_STATUS_UNSUPPORTED;
    handle = self.next_handle++;
    self.endpoints.emplace(handle, Endpoint{*descriptor});
    if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
        descriptor->kind == OVF_COM_ENDPOINT_METHOD_SERVER) {
      for (auto const& [_, watch] : self.watches)
        if (Equal(watch.service, descriptor->service_id))
          notify.push_back(watch);
    }
    *out = handle;
  }
  for (auto const& watch : notify) {
    ovf_com_discovery_entry_v1 entry{sizeof(entry),           descriptor->service_id,
                                     descriptor->instance_id, handle,
                                     descriptor->route_epoch, 1};
    (void)Dispatch(self, [watch, entry] { watch.callback(watch.user, &entry); });
  }
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 EndpointDestroy(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) {
  auto& self = Self(api);
  std::vector<Watch> notify;
  Endpoint removed{};
  bool announce{};
  {
    std::lock_guard lock(self.mutex);
    if (std::any_of(self.subscriptions.begin(), self.subscriptions.end(),
                    [handle](auto const& item) { return item.second.endpoint == handle; }))
      return OVF_COM_STATUS_INVALID_STATE;
    auto endpoint = self.endpoints.find(handle);
    if (endpoint == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    removed = endpoint->second;
    announce = self.running && (removed.descriptor.kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
                                removed.descriptor.kind == OVF_COM_ENDPOINT_METHOD_SERVER);
    if (announce)
      for (auto const& [_, watch] : self.watches)
        if (Equal(watch.service, removed.descriptor.service_id))
          notify.push_back(watch);
    self.servers.erase(handle);
    self.endpoints.erase(endpoint);
  }
  for (auto const& watch : notify) {
    ovf_com_discovery_entry_v1 entry{
        sizeof(entry), removed.descriptor.service_id,  removed.descriptor.instance_id,
        handle,        removed.descriptor.route_epoch, 0};
    (void)Dispatch(self, [watch, entry] { watch.callback(watch.user, &entry); });
  }
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 Subscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                            ovf_com_sample_callback_v1 callback, void* user,
                            ovf_com_handle_v1* out) {
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
  *out = handle;
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 Unsubscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  return self.subscriptions.erase(handle) ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
}

ovf_com_status_v1 Deliver(InprocTransport& self, ovf_com_handle_v1 publisher,
                          std::shared_ptr<std::vector<std::uint8_t> const> payload) {
  std::vector<Subscription> targets;
  std::uint64_t sequence{};
  std::uint64_t epoch{};
  {
    std::lock_guard lock(self.mutex);
    auto source = self.endpoints.find(publisher);
    if (!self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    if (source == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    if (source->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_PUBLISHER)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    if (payload->size() > source->second.descriptor.max_payload_size)
      return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
    for (auto const& [_, subscription] : self.subscriptions) {
      auto endpoint = self.endpoints.find(subscription.endpoint);
      if (endpoint != self.endpoints.end() && SameElement(source->second, endpoint->second))
        targets.push_back(subscription);
    }
    sequence = ++self.sequence;
    epoch = source->second.descriptor.route_epoch;
  }
  for (auto const& target : targets) {
    (void)Dispatch(self, [target, payload, sequence, epoch] {
      ovf_com_sample_v1 sample{sizeof(sample),
                               {payload->data(), payload->size()},
                               OVF_COM_INVALID_HANDLE_V1,
                               sequence,
                               epoch};
      target.callback(target.user, &sample);
    });
  }
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 PublishBytes(InprocTransport& self, ovf_com_handle_v1 publisher,
                               ovf_com_bytes_view_v1 payload) {
  if (payload.size && !payload.data)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto owned = std::make_shared<std::vector<std::uint8_t>>();
  if (payload.size)
    owned->assign(payload.data, payload.data + payload.size);
  return Deliver(self, publisher, std::move(owned));
}

ovf_com_status_v1 Publish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                          ovf_com_bytes_view_v1 payload) {
  return PublishBytes(Self(api), endpoint, payload);
}

ovf_com_status_v1 PublishIov(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                             const ovf_com_iovec_v1* segments, size_t count) {
  if (!segments || count == 0)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (count != 1)
    return OVF_COM_STATUS_UNSUPPORTED;
  return Publish(api, endpoint, {segments[0].data, segments[0].size});
}

ovf_com_status_v1 LoanAcquire(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint, size_t size,
                              ovf_com_loan_v1* out) {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto source = self.endpoints.find(endpoint);
  if (source == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (source->second.descriptor.kind != OVF_COM_ENDPOINT_EVENT_PUBLISHER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (size > source->second.descriptor.max_payload_size)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto handle = self.next_handle++;
  auto [where, inserted] = self.loans.emplace(handle, std::vector<std::uint8_t>(size));
  if (!inserted)
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  *out = {sizeof(*out), handle, {where->second.data(), where->second.size()}};
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 LoanPublish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                              ovf_com_handle_v1 loan, size_t used) {
  auto& self = Self(api);
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.loans.find(loan);
    if (found == self.loans.end())
      return OVF_COM_STATUS_NOT_FOUND;
    if (used > found->second.size())
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    found->second.resize(used);
    bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(found->second));
    self.loans.erase(found);
  }
  return Deliver(self, endpoint, std::move(bytes));
}

ovf_com_status_v1 LoanRelease(ovf_com_transport_v1* api, ovf_com_handle_v1 loan) {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  return self.loans.erase(loan) ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
}

ovf_com_status_v1 SetRequestHandler(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                                    ovf_com_request_callback_v1 callback, void* user) {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(endpoint);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (found->second.descriptor.kind != OVF_COM_ENDPOINT_METHOD_SERVER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (callback)
    self.servers[endpoint] = {callback, user};
  else
    self.servers.erase(endpoint);
  return OVF_COM_STATUS_OK;
}

ovf_com_status_v1 Request(ovf_com_transport_v1* api, ovf_com_handle_v1 client,
                          ovf_com_bytes_view_v1 payload, uint64_t deadline,
                          ovf_com_completion_callback_v1 callback, void* user,
                          ovf_com_handle_v1* out) {
  if (!callback || !out || (payload.size && !payload.data))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  if (deadline <= self.host->monotonic_time_ns(self.host->user_data))
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  Server server{};
  ovf_com_handle_v1 operation{};
  {
    std::lock_guard lock(self.mutex);
    if (!self.running)
      return OVF_COM_STATUS_INVALID_STATE;
    auto source = self.endpoints.find(client);
    if (source == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    if (source->second.descriptor.kind != OVF_COM_ENDPOINT_METHOD_CLIENT)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    if (self.pending.size() >= self.max_operations)
      return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
    auto target = std::find_if(self.servers.begin(), self.servers.end(), [&](auto const& item) {
      auto endpoint = self.endpoints.find(item.first);
      return endpoint != self.endpoints.end() && SameElement(source->second, endpoint->second);
    });
    if (target == self.servers.end())
      return OVF_COM_STATUS_NOT_FOUND;
    operation = self.next_handle++;
    self.pending.emplace(operation, Pending{callback, user, false});
    server = target->second;
    *out = operation;
  }
  std::vector<std::uint8_t> copy(payload.data, payload.data + payload.size);
  return Dispatch(self, [server, operation, deadline, copy = std::move(copy)] {
    server.callback(server.user, operation, {copy.data(), copy.size()}, deadline);
  });
}

ovf_com_status_v1 Complete(InprocTransport& self, ovf_com_handle_v1 operation,
                           ovf_com_status_v1 status, ovf_com_bytes_view_v1 payload) {
  Pending pending{};
  {
    std::lock_guard lock(self.mutex);
    auto found = self.pending.find(operation);
    if (found == self.pending.end())
      return OVF_COM_STATUS_NOT_FOUND;
    pending = found->second;
    self.pending.erase(found);
  }
  std::vector<std::uint8_t> copy(payload.data, payload.data + payload.size);
  return Dispatch(self, [pending, operation, status, copy = std::move(copy)] {
    pending.callback(pending.user, operation, status, {copy.data(), copy.size()});
  });
}

ovf_com_status_v1 Cancel(ovf_com_transport_v1* api, ovf_com_handle_v1 operation) {
  return Complete(Self(api), operation, OVF_COM_STATUS_CANCELLED, {nullptr, 0});
}

ovf_com_status_v1 Respond(ovf_com_transport_v1* api, ovf_com_handle_v1 request,
                          ovf_com_status_v1 status, ovf_com_bytes_view_v1 payload) {
  if (payload.size && !payload.data)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  return Complete(Self(api), request, status, payload);
}

ovf_com_status_v1 Create(const ovf_com_host_api_v1* host, const ovf_com_transport_config_v1* config,
                         ovf_com_transport_v1** out) {
  if (!host || !config || !out || host->struct_size < sizeof(*host) ||
      config->struct_size < sizeof(*config) || !host->dispatch || !host->monotonic_time_ns)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto* self = new (std::nothrow) InprocTransport;
  if (!self)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  self->host = host;
  self->max_endpoints = config->max_endpoints ? config->max_endpoints : 256;
  self->max_operations =
      config->max_outstanding_operations ? config->max_outstanding_operations : 256;
  self->api.struct_size = sizeof(ovf_com_transport_v1);
  self->api.abi_version = OVF_COM_TRANSPORT_ABI_VERSION_1;
  self->api.implementation = self;
  self->api.name = {name, sizeof(name) - 1};
  *out = &self->api;
  return OVF_COM_STATUS_OK;
}

template <auto Function, class... Args> ovf_com_status_v1 Safe(Args... args) noexcept {
  try {
    return Function(args...);
  } catch (std::bad_alloc const&) {
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  }
}

void Destroy(ovf_com_transport_v1* api) {
  if (api)
    delete static_cast<InprocTransport*>(api->implementation);
}

ovf_com_status_v1 SafeCreate(const ovf_com_host_api_v1* host,
                             const ovf_com_transport_config_v1* config,
                             ovf_com_transport_v1** out) noexcept {
  auto result = Safe<Create>(host, config, out);
  if (result == OVF_COM_STATUS_OK) {
    auto& api = **out;
    api.start = &Safe<Start, ovf_com_transport_v1*>;
    api.stop = &Safe<Stop, ovf_com_transport_v1*>;
    api.get_capabilities = &Safe<Capabilities, ovf_com_transport_v1*, ovf_com_capabilities_v1*>;
    api.watch_start = &Safe<WatchStart, ovf_com_transport_v1*, const ovf_com_discovery_filter_v1*,
                            ovf_com_discovery_callback_v1, void*, ovf_com_handle_v1*>;
    api.watch_stop = &Safe<WatchStop, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.endpoint_create = &Safe<EndpointCreate, ovf_com_transport_v1*,
                                const ovf_com_endpoint_descriptor_v1*, ovf_com_handle_v1*>;
    api.endpoint_destroy = &Safe<EndpointDestroy, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.subscribe = &Safe<Subscribe, ovf_com_transport_v1*, ovf_com_handle_v1,
                          ovf_com_sample_callback_v1, void*, ovf_com_handle_v1*>;
    api.unsubscribe = &Safe<Unsubscribe, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.publish = &Safe<Publish, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1>;
    api.publish_iov = &Safe<PublishIov, ovf_com_transport_v1*, ovf_com_handle_v1,
                            const ovf_com_iovec_v1*, size_t>;
    api.loan_acquire =
        &Safe<LoanAcquire, ovf_com_transport_v1*, ovf_com_handle_v1, size_t, ovf_com_loan_v1*>;
    api.loan_publish =
        &Safe<LoanPublish, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_handle_v1, size_t>;
    api.loan_release = &Safe<LoanRelease, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.request = &Safe<Request, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1,
                        uint64_t, ovf_com_completion_callback_v1, void*, ovf_com_handle_v1*>;
    api.cancel = &Safe<Cancel, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.set_request_handler = &Safe<SetRequestHandler, ovf_com_transport_v1*, ovf_com_handle_v1,
                                    ovf_com_request_callback_v1, void*>;
    api.respond = &Safe<Respond, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_status_v1,
                        ovf_com_bytes_view_v1>;
  }
  return result;
}

const ovf_com_transport_factory_v1 factory{sizeof(factory),
                                           OVF_COM_TRANSPORT_ABI_VERSION_1,
                                           {name, sizeof(name) - 1},
                                           &SafeCreate,
                                           &Destroy};
} // namespace

extern "C" const ovf_com_transport_factory_v1* ovf_com_inproc_transport_query_v1(void) {
  return &factory;
}
