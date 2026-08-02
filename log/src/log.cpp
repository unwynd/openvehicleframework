// SPDX-License-Identifier: Apache-2.0

#include "ovf/log/log.hpp"
#include "ovf/log/sink.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ovf::log {
namespace {

template <std::size_t Size, typename Length>
bool CopyText(std::array<char, Size>& destination, std::string_view source,
              Length& length) noexcept {
  if (source.empty() || source.size() >= Size) {
    return false;
  }
  std::memcpy(destination.data(), source.data(), source.size());
  destination[source.size()] = '\0';
  length = static_cast<Length>(source.size());
  return true;
}

bool IsCritical(Level level) noexcept { return level == Level::fatal || level == Level::error; }

} // namespace

Field Field::Boolean(std::string_view name, bool value, Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::boolean;
  field.sensitivity_ = sensitivity;
  field.value_.boolean = value;
  return field;
}

Field Field::Signed(std::string_view name, std::int64_t value, Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::signed_integer;
  field.sensitivity_ = sensitivity;
  field.value_.signed_integer = value;
  return field;
}

Field Field::Unsigned(std::string_view name, std::uint64_t value,
                      Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::unsigned_integer;
  field.sensitivity_ = sensitivity;
  field.value_.unsigned_integer = value;
  return field;
}

Field Field::Floating(std::string_view name, double value, Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::floating_point;
  field.sensitivity_ = sensitivity;
  field.value_.floating_point = value;
  return field;
}

Field Field::Text(std::string_view name, std::string_view value, Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::text;
  field.sensitivity_ = sensitivity;
  field.text_ = value;
  return field;
}

Field Field::Binary(std::string_view name, std::span<const std::byte> value,
                    Sensitivity sensitivity) noexcept {
  Field field;
  field.name_ = name;
  field.type_ = Type::binary;
  field.sensitivity_ = sensitivity;
  field.binary_ = value;
  return field;
}

namespace detail {

class RuntimeState final {
public:
  RuntimeState(RuntimeConfig config, std::unique_ptr<Sink> sink)
      : config_(config), application_name_(config.application_name), sink_(std::move(sink)),
        queue_(config.queue_capacity) {}

  bool Start() noexcept {
    if (application_name_.empty() || application_name_.size() > kMaxLoggerName || queue_.empty() ||
        config_.critical_reserve >= queue_.size() || sink_ == nullptr ||
        !sink_->Start(application_name_)) {
      return false;
    }
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { Dispatch(); });
    return true;
  }

  bool Enabled(Level level) const noexcept {
    return static_cast<unsigned>(level) <=
           static_cast<unsigned>(level_.load(std::memory_order_relaxed));
  }

  WriteResult SubmitEvent(std::string_view logger, Event event,
                          std::span<const Field> fields) noexcept {
    if (event.id == 0 || event.name.empty() || event.name.size() > kMaxEventName)
      return Invalid();
    return Submit(logger, RecordKind::event, event.id, event.name, event.level, fields);
  }

  WriteResult SubmitDiagnostic(std::string_view logger, Level level, std::string_view message,
                               std::span<const Field> fields) noexcept {
    if (message.empty() || message.size() > kMaxDiagnosticMessage)
      return Invalid();
    return Submit(logger, RecordKind::diagnostic, 0, message, level, fields);
  }

  WriteResult Submit(std::string_view logger, RecordKind kind, std::uint32_t event_id,
                     std::string_view text, Level level, std::span<const Field> fields) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
      return WriteResult::shutting_down;
    }
    if (!Enabled(level)) {
      filtered_.fetch_add(1, std::memory_order_relaxed);
      return WriteResult::filtered;
    }
    Record record;
    if (!Build(record, logger, kind, event_id, text, level, fields)) {
      invalid_.fetch_add(1, std::memory_order_relaxed);
      return WriteResult::invalid;
    }
    std::unique_lock lock(mutex_);
    const auto usable =
        IsCritical(level) ? queue_.size() : queue_.size() - config_.critical_reserve;
    if (count_ >= usable) {
      if (!IsCritical(level) || !space_.wait_for(lock, config_.producer_wait, [this] {
            return count_ < queue_.size() || !running_.load(std::memory_order_relaxed);
          })) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return WriteResult::dropped;
      }
    }
    if (!running_.load(std::memory_order_relaxed)) {
      return WriteResult::shutting_down;
    }
    queue_[tail_] = record;
    tail_ = (tail_ + 1) % queue_.size();
    ++count_;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    data_.notify_one();
    return WriteResult::accepted;
  }

  void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }

  Health GetHealth() const noexcept {
    return {accepted_.load(), filtered_.load(), dropped_.load(), invalid_.load(),
            binding_errors_.load()};
  }

  bool Flush(std::chrono::milliseconds timeout) noexcept {
    std::unique_lock lock(mutex_);
    if (!empty_.wait_for(lock, timeout, [this] { return count_ == 0 && !dispatching_; })) {
      return false;
    }
    lock.unlock();
    return sink_->Flush(timeout);
  }

  void Stop() noexcept {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
      return;
    }
    data_.notify_all();
    space_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    static_cast<void>(sink_->Flush(config_.shutdown_flush));
    sink_->Stop();
  }

