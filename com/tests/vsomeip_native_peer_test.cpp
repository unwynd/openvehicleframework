// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/vsomeip.h"

#include <vsomeip/vsomeip.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr vsomeip::service_t kService = 0x1234;
constexpr vsomeip::instance_t kInstance = 1;
constexpr vsomeip::method_t kMethod = 1;
constexpr vsomeip::event_t kEvent = 0x8001;
constexpr vsomeip::eventgroup_t kEventGroup = 1;
constexpr vsomeip::service_t kPeerService = 0x1235;
constexpr vsomeip::instance_t kPeerInstance = 2;
constexpr vsomeip::method_t kSilentMethod = 2;
constexpr vsomeip::method_t kErrorMethod = 3;

auto Dispatch(void*, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release, void* user)
    -> ovf_com_status_v1 {
  task(user);
  release(user);
  return OVF_COM_STATUS_OK;
}
auto Now(void*) -> std::uint64_t {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}
auto Id(std::uint8_t value) -> ovf_com_uuid_v1 {
  ovf_com_uuid_v1 result{};
  result.bytes[15] = value;
  return result;
}
auto Descriptor(ovf_com_endpoint_kind_v1 kind, ovf_com_uuid_v1 element, std::string_view mapping)
    -> ovf_com_endpoint_descriptor_v1 {
  return {sizeof(ovf_com_endpoint_descriptor_v1),
          kind,
          Id(1),
          Id(2),
          element,
          1,
          1024,
          1,
          static_cast<std::uint64_t>(kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER
                                         ? OVF_COM_CAP_EVENTS | OVF_COM_CAP_RELIABLE
                                         : OVF_COM_CAP_METHODS | OVF_COM_CAP_RELIABLE),
          {mapping.data(), mapping.size()}};
}
struct Server {
  ovf_com_transport_v1* provider{};
  int requests{};
};
void OnRequest(void* user, ovf_com_handle_v1 request, ovf_com_bytes_view_v1 payload,
               std::uint64_t deadline) {
  auto& server = *static_cast<Server*>(user);
  assert(payload.size == 2 && payload.data[0] == 1 && payload.data[1] == 2);
  assert(deadline == 0);
  ++server.requests;
  std::uint8_t response[]{3, 4, 5};
  assert(server.provider->respond(server.provider, request, OVF_COM_STATUS_OK,
                                  {response, sizeof(response)}) == OVF_COM_STATUS_OK);
}

struct Peer {
  std::shared_ptr<vsomeip::application> application;
  std::mutex mutex;
  std::condition_variable condition;
  bool available{};
  bool response{};
  bool event{};

