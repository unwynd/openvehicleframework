// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2.h"
#include "ovf/com/transports/iceoryx2_mapping.hpp"

#include <iox2/iceoryx2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace native = ovf::com::transports::iceoryx2;

struct Endpoint final {
  ovf_com_endpoint_descriptor_v1 descriptor{};
  native::Mapping mapping;
  iox2_service_name_h service_name{};
  iox2_port_factory_pub_sub_h service{};
  iox2_publisher_h publisher{};
  iox2_subscriber_h subscriber{};
  iox2_port_factory_request_response_h request_response{};
  iox2_client_h client{};
  iox2_server_h server{};
  ovf_com_request_callback_v1 request_callback{};
  void* request_user{};
  std::string announcement;

  ~Endpoint() {
    if (server)
      iox2_server_drop(server);
    if (client)
      iox2_client_drop(client);
    if (request_response)
      iox2_port_factory_request_response_drop(request_response);
    if (subscriber)
      iox2_subscriber_drop(subscriber);
    if (publisher)
      iox2_publisher_drop(publisher);
    if (service)
      iox2_port_factory_pub_sub_drop(service);
    if (service_name)
      iox2_service_name_drop(service_name);
  }
};
struct Announcement final {
  iox2_service_name_h name{};
  iox2_port_factory_pub_sub_h service{};
  iox2_publisher_h publisher{};
  std::size_t references{};
  ~Announcement() {
    if (publisher)
      iox2_publisher_drop(publisher);
    if (service)
      iox2_port_factory_pub_sub_drop(service);
    if (name)
      iox2_service_name_drop(name);
  }
};
struct Subscription final {
  ovf_com_handle_v1 endpoint{};
  ovf_com_sample_callback_v1 callback{};
  void* user{};
  std::atomic_bool active{true};
};
struct Loan final {
  iox2_sample_mut_h sample{};
  ovf_com_handle_v1 endpoint{};
  std::size_t size{};
};
struct ReceivedLoan final {
  std::atomic<iox2_sample_h> sample{};
  ~ReceivedLoan() {
    if (auto value = sample.exchange(nullptr))
      iox2_sample_drop(value);
  }
};
struct ReceivedRegistry final {
  std::mutex mutex;
  std::map<ovf_com_handle_v1, std::shared_ptr<ReceivedLoan>> loans;
};
struct Watch final {
  ovf_com_discovery_filter_v1 filter{};
  std::string mapping;
  ovf_com_discovery_callback_v1 callback{};
  void* user{};
  bool available{};
  std::atomic_bool active{true};
};
struct Pending final {
  ovf_com_handle_v1 endpoint{};
  iox2_pending_response_h response{};
  uint64_t deadline_ns{};
  ovf_com_completion_callback_v1 callback{};
  void* user{};
};
struct ActiveRequest final {
  ovf_com_handle_v1 endpoint{};
  iox2_active_request_h request{};
};
struct Transport final {
  ovf_com_transport_v1 api{};
  ovf_com_host_api_v1 const* host{};
  iox2_node_h node{};
  std::atomic_bool started{};
  std::atomic_bool running{};
  std::thread receiver;
  std::mutex mutex;
  ovf_com_handle_v1 next_handle{1};
  std::uint64_t sequence{};
  std::uint32_t max_endpoints{128};
  std::uint32_t max_operations{128};
  std::map<ovf_com_handle_v1, std::shared_ptr<Endpoint>> endpoints;
  std::map<ovf_com_handle_v1, std::shared_ptr<Subscription>> subscriptions;
  std::map<ovf_com_handle_v1, Loan> loans;
  std::shared_ptr<ReceivedRegistry> received_registry{std::make_shared<ReceivedRegistry>()};
  std::map<ovf_com_handle_v1, std::shared_ptr<Watch>> watches;
  std::map<ovf_com_handle_v1, Pending> pending;
  std::map<ovf_com_handle_v1, ActiveRequest> active_requests;
  std::map<std::string, std::shared_ptr<Announcement>> announcements;
};

