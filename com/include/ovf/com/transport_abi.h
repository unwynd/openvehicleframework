// SPDX-License-Identifier: Apache-2.0

#ifndef OVF_COM_TRANSPORT_ABI_H
#define OVF_COM_TRANSPORT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVF_COM_TRANSPORT_ABI_VERSION_1 UINT32_C(1)
#define OVF_COM_INVALID_HANDLE_V1 UINT64_C(0)
#define OVF_COM_TRANSPORT_QUERY_SYMBOL_V1 "ovf_com_transport_query_v1"

typedef uint64_t ovf_com_handle_v1;

typedef enum ovf_com_status_v1 {
  OVF_COM_STATUS_OK = 0,
  OVF_COM_STATUS_INVALID_ARGUMENT = 1,
  OVF_COM_STATUS_INCOMPATIBLE_ABI = 2,
  OVF_COM_STATUS_ALREADY_EXISTS = 3,
  OVF_COM_STATUS_INVALID_STATE = 4,
  OVF_COM_STATUS_TRANSPORT_ERROR = 5,
  OVF_COM_STATUS_UNSUPPORTED = 6,
  OVF_COM_STATUS_RESOURCE_EXHAUSTED = 7,
  OVF_COM_STATUS_NOT_FOUND = 8,
  OVF_COM_STATUS_CANCELLED = 9,
  OVF_COM_STATUS_DEADLINE_EXCEEDED = 10,
  OVF_COM_STATUS_SHUTTING_DOWN = 11,
  OVF_COM_STATUS_APPLICATION_ERROR = 12
} ovf_com_status_v1;

typedef enum ovf_com_log_level_v1 {
  OVF_COM_LOG_DEBUG = 0,
  OVF_COM_LOG_INFO = 1,
  OVF_COM_LOG_WARNING = 2,
  OVF_COM_LOG_ERROR = 3
} ovf_com_log_level_v1;

typedef enum ovf_com_isolation_v1 {
  OVF_COM_ISOLATION_INDEPENDENT = 0,
  OVF_COM_ISOLATION_SHARED_ENGINE = 1,
  OVF_COM_ISOLATION_PROCESS_SINGLETON = 2
} ovf_com_isolation_v1;

typedef enum ovf_com_endpoint_kind_v1 {
  OVF_COM_ENDPOINT_EVENT_PUBLISHER = 0,
  OVF_COM_ENDPOINT_EVENT_SUBSCRIBER = 1,
  OVF_COM_ENDPOINT_METHOD_CLIENT = 2,
  OVF_COM_ENDPOINT_METHOD_SERVER = 3
} ovf_com_endpoint_kind_v1;

enum {
  OVF_COM_CAP_DISCOVERY = UINT64_C(1) << 0,
  OVF_COM_CAP_EVENTS = UINT64_C(1) << 1,
  OVF_COM_CAP_METHODS = UINT64_C(1) << 2,
  OVF_COM_CAP_LOANS = UINT64_C(1) << 3,
  OVF_COM_CAP_SCATTER_GATHER = UINT64_C(1) << 4,
  OVF_COM_CAP_RELIABLE = UINT64_C(1) << 5,
  OVF_COM_CAP_ORDERED = UINT64_C(1) << 6,
  OVF_COM_CAP_DEADLINES = UINT64_C(1) << 7,
  OVF_COM_CAP_CANCELLATION = UINT64_C(1) << 8,
  OVF_COM_CAP_REQUEST_LOANS = UINT64_C(1) << 9,
  OVF_COM_CAP_RESPONSE_LOANS = UINT64_C(1) << 10,
  OVF_COM_CAP_SUBSCRIPTION_STATE = UINT64_C(1) << 11,
  OVF_COM_CAP_HEALTH = UINT64_C(1) << 12,
  OVF_COM_CAP_DIAGNOSTICS = UINT64_C(1) << 13
};

typedef enum ovf_com_health_state_v1 {
  OVF_COM_HEALTH_INITIALIZING = 0,
  OVF_COM_HEALTH_READY = 1,
  OVF_COM_HEALTH_DEGRADED = 2,
  OVF_COM_HEALTH_FAILED = 3,
  OVF_COM_HEALTH_STOPPED = 4
} ovf_com_health_state_v1;

