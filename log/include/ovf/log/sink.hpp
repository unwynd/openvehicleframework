// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/log/log.hpp"

#include <array>
#include <chrono>
#include <span>
#include <string_view>

namespace ovf::log {

enum class RecordKind : std::uint8_t { diagnostic, event };

struct StoredField final {
  std::array<char, kMaxFieldName + 1> name{};
  std::array<std::byte, kMaxBinaryValue> bytes{};
  std::uint16_t size{};
  Field::Type type{Field::Type::boolean};
  Sensitivity sensitivity{Sensitivity::public_value};
  union Value {
    bool boolean;
    std::int64_t signed_integer;
    std::uint64_t unsigned_integer;
    double floating_point;
    constexpr Value() : unsigned_integer(0) {}
  } value;
};

struct Record final {
  std::array<char, kMaxLoggerName + 1> logger{};
  std::array<char, kMaxDiagnosticMessage + 1> text{};
  std::array<StoredField, kMaxFields> fields{};
  std::chrono::system_clock::time_point wall_time{};
  std::chrono::steady_clock::time_point monotonic_time{};
  std::uint64_t sequence{};
  std::uint32_t event_id{};
  std::uint8_t logger_size{};
  std::uint16_t text_size{};
  std::uint8_t field_count{};
  Level level{Level::info};
  RecordKind kind{RecordKind::diagnostic};
};

class Sink {
public:
  virtual ~Sink() = default;
  virtual bool Start(std::string_view application_name) noexcept = 0;
  virtual bool Write(Record const& record) noexcept = 0;
  virtual bool Flush(std::chrono::milliseconds timeout) noexcept = 0;
  virtual void Stop() noexcept = 0;
};

class ConsoleSink final : public Sink {
public:
  bool Start(std::string_view application_name) noexcept override;
  bool Write(Record const& record) noexcept override;
  bool Flush(std::chrono::milliseconds timeout) noexcept override;
  void Stop() noexcept override;
};

} // namespace ovf::log
