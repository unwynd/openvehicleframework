// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace ovf::log {

inline constexpr std::size_t kMaxLoggerName = 63;
inline constexpr std::size_t kMaxEventName = 63;
inline constexpr std::size_t kMaxDiagnosticMessage = 255;
inline constexpr std::size_t kMaxFieldName = 31;
inline constexpr std::size_t kMaxStringValue = 255;
inline constexpr std::size_t kMaxBinaryValue = 256;
inline constexpr std::size_t kMaxFields = 16;

enum class Level : std::uint8_t { fatal, error, warning, info, debug, trace };
enum class WriteResult : std::uint8_t { accepted, filtered, dropped, invalid, shutting_down };
enum class Sensitivity : std::uint8_t { public_value, confidential, secret };

struct Event final {
  std::uint32_t id;
  std::string_view name;
  Level level;
};

class Field final {
public:
  enum class Type : std::uint8_t {
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    text,
    binary
  };

  static Field Boolean(std::string_view name, bool value,
                       Sensitivity sensitivity = Sensitivity::public_value) noexcept;
  static Field Signed(std::string_view name, std::int64_t value,
                      Sensitivity sensitivity = Sensitivity::public_value) noexcept;
  static Field Unsigned(std::string_view name, std::uint64_t value,
                        Sensitivity sensitivity = Sensitivity::public_value) noexcept;
  static Field Floating(std::string_view name, double value,
                        Sensitivity sensitivity = Sensitivity::public_value) noexcept;
  static Field Text(std::string_view name, std::string_view value,
                    Sensitivity sensitivity = Sensitivity::public_value) noexcept;
  static Field Binary(std::string_view name, std::span<const std::byte> value,
                      Sensitivity sensitivity = Sensitivity::public_value) noexcept;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] Sensitivity sensitivity() const noexcept { return sensitivity_; }
  [[nodiscard]] bool boolean() const noexcept { return value_.boolean; }
  [[nodiscard]] std::int64_t signed_integer() const noexcept { return value_.signed_integer; }
  [[nodiscard]] std::uint64_t unsigned_integer() const noexcept { return value_.unsigned_integer; }
  [[nodiscard]] double floating_point() const noexcept { return value_.floating_point; }
  [[nodiscard]] std::string_view text() const noexcept { return text_; }
  [[nodiscard]] std::span<const std::byte> binary() const noexcept { return binary_; }

private:
  union Value {
    bool boolean;
    std::int64_t signed_integer;
    std::uint64_t unsigned_integer;
    double floating_point;
    constexpr Value() : unsigned_integer(0) {}
  } value_;
  std::string_view name_;
  std::string_view text_;
  std::span<const std::byte> binary_;
  Type type_{Type::boolean};
  Sensitivity sensitivity_{Sensitivity::public_value};
};

struct RuntimeConfig final {
  std::string_view application_name;
  std::size_t queue_capacity{4096};
  std::size_t critical_reserve{64};
  std::chrono::milliseconds producer_wait{5};
  std::chrono::milliseconds shutdown_flush{250};
  Level initial_level{Level::info};
};

struct Health final {
  std::uint64_t accepted{};
  std::uint64_t filtered{};
  std::uint64_t dropped{};
  std::uint64_t invalid{};
  std::uint64_t binding_errors{};
};

class Sink;
namespace detail {
class RuntimeState;
}

class Logger final {
public:
  Logger() = default;
  [[nodiscard]] bool Enabled(Level level) const noexcept;
  [[nodiscard]] WriteResult Event(ovf::log::Event event,
                                  std::span<const Field> fields = {}) const noexcept;
  [[nodiscard]] WriteResult Log(Level level, std::string_view message,
                                std::span<const Field> fields = {}) const noexcept;

  template <typename... Fields>
  [[nodiscard]] WriteResult Event(ovf::log::Event event, Fields const&... fields) const noexcept {
    const std::array<Field, sizeof...(Fields)> values{fields...};
    return Event(event, std::span<const Field>(values));
  }

  template <typename... Fields>
  [[nodiscard]] WriteResult Log(Level level, std::string_view message,
                                Fields const&... fields) const noexcept {
    const std::array<Field, sizeof...(Fields)> values{fields...};
    return Log(level, message, std::span<const Field>(values));
  }

  // Level-shortcut helpers do not mark WriteResult as [[nodiscard]] because most
  // call sites are fire-and-forget log statements; use Event() or Log() when
  // the WriteResult (drop accounting) matters.
  template <typename... Fields>
  WriteResult Fatal(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::fatal, message, fields...);
  }
  template <typename... Fields>
  WriteResult Error(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::error, message, fields...);
  }
  template <typename... Fields>
  WriteResult Warning(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::warning, message, fields...);
  }
  template <typename... Fields>
  WriteResult Info(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::info, message, fields...);
  }
  template <typename... Fields>
  WriteResult Debug(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::debug, message, fields...);
  }
  template <typename... Fields>
  WriteResult Trace(std::string_view message, Fields const&... fields) const noexcept {
    return Log(Level::trace, message, fields...);
  }

private:
  friend class Runtime;
  Logger(std::shared_ptr<detail::RuntimeState> state, std::string_view name) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
  std::array<char, kMaxLoggerName + 1> name_{};
  std::uint8_t name_size_{};
};

class Runtime final {
public:
  static std::unique_ptr<Runtime> Create(RuntimeConfig config, std::unique_ptr<Sink> sink) noexcept;
  ~Runtime();
  Runtime(Runtime const&) = delete;
  Runtime& operator=(Runtime const&) = delete;

  [[nodiscard]] Logger CreateLogger(std::string_view name) noexcept;
  void SetLevel(Level level) noexcept;
  [[nodiscard]] Health GetHealth() const noexcept;
  [[nodiscard]] bool Flush(std::chrono::milliseconds timeout) noexcept;
  void Stop() noexcept;

private:
  explicit Runtime(std::shared_ptr<detail::RuntimeState> state) noexcept;
  std::shared_ptr<detail::RuntimeState> state_;
};

} // namespace ovf::log