  void OnState(vsomeip::state_type_e state) {
    if (state != vsomeip::state_type_e::ST_REGISTERED)
      return;
    application->request_service(kService, kInstance, 1, 0);
    application->request_event(kService, kInstance, kEvent, {kEventGroup},
                               vsomeip::event_type_e::ET_EVENT,
                               vsomeip::reliability_type_e::RT_RELIABLE);
    application->subscribe(kService, kInstance, kEventGroup, 1);
    application->offer_service(kPeerService, kPeerInstance, 1, 0);
  }
  void OnAvailability(vsomeip::service_t, vsomeip::instance_t, bool value) {
    if (!value)
      return;
    auto request = vsomeip::runtime::get()->create_request(true);
    request->set_service(kService);
    request->set_instance(kInstance);
    request->set_method(kMethod);
    request->set_interface_version(1);
    std::uint8_t bytes[]{1, 2};
    request->set_payload(vsomeip::runtime::get()->create_payload(bytes, sizeof(bytes)));
    {
      std::lock_guard lock(mutex);
      available = true;
    }
    application->send(request);
    condition.notify_all();
  }
  void OnMessage(std::shared_ptr<vsomeip::message> const& message) {
    if (message->get_service() == kPeerService && message->get_method() == kErrorMethod) {
      auto response = vsomeip::runtime::get()->create_response(message);
      std::uint8_t error[]{0x42};
      response->set_payload(vsomeip::runtime::get()->create_payload(error, sizeof(error)));
      response->set_return_code(vsomeip::return_code_e::E_NOT_OK);
      application->send(response);
      return;
    }
    if (message->get_service() == kPeerService && message->get_method() == kSilentMethod)
      return;
    auto payload = message->get_payload();
    std::lock_guard lock(mutex);
    if (message->get_message_type() == vsomeip::message_type_e::MT_RESPONSE) {
      assert(payload && payload->get_length() == 3 && payload->get_data()[0] == 3);
      response = true;
    } else if (message->get_method() == kEvent) {
      assert(payload && payload->get_length() == 2 && payload->get_data()[0] == 8);
      event = true;
    }
    condition.notify_all();
  }
};
struct CompletionState {
  std::mutex mutex;
  std::condition_variable condition;
  ovf_com_status_v1 status{OVF_COM_STATUS_TRANSPORT_ERROR};
  std::vector<std::uint8_t> payload;
  bool complete{};
};
void OnCompletion(void* user, ovf_com_handle_v1, ovf_com_status_v1 status,
                  ovf_com_bytes_view_v1 payload) {
  auto& completion = *static_cast<CompletionState*>(user);
  {
    std::lock_guard lock(completion.mutex);
    completion.status = status;
    completion.payload.clear();
    if (payload.size)
      completion.payload.assign(payload.data, payload.data + payload.size);
    completion.complete = true;
  }
  completion.condition.notify_all();
}
void Reset(CompletionState& completion) {
  std::lock_guard lock(completion.mutex);
  completion.status = OVF_COM_STATUS_TRANSPORT_ERROR;
  completion.payload.clear();
  completion.complete = false;
}
void WaitFor(CompletionState& completion, ovf_com_status_v1 expected) {
  std::unique_lock lock(completion.mutex);
  assert(completion.condition.wait_for(lock, std::chrono::seconds(3),
                                       [&completion] { return completion.complete; }));
  assert(completion.status == expected);
}
} // namespace

