// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr char kMapping[] = "pattern=pubsub;service=ovf/test/radar/objects;type=OvfRadarObjects;"
                            "payloadSize=64;alignment=8;history=1;subscriberBuffer=8;"
                            "maxPublishers=2;maxSubscribers=8;maxLoanedSamples=2;"
                            "maxBorrowedSamples=2;safeOverflow=false";
constexpr char kMethodMapping[] =
    "pattern=requestResponse;service=ovf/test/radar/calibrate;"
    "requestType=OvfCalibrateRequest;responseType=OvfCalibrateResponse;"
    "requestPayloadSize=64;responsePayloadSize=64;alignment=8;"
    "requestBuffer=8;responseBuffer=8;maxClients=8;maxServers=1;maxLoanedRequests=2;"
    "maxBorrowedResponses=2;maxLoanedResponses=1;safeOverflow=false";
constexpr char kServiceMapping[] = "ovf/test/radar";

struct Context {
  ovf_com_transport_v1* transport{};
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::uint8_t, 64> received{};
  bool delivered{};
  bool native_loan{};
  bool discovered{};
  bool completed{};
  ovf_com_status_v1 completion_status{OVF_COM_STATUS_TRANSPORT_ERROR};
  std::vector<std::uint8_t> response;
};
void Log(void*, ovf_com_log_level_v1, ovf_com_string_view_v1) {}
auto Dispatch(void*, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release, void* user)
    -> ovf_com_status_v1 {
  task(user);
  release(user);
  return OVF_COM_STATUS_OK;
}
auto Clock(void*) -> std::uint64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void Sample(void* user, ovf_com_sample_v1 const* sample) {
  auto& context = *static_cast<Context*>(user);
  assert(sample);
  assert(sample->payload.size == context.received.size());
  {
    std::lock_guard lock(context.mutex);
    std::memcpy(context.received.data(), sample->payload.data, sample->payload.size);
    context.native_loan = sample->provider_loan != OVF_COM_INVALID_HANDLE_V1;
    context.delivered = true;
  }
  assert(context.transport->loan_release(context.transport, sample->provider_loan) ==
         OVF_COM_STATUS_OK);
  context.changed.notify_all();
}
void Discovery(void* user, ovf_com_discovery_entry_v1 const* entry) {
  auto& context = *static_cast<Context*>(user);
  {
    std::lock_guard lock(context.mutex);
    context.discovered = entry && entry->available;
  }
  context.changed.notify_all();
}
void Completion(void* user, ovf_com_handle_v1, ovf_com_status_v1 status,
                ovf_com_bytes_view_v1 payload) {
  auto& context = *static_cast<Context*>(user);
  {
    std::lock_guard lock(context.mutex);
    context.completion_status = status;
    context.response.assign(payload.data, payload.data + payload.size);
    context.completed = true;
  }
  context.changed.notify_all();
}
void Method(void* user, ovf_com_handle_v1 request, ovf_com_bytes_view_v1 payload,
            std::uint64_t deadline) {
  auto& context = *static_cast<Context*>(user);
  assert(deadline != 0);
  std::vector<std::uint8_t> response(payload.data, payload.data + payload.size);
  std::reverse(response.begin(), response.end());
  assert(context.transport->respond(context.transport, request, OVF_COM_STATUS_OK,
                                    {response.data(), response.size()}) == OVF_COM_STATUS_OK);
  {
    std::lock_guard lock(context.mutex);
    context.completed = true;
  }
  context.changed.notify_all();
}
auto Descriptor(ovf_com_endpoint_kind_v1 kind) -> ovf_com_endpoint_descriptor_v1 {
  ovf_com_endpoint_descriptor_v1 descriptor{};
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = kind;
  descriptor.service_id.bytes[15] = 1;
  descriptor.instance_id.bytes[15] = 2;
  descriptor.element_id.bytes[15] = 3;
  descriptor.route_epoch = 7;
  descriptor.max_payload_size = 64;
  descriptor.history_depth = 1;
  descriptor.required_features = OVF_COM_CAP_EVENTS | OVF_COM_CAP_LOANS;
  descriptor.native_mapping = {kMapping, sizeof(kMapping) - 1};
  return descriptor;
}
auto MethodDescriptor(ovf_com_endpoint_kind_v1 kind) -> ovf_com_endpoint_descriptor_v1 {
  auto descriptor = Descriptor(kind);
  descriptor.element_id.bytes[15] = 4;
  descriptor.required_features =
      OVF_COM_CAP_METHODS | OVF_COM_CAP_DEADLINES | OVF_COM_CAP_CANCELLATION;
  descriptor.native_mapping = {kMethodMapping, sizeof(kMethodMapping) - 1};
  return descriptor;
}
auto CrashWithOutstandingLoan() -> int {
  auto factory = ovf_com_iceoryx2_transport_query_v1();
  ovf_com_host_api_v1 host{sizeof(host), nullptr, Log, Dispatch, Clock};
  constexpr char instance[] = "crashing-peer";
  ovf_com_transport_config_v1 config{
      sizeof(config), {instance, sizeof(instance) - 1}, {nullptr, 0}, 8, 8};
  ovf_com_transport_v1* transport{};
  if (factory->create(&host, &config, &transport) != OVF_COM_STATUS_OK ||
      transport->start(transport) != OVF_COM_STATUS_OK)
    return 2;
  auto descriptor = Descriptor(OVF_COM_ENDPOINT_EVENT_PUBLISHER);
  ovf_com_handle_v1 publisher{};
  ovf_com_loan_v1 loan{};
  loan.struct_size = sizeof(loan);
  if (transport->endpoint_create(transport, &descriptor, &publisher) != OVF_COM_STATUS_OK ||
      transport->loan_acquire(transport, publisher, 64, &loan) != OVF_COM_STATUS_OK)
    return 3;
  loan.bytes.data[0] = 0xa5;
  _Exit(0);
}
auto PublishPattern() -> int {
  auto factory = ovf_com_iceoryx2_transport_query_v1();
  ovf_com_host_api_v1 host{sizeof(host), nullptr, Log, Dispatch, Clock};
  constexpr char instance[] = "publishing-peer";
  ovf_com_transport_config_v1 config{
      sizeof(config), {instance, sizeof(instance) - 1}, {nullptr, 0}, 8, 8};
  ovf_com_transport_v1* transport{};
  if (factory->create(&host, &config, &transport) != OVF_COM_STATUS_OK ||
      transport->start(transport) != OVF_COM_STATUS_OK)
    return 4;
  auto descriptor = Descriptor(OVF_COM_ENDPOINT_EVENT_PUBLISHER);
  ovf_com_handle_v1 publisher{};
  ovf_com_loan_v1 loan{};
  loan.struct_size = sizeof(loan);
  if (transport->endpoint_create(transport, &descriptor, &publisher) != OVF_COM_STATUS_OK ||
      transport->loan_acquire(transport, publisher, 64, &loan) != OVF_COM_STATUS_OK)
    return 5;
  for (std::size_t i = 0; i < loan.bytes.size; ++i)
    loan.bytes.data[i] = static_cast<std::uint8_t>(i ^ 0x5aU);
  if (transport->loan_publish(transport, publisher, loan.handle, 64) != OVF_COM_STATUS_OK)
    return 6;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (transport->endpoint_destroy(transport, publisher) != OVF_COM_STATUS_OK ||
      transport->stop(transport) != OVF_COM_STATUS_OK)
    return 7;
  factory->destroy(transport);
  return 0;
}
auto ServeMethod() -> int {
  auto factory = ovf_com_iceoryx2_transport_query_v1();
  ovf_com_host_api_v1 host{sizeof(host), nullptr, Log, Dispatch, Clock};
  constexpr char instance[] = "method-server";
  ovf_com_transport_config_v1 config{
      sizeof(config), {instance, sizeof(instance) - 1}, {nullptr, 0}, 8, 8};
  ovf_com_transport_v1* transport{};
  if (factory->create(&host, &config, &transport) != OVF_COM_STATUS_OK ||
      transport->start(transport) != OVF_COM_STATUS_OK)
    return 8;
  Context context{};
  context.transport = transport;
  auto descriptor = MethodDescriptor(OVF_COM_ENDPOINT_METHOD_SERVER);
  ovf_com_handle_v1 server{};
  if (transport->endpoint_create(transport, &descriptor, &server) != OVF_COM_STATUS_OK ||
      transport->set_request_handler(transport, server, Method, &context) != OVF_COM_STATUS_OK)
    return 9;
  {
    std::unique_lock lock(context.mutex);
    if (!context.changed.wait_for(lock, std::chrono::seconds(5),
                                  [&context] { return context.completed; }))
      return 10;
  }
  if (transport->endpoint_destroy(transport, server) != OVF_COM_STATUS_OK ||
      transport->stop(transport) != OVF_COM_STATUS_OK)
    return 11;
  factory->destroy(transport);
  return 0;
}
} // namespace