private:
  WriteResult Invalid() noexcept {
    invalid_.fetch_add(1, std::memory_order_relaxed);
    return WriteResult::invalid;
  }

  bool Build(Record& output, std::string_view logger, RecordKind kind, std::uint32_t event_id,
             std::string_view text, Level level, std::span<const Field> fields) noexcept {
    if (fields.size() > kMaxFields || !CopyText(output.logger, logger, output.logger_size) ||
        !CopyText(output.text, text, output.text_size)) {
      return false;
    }
    output.kind = kind;
    output.event_id = event_id;
    output.level = level;
    output.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    output.wall_time = std::chrono::system_clock::now();
    output.monotonic_time = std::chrono::steady_clock::now();
    output.field_count = static_cast<std::uint8_t>(fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      auto const& source = fields[index];
      auto& target = output.fields[index];
      std::uint8_t ignored{};
      if (!CopyText(target.name, source.name(), ignored)) {
        return false;
      }
      target.type = source.type();
      target.sensitivity = source.sensitivity();
      switch (source.type()) {
      case Field::Type::boolean:
        target.value.boolean = source.boolean();
        break;
      case Field::Type::signed_integer:
        target.value.signed_integer = source.signed_integer();
        break;
      case Field::Type::unsigned_integer:
        target.value.unsigned_integer = source.unsigned_integer();
        break;
      case Field::Type::floating_point:
        target.value.floating_point = source.floating_point();
        break;
      case Field::Type::text: {
        if (source.text().size() > kMaxStringValue)
          return false;
        target.size = static_cast<std::uint16_t>(source.text().size());
        std::memcpy(target.bytes.data(), source.text().data(), target.size);
        break;
      }
      case Field::Type::binary: {
        if (source.binary().size() > target.bytes.size())
          return false;
        target.size = static_cast<std::uint16_t>(source.binary().size());
        std::memcpy(target.bytes.data(), source.binary().data(), target.size);
        break;
      }
      }
    }
    return true;
  }

  void Dispatch() noexcept {
    for (;;) {
      Record record;
      {
        std::unique_lock lock(mutex_);
        data_.wait(lock,
                   [this] { return count_ != 0 || !running_.load(std::memory_order_relaxed); });
        if (count_ == 0 && !running_.load(std::memory_order_relaxed))
          break;
        record = queue_[head_];
        head_ = (head_ + 1) % queue_.size();
        --count_;
        dispatching_ = true;
        space_.notify_all();
      }
      if (!sink_->Write(record))
        binding_errors_.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard lock(mutex_);
        dispatching_ = false;
        if (count_ == 0)
          empty_.notify_all();
      }
    }
  }

  RuntimeConfig config_;
  std::string application_name_;
  std::unique_ptr<Sink> sink_;
  std::vector<Record> queue_;
  mutable std::mutex mutex_;
  std::condition_variable data_;
  std::condition_variable space_;
  std::condition_variable empty_;
  std::thread worker_;
  std::size_t head_{};
  std::size_t tail_{};
  std::size_t count_{};
  bool dispatching_{};
  std::atomic<bool> running_{};
  std::atomic<Level> level_{config_.initial_level};
  std::atomic<std::uint64_t> sequence_{};
  std::atomic<std::uint64_t> accepted_{};
  std::atomic<std::uint64_t> filtered_{};
  std::atomic<std::uint64_t> dropped_{};
  std::atomic<std::uint64_t> invalid_{};
  std::atomic<std::uint64_t> binding_errors_{};
};

} // namespace detail

Logger::Logger(std::shared_ptr<detail::RuntimeState> state, std::string_view name) noexcept
    : state_(std::move(state)) {
  static_cast<void>(CopyText(name_, name, name_size_));
}

bool Logger::Enabled(Level level) const noexcept { return state_ && state_->Enabled(level); }

WriteResult Logger::Event(ovf::log::Event event, std::span<const Field> fields) const noexcept {
  if (!state_ || name_size_ == 0)
    return WriteResult::invalid;
  return state_->SubmitEvent(std::string_view(name_.data(), name_size_), event, fields);
}

WriteResult Logger::Log(Level level, std::string_view message,
                        std::span<const Field> fields) const noexcept {
  if (!state_ || name_size_ == 0)
    return WriteResult::invalid;
  return state_->SubmitDiagnostic(std::string_view(name_.data(), name_size_), level, message,
                                  fields);
}

std::unique_ptr<Runtime> Runtime::Create(RuntimeConfig config,
                                         std::unique_ptr<Sink> sink) noexcept {
  try {
    auto state = std::make_shared<detail::RuntimeState>(config, std::move(sink));
    if (!state->Start())
      return nullptr;
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
  } catch (...) {
    return nullptr;
  }
}

Runtime::Runtime(std::shared_ptr<detail::RuntimeState> state) noexcept : state_(std::move(state)) {}
Runtime::~Runtime() { Stop(); }
Logger Runtime::CreateLogger(std::string_view name) noexcept { return Logger(state_, name); }
void Runtime::SetLevel(Level level) noexcept { state_->SetLevel(level); }
Health Runtime::GetHealth() const noexcept { return state_->GetHealth(); }
bool Runtime::Flush(std::chrono::milliseconds timeout) noexcept { return state_->Flush(timeout); }
void Runtime::Stop() noexcept {
  if (state_)
    state_->Stop();
}

} // namespace ovf::log