int main() {
  ovf_com_host_api_v1 host{sizeof(host), nullptr, nullptr, &Dispatch, &Now};
  ovf_com_transport_config_v1 config{sizeof(config), {"ovf-i4-provider", 15}, {nullptr, 0}, 8, 8};
  auto const* factory = ovf_com_vsomeip_transport_query_v1();
  ovf_com_transport_v1* provider{};
  assert(factory->create(&host, &config, &provider) == OVF_COM_STATUS_OK);
  assert(provider->start(provider) == OVF_COM_STATUS_OK);

  constexpr std::string_view method_mapping =
      "service=4660;instance=1;element=1;eventGroup=0;major=1;minor=0;"
      "kind=method;reliable=true";
  constexpr std::string_view event_mapping =
      "service=4660;instance=1;element=32769;eventGroup=1;major=1;minor=0;"
      "kind=event;reliable=true";
  auto method = Descriptor(OVF_COM_ENDPOINT_METHOD_SERVER, Id(3), method_mapping);
  auto event = Descriptor(OVF_COM_ENDPOINT_EVENT_PUBLISHER, Id(4), event_mapping);
  ovf_com_handle_v1 method_endpoint{}, event_endpoint{};
  assert(provider->endpoint_create(provider, &method, &method_endpoint) == OVF_COM_STATUS_OK);
  assert(provider->endpoint_create(provider, &event, &event_endpoint) == OVF_COM_STATUS_OK);
  Server server{provider};
  assert(provider->set_request_handler(provider, method_endpoint, &OnRequest, &server) ==
         OVF_COM_STATUS_OK);

  Peer peer;
  peer.application = vsomeip::runtime::get()->create_application("ovf-i4-native-peer");
  assert(peer.application && peer.application->init());
  peer.application->register_state_handler(
      [&peer](vsomeip::state_type_e state) { peer.OnState(state); });
  peer.application->register_availability_handler(
      kService, kInstance,
      [&peer](vsomeip::service_t service, vsomeip::instance_t instance, bool available) {
        peer.OnAvailability(service, instance, available);
      });
  peer.application->register_message_handler(
      kService, kInstance, vsomeip::ANY_METHOD,
      [&peer](std::shared_ptr<vsomeip::message> const& message) { peer.OnMessage(message); });
  peer.application->register_message_handler(
      kPeerService, kPeerInstance, vsomeip::ANY_METHOD,
      [&peer](std::shared_ptr<vsomeip::message> const& message) { peer.OnMessage(message); });
  std::thread peer_thread([&peer] { peer.application->start(); });

  {
    std::unique_lock lock(peer.mutex);
    assert(peer.condition.wait_for(lock, std::chrono::seconds(10),
                                   [&peer] { return peer.available; }));
  }
  std::uint8_t sample[]{8, 9};
  assert(provider->publish(provider, event_endpoint, {sample, sizeof(sample)}) ==
         OVF_COM_STATUS_OK);
  {
    std::unique_lock lock(peer.mutex);
    assert(peer.condition.wait_for(lock, std::chrono::seconds(10),
                                   [&peer] { return peer.response && peer.event; }));
  }
  assert(server.requests == 1);

  constexpr std::string_view silent_mapping =
      "service=4661;instance=2;element=2;eventGroup=0;major=1;minor=0;"
      "kind=method;reliable=true";
  constexpr std::string_view error_mapping =
      "service=4661;instance=2;element=3;eventGroup=0;major=1;minor=0;"
      "kind=method;reliable=true";
  auto silent = Descriptor(OVF_COM_ENDPOINT_METHOD_CLIENT, Id(5), silent_mapping);
  auto error = Descriptor(OVF_COM_ENDPOINT_METHOD_CLIENT, Id(6), error_mapping);
  ovf_com_handle_v1 silent_client{}, duplicate_client{}, error_client{};
  assert(provider->endpoint_create(provider, &silent, &silent_client) == OVF_COM_STATUS_OK);
  assert(provider->endpoint_create(provider, &silent, &duplicate_client) == OVF_COM_STATUS_OK);
  assert(provider->endpoint_create(provider, &error, &error_client) == OVF_COM_STATUS_OK);

  CompletionState completion;
  std::uint8_t request_payload[]{7};
  ovf_com_handle_v1 operation{};
  assert(provider->request(provider, silent_client, {request_payload, sizeof(request_payload)},
                           Now(nullptr) + 100'000'000ULL, &OnCompletion, &completion,
                           &operation) == OVF_COM_STATUS_OK);
  WaitFor(completion, OVF_COM_STATUS_DEADLINE_EXCEEDED);
  assert(provider->endpoint_destroy(provider, silent_client) == OVF_COM_STATUS_OK);

  Reset(completion);
  assert(provider->request(provider, duplicate_client, {request_payload, sizeof(request_payload)},
                           Now(nullptr) + 2'000'000'000ULL, &OnCompletion, &completion,
                           &operation) == OVF_COM_STATUS_OK);
  assert(provider->cancel(provider, operation) == OVF_COM_STATUS_OK);
  WaitFor(completion, OVF_COM_STATUS_CANCELLED);

  Reset(completion);
  assert(provider->request(provider, error_client, {request_payload, sizeof(request_payload)},
                           Now(nullptr) + 2'000'000'000ULL, &OnCompletion, &completion,
                           &operation) == OVF_COM_STATUS_OK);
  WaitFor(completion, OVF_COM_STATUS_APPLICATION_ERROR);
  assert((completion.payload == std::vector<std::uint8_t>{0x42}));
  assert(provider->endpoint_destroy(provider, error_client) == OVF_COM_STATUS_OK);
  assert(provider->endpoint_destroy(provider, duplicate_client) == OVF_COM_STATUS_OK);

  peer.application->clear_all_handler();
  peer.application->stop();
  peer_thread.join();
  assert(provider->set_request_handler(provider, method_endpoint, nullptr, nullptr) ==
         OVF_COM_STATUS_OK);
  assert(provider->endpoint_destroy(provider, event_endpoint) == OVF_COM_STATUS_OK);
  assert(provider->endpoint_destroy(provider, method_endpoint) == OVF_COM_STATUS_OK);
  assert(provider->stop(provider) == OVF_COM_STATUS_OK);
  factory->destroy(provider);
}
