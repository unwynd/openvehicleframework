// SPDX-License-Identifier: Apache-2.0

#ifndef OVF_EXEC_BACKEND_ABI_H_
#define OVF_EXEC_BACKEND_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVF_EXEC_BACKEND_ABI_VERSION_1 1U

typedef struct ovf_exec_string_view_v1 {
  const char* data;
  size_t size;
} ovf_exec_string_view_v1;

typedef enum ovf_exec_status_v1 {
  OVF_EXEC_STATUS_OK = 0,
  OVF_EXEC_STATUS_INVALID_ARGUMENT = 1,
  OVF_EXEC_STATUS_INVALID_STATE = 2,
  OVF_EXEC_STATUS_NOT_FOUND = 3,
  OVF_EXEC_STATUS_PERMISSION_DENIED = 4,
  OVF_EXEC_STATUS_BUSY = 5,
  OVF_EXEC_STATUS_CANCELLED = 6,
  OVF_EXEC_STATUS_DEADLINE_EXCEEDED = 7,
  OVF_EXEC_STATUS_RESOURCE_EXHAUSTED = 8,
  OVF_EXEC_STATUS_UNSUPPORTED = 9,
  OVF_EXEC_STATUS_BACKEND_ERROR = 10
} ovf_exec_status_v1;

typedef enum ovf_exec_application_state_v1 {
  OVF_EXEC_APPLICATION_UNKNOWN = 0,
  OVF_EXEC_APPLICATION_STARTING = 1,
  OVF_EXEC_APPLICATION_READY = 2,
  OVF_EXEC_APPLICATION_STOPPING = 3,
  OVF_EXEC_APPLICATION_STOPPED = 4,
  OVF_EXEC_APPLICATION_FAILED = 5,
  OVF_EXEC_APPLICATION_KILLED = 6,
  OVF_EXEC_APPLICATION_UNAVAILABLE = 7
} ovf_exec_application_state_v1;

typedef enum ovf_exec_stop_reason_v1 {
  OVF_EXEC_STOP_MODE_CHANGE = 1,
  OVF_EXEC_STOP_SYSTEM_SHUTDOWN = 2,
  OVF_EXEC_STOP_RESTART = 3,
  OVF_EXEC_STOP_SUPERVISOR_REQUEST = 4,
  OVF_EXEC_STOP_DEPENDENCY_FAILURE = 5,
  OVF_EXEC_STOP_RECOVERY = 6,
  OVF_EXEC_STOP_UNKNOWN = 7
} ovf_exec_stop_reason_v1;

typedef struct ovf_exec_evidence_v1 {
  size_t struct_size;
  ovf_exec_application_state_v1 state;
  int32_t exit_code;
  int32_t signal;
  uint64_t native_code;
  ovf_exec_string_view_v1 message;
} ovf_exec_evidence_v1;

typedef struct ovf_exec_capabilities_v1 {
  size_t struct_size;
  uint32_t max_parallel_operations;
  uint8_t supports_readiness;
  uint8_t supports_graceful_stop;
  uint8_t supports_exit_evidence;
  uint8_t supports_log_access;
  uint8_t supports_resource_control;
  uint8_t reserved[3];
} ovf_exec_capabilities_v1;

typedef enum ovf_exec_log_level_v1 {
  OVF_EXEC_LOG_DEBUG = 0,
  OVF_EXEC_LOG_INFO = 1,
  OVF_EXEC_LOG_WARNING = 2,
  OVF_EXEC_LOG_ERROR = 3
} ovf_exec_log_level_v1;

typedef struct ovf_exec_host_api_v1 {
  size_t struct_size;
  void* user_data;
  void (*log)(void* user_data, ovf_exec_log_level_v1 level, ovf_exec_string_view_v1 message);
  uint64_t (*monotonic_time_ns)(void* user_data);
} ovf_exec_host_api_v1;

typedef struct ovf_exec_backend_config_v1 {
  size_t struct_size;
  ovf_exec_string_view_v1 configuration;
  uint32_t required_parallel_operations;
} ovf_exec_backend_config_v1;

struct ovf_exec_backend_v1;

typedef ovf_exec_status_v1 (*ovf_exec_backend_capabilities_fn_v1)(
    struct ovf_exec_backend_v1* self, ovf_exec_capabilities_v1* capabilities);
typedef ovf_exec_status_v1 (*ovf_exec_backend_inspect_fn_v1)(struct ovf_exec_backend_v1* self,
                                                             uint64_t application_id,
                                                             ovf_exec_evidence_v1* evidence);
typedef ovf_exec_status_v1 (*ovf_exec_backend_start_fn_v1)(struct ovf_exec_backend_v1* self,
                                                           uint64_t application_id,
                                                           uint64_t deadline_ns,
                                                           ovf_exec_evidence_v1* evidence);
typedef ovf_exec_status_v1 (*ovf_exec_backend_stop_fn_v1)(struct ovf_exec_backend_v1* self,
                                                          uint64_t application_id,
                                                          ovf_exec_stop_reason_v1 reason,
                                                          uint64_t deadline_ns,
                                                          ovf_exec_evidence_v1* evidence);

typedef struct ovf_exec_backend_v1 {
  size_t struct_size;
  uint32_t abi_version;
  void* implementation;
  ovf_exec_backend_capabilities_fn_v1 get_capabilities;
  ovf_exec_backend_inspect_fn_v1 inspect;
  ovf_exec_backend_start_fn_v1 start;
  ovf_exec_backend_stop_fn_v1 stop;
} ovf_exec_backend_v1;

typedef struct ovf_exec_backend_factory_v1 {
  size_t struct_size;
  uint32_t abi_version;
  ovf_exec_string_view_v1 name;
  ovf_exec_status_v1 (*create)(const ovf_exec_host_api_v1* host,
                               const ovf_exec_backend_config_v1* config,
                               ovf_exec_backend_v1** backend);
  void (*destroy)(ovf_exec_backend_v1* backend);
} ovf_exec_backend_factory_v1;

typedef const ovf_exec_backend_factory_v1* (*ovf_exec_backend_query_fn_v1)(void);

const ovf_exec_backend_factory_v1* ovf_exec_backend_query_v1(void);

#ifdef __cplusplus
}
#endif

#endif
