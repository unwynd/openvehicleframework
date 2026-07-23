// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2.h"
#include "ovf/com/transports/iceoryx2_mapping.hpp"

#include <iox2/iceoryx2.h>

#include <atomic>
#include <chrono>
#include <cstring>
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

  ~Endpoint() {
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
struct Subscription final {
  ovf_com_handle_v1 endpoint{};
  ovf_com_sample_callback_v1 callback{};
  void* user{};
};
struct Loan final {
  iox2_sample_mut_h sample{};
  ovf_com_handle_v1 endpoint{};
};
struct ReceivedLoan final {
  iox2_sample_h sample{};
};
struct Transport final {
  ovf_com_transport_v1 api{};
  ovf_com_host_api_v1 const* host{};
  iox2_node_h node{};
  std::atomic_bool running{};
  std::thread receiver;
  std::mutex mutex;
  ovf_com_handle_v1 next_handle{1};
  std::uint64_t sequence{};
  std::uint32_t max_endpoints{128};
  std::uint32_t max_operations{128};
  std::map<ovf_com_handle_v1, std::shared_ptr<Endpoint>> endpoints;
  std::map<ovf_com_handle_v1, Subscription> subscriptions;
  std::map<ovf_com_handle_v1, Loan> loans;
  std::map<ovf_com_handle_v1, ReceivedLoan> received_loans;
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
auto Unsupported(ovf_com_transport_v1*, ...) -> ovf_com_status_v1 {
  return OVF_COM_STATUS_UNSUPPORTED;
}
auto IoStatus(int value) -> ovf_com_status_v1 {
  return value == IOX2_OK ? OVF_COM_STATUS_OK : OVF_COM_STATUS_TRANSPORT_ERROR;
}
void ReceiveLoop(Transport* self) {
  while (self->running.load(std::memory_order_acquire)) {
    std::vector<std::pair<Subscription, std::shared_ptr<Endpoint>>> active;
    {
      std::lock_guard lock(self->mutex);
      active.reserve(self->subscriptions.size());
      for (auto const& [_, subscription] : self->subscriptions) {
        auto endpoint = self->endpoints.find(subscription.endpoint);
        if (endpoint != self->endpoints.end())
          active.emplace_back(subscription, endpoint->second);
      }
    }
    bool received_any{};
    for (auto const& [subscription, endpoint] : active) {
      iox2_sample_h native_sample{};
      if (iox2_subscriber_receive(&endpoint->subscriber, nullptr, &native_sample) != IOX2_OK ||
          native_sample == nullptr)
        continue;
      received_any = true;
      void const* payload{};
      std::size_t payload_elements{};
      iox2_sample_payload(&native_sample, &payload, &payload_elements);
      (void)payload_elements;
      ovf_com_handle_v1 loan_handle{};
      std::uint64_t sequence{};
      {
        std::lock_guard lock(self->mutex);
        loan_handle = self->next_handle++;
        sequence = ++self->sequence;
        self->received_loans.emplace(loan_handle, ReceivedLoan{native_sample});
      }
      ovf_com_sample_v1 sample{
          sizeof(sample),
          {static_cast<std::uint8_t const*>(payload), endpoint->mapping.payload_size},
          loan_handle,
          sequence,
          endpoint->descriptor.route_epoch};
      subscription.callback(subscription.user, &sample);
      {
        std::lock_guard lock(self->mutex);
        auto found = self->received_loans.find(loan_handle);
        if (found != self->received_loans.end()) {
          iox2_sample_drop(found->second.sample);
          self->received_loans.erase(found);
        }
      }
    }
    if (!received_any)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

auto Start(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  bool expected{};
  if (!self.running.compare_exchange_strong(expected, true))
    return OVF_COM_STATUS_INVALID_STATE;
  auto builder = iox2_node_builder_new(nullptr);
  auto result = iox2_node_builder_create(builder, nullptr, iox2_service_type_e_IPC, &self.node);
  if (result != IOX2_OK) {
    self.running = false;
    return IoStatus(result);
  }
  self.receiver = std::thread(ReceiveLoop, &self);
  return OVF_COM_STATUS_OK;
}
auto Stop(ovf_com_transport_v1* api) -> ovf_com_status_v1 {
  auto& self = Self(api);
  if (!self.running.exchange(false))
    return OVF_COM_STATUS_OK;
  if (self.receiver.joinable())
    self.receiver.join();
  std::lock_guard lock(self.mutex);
  for (auto& [_, loan] : self.loans)
    iox2_sample_mut_drop(loan.sample);
  for (auto& [_, loan] : self.received_loans)
    iox2_sample_drop(loan.sample);
  self.loans.clear();
  self.received_loans.clear();
  self.subscriptions.clear();
  self.endpoints.clear();
  if (self.node) {
    iox2_node_drop(self.node);
    self.node = nullptr;
  }
  return OVF_COM_STATUS_OK;
}
auto Capabilities(ovf_com_transport_v1* api, ovf_com_capabilities_v1* out) -> ovf_com_status_v1 {
  if (!out || out->struct_size < sizeof(*out))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& self = Self(api);
  *out = {sizeof(*out),
          OVF_COM_CAP_EVENTS | OVF_COM_CAP_LOANS | OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED,
          OVF_COM_ISOLATION_INDEPENDENT,
          self.max_endpoints,
          self.max_endpoints,
          self.max_operations,
          1,
          UINT64_MAX,
          UINT64_MAX,
          UINT32_MAX};
  return OVF_COM_STATUS_OK;
}

auto EndpointCreate(ovf_com_transport_v1* api, ovf_com_endpoint_descriptor_v1 const* descriptor,
                    ovf_com_handle_v1* out) -> ovf_com_status_v1 {
  if (!descriptor || !out || descriptor->struct_size < sizeof(*descriptor))
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  if (descriptor->kind != OVF_COM_ENDPOINT_EVENT_PUBLISHER &&
      descriptor->kind != OVF_COM_ENDPOINT_EVENT_SUBSCRIBER)
    return OVF_COM_STATUS_UNSUPPORTED;
  auto& self = Self(api);
  if (!self.running)
    return OVF_COM_STATUS_INVALID_STATE;
  auto endpoint = std::make_shared<Endpoint>();
  endpoint->descriptor = *descriptor;
  if (!Parse(descriptor->native_mapping, endpoint->mapping) ||
      descriptor->max_payload_size != endpoint->mapping.payload_size)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  auto& mapping = endpoint->mapping;
  auto result = iox2_service_name_new(nullptr, mapping.service.data(), mapping.service.size(),
                                      &endpoint->service_name);
  if (result != IOX2_OK)
    return IoStatus(result);
  auto service_builder = iox2_node_service_builder(
      &self.node, nullptr, iox2_cast_service_name_ptr(endpoint->service_name));
  auto pubsub = iox2_service_builder_pub_sub(service_builder);
  result = iox2_service_builder_pub_sub_set_payload_type_details(
      &pubsub, iox2_type_variant_e_FIXED_SIZE, mapping.type_name.data(), mapping.type_name.size(),
      mapping.payload_size, mapping.payload_alignment);
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
    result = iox2_port_factory_publisher_builder_create(builder, nullptr, &endpoint->publisher);
  } else {
    auto builder = iox2_port_factory_pub_sub_subscriber_builder(&endpoint->service, nullptr);
    result = iox2_port_factory_subscriber_builder_create(builder, nullptr, &endpoint->subscriber);
  }
  if (result != IOX2_OK)
    return IoStatus(result);
  std::lock_guard lock(self.mutex);
  if (self.endpoints.size() >= self.max_endpoints)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  auto handle = self.next_handle++;
  self.endpoints.emplace(handle, std::move(endpoint));
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto EndpointDestroy(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  for (auto const& [_, subscription] : self.subscriptions)
    if (subscription.endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  for (auto const& [_, loan] : self.loans)
    if (loan.endpoint == handle)
      return OVF_COM_STATUS_INVALID_STATE;
  return self.endpoints.erase(handle) ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
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
  self.subscriptions.emplace(handle, Subscription{endpoint_handle, callback, user});
  *out = handle;
  return OVF_COM_STATUS_OK;
}
auto Unsubscribe(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  auto& self = Self(api);
  std::lock_guard lock(self.mutex);
  return self.subscriptions.erase(handle) ? OVF_COM_STATUS_OK : OVF_COM_STATUS_NOT_FOUND;
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
  if (!endpoint->publisher || size != endpoint->mapping.payload_size)
    return OVF_COM_STATUS_INVALID_ARGUMENT;
  iox2_sample_mut_h sample{};
  auto result = iox2_publisher_loan_slice_uninit(&endpoint->publisher, nullptr, &sample, 1);
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
  self.loans.emplace(handle, Loan{sample, endpoint_handle});
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
    if (endpoint == self.endpoints.end() || used != endpoint->second->mapping.payload_size)
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
  if (auto found = self.received_loans.find(handle); found != self.received_loans.end()) {
    iox2_sample_drop(found->second.sample);
    self.received_loans.erase(found);
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
auto WatchStart(ovf_com_transport_v1* api, ovf_com_discovery_filter_v1 const* filter,
                ovf_com_discovery_callback_v1 callback, void* user, ovf_com_handle_v1* out)
    -> ovf_com_status_v1 {
  (void)api;
  (void)filter;
  (void)callback;
  (void)user;
  (void)out;
  return OVF_COM_STATUS_UNSUPPORTED;
}
auto WatchStop(ovf_com_transport_v1* api, ovf_com_handle_v1 handle) -> ovf_com_status_v1 {
  (void)api;
  (void)handle;
  return OVF_COM_STATUS_UNSUPPORTED;
}

auto Create(ovf_com_host_api_v1 const* host, ovf_com_transport_config_v1 const* config,
            ovf_com_transport_v1** out) -> ovf_com_status_v1 {
  if (!host || !config || !out || host->struct_size < sizeof(*host) ||
      config->struct_size < sizeof(*config))
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
               reinterpret_cast<decltype(self->api.publish_iov)>(Unsupported),
               LoanAcquire,
               LoanPublish,
               LoanRelease,
               reinterpret_cast<decltype(self->api.request)>(Unsupported),
               reinterpret_cast<decltype(self->api.cancel)>(Unsupported),
               reinterpret_cast<decltype(self->api.set_request_handler)>(Unsupported),
               reinterpret_cast<decltype(self->api.respond)>(Unsupported)};
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
const ovf_com_transport_factory_v1 kFactory{
    sizeof(kFactory), OVF_COM_TRANSPORT_ABI_VERSION_1, {kName, sizeof(kName) - 1}, Create, Destroy};
} // namespace

extern "C" const ovf_com_transport_factory_v1* ovf_com_iceoryx2_transport_query_v1() {
  return &kFactory;
}
