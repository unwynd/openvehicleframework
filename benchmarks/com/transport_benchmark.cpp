// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transport_abi.h"

#if defined(OVF_BENCHMARK_ICEORYX2)
#include "ovf/com/transports/iceoryx2.h"
#elif defined(OVF_BENCHMARK_VSOMEIP)
#include "ovf/com/transports/vsomeip.h"
#else
#error "A benchmark transport must be selected"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kPayloadSize = 256;
constexpr std::size_t kWarmupIterations = 100;
constexpr std::size_t kLatencyIterations = 500;
constexpr std::size_t kThroughputIterations = 2000;
constexpr std::size_t kWindow = 32;

#if defined(OVF_BENCHMARK_ICEORYX2)
constexpr std::string_view kProvider = "iceoryx2";
constexpr std::string_view kMethodMapping =
    "pattern=requestResponse;service=ovf/benchmark/echo;requestType=OvfBenchmarkRequest;"
    "responseType=OvfBenchmarkResponse;requestPayloadSize=256;responsePayloadSize=256;"
    "alignment=8;requestBuffer=64;responseBuffer=64;maxClients=2;maxServers=1;"
    "maxLoanedRequests=32;maxBorrowedResponses=32;maxLoanedResponses=32;safeOverflow=false";
constexpr std::string_view kEventMapping =
    "pattern=pubsub;service=ovf/benchmark/event;type=OvfBenchmarkEvent;payloadSize=256;"
    "alignment=8;history=0;subscriberBuffer=4096;maxPublishers=1;maxSubscribers=2;"
    "maxLoanedSamples=32;maxBorrowedSamples=64;safeOverflow=false";
#else
constexpr std::string_view kProvider = "vsomeip";
constexpr std::string_view kMethodMapping =
    "service=28672;instance=1;element=1;eventGroup=0;major=1;minor=0;kind=method;"
    "reliable=true";
constexpr std::string_view kEventMapping =
    "service=28672;instance=1;element=32769;eventGroup=1;major=1;minor=0;kind=event;"
    "reliable=true";
#endif

auto Factory() -> ovf_com_transport_factory_v1 const* {
#if defined(OVF_BENCHMARK_ICEORYX2)
  return ovf_com_iceoryx2_transport_query_v1();
#else
  return ovf_com_vsomeip_transport_query_v1();
#endif
}

auto Now(void*) -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

auto Dispatch(void*, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release, void* user)
    -> ovf_com_status_v1 {
  task(user);
  release(user);
  return OVF_COM_STATUS_OK;
}

auto Id(std::uint8_t value) -> ovf_com_uuid_v1 {
  ovf_com_uuid_v1 result{};
  result.bytes[15] = value;
  return result;
}

auto Descriptor(ovf_com_endpoint_kind_v1 kind, ovf_com_uuid_v1 element, std::string_view mapping,
                ovf_com_element_operation_v1 operation) -> ovf_com_endpoint_descriptor_v1 {
  return {sizeof(ovf_com_endpoint_descriptor_v1),
          kind,
          Id(1),
          Id(2),
          element,
          1,
          kPayloadSize,
          1,
          static_cast<std::uint64_t>(
              kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER || kind == OVF_COM_ENDPOINT_EVENT_SUBSCRIBER
                  ? OVF_COM_CAP_EVENTS | OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED
                  : OVF_COM_CAP_METHODS | OVF_COM_CAP_RELIABLE | OVF_COM_CAP_DEADLINES |
                        OVF_COM_CAP_CANCELLATION),
          {mapping.data(), mapping.size()},
          operation};
}

struct Transport final {
  ovf_com_transport_factory_v1 const* factory{};
  ovf_com_transport_v1* api{};