extern char** environ;

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--crash-with-loan")
    return CrashWithOutstandingLoan();
  if (argc == 2 && std::string_view(argv[1]) == "--publish-pattern")
    return PublishPattern();
  if (argc == 2 && std::string_view(argv[1]) == "--serve-method")
    return ServeMethod();
  pid_t child{};
  char mode[] = "--crash-with-loan";
  char* child_argv[]{argv[0], mode, nullptr};
  assert(posix_spawn(&child, argv[0], nullptr, nullptr, child_argv, environ) == 0);
  int child_status{};
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

  auto factory = ovf_com_iceoryx2_transport_query_v1();
  assert(factory && factory->abi_version == OVF_COM_TRANSPORT_ABI_VERSION_1);
  ovf_com_host_api_v1 host{sizeof(host), nullptr, Log, Dispatch, Clock};
  constexpr char instance[] = "native-peer";
  ovf_com_transport_config_v1 config{
      sizeof(config), {instance, sizeof(instance) - 1}, {nullptr, 0}, 16, 16};
  ovf_com_transport_v1* transport{};
  assert(factory->create(&host, &config, &transport) == OVF_COM_STATUS_OK);
  assert(transport->start(transport) == OVF_COM_STATUS_OK);
  Context context{};
  context.transport = transport;
  auto subscriber_descriptor = Descriptor(OVF_COM_ENDPOINT_EVENT_SUBSCRIBER);
  ovf_com_handle_v1 subscriber{}, subscription{};
  assert(transport->endpoint_create(transport, &subscriber_descriptor, &subscriber) ==
         OVF_COM_STATUS_OK);
  assert(transport->subscribe(transport, subscriber, Sample, &context, &subscription) ==
         OVF_COM_STATUS_OK);

  char publish_mode[] = "--publish-pattern";
  char* publisher_argv[]{argv[0], publish_mode, nullptr};
  assert(posix_spawn(&child, argv[0], nullptr, nullptr, publisher_argv, environ) == 0);
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

  {
    std::unique_lock lock(context.mutex);
    assert(context.changed.wait_for(lock, std::chrono::seconds(3),
                                    [&context] { return context.delivered; }));
    assert(context.native_loan);
    for (std::size_t i = 0; i < context.received.size(); ++i)
      assert(context.received[i] == static_cast<std::uint8_t>(i ^ 0x5aU));
  }
  assert(transport->unsubscribe(transport, subscription) == OVF_COM_STATUS_OK);
  assert(transport->endpoint_destroy(transport, subscriber) == OVF_COM_STATUS_OK);

  ovf_com_discovery_filter_v1 filter{sizeof(filter),
                                     Descriptor(OVF_COM_ENDPOINT_EVENT_SUBSCRIBER).service_id,
                                     {kServiceMapping, sizeof(kServiceMapping) - 1}};
  ovf_com_handle_v1 watch{};
  assert(transport->watch_start(transport, &filter, Discovery, &context, &watch) ==
         OVF_COM_STATUS_OK);
  char method_mode[] = "--serve-method";
  char* method_argv[]{argv[0], method_mode, nullptr};
  assert(posix_spawn(&child, argv[0], nullptr, nullptr, method_argv, environ) == 0);
  {
    std::unique_lock lock(context.mutex);
    assert(context.changed.wait_for(lock, std::chrono::seconds(3),
                                    [&context] { return context.discovered; }));
  }
  auto client_descriptor = MethodDescriptor(OVF_COM_ENDPOINT_METHOD_CLIENT);
  ovf_com_handle_v1 client{}, operation{};
  assert(transport->endpoint_create(transport, &client_descriptor, &client) == OVF_COM_STATUS_OK);
  std::array<std::uint8_t, 5> request{1, 2, 3, 4, 5};
  auto deadline = Clock(nullptr) + 2'000'000'000ULL;
  assert(transport->request(transport, client, {request.data(), request.size()}, deadline,
                            Completion, &context, &operation) == OVF_COM_STATUS_OK);
  {
    std::unique_lock lock(context.mutex);
    assert(context.changed.wait_for(lock, std::chrono::seconds(3),
                                    [&context] { return context.completed; }));
    assert(context.completion_status == OVF_COM_STATUS_OK);
    assert((context.response == std::vector<std::uint8_t>{5, 4, 3, 2, 1}));
  }
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  {
    std::lock_guard lock(context.mutex);
    context.completed = false;
  }
  assert(transport->request(transport, client, {request.data(), request.size()},
                            Clock(nullptr) + 2'000'000'000ULL, Completion, &context,
                            &operation) == OVF_COM_STATUS_OK);
  assert(transport->cancel(transport, operation) == OVF_COM_STATUS_OK);
  {
    std::unique_lock lock(context.mutex);
    assert(context.changed.wait_for(lock, std::chrono::seconds(1),
                                    [&context] { return context.completed; }));
    assert(context.completion_status == OVF_COM_STATUS_CANCELLED);
    context.completed = false;
  }
  assert(transport->request(transport, client, {request.data(), request.size()},
                            Clock(nullptr) + 50'000'000ULL, Completion, &context,
                            &operation) == OVF_COM_STATUS_OK);
  {
    std::unique_lock lock(context.mutex);
    assert(context.changed.wait_for(lock, std::chrono::seconds(1),
                                    [&context] { return context.completed; }));
    assert(context.completion_status == OVF_COM_STATUS_DEADLINE_EXCEEDED);
  }
  assert(transport->endpoint_destroy(transport, client) == OVF_COM_STATUS_OK);
  assert(transport->watch_stop(transport, watch) == OVF_COM_STATUS_OK);
  assert(transport->stop(transport) == OVF_COM_STATUS_OK);
  factory->destroy(transport);
}
