// SPDX-License-Identifier: Apache-2.0

#ifndef OVF_PER_BACKEND_ABI_H_
#define OVF_PER_BACKEND_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVF_PER_BACKEND_ABI_VERSION_1 UINT32_C(1)
#define OVF_PER_BACKEND_QUERY_SYMBOL_V1 "ovf_per_backend_query_v1"
#define OVF_PER_INVALID_HANDLE_V1 UINT64_C(0)

typedef uint64_t ovf_per_handle_v1;

typedef enum ovf_per_status_v1 {
  OVF_PER_STATUS_OK = 0,
  OVF_PER_STATUS_INVALID_ARGUMENT = 1,
  OVF_PER_STATUS_INCOMPATIBLE_ABI = 2,
  OVF_PER_STATUS_INVALID_STATE = 3,
  OVF_PER_STATUS_NOT_FOUND = 4,
  OVF_PER_STATUS_PERMISSION_DENIED = 5,
  OVF_PER_STATUS_UNSUPPORTED = 6,
  OVF_PER_STATUS_RESOURCE_EXHAUSTED = 7,
  OVF_PER_STATUS_BUFFER_TOO_SMALL = 8,
  OVF_PER_STATUS_CONFLICT = 9,
  OVF_PER_STATUS_QUOTA_EXCEEDED = 10,
  OVF_PER_STATUS_CORRUPTED = 11,
  OVF_PER_STATUS_IO_ERROR = 12,
  OVF_PER_STATUS_BUSY = 13,
  OVF_PER_STATUS_SHUTTING_DOWN = 14,
  OVF_PER_STATUS_BACKEND_ERROR = 15
} ovf_per_status_v1;

typedef enum ovf_per_durability_v1 {
  OVF_PER_DURABILITY_BUFFERED = 1,
  OVF_PER_DURABILITY_PROCESS_CRASH = 2,
  OVF_PER_DURABILITY_MEDIA = 3
} ovf_per_durability_v1;

typedef enum ovf_per_access_v1 {
  OVF_PER_ACCESS_READ_ONLY = 1,
  OVF_PER_ACCESS_READ_WRITE = 2
} ovf_per_access_v1;

typedef struct ovf_per_bytes_view_v1 {
  const uint8_t* data;
  size_t size;
} ovf_per_bytes_view_v1;

typedef struct ovf_per_mutable_bytes_v1 {
  uint8_t* data;
  size_t size;
} ovf_per_mutable_bytes_v1;

typedef struct ovf_per_string_view_v1 {
  const char* data;
  size_t size;
} ovf_per_string_view_v1;

typedef struct ovf_per_backend_config_v1 {
  size_t struct_size;
  ovf_per_string_view_v1 configuration;
  uint32_t max_stores;
  uint32_t max_transactions;
} ovf_per_backend_config_v1;

typedef struct ovf_per_store_descriptor_v1 {
  size_t struct_size;
  ovf_per_string_view_v1 logical_name;
  ovf_per_access_v1 access;
  ovf_per_durability_v1 minimum_durability;
  uint64_t capacity_bytes;
  uint32_t max_entries;
  uint32_t max_key_size;
  uint32_t max_value_size;
  uint64_t max_blob_size;
} ovf_per_store_descriptor_v1;

typedef struct ovf_per_capabilities_v1 {
  size_t struct_size;
  uint32_t max_stores;
  uint32_t max_transactions;
  uint64_t max_store_bytes;
  uint32_t max_key_size;
  uint32_t max_value_size;
  uint64_t max_blob_size;
  ovf_per_durability_v1 maximum_durability;
  uint8_t persistent;
  uint8_t cross_process_leases;
  uint8_t reserved[6];
} ovf_per_capabilities_v1;

typedef struct ovf_per_commit_result_v1 {
  size_t struct_size;
  uint64_t generation;
  ovf_per_durability_v1 achieved_durability;
} ovf_per_commit_result_v1;

typedef enum ovf_per_recovery_state_v1 {
  OVF_PER_RECOVERY_CLEAN_V1 = 1,
  OVF_PER_RECOVERY_JOURNAL_REPLAYED_V1 = 2,
  OVF_PER_RECOVERY_FAILED_CLOSED_V1 = 3,
  OVF_PER_RECOVERY_RESET_V1 = 4,
  OVF_PER_RECOVERY_MIGRATED_V1 = 5,
  OVF_PER_RECOVERY_ROLLED_BACK_V1 = 6
} ovf_per_recovery_state_v1;

