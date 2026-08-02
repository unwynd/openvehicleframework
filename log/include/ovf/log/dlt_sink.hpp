// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/log/sink.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace ovf::log {

struct DltContextMapping final {
  std::string_view logger;
  std::string_view id;
  std::string_view description;
};

struct DltSinkConfig final {
  std::string_view application_id;
  std::string_view application_description;
  std::span<const DltContextMapping> contexts;
  bool verbose{false};
  std::chrono::milliseconds shutdown_flush{250};
};

[[nodiscard]] std::unique_ptr<Sink> CreateDltSink(DltSinkConfig config) noexcept;

} // namespace ovf::log
