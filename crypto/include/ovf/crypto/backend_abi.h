// SPDX-License-Identifier: Apache-2.0

#ifndef OVF_CRYPTO_BACKEND_ABI_H_
#define OVF_CRYPTO_BACKEND_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVF_CRYPTO_BACKEND_ABI_VERSION_1 UINT32_C(1)
#define OVF_CRYPTO_BACKEND_QUERY_SYMBOL_V1 "ovf_crypto_backend_query_v1"
#define OVF_CRYPTO_INVALID_HANDLE_V1 UINT64_C(0)

typedef uint64_t ovf_crypto_handle_v1;

typedef enum ovf_crypto_status_v1 {
  OVF_CRYPTO_STATUS_OK = 0,
  OVF_CRYPTO_STATUS_INVALID_ARGUMENT = 1,
  OVF_CRYPTO_STATUS_INCOMPATIBLE_ABI = 2,
  OVF_CRYPTO_STATUS_INVALID_STATE = 3,
  OVF_CRYPTO_STATUS_NOT_FOUND = 4,
  OVF_CRYPTO_STATUS_PERMISSION_DENIED = 5,
  OVF_CRYPTO_STATUS_UNSUPPORTED = 6,
  OVF_CRYPTO_STATUS_RESOURCE_EXHAUSTED = 7,
  OVF_CRYPTO_STATUS_BUFFER_TOO_SMALL = 8,
  OVF_CRYPTO_STATUS_AUTHENTICATION_FAILED = 9,
  OVF_CRYPTO_STATUS_ENTROPY_FAILURE = 10,
  OVF_CRYPTO_STATUS_BACKEND_ERROR = 11,
  OVF_CRYPTO_STATUS_SHUTTING_DOWN = 12
} ovf_crypto_status_v1;

typedef enum ovf_crypto_algorithm_v1 {
  OVF_CRYPTO_ALGORITHM_SHA2_256 = 0x0101,
  OVF_CRYPTO_ALGORITHM_SHA2_384 = 0x0102,
  OVF_CRYPTO_ALGORITHM_SHA2_512 = 0x0103,
  OVF_CRYPTO_ALGORITHM_HMAC_SHA2_256 = 0x0201,
  OVF_CRYPTO_ALGORITHM_AES_128_GCM = 0x0301,
  OVF_CRYPTO_ALGORITHM_AES_256_GCM = 0x0302,
  OVF_CRYPTO_ALGORITHM_HKDF_SHA2_256 = 0x0401,
  OVF_CRYPTO_ALGORITHM_ECDSA_P256_SHA2_256 = 0x0501,
  OVF_CRYPTO_ALGORITHM_RSA_PSS_SHA2_256 = 0x0502,
  OVF_CRYPTO_ALGORITHM_ED25519 = 0x0503,
  OVF_CRYPTO_ALGORITHM_ECDH_P256 = 0x0601
} ovf_crypto_algorithm_v1;

typedef enum ovf_crypto_key_usage_v1 {
  OVF_CRYPTO_KEY_USAGE_SIGN = UINT32_C(1) << 0,
  OVF_CRYPTO_KEY_USAGE_VERIFY = UINT32_C(1) << 1,
  OVF_CRYPTO_KEY_USAGE_ENCRYPT = UINT32_C(1) << 2,
  OVF_CRYPTO_KEY_USAGE_DECRYPT = UINT32_C(1) << 3,
  OVF_CRYPTO_KEY_USAGE_MAC_GENERATE = UINT32_C(1) << 4,
  OVF_CRYPTO_KEY_USAGE_MAC_VERIFY = UINT32_C(1) << 5,
  OVF_CRYPTO_KEY_USAGE_DERIVE = UINT32_C(1) << 6
} ovf_crypto_key_usage_v1;

typedef enum ovf_crypto_key_format_v1 {
  OVF_CRYPTO_KEY_FORMAT_RAW = 1,
  OVF_CRYPTO_KEY_FORMAT_DER = 2,
  OVF_CRYPTO_KEY_FORMAT_PEM = 3
} ovf_crypto_key_format_v1;

typedef struct ovf_crypto_bytes_view_v1 {
  const uint8_t* data;
  size_t size;
} ovf_crypto_bytes_view_v1;

typedef struct ovf_crypto_mutable_bytes_v1 {
  uint8_t* data;
  size_t size;
} ovf_crypto_mutable_bytes_v1;

typedef struct ovf_crypto_string_view_v1 {
  const char* data;
  size_t size;
} ovf_crypto_string_view_v1;

typedef struct ovf_crypto_key_descriptor_v1 {
  size_t struct_size;
  uint32_t algorithm;
  uint32_t permitted_usage;
  uint8_t exportable;
  uint8_t persistent;
  uint8_t reserved[6];
} ovf_crypto_key_descriptor_v1;