typedef enum ovf_com_diagnostic_operation_v1 {
  OVF_COM_DIAGNOSTIC_PROVIDER = 0,
  OVF_COM_DIAGNOSTIC_DISCOVERY = 1,
  OVF_COM_DIAGNOSTIC_ENDPOINT = 2,
  OVF_COM_DIAGNOSTIC_SUBSCRIPTION = 3,
  OVF_COM_DIAGNOSTIC_PUBLISH = 4,
  OVF_COM_DIAGNOSTIC_REQUEST = 5,
  OVF_COM_DIAGNOSTIC_RESPONSE = 6
} ovf_com_diagnostic_operation_v1;

typedef enum ovf_com_subscription_state_v1 {
  OVF_COM_SUBSCRIPTION_REQUESTED = 0,
  OVF_COM_SUBSCRIPTION_ACTIVE = 1,
  OVF_COM_SUBSCRIPTION_REJECTED = 2,
  OVF_COM_SUBSCRIPTION_SUSPENDED = 3,
  OVF_COM_SUBSCRIPTION_WITHDRAWN = 4
} ovf_com_subscription_state_v1;

typedef struct ovf_com_string_view_v1 {
  const char* data;
  size_t size;
} ovf_com_string_view_v1;
typedef struct ovf_com_bytes_view_v1 {
  const uint8_t* data;
  size_t size;
} ovf_com_bytes_view_v1;
typedef struct ovf_com_mutable_bytes_v1 {
  uint8_t* data;
  size_t size;
} ovf_com_mutable_bytes_v1;
typedef struct ovf_com_iovec_v1 {
  const uint8_t* data;
  size_t size;
} ovf_com_iovec_v1;
typedef struct ovf_com_uuid_v1 {
  uint8_t bytes[16];
} ovf_com_uuid_v1;

typedef void (*ovf_com_task_fn_v1)(void* task_user_data);
typedef void (*ovf_com_task_release_fn_v1)(void* task_user_data);
typedef void (*ovf_com_log_fn_v1)(void*, ovf_com_log_level_v1, ovf_com_string_view_v1);
typedef ovf_com_status_v1 (*ovf_com_dispatch_fn_v1)(void* host_user_data, ovf_com_task_fn_v1 task,
                                                    ovf_com_task_release_fn_v1 release,
                                                    void* task_user_data);
typedef uint64_t (*ovf_com_monotonic_time_fn_v1)(void* host_user_data);

typedef struct ovf_com_host_api_v1 {
  uint32_t struct_size;
  void* user_data;
  ovf_com_log_fn_v1 log;
  ovf_com_dispatch_fn_v1 dispatch;
  ovf_com_monotonic_time_fn_v1 monotonic_time_ns;
} ovf_com_host_api_v1;

typedef struct ovf_com_transport_config_v1 {
  uint32_t struct_size;
  ovf_com_string_view_v1 instance_name;
  ovf_com_string_view_v1 configuration;
  uint32_t max_endpoints;
  uint32_t max_outstanding_operations;
  /* Absolute lifecycle bounds supplied by deployment. Zero selects the runtime default. */
  uint64_t start_timeout_ns;
  uint64_t stop_timeout_ns;
} ovf_com_transport_config_v1;
#define OVF_COM_TRANSPORT_CONFIG_V1_BASE_SIZE                                                      \
  offsetof(ovf_com_transport_config_v1, start_timeout_ns)

typedef struct ovf_com_diagnostic_v1 {
  uint32_t struct_size;
  ovf_com_status_v1 status;
  ovf_com_diagnostic_operation_v1 operation;
  int64_t native_code;
  ovf_com_handle_v1 endpoint;
  ovf_com_handle_v1 operation_handle;
  ovf_com_string_view_v1 message;
} ovf_com_diagnostic_v1;

typedef struct ovf_com_health_v1 {
  uint32_t struct_size;
  ovf_com_health_state_v1 state;
  uint64_t sequence;
  ovf_com_diagnostic_v1 diagnostic;
} ovf_com_health_v1;

