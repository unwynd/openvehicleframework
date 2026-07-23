// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transports/iceoryx2.h"

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

namespace {
constexpr char kMapping[] = "service=ovf/test/radar/objects;type=OvfRadarObjects;"
                            "payloadSize=64;alignment=8;history=1;subscriberBuffer=8;"
                            "maxPublishers=2;maxSubscribers=8;safeOverflow=false";

struct Context {
  ovf_com_transport_v1* transport{};
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::uint8_t, 64> received{};
  bool delivered{};
  bool native_loan{};
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
} // namespace

extern char** environ;

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--crash-with-loan")
    return CrashWithOutstandingLoan();
  if (argc == 2 && std::string_view(argv[1]) == "--publish-pattern")
    return PublishPattern();
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
  assert(transport->stop(transport) == OVF_COM_STATUS_OK);
  factory->destroy(transport);
}