constexpr char kName[] = "iceoryx2";
auto Self(ovf_com_transport_v1* api) -> Transport& {
  return *static_cast<Transport*>(api->implementation);
}
auto View(ovf_com_string_view_v1 value) -> std::string_view {
  return {value.data ? value.data : "", value.data ? value.size : 0};
}
auto Parse(ovf_com_string_view_v1 text, native::Mapping& mapping) -> bool {
  std::string error;
  return native::ParseMapping(View(text), mapping, error);
}
auto IoStatus(int value) -> ovf_com_status_v1 {
  return value == IOX2_OK ? OVF_COM_STATUS_OK : OVF_COM_STATUS_TRANSPORT_ERROR;
}
auto Dispatch(Transport& self, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release,
              void* data) -> ovf_com_status_v1 {
  auto status = self.host->dispatch(self.host->user_data, task, release, data);
  if (status != OVF_COM_STATUS_OK)
    release(data);
  return status;
}
struct SampleTask {
  std::shared_ptr<Subscription> subscription;
  std::shared_ptr<ReceivedRegistry> registry;
  std::shared_ptr<ReceivedLoan> loan;
  ovf_com_sample_v1 sample{};
};
void RunSample(void* data) {
  auto& task = *static_cast<SampleTask*>(data);
  if (task.subscription->active.load(std::memory_order_acquire))
    task.subscription->callback(task.subscription->user, &task.sample);
}
void ReleaseSample(void* data) {
  auto task = std::unique_ptr<SampleTask>(static_cast<SampleTask*>(data));
  if (auto value = task->loan->sample.exchange(nullptr))
    iox2_sample_drop(value);
  std::lock_guard lock(task->registry->mutex);
  task->registry->loans.erase(task->sample.provider_loan);
}
struct CompletionTask {
  ovf_com_completion_callback_v1 callback{};
  void* user{};
  ovf_com_handle_v1 operation{};
  ovf_com_status_v1 status{};
  std::vector<std::uint8_t> payload;
};
void RunCompletion(void* data) {
  auto& task = *static_cast<CompletionTask*>(data);
  task.callback(task.user, task.operation, task.status, {task.payload.data(), task.payload.size()});
}
void DeleteCompletion(void* data) { delete static_cast<CompletionTask*>(data); }
void Complete(Transport& self, Pending pending, ovf_com_handle_v1 operation,
              ovf_com_status_v1 status, std::vector<std::uint8_t> payload = {}) {
  if (pending.response)
    iox2_pending_response_drop(pending.response);
  auto* task = new (std::nothrow)
      CompletionTask{pending.callback, pending.user, operation, status, std::move(payload)};
  if (task)
    (void)Dispatch(self, RunCompletion, DeleteCompletion, task);
}
struct RequestTask {
  ovf_com_request_callback_v1 callback{};
  void* user{};
  ovf_com_handle_v1 request{};
  uint64_t deadline{};
  std::vector<std::uint8_t> payload;
};
void RunRequest(void* data) {
  auto& task = *static_cast<RequestTask*>(data);
  task.callback(task.user, task.request, {task.payload.data(), task.payload.size()}, task.deadline);
}
void DeleteRequest(void* data) { delete static_cast<RequestTask*>(data); }
struct RequestHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint64_t deadline_ns;
  std::uint32_t payload_size;
  std::uint32_t flags;
};
struct ResponseHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t status;
  std::uint32_t payload_size;
  std::uint32_t reserved;
};
constexpr std::uint32_t kRequestMagic{0x3152464fU};
constexpr std::uint32_t kResponseMagic{0x3153464fU};
constexpr std::uint16_t kEnvelopeVersion{1};
static_assert(sizeof(RequestHeader) == 24);
static_assert(sizeof(ResponseHeader) == 16);
auto ServiceAvailable(std::string_view prefix) -> bool {
  auto name_value = std::string(prefix) + "/__ovf_provider";
  iox2_service_name_h name{};
  if (iox2_service_name_new(nullptr, name_value.data(), name_value.size(), &name) != IOX2_OK)
    return false;
  bool exists{};
  auto result = iox2_service_does_exist(iox2_service_type_e_IPC, iox2_cast_service_name_ptr(name),
                                        iox2_config_global_config(),
                                        iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE, &exists);
  iox2_service_name_drop(name);
  return result == IOX2_OK && exists;
}
struct DiscoveryTask {
  std::shared_ptr<Watch> watch;
  ovf_com_discovery_entry_v1 entry{};
};
void RunDiscovery(void* data) {
  auto& task = *static_cast<DiscoveryTask*>(data);
  if (task.watch->active.load(std::memory_order_acquire))
    task.watch->callback(task.watch->user, &task.entry);
}
void DeleteDiscovery(void* data) { delete static_cast<DiscoveryTask*>(data); }
auto AnnouncementBase(std::string const& service) -> std::string {
  auto separator = service.rfind('/');
  return separator == std::string::npos ? service : service.substr(0, separator);
}
auto AcquireAnnouncement(Transport& self, std::string const& base) -> ovf_com_status_v1 {
  std::lock_guard lock(self.mutex);
  if (auto found = self.announcements.find(base); found != self.announcements.end()) {
    ++found->second->references;
    return OVF_COM_STATUS_OK;
  }
  auto announcement = std::make_shared<Announcement>();
  auto native_name = base + "/__ovf_provider";
  auto result =
      iox2_service_name_new(nullptr, native_name.data(), native_name.size(), &announcement->name);
  if (result != IOX2_OK)
    return IoStatus(result);
  auto builder = iox2_node_service_builder(&self.node, nullptr,
                                           iox2_cast_service_name_ptr(announcement->name));
  auto pubsub = iox2_service_builder_pub_sub(builder);
  constexpr char type[] = "ovf.discovery.v1";
  result = iox2_service_builder_pub_sub_set_payload_type_details(
      &pubsub, iox2_type_variant_e_FIXED_SIZE, type, sizeof(type) - 1, 1, 1);
  if (result != IOX2_OK)
    return IoStatus(result);
  iox2_service_builder_pub_sub_set_max_publishers(&pubsub, 128);
  iox2_service_builder_pub_sub_set_max_subscribers(&pubsub, 1);
  result = iox2_service_builder_pub_sub_open_or_create(pubsub, nullptr, &announcement->service);
  if (result != IOX2_OK)
    return IoStatus(result);
  auto publisher = iox2_port_factory_pub_sub_publisher_builder(&announcement->service, nullptr);
  result = iox2_port_factory_publisher_builder_create(publisher, nullptr, &announcement->publisher);
  if (result != IOX2_OK)
    return IoStatus(result);
  announcement->references = 1;
  self.announcements.emplace(base, std::move(announcement));
  return OVF_COM_STATUS_OK;
}
void ReleaseAnnouncement(Transport& self, std::string const& base) {
  auto found = self.announcements.find(base);
  if (found != self.announcements.end() && --found->second->references == 0)
    self.announcements.erase(found);
}
auto Respond(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_status_v1, ovf_com_bytes_view_v1)
    -> ovf_com_status_v1;