typedef struct ovf_com_capabilities_v1 {
  uint32_t struct_size;
  uint64_t feature_bits;
  ovf_com_isolation_v1 isolation;
  uint32_t max_endpoints;
  uint32_t max_subscriptions;
  uint32_t max_outstanding_operations;
  uint32_t max_iovecs;
  uint64_t max_payload_size;
  uint64_t max_loan_size;
  uint32_t max_history_depth;
} ovf_com_capabilities_v1;

typedef struct ovf_com_endpoint_descriptor_v1 {
  uint32_t struct_size;
  ovf_com_endpoint_kind_v1 kind;
  ovf_com_uuid_v1 service_id;
  ovf_com_uuid_v1 instance_id;
  ovf_com_uuid_v1 element_id;
  uint64_t route_epoch;
  uint64_t max_payload_size;
  uint32_t history_depth;
  uint64_t required_features;
  ovf_com_string_view_v1 native_mapping;
} ovf_com_endpoint_descriptor_v1;

typedef struct ovf_com_loan_v1 {
  uint32_t struct_size;
  ovf_com_handle_v1 handle;
  ovf_com_mutable_bytes_v1 bytes;
} ovf_com_loan_v1;

typedef struct ovf_com_sample_v1 {
  uint32_t struct_size;
  ovf_com_bytes_view_v1 payload;
  ovf_com_handle_v1 provider_loan;
  uint64_t sequence;
  uint64_t route_epoch;
} ovf_com_sample_v1;

typedef struct ovf_com_discovery_filter_v1 {
  uint32_t struct_size;
  ovf_com_uuid_v1 service_id;
  ovf_com_string_view_v1 native_mapping;
} ovf_com_discovery_filter_v1;

typedef struct ovf_com_discovery_entry_v1 {
  uint32_t struct_size;
  ovf_com_uuid_v1 service_id;
  ovf_com_uuid_v1 instance_id;
  ovf_com_handle_v1 route;
  uint64_t route_epoch;
  uint8_t available;
} ovf_com_discovery_entry_v1;

typedef void (*ovf_com_sample_callback_v1)(void*, const ovf_com_sample_v1*);
typedef void (*ovf_com_discovery_callback_v1)(void*, const ovf_com_discovery_entry_v1*);
typedef void (*ovf_com_completion_callback_v1)(void*, ovf_com_handle_v1 operation,
                                               ovf_com_status_v1, ovf_com_bytes_view_v1 payload);
/* deadline_ns is zero when the provider cannot express a meaningful deadline in the server
   host's monotonic clock. */
typedef void (*ovf_com_request_callback_v1)(void*, ovf_com_handle_v1 request,
                                            ovf_com_bytes_view_v1 payload, uint64_t deadline_ns);
typedef void (*ovf_com_subscription_state_callback_v1)(void*, ovf_com_handle_v1 subscription,
                                                       ovf_com_subscription_state_v1,
                                                       ovf_com_status_v1 reason);
typedef void (*ovf_com_health_callback_v1)(void*, const ovf_com_health_v1*);
typedef void (*ovf_com_diagnostic_callback_v1)(void*, const ovf_com_diagnostic_v1*);

typedef struct ovf_com_transport_v1 ovf_com_transport_v1;
typedef ovf_com_status_v1 (*ovf_com_transport_create_fn_v1)(const ovf_com_host_api_v1*,
                                                            const ovf_com_transport_config_v1*,
                                                            ovf_com_transport_v1**);
typedef void (*ovf_com_transport_destroy_fn_v1)(ovf_com_transport_v1*);

