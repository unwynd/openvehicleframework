// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/transport_abi.h"
#include "ovf/com/transports/inproc.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
struct DeferredTask {
  ovf_com_task_fn_v1 task{};
  ovf_com_task_release_fn_v1 release{};
  void* user{};
};
bool defer_dispatch{};
bool reject_dispatch{};
std::vector<DeferredTask> deferred_tasks;
ovf_com_status_v1 Dispatch(void*, ovf_com_task_fn_v1 task, ovf_com_task_release_fn_v1 release,
                           void* user) {
  if (defer_dispatch) {
    deferred_tasks.push_back({task, release, user});
    return OVF_COM_STATUS_OK;
  }
  if (reject_dispatch)
    return OVF_COM_STATUS_RESOURCE_EXHAUSTED;
  task(user);
  release(user);
  return OVF_COM_STATUS_OK;
}
void RunDeferred() {
  auto tasks = std::move(deferred_tasks);
  for (auto const& item : tasks) {
    item.task(item.user);
    item.release(item.user);
  }
}
std::uint64_t Now(void*) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}
ovf_com_uuid_v1 Id(std::uint8_t value) {
  ovf_com_uuid_v1 id{};
  id.bytes[15] = value;
  return id;
}
ovf_com_endpoint_descriptor_v1 Endpoint(ovf_com_endpoint_kind_v1 kind) {
  return {sizeof(ovf_com_endpoint_descriptor_v1),
          kind,
          Id(1),
          Id(2),
          Id(3),
          7,
          64,
          4,
          OVF_COM_CAP_RELIABLE | OVF_COM_CAP_ORDERED,
          {nullptr, 0},
          kind == OVF_COM_ENDPOINT_EVENT_PUBLISHER || kind == OVF_COM_ENDPOINT_EVENT_SUBSCRIBER
              ? OVF_COM_OPERATION_EVENT
              : OVF_COM_OPERATION_METHOD};
}
struct State {
  int discoveries{};
  int withdrawals{};
  int samples{};
  int requests{};
  int completions{};
  int cancellations{};
  int shutdowns{};
  int subscription_active{};
  int diagnostics{};
  std::array<std::uint8_t, 8> last{};
  std::size_t last_size{};
  ovf_com_transport_v1* transport{};
};
void Diagnostic(void* user, ovf_com_diagnostic_v1 const* diagnostic) {
  auto& state = *static_cast<State*>(user);
  assert(diagnostic && diagnostic->status == OVF_COM_STATUS_RESOURCE_EXHAUSTED);
  ++state.diagnostics;
}
void SubscriptionState(void* user, ovf_com_handle_v1, ovf_com_subscription_state_v1 state,
                       ovf_com_status_v1 reason) {
  auto& context = *static_cast<State*>(user);
  assert(reason == OVF_COM_STATUS_OK);
  if (state == OVF_COM_SUBSCRIPTION_ACTIVE)
    ++context.subscription_active;
}
void Discovery(void* user, const ovf_com_discovery_entry_v1* entry) {
  auto& state = *static_cast<State*>(user);
  assert(entry && entry->available <= 1 && entry->route_epoch == 7);
  if (entry->available)
    ++state.discoveries;
  else
    ++state.withdrawals;
}
void Sample(void* user, const ovf_com_sample_v1* sample) {
  auto& state = *static_cast<State*>(user);
  assert(sample && sample->route_epoch == 7 && sample->payload.size <= state.last.size());
  std::memcpy(state.last.data(), sample->payload.data, sample->payload.size);
  state.last_size = sample->payload.size;
  ++state.samples;
}
void Completion(void* user, ovf_com_handle_v1, ovf_com_status_v1 status,
                ovf_com_bytes_view_v1 payload) {
  auto& state = *static_cast<State*>(user);
  if (status == OVF_COM_STATUS_CANCELLED)
    ++state.cancellations;
  else if (status == OVF_COM_STATUS_SHUTTING_DOWN)
    ++state.shutdowns;
  else {
    assert(status == OVF_COM_STATUS_OK);
    assert(payload.size == 2 && payload.data[0] == 9 && payload.data[1] == 8);
    ++state.completions;
  }
}
void Request(void* user, ovf_com_handle_v1 request, ovf_com_bytes_view_v1 payload,
             std::uint64_t deadline) {
  auto& state = *static_cast<State*>(user);
  assert(payload.size == 3 && deadline > 0);
  ++state.requests;
  const std::uint8_t response[]{9, 8};
  assert(state.transport->respond(state.transport, request, OVF_COM_STATUS_OK,
                                  {response, sizeof(response)}) == OVF_COM_STATUS_OK);
}
void NoResponse(void* user, ovf_com_handle_v1, ovf_com_bytes_view_v1, std::uint64_t) {
  ++static_cast<State*>(user)->requests;
}
} // namespace