typedef struct ovf_crypto_capabilities_v1 {
  size_t struct_size;
  uint32_t algorithm_count;
  uint32_t algorithms[32];
  uint32_t max_keys;
  uint32_t max_contexts;
  uint64_t max_input_size;
  uint8_t supports_persistent_keys;
  uint8_t supports_hardware_keys;
  uint8_t supports_secure_memory;
  uint8_t reserved[5];
} ovf_crypto_capabilities_v1;

typedef struct ovf_crypto_aead_parameters_v1 {
  size_t struct_size;
  ovf_crypto_bytes_view_v1 nonce;
  ovf_crypto_bytes_view_v1 associated_data;
  uint32_t tag_size;
} ovf_crypto_aead_parameters_v1;

typedef enum ovf_crypto_stream_operation_v1 {
  OVF_CRYPTO_STREAM_HASH = 1,
  OVF_CRYPTO_STREAM_MAC = 2,
  OVF_CRYPTO_STREAM_SIGN = 3,
  OVF_CRYPTO_STREAM_VERIFY = 4,
  OVF_CRYPTO_STREAM_AEAD_ENCRYPT_RECORDS = 5,
  OVF_CRYPTO_STREAM_AEAD_DECRYPT_RECORDS = 6
} ovf_crypto_stream_operation_v1;

typedef struct ovf_crypto_stream_descriptor_v1 {
  size_t struct_size;
  ovf_crypto_stream_operation_v1 operation;
  uint32_t algorithm;
  ovf_crypto_handle_v1 key;
  ovf_crypto_aead_parameters_v1 aead;
  uint8_t reserved[8];
} ovf_crypto_stream_descriptor_v1;

typedef enum ovf_crypto_certificate_usage_v1 {
  OVF_CRYPTO_CERTIFICATE_USAGE_UNSPECIFIED = 0,
  OVF_CRYPTO_CERTIFICATE_USAGE_SERVER_AUTHENTICATION = 1,
  OVF_CRYPTO_CERTIFICATE_USAGE_CLIENT_AUTHENTICATION = 2,
  OVF_CRYPTO_CERTIFICATE_USAGE_CERTIFICATE_AUTHORITY = 3,
  OVF_CRYPTO_CERTIFICATE_USAGE_OCSP_SIGNING = 4,
  OVF_CRYPTO_CERTIFICATE_USAGE_ENCRYPTION = 5
} ovf_crypto_certificate_usage_v1;

typedef enum ovf_crypto_certificate_verdict_v1 {
  OVF_CRYPTO_CERTIFICATE_VERDICT_TRUSTED = 0,
  OVF_CRYPTO_CERTIFICATE_VERDICT_UNTRUSTED = 1,
  OVF_CRYPTO_CERTIFICATE_VERDICT_EXPIRED = 2,
  OVF_CRYPTO_CERTIFICATE_VERDICT_REVOKED = 3,
  OVF_CRYPTO_CERTIFICATE_VERDICT_NAME_MISMATCH = 4,
  OVF_CRYPTO_CERTIFICATE_VERDICT_USAGE_REJECTED = 5,
  OVF_CRYPTO_CERTIFICATE_VERDICT_MALFORMED = 6,
  OVF_CRYPTO_CERTIFICATE_VERDICT_REVOCATION_UNKNOWN = 7,
  OVF_CRYPTO_CERTIFICATE_VERDICT_POLICY_REJECTED = 8
} ovf_crypto_certificate_verdict_v1;

typedef struct ovf_crypto_certificate_validation_request_v1 {
  size_t struct_size;
  ovf_crypto_bytes_view_v1 leaf;
  const ovf_crypto_bytes_view_v1* intermediates;
  size_t intermediate_count;
  const ovf_crypto_bytes_view_v1* trust_anchors;
  size_t trust_anchor_count;
  const ovf_crypto_bytes_view_v1* crls;
  size_t crl_count;
  ovf_crypto_string_view_v1 expected_name;
  uint64_t validation_time_unix_seconds;
  uint32_t minimum_security_bits;
  ovf_crypto_certificate_usage_v1 usage;
  uint8_t require_revocation;
  uint8_t require_self_signed_anchor;
  uint8_t reserved[6];
} ovf_crypto_certificate_validation_request_v1;

typedef struct ovf_crypto_certificate_validation_result_v1 {
  size_t struct_size;
  uint8_t valid;
  uint8_t reserved[3];
  ovf_crypto_certificate_verdict_v1 verdict;
  uint32_t verified_chain_length;
  uint64_t native_status;
} ovf_crypto_certificate_validation_result_v1;

typedef struct ovf_crypto_backend_config_v1 {
  size_t struct_size;
  ovf_crypto_string_view_v1 configuration;
  uint32_t max_keys;
  uint32_t max_contexts;
} ovf_crypto_backend_config_v1;

typedef struct ovf_crypto_host_api_v1 {
  size_t struct_size;
  void* user_data;
  void (*audit)(void* user_data, uint32_t event_id, ovf_crypto_status_v1 status);
  uint64_t (*monotonic_time_ns)(void* user_data);
} ovf_crypto_host_api_v1;