  explicit Transport(std::string_view name) : factory(Factory()) {
    static ovf_com_host_api_v1 host{sizeof(host), nullptr, nullptr, Dispatch, Now};
    ovf_com_transport_config_v1 config{
        sizeof(config),  {name.data(), name.size()}, {nullptr, 0}, 16, 512, 5'000'000'000ULL,
        5'000'000'000ULL};
    if (!factory || factory->create(&host, &config, &api) != OVF_COM_STATUS_OK || !api ||
        api->start(api) != OVF_COM_STATUS_OK)
      api = nullptr;
  }
  ~Transport() {
    if (api) {
      (void)api->stop(api);
      factory->destroy(api);
    }
  }
  Transport(Transport const&) = delete;
  Transport& operator=(Transport const&) = delete;
};

void Echo(void* user, ovf_com_handle_v1 request, ovf_com_bytes_view_v1 payload, std::uint64_t) {
  auto& transport = *static_cast<ovf_com_transport_v1*>(user);
  if (payload.size != kPayloadSize ||
      transport.respond(&transport, request, OVF_COM_STATUS_OK, payload) != OVF_COM_STATUS_OK)
    std::abort();
}

struct Completion final {
  std::mutex mutex;
  std::condition_variable changed;
  bool done{};
  bool valid{};
};

void Complete(void* user, ovf_com_handle_v1, ovf_com_status_v1 status,
              ovf_com_bytes_view_v1 payload) {
  auto& completion = *static_cast<Completion*>(user);
  {
    std::lock_guard lock(completion.mutex);
    completion.valid = status == OVF_COM_STATUS_OK && payload.size == kPayloadSize;
    completion.done = true;
  }
  completion.changed.notify_one();
}

auto Wait(Completion& completion, std::chrono::seconds timeout = std::chrono::seconds(5)) -> bool {
  std::unique_lock lock(completion.mutex);
  return completion.changed.wait_for(lock, timeout, [&] { return completion.done; }) &&
         completion.valid;
}

struct Subscription final {
  ovf_com_transport_v1* transport{};
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t received{};
  bool active{};
  bool valid{true};
};

void OnState(void* user, ovf_com_handle_v1, ovf_com_subscription_state_v1 state,
             ovf_com_status_v1 reason) {
  auto& subscription = *static_cast<Subscription*>(user);
  {
    std::lock_guard lock(subscription.mutex);
    subscription.active = state == OVF_COM_SUBSCRIPTION_ACTIVE && reason == OVF_COM_STATUS_OK;
  }
  subscription.changed.notify_all();
}

void OnSample(void* user, ovf_com_sample_v1 const* sample) {
  auto& subscription = *static_cast<Subscription*>(user);
  {
    std::lock_guard lock(subscription.mutex);
    if (!sample || sample->payload.size != kPayloadSize) {
      subscription.valid = false;
    } else {
      ++subscription.received;
    }
  }
  if (sample && sample->provider_loan != OVF_COM_INVALID_HANDLE_V1 &&
      subscription.transport->loan_release(subscription.transport, sample->provider_loan) !=
          OVF_COM_STATUS_OK)
    std::abort();
  subscription.changed.notify_all();
}

auto Percentile(std::vector<double> values, double quantile) -> double {
  std::sort(values.begin(), values.end());
  auto index = static_cast<std::size_t>(quantile * static_cast<double>(values.size() - 1));
  return values[index];
}

auto Invoke(ovf_com_transport_v1& client, ovf_com_handle_v1 endpoint,
            std::array<std::uint8_t, kPayloadSize> const& payload, Completion& completion) -> bool {
  ovf_com_handle_v1 operation{};
  auto deadline = Now(nullptr) + 5'000'000'000ULL;
  return client.request(&client, endpoint, {payload.data(), payload.size()}, deadline, Complete,
                        &completion, &operation) == OVF_COM_STATUS_OK &&
         Wait(completion);
}

auto Run() -> int {
  std::cerr << kProvider << ": starting transports\n";
  Transport server("ovf-benchmark-server");
  Transport client("ovf-benchmark-client");
  if (!server.api || !client.api)
    return 2;

  auto method_server_descriptor =
      Descriptor(OVF_COM_ENDPOINT_METHOD_SERVER, Id(3), kMethodMapping, OVF_COM_OPERATION_METHOD);
  auto method_client_descriptor =
      Descriptor(OVF_COM_ENDPOINT_METHOD_CLIENT, Id(3), kMethodMapping, OVF_COM_OPERATION_METHOD);
  auto event_publisher_descriptor =
      Descriptor(OVF_COM_ENDPOINT_EVENT_PUBLISHER, Id(4), kEventMapping, OVF_COM_OPERATION_EVENT);
  auto event_subscriber_descriptor =
      Descriptor(OVF_COM_ENDPOINT_EVENT_SUBSCRIBER, Id(4), kEventMapping, OVF_COM_OPERATION_EVENT);
  ovf_com_handle_v1 method_server{}, method_client{}, event_publisher{}, event_subscriber{};
  if (server.api->endpoint_create(server.api, &method_server_descriptor, &method_server) !=
          OVF_COM_STATUS_OK ||
      server.api->set_request_handler(server.api, method_server, Echo, server.api) !=
          OVF_COM_STATUS_OK ||
      server.api->endpoint_create(server.api, &event_publisher_descriptor, &event_publisher) !=
          OVF_COM_STATUS_OK ||
      client.api->endpoint_create(client.api, &method_client_descriptor, &method_client) !=
          OVF_COM_STATUS_OK ||
      client.api->endpoint_create(client.api, &event_subscriber_descriptor, &event_subscriber) !=
          OVF_COM_STATUS_OK)
    return 3;

  Subscription subscription;
  subscription.transport = client.api;
  ovf_com_handle_v1 subscription_handle{};
  if (client.api->subscribe(client.api, event_subscriber, OnSample, &subscription,
                            &subscription_handle) != OVF_COM_STATUS_OK ||
      client.api->subscription_set_state_handler(client.api, subscription_handle, OnState,
                                                 &subscription) != OVF_COM_STATUS_OK)
    return 4;
  {
    std::unique_lock lock(subscription.mutex);
    if (!subscription.changed.wait_for(lock, std::chrono::seconds(10),
                                       [&] { return subscription.active; }))
      return 5;
  }
  std::cerr << kProvider << ": subscription active\n";

  std::array<std::uint8_t, kPayloadSize> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<std::uint8_t>(index);
  for (std::size_t index = 0; index < kWarmupIterations; ++index) {
    Completion completion;
    if (!Invoke(*client.api, method_client, payload, completion))
      return 6;
  }
  std::cerr << kProvider << ": warmup complete\n";

  std::vector<double> latency_us;
  latency_us.reserve(kLatencyIterations);
  for (std::size_t index = 0; index < kLatencyIterations; ++index) {
    Completion completion;
    auto begin = Clock::now();
    if (!Invoke(*client.api, method_client, payload, completion))
      return 7;
    latency_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  std::cerr << kProvider << ": latency complete\n";

  auto throughput_begin = Clock::now();
  for (std::size_t offset = 0; offset < kThroughputIterations; offset += kWindow) {
    auto count = std::min(kWindow, kThroughputIterations - offset);
    std::vector<std::unique_ptr<Completion>> completions;
    completions.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      completions.push_back(std::make_unique<Completion>());
      ovf_com_handle_v1 operation{};
      if (client.api->request(client.api, method_client, {payload.data(), payload.size()},
                              Now(nullptr) + 5'000'000'000ULL, Complete, completions.back().get(),
                              &operation) != OVF_COM_STATUS_OK)
        return 8;
    }
    for (auto& completion : completions)
      if (!Wait(*completion))
        return 9;
  }
  auto throughput_seconds = std::chrono::duration<double>(Clock::now() - throughput_begin).count();
  std::cerr << kProvider << ": request throughput complete\n";

  auto event_begin = Clock::now();
  for (std::size_t index = 0; index < kThroughputIterations; ++index) {
    std::memcpy(payload.data(), &index, sizeof(index));
    if (server.api->publish(server.api, event_publisher, {payload.data(), payload.size()}) !=
        OVF_COM_STATUS_OK)
      return 10;
  }
  {
    std::unique_lock lock(subscription.mutex);
    if (!subscription.changed.wait_for(lock, std::chrono::seconds(10), [&] {
          return subscription.received >= kThroughputIterations;
        }))
      return 11;
  }
  auto event_seconds = std::chrono::duration<double>(Clock::now() - event_begin).count();
  std::cerr << kProvider << ": event throughput complete\n";
  {
    std::lock_guard lock(subscription.mutex);
    if (!subscription.valid || subscription.received != kThroughputIterations)
      return 12;
  }

  auto mean = std::accumulate(latency_us.begin(), latency_us.end(), 0.0) /
              static_cast<double>(latency_us.size());
  std::cout << std::fixed << std::setprecision(3) << "{\"transport\":\"" << kProvider
            << "\",\"payloadBytes\":" << kPayloadSize
            << ",\"latencyIterations\":" << kLatencyIterations
            << ",\"latencyUs\":{\"mean\":" << mean << ",\"p50\":" << Percentile(latency_us, 0.50)
            << ",\"p95\":" << Percentile(latency_us, 0.95)
            << ",\"p99\":" << Percentile(latency_us, 0.99) << "},\"requestThroughputOpsPerSec\":"
            << static_cast<double>(kThroughputIterations) / throughput_seconds
            << ",\"eventThroughputSamplesPerSec\":"
            << static_cast<double>(kThroughputIterations) / event_seconds
            << ",\"deliveredSamples\":" << kThroughputIterations << "}\n";

  (void)client.api->unsubscribe(client.api, subscription_handle);
  (void)client.api->endpoint_destroy(client.api, event_subscriber);
  (void)client.api->endpoint_destroy(client.api, method_client);
  (void)server.api->set_request_handler(server.api, method_server, nullptr, nullptr);
  (void)server.api->endpoint_destroy(server.api, event_publisher);
  (void)server.api->endpoint_destroy(server.api, method_server);
  return 0;
}
} // namespace

int main() { return Run(); }