typedef struct ovf_per_store_status_v1 {
  size_t struct_size;
  uint64_t generation;
  uint64_t schema_version;
  ovf_per_recovery_state_v1 recovery_state;
  uint64_t successful_commits;
  uint64_t rejected_operations;
  uint64_t recovery_count;
} ovf_per_store_status_v1;

typedef struct ovf_per_entry_v1 {
  ovf_per_bytes_view_v1 key;
  ovf_per_bytes_view_v1 value;
} ovf_per_entry_v1;

typedef struct ovf_per_backend_v1 ovf_per_backend_v1;

struct ovf_per_backend_v1 {
  size_t struct_size;
  uint32_t abi_version;
  void* implementation;
  ovf_per_status_v1 (*start)(ovf_per_backend_v1*);
  void (*stop)(ovf_per_backend_v1*);
  ovf_per_status_v1 (*get_capabilities)(ovf_per_backend_v1*, ovf_per_capabilities_v1*);
  ovf_per_status_v1 (*store_open)(ovf_per_backend_v1*, const ovf_per_store_descriptor_v1*,
                                  ovf_per_handle_v1*);
  ovf_per_status_v1 (*store_close)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*read_begin)(ovf_per_backend_v1*, ovf_per_handle_v1, ovf_per_handle_v1*,
                                  uint64_t*);
  ovf_per_status_v1 (*write_begin)(ovf_per_backend_v1*, ovf_per_handle_v1, ovf_per_durability_v1,
                                   ovf_per_handle_v1*, uint64_t*);
  ovf_per_status_v1 (*transaction_get)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                       ovf_per_bytes_view_v1, ovf_per_mutable_bytes_v1*);
  ovf_per_status_v1 (*transaction_put)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                       ovf_per_bytes_view_v1, ovf_per_bytes_view_v1);
  ovf_per_status_v1 (*transaction_erase)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                         ovf_per_bytes_view_v1, uint8_t*);
  ovf_per_status_v1 (*transaction_commit)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                          ovf_per_commit_result_v1*);
  ovf_per_status_v1 (*transaction_abort)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*transaction_close)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*blob_read_open)(ovf_per_backend_v1*, ovf_per_handle_v1, ovf_per_bytes_view_v1,
                                      ovf_per_handle_v1*, uint64_t*, uint64_t*);
  ovf_per_status_v1 (*blob_replace_begin)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                          ovf_per_bytes_view_v1, uint64_t, ovf_per_durability_v1,
                                          ovf_per_handle_v1*, uint64_t*);
  ovf_per_status_v1 (*blob_read)(ovf_per_backend_v1*, ovf_per_handle_v1, uint64_t,
                                 ovf_per_mutable_bytes_v1*);
  ovf_per_status_v1 (*blob_write)(ovf_per_backend_v1*, ovf_per_handle_v1, uint64_t,
                                  ovf_per_bytes_view_v1);
  ovf_per_status_v1 (*blob_commit)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                   ovf_per_commit_result_v1*);
  ovf_per_status_v1 (*blob_abort)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*blob_close)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*write_begin_at)(ovf_per_backend_v1*, ovf_per_handle_v1, ovf_per_durability_v1,
                                      uint64_t, ovf_per_handle_v1*, uint64_t*);
  ovf_per_status_v1 (*cursor_open)(ovf_per_backend_v1*, ovf_per_handle_v1, ovf_per_bytes_view_v1,
                                   ovf_per_handle_v1*);
  ovf_per_status_v1 (*cursor_next)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                   ovf_per_mutable_bytes_v1*, ovf_per_mutable_bytes_v1*);
  ovf_per_status_v1 (*cursor_close)(ovf_per_backend_v1*, ovf_per_handle_v1);
  ovf_per_status_v1 (*store_reset)(ovf_per_backend_v1*, ovf_per_handle_v1, const ovf_per_entry_v1*,
                                   size_t, ovf_per_durability_v1, ovf_per_commit_result_v1*);
  ovf_per_status_v1 (*store_status)(ovf_per_backend_v1*, ovf_per_handle_v1,
                                    ovf_per_store_status_v1*);
  ovf_per_status_v1 (*last_error)(ovf_per_backend_v1*, ovf_per_mutable_bytes_v1*);
};

typedef struct ovf_per_backend_factory_v1 {
  size_t struct_size;
  uint32_t abi_version;
  ovf_per_string_view_v1 name;
  ovf_per_status_v1 (*create)(const ovf_per_backend_config_v1*, ovf_per_backend_v1**);
  void (*destroy)(ovf_per_backend_v1*);
} ovf_per_backend_factory_v1;

typedef const ovf_per_backend_factory_v1* (*ovf_per_backend_query_fn_v1)(void);

#ifdef __cplusplus
}
#endif

#endif
