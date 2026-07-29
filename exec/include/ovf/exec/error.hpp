// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

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

template <typename T> class Result final {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) { assert(std::get<Error>(storage_)); }

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & {
    assert(has_value());
    return std::get<T>(storage_);
  }

  [[nodiscard]] const T& value() const& {
    assert(has_value());
    return std::get<T>(storage_);
  }

  [[nodiscard]] T&& value() && {
    assert(has_value());
    return std::get<T>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    assert(!has_value());
    return std::get<Error>(storage_);
  }

private:
  std::variant<T, Error> storage_;
};

template <> class Result<void> final {
public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) { assert(error_); }

  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const Error& error() const& {
    assert(error_.has_value());
    return *error_;
  }

private:
  std::optional<Error> error_;
};

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message,
                                     std::uint64_t support_data = 0U) {
  return Error{code, std::move(message), support_data};
}

} // namespace ovf::exec
