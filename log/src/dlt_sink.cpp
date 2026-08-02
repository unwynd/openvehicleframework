// SPDX-License-Identifier: Apache-2.0

#include "ovf/log/dlt_sink.hpp"

#include <dlt/dlt.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ovf::log {
namespace {

std::atomic_bool dlt_instance_active{};

DltLogLevelType Convert(Level level) noexcept {
  switch (level) {
  case Level::fatal:
    return DLT_LOG_FATAL;
  case Level::error:
    return DLT_LOG_ERROR;
  case Level::warning:
    return DLT_LOG_WARN;
  case Level::info:
    return DLT_LOG_INFO;
  case Level::debug:
    return DLT_LOG_DEBUG;
  case Level::trace:
    return DLT_LOG_VERBOSE;
  }
  return DLT_LOG_OFF;
}

struct Context final {
  std::string logger;
  std::string id;
  std::string description;
  DltContext handle{};
};

class DltSink final : public Sink {
public:
  explicit DltSink(DltSinkConfig config)
      : application_id_(config.application_id), description_(config.application_description),
        verbose_(config.verbose), shutdown_flush_(config.shutdown_flush) {
    contexts_.reserve(config.contexts.size());
    for (auto const& mapping : config.contexts) {
      contexts_.push_back({std::string(mapping.logger),
                           std::string(mapping.id),
                           std::string(mapping.description),
                           {}});
    }
  }

  bool Start(std::string_view) noexcept override {
    bool expected = false;
    if (application_id_.size() != DLT_ID_SIZE || contexts_.empty() ||
        !dlt_instance_active.compare_exchange_strong(expected, true))
      return false;
    for (std::size_t left = 0; left < contexts_.size(); ++left) {
      if (contexts_[left].logger.empty() || contexts_[left].id.size() != DLT_ID_SIZE) {
        dlt_instance_active.store(false);
        return false;
      }
      for (std::size_t right = left + 1; right < contexts_.size(); ++right) {
        if (contexts_[left].logger == contexts_[right].logger ||
            contexts_[left].id == contexts_[right].id) {
          dlt_instance_active.store(false);
          return false;
        }
      }
    }
    if (dlt_init() < DLT_RETURN_OK) {
      dlt_instance_active.store(false);
      return false;
    }
    if (dlt_register_app(application_id_.c_str(), description_.c_str()) < DLT_RETURN_OK) {
      dlt_free();
      dlt_instance_active.store(false);
      return false;
    }
    dlt_set_resend_timeout_atexit(static_cast<std::uint32_t>(shutdown_flush_.count()));
    if (verbose_)
      dlt_verbose_mode();
    else
      dlt_nonverbose_mode();
    std::size_t registered{};
    for (auto& context : contexts_) {
      if (context.id.size() != DLT_ID_SIZE ||
          dlt_register_context(&context.handle, context.id.c_str(), context.description.c_str()) <
              DLT_RETURN_OK) {
        while (registered != 0)
          dlt_unregister_context(&contexts_[--registered].handle);
        dlt_unregister_app();
        dlt_free();
        dlt_instance_active.store(false);
        return false;
      }
      ++registered;
    }
    started_ = true;
    return true;
  }

  bool Write(Record const& record) noexcept override {
    auto* context = Find(std::string_view(record.logger.data(), record.logger_size));
    if (context == nullptr)
      return false;
    std::size_t encoded_upper_bound = 64;
    for (std::size_t index = 0; index < record.field_count; ++index) {
      auto const& field = record.fields[index];
      encoded_upper_bound += std::strlen(field.name.data()) + 16;
      if (field.type == Field::Type::text || field.type == Field::Type::binary)
        encoded_upper_bound += field.size;
      else
        encoded_upper_bound += sizeof(std::uint64_t);
    }
    if (encoded_upper_bound > 1200)
      return false;
    DltContextData data{};
    if (!verbose_ && record.kind == RecordKind::diagnostic)
      return false;
    const auto start =
        verbose_ ? dlt_user_log_write_start(&context->handle, &data, Convert(record.level))
                 : dlt_user_log_write_start_id(&context->handle, &data, Convert(record.level),
                                               record.event_id);
    if (start == DLT_RETURN_OK)
      return true;
    if (start != DLT_RETURN_TRUE)
      return false;
    auto const* attribute = record.kind == RecordKind::event ? "event" : "message";
    if (verbose_ && dlt_user_log_write_sized_utf8_string_attr(
                        &data, record.text.data(), record.text_size, attribute) < DLT_RETURN_OK)
      return false;
    for (std::size_t index = 0; index < record.field_count; ++index) {
      auto const& field = record.fields[index];
      if (field.sensitivity != Sensitivity::public_value) {
        if (dlt_user_log_write_constant_string_attr(&data, "[REDACTED]", field.name.data()) <
            DLT_RETURN_OK)
          return false;
        continue;
      }
      DltReturnValue result = DLT_RETURN_ERROR;
      switch (field.type) {
      case Field::Type::boolean:
        result = dlt_user_log_write_bool_attr(&data, field.value.boolean, field.name.data());
        break;
      case Field::Type::signed_integer:
        result = dlt_user_log_write_int64_attr(&data, field.value.signed_integer, field.name.data(),
                                               nullptr);
        break;
      case Field::Type::unsigned_integer:
        result = dlt_user_log_write_uint64_attr(&data, field.value.unsigned_integer,
                                                field.name.data(), nullptr);
        break;
      case Field::Type::floating_point:
        result = dlt_user_log_write_float64_attr(&data, field.value.floating_point,
                                                 field.name.data(), nullptr);
        break;
      case Field::Type::text:
        result = dlt_user_log_write_sized_utf8_string_attr(
            &data, reinterpret_cast<char const*>(field.bytes.data()), field.size,
            field.name.data());
        break;
      case Field::Type::binary:
        result =
            dlt_user_log_write_raw_attr(&data, field.bytes.data(), field.size, field.name.data());
        break;
      }
      if (result < DLT_RETURN_OK)
        return false;
    }
    return dlt_user_log_write_finish(&data) >= DLT_RETURN_OK;
  }

  bool Flush(std::chrono::milliseconds) noexcept override { return true; }

  void Stop() noexcept override {
    if (!started_)
      return;
    for (auto& context : contexts_)
      dlt_unregister_context(&context.handle);
    dlt_unregister_app_flush_buffered_logs();
    dlt_free();
    started_ = false;
    dlt_instance_active.store(false);
  }

private:
  Context* Find(std::string_view logger) noexcept {
    for (auto& context : contexts_)
      if (context.logger == logger)
        return &context;
    return nullptr;
  }

  std::string application_id_;
  std::string description_;
  std::vector<Context> contexts_;
  bool verbose_{};
  std::chrono::milliseconds shutdown_flush_;
  bool started_{};
};

} // namespace

std::unique_ptr<Sink> CreateDltSink(DltSinkConfig config) noexcept {
  try {
    return std::make_unique<DltSink>(config);
  } catch (...) {
    return nullptr;
  }
}

} // namespace ovf::log