typedef struct ovf_crypto_backend_v1 ovf_crypto_backend_v1;

struct ovf_crypto_backend_v1 {
  size_t struct_size;
  uint32_t abi_version;
  void* implementation;
  ovf_crypto_status_v1 (*start)(ovf_crypto_backend_v1*);
  void (*stop)(ovf_crypto_backend_v1*);
  ovf_crypto_status_v1 (*get_capabilities)(ovf_crypto_backend_v1*, ovf_crypto_capabilities_v1*);
  ovf_crypto_status_v1 (*random_bytes)(ovf_crypto_backend_v1*, ovf_crypto_mutable_bytes_v1);
  ovf_crypto_status_v1 (*key_import)(ovf_crypto_backend_v1*, const ovf_crypto_key_descriptor_v1*,
                                     ovf_crypto_key_format_v1, ovf_crypto_bytes_view_v1,
                                     ovf_crypto_handle_v1*);
  ovf_crypto_status_v1 (*key_generate)(ovf_crypto_backend_v1*, const ovf_crypto_key_descriptor_v1*,
                                       ovf_crypto_handle_v1*);
  ovf_crypto_status_v1 (*key_destroy)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1);
  ovf_crypto_status_v1 (*hash)(ovf_crypto_backend_v1*, uint32_t algorithm, ovf_crypto_bytes_view_v1,
                               ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*mac)(ovf_crypto_backend_v1*, uint32_t algorithm, ovf_crypto_handle_v1,
                              ovf_crypto_bytes_view_v1, ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*aead_encrypt)(ovf_crypto_backend_v1*, uint32_t algorithm,
                                       ovf_crypto_handle_v1, const ovf_crypto_aead_parameters_v1*,
                                       ovf_crypto_bytes_view_v1, ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*aead_decrypt)(ovf_crypto_backend_v1*, uint32_t algorithm,
                                       ovf_crypto_handle_v1, const ovf_crypto_aead_parameters_v1*,
                                       ovf_crypto_bytes_view_v1, ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*sign)(ovf_crypto_backend_v1*, uint32_t algorithm, ovf_crypto_handle_v1,
                               ovf_crypto_bytes_view_v1, ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*verify)(ovf_crypto_backend_v1*, uint32_t algorithm, ovf_crypto_handle_v1,
                                 ovf_crypto_bytes_view_v1, ovf_crypto_bytes_view_v1,
                                 uint8_t* valid);
  ovf_crypto_status_v1 (*derive)(ovf_crypto_backend_v1*, uint32_t algorithm, ovf_crypto_handle_v1,
                                 ovf_crypto_bytes_view_v1 salt, ovf_crypto_bytes_view_v1 info,
                                 ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*key_public_value)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1,
                                           ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*key_agree)(ovf_crypto_backend_v1*, uint32_t algorithm,
                                    ovf_crypto_handle_v1,
                                    ovf_crypto_bytes_view_v1 peer_public_value,
                                    ovf_crypto_bytes_view_v1 salt,
                                    const ovf_crypto_key_descriptor_v1* derived_key,
                                    ovf_crypto_handle_v1*);
  ovf_crypto_status_v1 (*certificate_validate)(ovf_crypto_backend_v1*,
                                               const ovf_crypto_certificate_validation_request_v1*,
                                               ovf_crypto_certificate_validation_result_v1*);
  ovf_crypto_status_v1 (*stream_create)(ovf_crypto_backend_v1*,
                                        const ovf_crypto_stream_descriptor_v1*,
                                        ovf_crypto_handle_v1*);
  ovf_crypto_status_v1 (*stream_update)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1,
                                        ovf_crypto_bytes_view_v1);
  ovf_crypto_status_v1 (*stream_finish)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1,
                                        ovf_crypto_bytes_view_v1 terminal_input,
                                        ovf_crypto_mutable_bytes_v1*, uint8_t* valid);
  ovf_crypto_status_v1 (*stream_destroy)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1);
  ovf_crypto_status_v1 (*stream_process_record)(ovf_crypto_backend_v1*, ovf_crypto_handle_v1,
                                                ovf_crypto_bytes_view_v1,
                                                ovf_crypto_mutable_bytes_v1*);
  ovf_crypto_status_v1 (*last_error)(ovf_crypto_backend_v1*, ovf_crypto_mutable_bytes_v1* message);
};

typedef struct ovf_crypto_backend_factory_v1 {
  size_t struct_size;
  uint32_t abi_version;
  ovf_crypto_string_view_v1 name;
  ovf_crypto_status_v1 (*create)(const ovf_crypto_host_api_v1*, const ovf_crypto_backend_config_v1*,
                                 ovf_crypto_backend_v1**);
  void (*destroy)(ovf_crypto_backend_v1*);
} ovf_crypto_backend_factory_v1;

typedef const ovf_crypto_backend_factory_v1* (*ovf_crypto_backend_query_fn_v1)(void);

#ifdef __cplusplus
}
#endif

#endif