struct ovf_com_transport_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  void* implementation;
  ovf_com_string_view_v1 name;
  ovf_com_status_v1 (*start)(ovf_com_transport_v1*);
  ovf_com_status_v1 (*stop)(ovf_com_transport_v1*);
  ovf_com_status_v1 (*get_capabilities)(ovf_com_transport_v1*, ovf_com_capabilities_v1*);
  ovf_com_status_v1 (*watch_start)(ovf_com_transport_v1*, const ovf_com_discovery_filter_v1*,
                                   ovf_com_discovery_callback_v1, void*, ovf_com_handle_v1*);
  /* Callback-quiescence boundary with the same reentrancy rule as unsubscribe. */
  ovf_com_status_v1 (*watch_stop)(ovf_com_transport_v1*, ovf_com_handle_v1);
  ovf_com_status_v1 (*endpoint_create)(ovf_com_transport_v1*, const ovf_com_endpoint_descriptor_v1*,
                                       ovf_com_handle_v1*);
  ovf_com_status_v1 (*endpoint_destroy)(ovf_com_transport_v1*, ovf_com_handle_v1);
  ovf_com_status_v1 (*subscribe)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                 ovf_com_sample_callback_v1, void*, ovf_com_handle_v1*);
  /* After this returns, no sample or subscription-state callback for the handle is executing or
     may begin. Calling it reentrantly from that subscription's callback is supported. */
  ovf_com_status_v1 (*unsubscribe)(ovf_com_transport_v1*, ovf_com_handle_v1);
  ovf_com_status_v1 (*publish)(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1);
  ovf_com_status_v1 (*publish_iov)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                   const ovf_com_iovec_v1*, size_t);
  ovf_com_status_v1 (*loan_acquire)(ovf_com_transport_v1*, ovf_com_handle_v1, size_t,
                                    ovf_com_loan_v1*);
  ovf_com_status_v1 (*loan_publish)(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_handle_v1,
                                    size_t);
  ovf_com_status_v1 (*loan_release)(ovf_com_transport_v1*, ovf_com_handle_v1);
  ovf_com_status_v1 (*request)(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_bytes_view_v1,
                               uint64_t, ovf_com_completion_callback_v1, void*, ovf_com_handle_v1*);
  /* Cancels local interest and produces a CANCELLED terminal completion. It does not promise
     cancellation of work already executing in a remote provider. */
  ovf_com_status_v1 (*cancel)(ovf_com_transport_v1*, ovf_com_handle_v1);
  /* Passing a null handler withdraws it and is a callback-quiescence boundary. */
  ovf_com_status_v1 (*set_request_handler)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                           ovf_com_request_callback_v1, void*);
  ovf_com_status_v1 (*respond)(ovf_com_transport_v1*, ovf_com_handle_v1, ovf_com_status_v1,
                               ovf_com_bytes_view_v1);
  /* Optional append-only v1 extensions. Presence is determined from struct_size and capability
     bits, preserving compatibility with providers built against the original v1 layout. */
  ovf_com_status_v1 (*request_loan_acquire)(ovf_com_transport_v1*, ovf_com_handle_v1, size_t,
                                            uint64_t, ovf_com_loan_v1*);
  ovf_com_status_v1 (*request_loan_send)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                         ovf_com_handle_v1, size_t, ovf_com_completion_callback_v1,
                                         void*, ovf_com_handle_v1*);
  ovf_com_status_v1 (*response_loan_acquire)(ovf_com_transport_v1*, ovf_com_handle_v1, size_t,
                                             ovf_com_status_v1, ovf_com_loan_v1*);
  ovf_com_status_v1 (*response_loan_send)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                          ovf_com_handle_v1, size_t);
  /* Registers for asynchronous activation state. The current state is reported after successful
     registration; middleware-native subscription identifiers remain provider-private. */
  ovf_com_status_v1 (*subscription_set_state_handler)(ovf_com_transport_v1*, ovf_com_handle_v1,
                                                      ovf_com_subscription_state_callback_v1,
                                                      void*);
  /* Health and diagnostics are append-only, provider-wide surfaces. Callback payload views are
     valid only for the duration of the callback. Replacing a handler is atomic. */
  ovf_com_status_v1 (*get_health)(ovf_com_transport_v1*, ovf_com_health_v1*);
  ovf_com_status_v1 (*set_health_handler)(ovf_com_transport_v1*, ovf_com_health_callback_v1, void*);
  ovf_com_status_v1 (*set_diagnostic_handler)(ovf_com_transport_v1*, ovf_com_diagnostic_callback_v1,
                                              void*);
};

#define OVF_COM_TRANSPORT_V1_BASE_SIZE offsetof(ovf_com_transport_v1, request_loan_acquire)

typedef struct ovf_com_transport_factory_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  ovf_com_string_view_v1 name;
  ovf_com_transport_create_fn_v1 create;
  ovf_com_transport_destroy_fn_v1 destroy;
} ovf_com_transport_factory_v1;

typedef const ovf_com_transport_factory_v1* (*ovf_com_transport_query_fn_v1)(void);

#ifdef __cplusplus
}
#endif
#endif
