// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ovf::core {

// Result<T, E> is the shared success/error return type used across all
// clusters. Each cluster keeps its own Error struct and passes it as E; this
// template unifies the accessors, ownership rules, and monadic combinators.
//
// Contract:
//   * Result is implicitly constructible from either a T or an E.
//   * value() and error() assert in debug builds; production code must gate
//     access with has_value() or operator bool().
//   * Result<void, E> is a partial specialization that only carries an error.
//   * map/and_then/or_else follow the usual sum-type semantics: map transforms
//     the success payload, and_then chains a Result-returning function, and
//     or_else recovers from an error.

template <typename T, typename E> class [[nodiscard]] Result final {
public:
  using value_type = T;
  using error_type = E;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & {
    assert(has_value());
    return std::get<0>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    assert(has_value());
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    assert(has_value());
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const E& error() const& {
    assert(!has_value());
    return std::get<1>(storage_);
  }
  [[nodiscard]] E&& error() && {
    assert(!has_value());
    return std::get<1>(std::move(storage_));
  }

  template <typename Fn> auto map(Fn&& fn) && {
    using Mapped = std::invoke_result_t<Fn, T&&>;
    if (has_value()) {
      return Result<Mapped, E>{std::forward<Fn>(fn)(std::move(*this).value())};
    }
    return Result<Mapped, E>{std::move(*this).error()};
  }

  template <typename Fn> auto and_then(Fn&& fn) && {
    using Next = std::invoke_result_t<Fn, T&&>;
    static_assert(std::is_same_v<typename Next::error_type, E>,
                  "and_then must return a Result with the same error type");
    if (has_value()) {
      return Next{std::forward<Fn>(fn)(std::move(*this).value())};
    }
    return Next{std::move(*this).error()};
  }

  template <typename Fn> auto or_else(Fn&& fn) && {
    using Next = std::invoke_result_t<Fn, E&&>;
    static_assert(std::is_same_v<typename Next::value_type, T>,
                  "or_else must return a Result with the same value type");
    if (has_value()) {
      return Next{std::move(*this).value()};
    }
    return Next{std::forward<Fn>(fn)(std::move(*this).error())};
  }

private:
  std::variant<T, E> storage_;
};

template <typename E> class [[nodiscard]] Result<void, E> final {
public:
  using value_type = void;
  using error_type = E;

  Result() = default;
  Result(E error) : error_(std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const E& error() const& {
    assert(!has_value());
    return *error_;
  }
  [[nodiscard]] E&& error() && {
    assert(!has_value());
    return std::move(*error_);
  }

  template <typename Fn> auto and_then(Fn&& fn) && {
    using Next = std::invoke_result_t<Fn>;
    static_assert(std::is_same_v<typename Next::error_type, E>,
                  "and_then must return a Result with the same error type");
    if (has_value()) {
      return Next{std::forward<Fn>(fn)()};
    }
    return Next{std::move(*this).error()};
  }

  template <typename Fn> auto or_else(Fn&& fn) && {
    using Next = std::invoke_result_t<Fn, E&&>;
    static_assert(std::is_same_v<typename Next::value_type, void>,
                  "or_else must return a Result<void, E>");
    if (has_value()) {
      return Next{};
    }
    return Next{std::forward<Fn>(fn)(std::move(*this).error())};
  }

private:
  std::optional<E> error_;
};

} // namespace ovf::core