int main() {
  ovf_com_host_api_v1 host{sizeof(host), nullptr, nullptr, &Dispatch, &Now};
  ovf_com_transport_config_v1 config{
      sizeof(config), {"conformance", 11}, {nullptr, 0}, 16, 8, 0, 0};
  auto const* factory = ovf_com_inproc_transport_query_v1();
  assert(factory && factory->abi_version == OVF_COM_TRANSPORT_ABI_VERSION_1);
  ovf_com_transport_v1* transport{};
  assert(factory->create(&host, &config, &transport) == OVF_COM_STATUS_OK);
  assert(transport && transport->struct_size >= sizeof(*transport));

  ovf_com_capabilities_v1 capabilities{};
  capabilities.struct_size = sizeof(capabilities);
  assert(transport->get_capabilities(transport, &capabilities) == OVF_COM_STATUS_OK);
  assert((capabilities.feature_bits & OVF_COM_CAP_LOANS) != 0);
  assert((capabilities.feature_bits & OVF_COM_CAP_HEALTH) != 0);
  assert((capabilities.feature_bits & OVF_COM_CAP_DIAGNOSTICS) != 0);
  assert(capabilities.isolation == OVF_COM_ISOLATION_INDEPENDENT);
  assert(transport->start(transport) == OVF_COM_STATUS_OK);
  ovf_com_health_v1 health{};
  health.struct_size = sizeof(health);
  assert(transport->get_health(transport, &health) == OVF_COM_STATUS_OK);
  assert(health.state == OVF_COM_HEALTH_READY);

  State state{};
  state.transport = transport;
  assert(transport->set_diagnostic_handler(transport, &Diagnostic, &state) == OVF_COM_STATUS_OK);
  ovf_com_discovery_filter_v1 filter{sizeof(filter), Id(1), {nullptr, 0}};
  ovf_com_handle_v1 watch{};
  assert(transport->watch_start(transport, &filter, &Discovery, &state, &watch) ==
         OVF_COM_STATUS_OK);

  auto publisher_desc = Endpoint(OVF_COM_ENDPOINT_EVENT_PUBLISHER);
  auto subscriber_desc = Endpoint(OVF_COM_ENDPOINT_EVENT_SUBSCRIBER);
  ovf_com_handle_v1 publisher{}, subscriber{};
  assert(transport->endpoint_create(transport, &publisher_desc, &publisher) == OVF_COM_STATUS_OK);
  assert(state.discoveries == 1);
  assert(transport->endpoint_create(transport, &subscriber_desc, &subscriber) == OVF_COM_STATUS_OK);
  ovf_com_handle_v1 subscription{};
  assert(transport->subscribe(transport, subscriber, &Sample, &state, &subscription) ==
         OVF_COM_STATUS_OK);
  assert(transport->subscription_set_state_handler(transport, subscription, &SubscriptionState,
                                                   &state) == OVF_COM_STATUS_OK);
  assert(state.subscription_active == 1);

  const std::uint8_t first[]{1, 2, 3};
  assert(transport->publish(transport, publisher, {first, sizeof(first)}) == OVF_COM_STATUS_OK);
  assert(state.samples == 1 && state.last_size == 3 && state.last[2] == 3);
  ovf_com_iovec_v1 segment{first, sizeof(first)};
  assert(transport->publish_iov(transport, publisher, &segment, 1) == OVF_COM_STATUS_OK);
  assert(state.samples == 2);

  ovf_com_loan_v1 loan{};
  loan.struct_size = sizeof(loan);
  assert(transport->loan_acquire(transport, publisher, 4, &loan) == OVF_COM_STATUS_OK);
  loan.bytes.data[0] = 4;
  loan.bytes.data[1] = 5;
  assert(transport->loan_publish(transport, publisher, loan.handle, 2) == OVF_COM_STATUS_OK);
  assert(state.samples == 3 && state.last_size == 2 && state.last[1] == 5);
  assert(transport->loan_release(transport, loan.handle) == OVF_COM_STATUS_NOT_FOUND);
  reject_dispatch = true;
  assert(transport->publish(transport, publisher, {first, sizeof(first)}) ==
         OVF_COM_STATUS_RESOURCE_EXHAUSTED);
  reject_dispatch = false;
  assert(state.diagnostics == 1 && state.samples == 3);

  auto server_desc = Endpoint(OVF_COM_ENDPOINT_METHOD_SERVER);
  auto client_desc = Endpoint(OVF_COM_ENDPOINT_METHOD_CLIENT);
  ovf_com_handle_v1 server{}, client{};
  assert(transport->endpoint_create(transport, &server_desc, &server) == OVF_COM_STATUS_OK);
  assert(transport->endpoint_create(transport, &client_desc, &client) == OVF_COM_STATUS_OK);
  assert(transport->set_request_handler(transport, server, &Request, &state) == OVF_COM_STATUS_OK);
  ovf_com_handle_v1 operation{};
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) + 1000000,
                            &Completion, &state, &operation) == OVF_COM_STATUS_OK);
  assert(state.requests == 1 && state.completions == 1);

  assert(transport->set_request_handler(transport, server, &NoResponse, &state) ==
         OVF_COM_STATUS_OK);
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) + 1000000,
                            &Completion, &state, &operation) == OVF_COM_STATUS_OK);
  assert(transport->cancel(transport, operation) == OVF_COM_STATUS_OK);
  assert(state.cancellations == 1);
  assert(transport->respond(transport, operation, OVF_COM_STATUS_OK, {first, sizeof(first)}) ==
         OVF_COM_STATUS_NOT_FOUND);

  assert(transport->set_request_handler(transport, server, nullptr, nullptr) == OVF_COM_STATUS_OK);
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) + 1000000,
                            &Completion, &state, &operation) == OVF_COM_STATUS_NOT_FOUND);

  assert(transport->set_request_handler(transport, server, &NoResponse, &state) ==
         OVF_COM_STATUS_OK);
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) - 1,
                            &Completion, &state, &operation) == OVF_COM_STATUS_DEADLINE_EXCEEDED);
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) + 1000000,
                            &Completion, &state, &operation) == OVF_COM_STATUS_OK);

  auto samples_before_close = state.samples;
  defer_dispatch = true;
  assert(transport->publish(transport, publisher, {first, sizeof(first)}) == OVF_COM_STATUS_OK);
  assert(transport->unsubscribe(transport, subscription) == OVF_COM_STATUS_OK);
  defer_dispatch = false;
  RunDeferred();
  assert(state.samples == samples_before_close);
  assert(transport->endpoint_destroy(transport, subscriber) == OVF_COM_STATUS_OK);
  assert(transport->endpoint_destroy(transport, publisher) == OVF_COM_STATUS_OK);
  assert(state.discoveries == 2 && state.withdrawals == 1);
  assert(transport->watch_stop(transport, watch) == OVF_COM_STATUS_OK);
  assert(transport->stop(transport) == OVF_COM_STATUS_OK);
  health.struct_size = sizeof(health);
  assert(transport->get_health(transport, &health) == OVF_COM_STATUS_OK);
  assert(health.state == OVF_COM_HEALTH_STOPPED);
  assert(state.shutdowns == 1);
  assert(transport->request(transport, client, {first, sizeof(first)}, Now(nullptr) + 1000000,
                            &Completion, &state, &operation) == OVF_COM_STATUS_INVALID_STATE);
  assert(transport->endpoint_destroy(transport, client) == OVF_COM_STATUS_NOT_FOUND);
  assert(transport->endpoint_destroy(transport, server) == OVF_COM_STATUS_NOT_FOUND);
  assert(transport->stop(transport) == OVF_COM_STATUS_OK);
  factory->destroy(transport);
}