void ReceiveLoop(Transport* self) {
  while (self->running.load(std::memory_order_acquire)) {
    std::vector<std::pair<std::shared_ptr<Subscription>, std::shared_ptr<Endpoint>>> active;
    std::vector<std::pair<ovf_com_handle_v1, std::shared_ptr<Endpoint>>> servers;
    std::vector<ovf_com_handle_v1> operations;
    std::vector<std::pair<ovf_com_handle_v1, std::shared_ptr<Watch>>> watches;
    {
      std::lock_guard lock(self->mutex);
      active.reserve(self->subscriptions.size());
      for (auto const& [_, subscription] : self->subscriptions) {
        auto endpoint = self->endpoints.find(subscription->endpoint);
        if (endpoint != self->endpoints.end())
          active.emplace_back(subscription, endpoint->second);
      }
      for (auto const& entry : self->endpoints)
        if (entry.second->server)
          servers.push_back(entry);
      operations.reserve(self->pending.size());
      for (auto const& [operation, _] : self->pending)
        operations.push_back(operation);
      watches.assign(self->watches.begin(), self->watches.end());
    }
    bool received_any{};
    for (auto const& [subscription, endpoint] : active) {
      iox2_sample_h native_sample{};
      if (iox2_subscriber_receive(&endpoint->subscriber, nullptr, &native_sample) != IOX2_OK ||
          native_sample == nullptr)
        continue;
      received_any = true;
      void const* payload{};
      iox2_sample_payload(&native_sample, &payload, nullptr);
      ovf_com_handle_v1 loan_handle{};
      std::uint64_t sequence{};
      auto received_loan = std::make_shared<ReceivedLoan>();
      received_loan->sample.store(native_sample);
      {
        std::lock_guard lock(self->mutex);
        loan_handle = self->next_handle++;
        sequence = ++self->sequence;
      }
      {
        std::lock_guard lock(self->received_registry->mutex);
        self->received_registry->loans.emplace(loan_handle, received_loan);
      }
      auto* task =
          new (std::nothrow) SampleTask{subscription,
                                        self->received_registry,
                                        received_loan,
                                        {sizeof(ovf_com_sample_v1),
                                         {static_cast<std::uint8_t const*>(payload),
                                          iox2_sample_payload_number_of_bytes(&native_sample)},
                                         loan_handle,
                                         sequence,
                                         endpoint->descriptor.route_epoch}};
      if (task)
        (void)Dispatch(*self, RunSample, ReleaseSample, task);
      else {
        std::lock_guard lock(self->received_registry->mutex);
        self->received_registry->loans.erase(loan_handle);
      }
    }
    for (auto const& [endpoint_handle, endpoint] : servers) {
      for (;;) {
        iox2_active_request_h native_request{};
        if (iox2_server_receive(&endpoint->server, nullptr, &native_request) != IOX2_OK ||
            !native_request)
          break;
        received_any = true;
        void const* bytes{};
        iox2_active_request_payload(&native_request, &bytes, nullptr);
        auto size = iox2_active_request_payload_number_of_bytes(&native_request);
        RequestHeader header{};
        if (!endpoint->request_callback || size < sizeof(header)) {
          iox2_active_request_drop(native_request);
          continue;
        }
        std::memcpy(&header, bytes, sizeof(header));
        if (header.magic != kRequestMagic || header.version != kEnvelopeVersion ||
            header.payload_size > size - sizeof(header)) {
          iox2_active_request_drop(native_request);
          continue;
        }
        ovf_com_handle_v1 request_handle{};
        {
          std::lock_guard lock(self->mutex);
          if (self->active_requests.size() >= self->max_operations) {
            iox2_active_request_drop(native_request);
            continue;
          }
          request_handle = self->next_handle++;
          self->active_requests.emplace(request_handle,
                                        ActiveRequest{endpoint_handle, native_request});
        }
        auto begin = static_cast<std::uint8_t const*>(bytes) + sizeof(header);
        auto* task = new (std::nothrow) RequestTask{endpoint->request_callback,
                                                    endpoint->request_user,
                                                    request_handle,
                                                    header.deadline_ns,
                                                    {begin, begin + header.payload_size}};
        if (!task || Dispatch(*self, RunRequest, DeleteRequest, task) != OVF_COM_STATUS_OK)
          (void)Respond(&self->api, request_handle, OVF_COM_STATUS_RESOURCE_EXHAUSTED, {});
      }
    }
    auto now = self->host->monotonic_time_ns(self->host->user_data);
    for (auto operation : operations) {
      iox2_response_h native_response{};
      Pending owned{};
      bool expired{};
      int receive_result{IOX2_OK};
      {
        std::lock_guard lock(self->mutex);
        auto found = self->pending.find(operation);
        if (found == self->pending.end())
          continue;
        receive_result =
            iox2_pending_response_receive(&found->second.response, nullptr, &native_response);
        expired = !native_response && found->second.deadline_ns && now >= found->second.deadline_ns;
        if ((receive_result == IOX2_OK && native_response) || receive_result != IOX2_OK ||
            expired) {
          owned = found->second;
          self->pending.erase(found);
        } else {
          continue;
        }
      }
      if (native_response) {
        received_any = true;
        void const* bytes{};
        iox2_response_payload(&native_response, &bytes, nullptr);
        auto size = iox2_response_payload_number_of_bytes(&native_response);
        ResponseHeader header{};
        bool valid = size >= sizeof(header);
        if (valid) {
          std::memcpy(&header, bytes, sizeof(header));
          valid = header.magic == kResponseMagic && header.version == kEnvelopeVersion &&
                  header.status <= OVF_COM_STATUS_APPLICATION_ERROR &&
                  header.payload_size <= size - sizeof(header);
        }
        std::vector<std::uint8_t> payload;
        if (valid) {
          auto begin = static_cast<std::uint8_t const*>(bytes) + sizeof(header);
          payload.assign(begin, begin + header.payload_size);
        }
        iox2_response_drop(native_response);
        Complete(*self, owned, operation,
                 valid ? static_cast<ovf_com_status_v1>(header.status)
                       : OVF_COM_STATUS_TRANSPORT_ERROR,
                 std::move(payload));
      } else {
        Complete(*self, owned, operation,
                 expired ? OVF_COM_STATUS_DEADLINE_EXCEEDED : OVF_COM_STATUS_TRANSPORT_ERROR);
      }
    }
    for (auto const& [handle, watch] : watches) {
      auto available = ServiceAvailable(watch->mapping);
      if (available == watch->available)
        continue;
      {
        std::lock_guard lock(self->mutex);
        auto found = self->watches.find(handle);
        if (found == self->watches.end() || found->second->available == available)
          continue;
        found->second->available = available;
      }
      auto* task = new (std::nothrow) DiscoveryTask{watch,
                                                    {sizeof(ovf_com_discovery_entry_v1),
                                                     watch->filter.service_id,
                                                     {},
                                                     handle,
                                                     0,
                                                     static_cast<std::uint8_t>(available)}};
      if (task)
        (void)Dispatch(*self, RunDiscovery, DeleteDiscovery, task);
    }
    if (!received_any)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

auto Start(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  bool expected{};
  if (!self.started.compare_exchange_strong(expected, true))
    return OVF_COM_STATUS_INVALID_STATE;
  self.running.store(true, std::memory_order_release);
  auto builder = iox2_node_builder_new(nullptr);
  auto result = iox2_node_builder_create(builder, nullptr, iox2_service_type_e_IPC, &self.node);
  if (result != IOX2_OK) {
    self.running = false;
    self.started = false;
    return IoStatus(result);
  }
  self.receiver = std::thread(
      [](Transport* transport) noexcept {
        try {
          ReceiveLoop(transport);
        } catch (...) {
          transport->running.store(false, std::memory_order_release);
        }
      },
      &self);
  return OVF_COM_STATUS_OK;
}
auto Stop(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  if (!self.started.exchange(false))
    return OVF_COM_STATUS_OK;
  self.running.store(false, std::memory_order_release);
  if (self.receiver.joinable())
    self.receiver.join();
  std::vector<std::pair<ovf_com_handle_v1, Pending>> pending;
  {
    std::lock_guard lock(self.mutex);
    for (auto& [_, loan] : self.loans)
      iox2_sample_mut_drop(loan.sample);
    self.loans.clear();
    {
      std::lock_guard received_lock(self.received_registry->mutex);
      self.received_registry->loans.clear();
    }
    for (auto& [_, request] : self.active_requests)
      iox2_active_request_drop(request.request);
    self.active_requests.clear();
    pending.assign(self.pending.begin(), self.pending.end());
    self.pending.clear();
    for (auto& [_, watch] : self.watches)
      watch->active.store(false, std::memory_order_release);
    self.watches.clear();
    self.announcements.clear();
    for (auto& [_, subscription] : self.subscriptions)
      subscription->active.store(false, std::memory_order_release);
    self.subscriptions.clear();
    self.endpoints.clear();
    if (self.node) {
      iox2_node_drop(self.node);
      self.node = nullptr;
    }
  }
  for (auto& [operation, value] : pending)
    Complete(self, value, operation, OVF_COM_STATUS_SHUTTING_DOWN);
  return OVF_COM_STATUS_OK;
}
auto Capabilities(ovf_com_transport_v1* api, ovf_com_capabilities_v1* out) -> ovf_com_status_v1 {
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
          64,
          UINT64_MAX,
          UINT64_MAX,
          UINT32_MAX};
  return OVF_COM_STATUS_OK;
}

auto EndpointCreate(ovf_com_transport_v1* api, ovf_com_endpoint_descriptor_v1 const* descriptor,
                    ovf_com_handle_v1* out) -> ovf_com_status_v1 {
  if (!descriptor || !out || descriptor->struct_size < sizeof(*descriptor))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (descriptor->kind < OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
      descriptor->kind > OVF_COM_ENDPOINT_METHOD_SERVER)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto endpoint = std::make_shared<Endpoint>();
  endpoint->descriptor = *descriptor;
  if (!Parse(descriptor->native_mapping, endpoint->mapping))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto event = descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
               descriptor->kind == OVF_COM_ENDPOINT_EVENT_SUBSCRIBER;
  if (event != (endpoint->mapping.pattern == native::Mapping::Pattern::kPublishSubscribe))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (event && descriptor->max_payload_size < endpoint->mapping.payload_size)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (!event && descriptor->max_payload_size < std::max(endpoint->mapping.request_payload_size,
                                                        endpoint->mapping.response_payload_size))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& mapping = endpoint->mapping;
  auto result = iox2_service_name_new(nullptr, mapping.service.data(), mapping.service.size(),
                                      &endpoint->service_name);
  if (result != IOX2_OK)
    return IoStatus(result);
  auto service_builder = iox2_node_service_builder(
      &self.node, nullptr, iox2_cast_service_name_ptr(endpoint->service_name));
  if (event) {
    auto pubsub = iox2_service_builder_pub_sub(service_builder);
    result = iox2_service_builder_pub_sub_set_payload_type_details(
        &pubsub, iox2_type_variant_e_DYNAMIC, mapping.type_name.data(), mapping.type_name.size(), 1,
        1);
    if (result != IOX2_OK)
      return IoStatus(result);
    iox2_service_builder_pub_sub_set_max_publishers(&pubsub, mapping.max_publishers);
    iox2_service_builder_pub_sub_set_max_subscribers(&pubsub, mapping.max_subscribers);
    iox2_service_builder_pub_sub_set_history_size(&pubsub, mapping.history_depth);
    iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&pubsub, mapping.subscriber_buffer);
    iox2_service_builder_pub_sub_set_enable_safe_overflow(&pubsub, mapping.safe_overflow);
    result = iox2_service_builder_pub_sub_open_or_create(pubsub, nullptr, &endpoint->service);
    if (result != IOX2_OK)
      return IoStatus(result);
    if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER) {
      auto builder = iox2_port_factory_pub_sub_publisher_builder(&endpoint->service, nullptr);
      iox2_port_factory_publisher_builder_set_initial_max_slice_len(&builder, mapping.payload_size);
      result = iox2_port_factory_publisher_builder_create(builder, nullptr, &endpoint->publisher);
    } else {
      auto builder = iox2_port_factory_pub_sub_subscriber_builder(&endpoint->service, nullptr);
      result = iox2_port_factory_subscriber_builder_create(builder, nullptr, &endpoint->subscriber);
    }
  } else {
    auto request_response = iox2_service_builder_request_response(service_builder);
    result = iox2_service_builder_request_response_set_request_payload_type_details(
        &request_response, iox2_type_variant_e_DYNAMIC, mapping.request_type.data(),
        mapping.request_type.size(), 1, 1);
    if (result != IOX2_OK)
      return IoStatus(result);
    result = iox2_service_builder_request_response_set_response_payload_type_details(
        &request_response, iox2_type_variant_e_DYNAMIC, mapping.response_type.data(),
        mapping.response_type.size(), 1, 1);
    if (result != IOX2_OK)
      return IoStatus(result);
    iox2_service_builder_request_response_max_active_requests_per_client(&request_response,
                                                                         mapping.request_buffer);
    iox2_service_builder_request_response_max_response_buffer_size(&request_response,
                                                                   mapping.response_buffer);
    iox2_service_builder_request_response_max_clients(&request_response, mapping.max_clients);
    iox2_service_builder_request_response_max_servers(&request_response, mapping.max_servers);
    iox2_service_builder_request_response_enable_safe_overflow_for_requests(&request_response,
                                                                            mapping.safe_overflow);
    iox2_service_builder_request_response_enable_safe_overflow_for_responses(&request_response,
                                                                             mapping.safe_overflow);
    result = iox2_service_builder_request_response_open_or_create(request_response, nullptr,
                                                                  &endpoint->request_response);
    if (result != IOX2_OK)
      return IoStatus(result);
    if (descriptor->kind == OVF_COM_ENDPOINT_METHOD_CLIENT) {
      auto builder =
          iox2_port_factory_request_response_client_builder(&endpoint->request_response, nullptr);
      iox2_port_factory_client_builder_set_initial_max_slice_len(
          &builder, mapping.request_payload_size + sizeof(RequestHeader));
      result = iox2_port_factory_client_builder_create(builder, nullptr, &endpoint->client);
    } else {
      auto builder =
          iox2_port_factory_request_response_server_builder(&endpoint->request_response, nullptr);
      iox2_port_factory_server_builder_set_initial_max_slice_len(
          &builder, mapping.response_payload_size + sizeof(ResponseHeader));
      result = iox2_port_factory_server_builder_create(builder, nullptr, &endpoint->server);
    }
  }
  if (result != IOX2_OK)
    return IoStatus(result);
  if (descriptor->kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER ||
      descriptor->kind == OVF_COM_ENDPOINT_METHOD_SERVER) {
    endpoint->announcement = AnnouncementBase(mapping.service);
    auto status = AcquireAnnouncement(self, endpoint->announcement);
    if (status != OVF_COM_STATUS_OK)
      return status;
  }
  std::lock_guard lock(self.mutex);
  if (self.endpoints.size() >= self.max_endpoints) {
    if (!endpoint->announcement.empty())
      ReleaseAnnouncement(self, endpoint->announcement);
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  }
  auto handle = self.next_handle++;
  self.endpoints.emplace(handle, std::move(endpoint));
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto EndpointDestroy(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  for (auto const& [_, subscription] : self.subscriptions)
    if (subscription->endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  for (auto const& [_, loan] : self.loans)
    if (loan.endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  for (auto const& [_, operation] : self.pending)
    if (operation.endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  for (auto const& [_, request] : self.active_requests)
    if (request.endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  auto found = self.endpoints.find(handle);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  auto announcement = found->second->announcement;
  self.endpoints.erase(found);
  if (!announcement.empty())
    ReleaseAnnouncement(self, announcement);
  return OVF_COM_STATUS_OK;
}
auto Subscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint_handle,
               ovf_com_sample_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!callback || !out)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto endpoint = self.endpoints.find(endpoint_handle);
  if (endpoint == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (!endpoint->second->subscriber)
    return OVF_COM_STATUS_INVALID_STATE;
  auto handle = self.next_handle++;
  auto subscription = std::make_shared<Subscription>();
  subscription->endpoint = endpoint_handle;
  subscription->callback = callback;
  subscription->user = user;
  self.subscriptions.emplace(handle, std::move(subscription));
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto Unsubscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.subscriptions.find(handle);
  if (found == self.subscriptions.end())
    return OVF_COM_STATUS_NOT_FOUND;
  found->second->active.store(false, std::memory_order_release);
  self.subscriptions.erase(found);
  return OVF_COM_STATUS_OK;
}
auto LoanAcquire(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint_handle, std::size_t size,
                 ovf_com_loan_v1* out) -> ovf_com_status_v1 {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::shared_ptr<Endpoint> endpoint;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.endpoints.find(endpoint_handle);
    if (found == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    endpoint = found->second;
  }
  if (!endpoint->publisher || size == 0 || size > endpoint->mapping.payload_size)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  iox2_sample_mut_h sample{};
  auto result = iox2_publisher_loan_slice_uninit(&endpoint->publisher, nullptr, &sample, size);
  if (result != IOX2_OK)
    return result == IOX2_OK ? OVF_COM_STATUS_OK : OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  void* payload{};
  std::size_t elements{};
  iox2_sample_mut_payload_mut(&sample, &payload, &elements);
  (void)elements;
  std::lock_guard lock(self.mutex);
  if (self.loans.size() >= self.max_operations) {
    iox2_sample_mut_drop(sample);
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  }
  auto handle = self.next_handle++;
  self.loans.emplace(handle, Loan{sample, endpoint_handle, size});
  *out = {sizeof(*out), handle, {static_cast<std::uint8_t*>(payload), size}};
  return OVF_COM_STATUS_OK;
}
auto LoanPublish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint_handle,
                 ovf_com_handle_v1 loan_handle, std::size_t used) -> ovf_com_status_v1 {
  auto& self = Self(api);
  iox2_sample_mut_h sample{};
  {
    std::lock_guard lock(self.mutex);
    auto found = self.loans.find(loan_handle);
    if (found == self.loans.end())
      return OVF_COM_STATUS_NOT_FOUND;
    if (found->second.endpoint != endpoint_handle)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    auto endpoint = self.endpoints.find(endpoint_handle);
    if (endpoint == self.endpoints.end() || used != found->second.size)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    sample = found->second.sample;
    self.loans.erase(found);
  }
  return IoStatus(iox2_sample_mut_send(sample, nullptr));
}
auto LoanRelease(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  if (auto found = self.loans.find(handle); found != self.loans.end()) {
    iox2_sample_mut_drop(found->second.sample);
    self.loans.erase(found);
    return OVF_COM_STATUS_OK;
  }
  std::lock_guard received_lock(self.received_registry->mutex);
  if (auto found = self.received_registry->loans.find(handle);
      found != self.received_registry->loans.end()) {
    if (auto value = found->second->sample.exchange(nullptr))
      iox2_sample_drop(value);
    self.received_registry->loans.erase(found);
    return OVF_COM_STATUS_OK;
  }
  return OVF_COM_STATUS_NOT_FOUND;
}
auto Publish(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint, ovf_com_bytes_view_v1 bytes)
    -> ovf_com_status_v1 {
  ovf_com_loan_v1 loan{};
  loan.struct_size = sizeof(loan);
  auto result = LoanAcquire(api, endpoint, bytes.size, &loan);
  if (result != OVF_COM_STATUS_OK)
    return result;
  std::memcpy(loan.bytes.data, bytes.data, bytes.size);
  return LoanPublish(api, endpoint, loan.handle, bytes.size);
}
auto PublishIov(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint,
                ovf_com_iovec_v1 const* iovecs, std::size_t count) -> ovf_com_status_v1 {
  if ((!iovecs && count) || count == 0)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  std::size_t size{};
  for (std::size_t index = 0; index < count; ++index) {
    if ((!iovecs[index].data && iovecs[index].size) ||
        iovecs[index].size > std::numeric_limits<std::size_t>::max() - size)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    size += iovecs[index].size;
  }
  ovf_com_loan_v1 loan{};
  loan.struct_size = sizeof(loan);
  auto status = LoanAcquire(api, endpoint, size, &loan);
  if (status != OVF_COM_STATUS_OK)
    return status;
  auto* destination = loan.bytes.data;
  for (std::size_t index = 0; index < count; ++index) {
    std::memcpy(destination, iovecs[index].data, iovecs[index].size);
    destination += iovecs[index].size;
  }
  return LoanPublish(api, endpoint, loan.handle, size);
}
auto Request(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint_handle,
             ovf_com_bytes_view_v1 payload, std::uint64_t deadline,
             ovf_com_completion_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!callback || !out || (!payload.data && payload.size))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::shared_ptr<Endpoint> endpoint;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.endpoints.find(endpoint_handle);
    if (found == self.endpoints.end())
      return OVF_COM_STATUS_NOT_FOUND;
    endpoint = found->second;
    if (!endpoint->client)
      return OVF_COM_STATUS_INVALID_STATE;
    if (self.pending.size() >= self.max_operations)
      return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  }
  if (payload.size > endpoint->mapping.request_payload_size)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (deadline && self.host->monotonic_time_ns(self.host->user_data) >= deadline)
    return OVF_COM_STATUS_DEADLINE_EXCEEDED;
  std::vector<std::uint8_t> wire(sizeof(RequestHeader) + payload.size);
  RequestHeader header{
      kRequestMagic, kEnvelopeVersion, 0, deadline, static_cast<std::uint32_t>(payload.size), 0};
  std::memcpy(wire.data(), &header, sizeof(header));
  std::memcpy(wire.data() + sizeof(header), payload.data, payload.size);
  std::lock_guard lock(self.mutex);
  if (!self.running)
    return OVF_COM_STATUS_SHUTTING_DOWN;
  if (self.pending.size() >= self.max_operations)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  iox2_pending_response_h native_response{};
  auto result = iox2_client_send_copy(&endpoint->client, wire.data(), 1, wire.size(), nullptr,
                                      &native_response);
  if (result != IOX2_OK)
    return IoStatus(result);
  auto operation = self.next_handle++;
  self.pending.emplace(operation,
                       Pending{endpoint_handle, native_response, deadline, callback, user});
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
  }
  Complete(self, pending, operation, OVF_COM_STATUS_CANCELLED);
  return OVF_COM_STATUS_OK;
}
auto SetRequestHandler(ovf_com_transport_v1* api, ovf_com_handle_v1 endpoint_handle,
                       ovf_com_request_callback_v1 callback, void* user) -> ovf_com_status_v1 {
  if (!callback)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.endpoints.find(endpoint_handle);
  if (found == self.endpoints.end())
    return OVF_COM_STATUS_NOT_FOUND;
  if (!found->second->server || found->second->request_callback)
    return OVF_COM_STATUS_INVALID_STATE;
  found->second->request_callback = callback;
  found->second->request_user = user;
  return OVF_COM_STATUS_OK;
}
auto Respond(ovf_com_transport_v1* api, ovf_com_handle_v1 request_handle, ovf_com_status_v1 status,
             ovf_com_bytes_view_v1 payload) -> ovf_com_status_v1 {
  if (status > OVF_COM_STATUS_APPLICATION_ERROR || (!payload.data && payload.size))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  ActiveRequest active{};
  std::shared_ptr<Endpoint> endpoint;
  {
    std::lock_guard lock(self.mutex);
    auto found = self.active_requests.find(request_handle);
    if (found == self.active_requests.end())
      return OVF_COM_STATUS_NOT_FOUND;
    active = found->second;
    auto endpoint_found = self.endpoints.find(active.endpoint);
    if (endpoint_found == self.endpoints.end())
      return OVF_COM_STATUS_INVALID_STATE;
    endpoint = endpoint_found->second;
    if (payload.size > endpoint->mapping.response_payload_size)
      return OVF_COM_STATUS_INVALID_ARGUMENT;
    self.active_requests.erase(found);
  }
  std::vector<std::uint8_t> wire(sizeof(ResponseHeader) + payload.size);
  ResponseHeader header{kResponseMagic, kEnvelopeVersion, static_cast<std::uint16_t>(status),
                        static_cast<std::uint32_t>(payload.size), 0};
  std::memcpy(wire.data(), &header, sizeof(header));
  std::memcpy(wire.data() + sizeof(header), payload.data, payload.size);
  auto result = iox2_active_request_send_copy(&active.request, wire.data(), 1, wire.size());
  iox2_active_request_drop(active.request);
  return IoStatus(result);
}
auto WatchStart(ovf_com_transport_v1* api, ovf_com_discovery_filter_v1 const* filter,
                ovf_com_discovery_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  if (!filter || filter->struct_size < sizeof(*filter) || !callback || !out ||
      View(filter->native_mapping).empty())
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  std::lock_guard lock(self.mutex);
  if (self.watches.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto handle = self.next_handle++;
  auto mapping = std::string(View(filter->native_mapping));
  auto copy = *filter;
  copy.native_mapping = {};
  auto watch = std::make_shared<Watch>();
  watch->filter = copy;
  watch->mapping = std::move(mapping);
  watch->callback = callback;
  watch->user = user;
  self.watches.emplace(handle, std::move(watch));
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto WatchStop(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  auto found = self.watches.find(handle);
  if (found == self.watches.end())
    return OVF_COM_STATUS_NOT_FOUND;
  found->second->active.store(false, std::memory_order_release);
  self.watches.erase(found);
  return OVF_COM_STATUS_OK;
}

auto Create(ovf_com_host_api_v1 const* host, ovf_com_transport_config_v1 const* config,
            ovf_com_transport_v1** out) -> ovf_com_status_v1 {
  if (!host || !config || !out || host->struct_size < sizeof(*host) ||
      config->struct_size < sizeof(*config) || !host->dispatch || !host->monotonic_time_ns)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto self = std::unique_ptr<Transport>(new (std::nothrow) Transport);
  if (!self)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  self->host = host;
  self->max_endpoints = config->max_endpoints ? config->max_endpoints : 128;
  self->max_operations =
      config->max_outstanding_operations ? config->max_outstanding_operations : 128;
  self->api = {sizeof(ovf_com_transport_v1),
               OVF_COM_TRANSPORT_ABI_VERSION_1,
               self.get(),
               {kName, sizeof(kName) - 1},
               Start,
               Stop,
               Capabilities,
               WatchStart,
               WatchStop,
               EndpointCreate,
               EndpointDestroy,
               Subscribe,
               Unsubscribe,
               Publish,
               PublishIov,
               LoanAcquire,
               LoanPublish,
               LoanRelease,
               Request,
               Cancel,
               SetRequestHandler,
               Respond};
  *out = &self.release()->api;
  return OVF_COM_STATUS_OK;
}
template <auto Function, class... Args> auto Safe(Args... args) noexcept -> ovf_com_status_v1 {
  try {
    return Function(args...);
  } catch (std::bad_alloc const&) {
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return OVF_COM_STATUS_TRANSPORT_ERROR;
  }
}
auto SafeCreate(ovf_com_host_api_v1 const* host, ovf_com_transport_config_v1 const* config,
                ovf_com_transport_v1** out) noexcept -> ovf_com_status_v1 {
  auto result = Safe<Create>(host, config, out);
  if (result == OVF_COM_STATUS_OK) {
    auto& api = **out;
    api.start = &Safe<Start, ovf_com_transport_v1*>;
    api.stop = &Safe<Stop, ovf_com_transport_v1*>;
    api.get_capabilities = &Safe<Capabilities, ovf_com_transport_v1*, ovf_com_capabilities_v1*>;
    api.watch_start = &Safe<WatchStart, ovf_com_transport_v1*, ovf_com_discovery_filter_v1 const*,
                            ovf_com_discovery_callback_v1, void*, ovf_com_handle_v1*>;
    api.watch_stop = &Safe<WatchStop, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.endpoint_create = &Safe<EndpointCreate, ovf_com_transport_v1*,
                                ovf_com_endpoint_descriptor_v1 const*, ovf_com_handle_v1*>;
    api.endpoint_destroy = &Safe<EndpointDestroy, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.subscribe = &Safe<Subscribe, ovf_com_transport_v1*, ovf_com_handle_v1,
                          ovf_com_sample_callback_v1, void*, ovf_com_handle_v1*>;
    api.unsubscribe = &Safe<Unsubscribe, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.publish = &Safe<Publish, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1>;
    api.publish_iov = &Safe<PublishIov, ovf_com_transport_v1*, ovf_com_handle_v1,
                            ovf_com_iovec_v1 const*, std::size_t>;
    api.loan_acquire =
        &Safe<LoanAcquire, ovf_com_transport_v1*, ovf_com_handle_v1, std::size_t, ovf_com_loan_v1*>;
    api.loan_publish = &Safe<LoanPublish, ovf_com_transport_v1*, ovf_com_handle_v1,
                             ovf_com_handle_v1, std::size_t>;
    api.loan_release = &Safe<LoanRelease, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.request = &Safe<Request, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1,
                        std::uint64_t, ovf_com_completion_callback_v1, void*, ovf_com_handle_v1*>;
    api.cancel = &Safe<Cancel, ovf_com_transport_v1*, ovf_com_handle_v1>;
    api.set_request_handler = &Safe<SetRequestHandler, ovf_com_transport_v1*, ovf_com_handle_v1,
                                    ovf_com_request_callback_v1, void*>;
    api.respond = &Safe<Respond, ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_status_v1,
                        ovf_com_bytes_view_v1>;
  }
  return result;
}
void Destroy(ovf_com_transport_v1* api) noexcept {
  if (!api)
    return;
  auto* self = static_cast<Transport*>(api->implementation);
  (void)Stop(api);
  delete self;
}
const ovf_com_transport_factory_v1 kFactory{sizeof(kFactory),
                                            OVF_COM_TRANSPORT_ABI_VERSION_1,
                                            {kName, sizeof(kName) - 1},
                                            SafeCreate,
                                            Destroy};
} // namespace

extern "C" const ovf_com_transport_factory_v1* ovf_com_iceoryx2_transport_query_v1() {
  return &kFactory;
}
