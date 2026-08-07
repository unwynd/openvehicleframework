// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/core/result.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace ovf::exec {

enum class ErrorCode : std::uint16_t {
  none,
  invalid_argument,
  invalid_identifier,
  invalid_transition,
  already_exists,
  not_found,
  permission_denied,
  busy,
  superseded,
  cancelled,
  deadline_exceeded,
  resource_exhausted,
  configuration_error,
  communication_error,
  backend_unavailable,
  backend_error,
  persistence_error,
  incompatible_abi,
  unsupported,
  internal_error
};

struct Error final {
  ErrorCode code{ErrorCode::none};
  std::string message;
  std::uint64_t support_data{};

  [[nodiscard]] explicit operator bool() const noexcept { return code != ErrorCode::none; }
};

template <typename T> using Result = ovf::core::Result<T, Error>;

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message,
                                     std::uint64_t support_data = 0U) {
  return Error{code, std::move(message), support_data};
}

} // namespace ovf::exec
