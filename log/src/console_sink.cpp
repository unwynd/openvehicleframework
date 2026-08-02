// SPDX-License-Identifier: Apache-2.0

#include "ovf/log/sink.hpp"

#include <cstdio>

namespace ovf::log {
namespace {
const char* Name(Level level) noexcept {
  switch (level) {
  case Level::fatal:
    return "FATAL";
  case Level::error:
    return "ERROR";
  case Level::warning:
    return "WARN";
  case Level::info:
    return "INFO";
  case Level::debug:
    return "DEBUG";
  case Level::trace:
    return "TRACE";
  }
  return "UNKNOWN";
}
} // namespace

bool ConsoleSink::Start(std::string_view) noexcept { return true; }

bool ConsoleSink::Write(Record const& record) noexcept {
  if (record.kind == RecordKind::event)
    return std::fprintf(stderr, "%s [%.*s] event=%08x %.*s sequence=%llu\n", Name(record.level),
                        record.logger_size, record.logger.data(), record.event_id, record.text_size,
                        record.text.data(), static_cast<unsigned long long>(record.sequence)) >= 0;
  return std::fprintf(stderr, "%s [%.*s] %.*s sequence=%llu\n", Name(record.level),
                      record.logger_size, record.logger.data(), record.text_size,
                      record.text.data(), static_cast<unsigned long long>(record.sequence)) >= 0;
}

bool ConsoleSink::Flush(std::chrono::milliseconds) noexcept { return std::fflush(stderr) == 0; }
void ConsoleSink::Stop() noexcept {}

} // namespace ovf::log
