// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/file_journal.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ovf::exec::detail {
namespace {

constexpr std::uint32_t kMagic = 0x4A46564FU;
constexpr std::uint16_t kFormatVersion = 1U;
constexpr std::size_t kHeaderSize = 16U;
constexpr std::size_t kMinimumMaximumRecordSize = 4096U;

class Encoder final {
public:
  template <typename T> bool Integer(T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const auto converted = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      bytes_.push_back(static_cast<std::uint8_t>(converted >> (index * 8U)));
    }
    return true;
  }

  bool String(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    Integer(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  template <typename IdentifierType> bool Id(IdentifierType id) { return Integer(id.value()); }

  const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
  std::vector<std::uint8_t> bytes_;
};

class Decoder final {
public:
  explicit Decoder(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  template <typename T> bool Integer(T& value) {
    if (remaining() < sizeof(T)) {
      return false;
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned decoded{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      decoded |= static_cast<Unsigned>(bytes_[offset_ + index]) << (index * 8U);
    }
    value = static_cast<T>(decoded);
    offset_ += sizeof(T);
    return true;
  }

  bool String(std::string& value) {
    std::uint32_t length{};
    if (!Integer(length) || length > remaining()) {
      return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
    offset_ += length;
    return true;
  }

  template <typename IdentifierType> bool Id(IdentifierType& id) {
    std::uint64_t value{};
    if (!Integer(value)) {
      return false;
    }
    id = IdentifierType{value};
    return true;
  }

  std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_{};
};

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

template <typename IdentifierType>
bool EncodeIds(Encoder& encoder, const std::vector<IdentifierType>& values) {
  if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  encoder.Integer(static_cast<std::uint32_t>(values.size()));
  for (const auto value : values) {
    encoder.Id(value);
  }
  return true;
}

template <typename IdentifierType>
bool DecodeIds(Decoder& decoder, std::vector<IdentifierType>& values) {
  std::uint32_t count{};
  if (!decoder.Integer(count) || count > decoder.remaining() / sizeof(std::uint64_t)) {
    return false;
  }
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    IdentifierType value;
    if (!decoder.Id(value)) {
      return false;
    }
    values.push_back(value);
  }
  return true;
}

Result<std::vector<std::uint8_t>> Encode(const JournalEvent& event, std::size_t maximum_size) {
  try {
    Encoder encoder;
    encoder.Integer(event.generation.value);
    encoder.Id(event.transition.id);
    encoder.Id(event.transition.domain);
    encoder.Id(event.transition.source_mode);
    encoder.Id(event.transition.target_mode);
    encoder.Integer(static_cast<std::uint8_t>(event.transition.phase));
    const auto updated = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             event.transition.updated_at.time_since_epoch())
                             .count();
    encoder.Integer(static_cast<std::int64_t>(updated));
    encoder.Integer(static_cast<std::uint8_t>(event.transition.failure.has_value()));
    if (event.transition.failure) {
      const auto& failure = *event.transition.failure;
      encoder.Integer(static_cast<std::uint16_t>(failure.error.code));
      encoder.Integer(failure.error.support_data);
      if (!encoder.String(failure.error.message)) {
        return MakeError(ErrorCode::resource_exhausted, "journal error message is too large");
      }
      encoder.Integer(static_cast<std::uint8_t>(failure.application.has_value()));
      if (failure.application) {
        encoder.Id(*failure.application);
      }
      encoder.Integer(failure.evidence.exit_code);
      encoder.Integer(failure.evidence.signal);
      encoder.Integer(failure.evidence.native_code);
      if (!encoder.String(failure.evidence.message)) {
        return MakeError(ErrorCode::resource_exhausted, "journal evidence is too large");
      }
    }
    encoder.Integer(static_cast<std::uint8_t>(event.plan.has_value()));
    if (event.plan) {
      const auto& plan = *event.plan;
      encoder.Integer(plan.generation.value);
      encoder.Id(plan.domain);
      encoder.Id(plan.source_mode);
      encoder.Id(plan.target_mode);
      if (!EncodeIds(encoder, plan.retain) || !EncodeIds(encoder, plan.stop) ||
          !EncodeIds(encoder, plan.start) || !EncodeIds(encoder, plan.guarded_domains) ||
          !EncodeIds(encoder, plan.affected_resources)) {
        return MakeError(ErrorCode::resource_exhausted, "journal plan is too large");
      }
    }
    if (encoder.bytes().size() > maximum_size) {
      return MakeError(ErrorCode::resource_exhausted, "journal record exceeds configured limit");
    }
    return encoder.bytes();
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot encode journal record");
  }
}

Result<JournalEvent> Decode(std::span<const std::uint8_t> bytes) {
  try {
    Decoder decoder(bytes);
    JournalEvent event;
    std::uint8_t phase{};
    std::int64_t updated{};
    std::uint8_t has_failure{};
    if (!decoder.Integer(event.generation.value) || !decoder.Id(event.transition.id) ||
        !decoder.Id(event.transition.domain) || !decoder.Id(event.transition.source_mode) ||
        !decoder.Id(event.transition.target_mode) || !decoder.Integer(phase) ||
        !decoder.Integer(updated) || !decoder.Integer(has_failure)) {
      return MakeError(ErrorCode::persistence_error, "journal transition record is truncated");
    }
    if (phase > static_cast<std::uint8_t>(TransitionPhase::recovery_failed) || has_failure > 1U) {
      return MakeError(ErrorCode::persistence_error, "journal transition record is invalid");
    }
    event.transition.phase = static_cast<TransitionPhase>(phase);
    event.transition.updated_at =
        std::chrono::steady_clock::time_point{std::chrono::nanoseconds{updated}};
    if (has_failure != 0U) {
      TransitionFailure failure;
      std::uint16_t error_code{};
      std::uint8_t has_application{};
      if (!decoder.Integer(error_code) || !decoder.Integer(failure.error.support_data) ||
          !decoder.String(failure.error.message) || !decoder.Integer(has_application) ||
          error_code > static_cast<std::uint16_t>(ErrorCode::internal_error) ||
          has_application > 1U) {
        return MakeError(ErrorCode::persistence_error, "journal failure record is invalid");
      }
      failure.error.code = static_cast<ErrorCode>(error_code);
      if (has_application != 0U) {
        ApplicationId application;
        if (!decoder.Id(application)) {
          return MakeError(ErrorCode::persistence_error,
                           "journal failure application is truncated");
        }
        failure.application = application;
      }
      if (!decoder.Integer(failure.evidence.exit_code) ||
          !decoder.Integer(failure.evidence.signal) ||
          !decoder.Integer(failure.evidence.native_code) ||
          !decoder.String(failure.evidence.message)) {
        return MakeError(ErrorCode::persistence_error, "journal backend evidence is truncated");
      }
      event.transition.failure = std::move(failure);
    }
    std::uint8_t has_plan{};
    if (!decoder.Integer(has_plan) || has_plan > 1U) {
      return MakeError(ErrorCode::persistence_error, "journal plan marker is invalid");
    }
    if (has_plan != 0U) {
      TransitionPlan plan;
      if (!decoder.Integer(plan.generation.value) || !decoder.Id(plan.domain) ||
          !decoder.Id(plan.source_mode) || !decoder.Id(plan.target_mode) ||
          !DecodeIds(decoder, plan.retain) || !DecodeIds(decoder, plan.stop) ||
          !DecodeIds(decoder, plan.start) || !DecodeIds(decoder, plan.guarded_domains) ||
          !DecodeIds(decoder, plan.affected_resources)) {
        return MakeError(ErrorCode::persistence_error, "journal transition plan is invalid");
      }
      event.plan = std::move(plan);
    }
    if (decoder.remaining() != 0U) {
      return MakeError(ErrorCode::persistence_error, "journal record has trailing data");
    }
    return event;
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted, "cannot decode journal record");
  }
}

void Put16(std::uint8_t* output, std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void Put32(std::uint8_t* output, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint16_t Get16(const std::uint8_t* input) noexcept {
  return static_cast<std::uint16_t>(input[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t Get32(const std::uint8_t* input) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

bool WriteAll(int descriptor, std::span<const std::uint8_t> bytes) noexcept {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

class FileTransitionJournal final : public TransitionJournal {
public:
  FileTransitionJournal(int descriptor, FileJournalOptions options)
      : descriptor_(descriptor), options_(std::move(options)) {}

  ~FileTransitionJournal() override {
    if (descriptor_ >= 0) {
      ::flock(descriptor_, LOCK_UN);
      ::close(descriptor_);
    }
  }

  Result<void> Append(const JournalEvent& event) noexcept override {
    auto encoded = Encode(event, options_.maximum_record_size);
    if (!encoded) {
      return encoded.error();
    }
    std::array<std::uint8_t, kHeaderSize> header{};
    Put32(header.data(), kMagic);
    Put16(header.data() + 4U, kFormatVersion);
    Put16(header.data() + 6U, 0U);
    Put32(header.data() + 8U, static_cast<std::uint32_t>(encoded.value().size()));
    Put32(header.data() + 12U, Crc32(encoded.value()));

    std::lock_guard lock(mutex_);
    if (::lseek(descriptor_, 0, SEEK_END) < 0 || !WriteAll(descriptor_, header) ||
        !WriteAll(descriptor_, encoded.value())) {
      return MakeError(ErrorCode::persistence_error, "cannot append transition journal", errno);
    }
    if (options_.synchronize && ::fsync(descriptor_) != 0) {
      return MakeError(ErrorCode::persistence_error, "cannot synchronize transition journal",
                       errno);
    }
    return {};
  }

  Result<std::vector<JournalEvent>> Replay() noexcept override {
    try {
      std::lock_guard lock(mutex_);
      if (::lseek(descriptor_, 0, SEEK_SET) < 0) {
        return MakeError(ErrorCode::persistence_error, "cannot seek transition journal", errno);
      }
      std::vector<JournalEvent> events;
      for (;;) {
        const auto frame_start = ::lseek(descriptor_, 0, SEEK_CUR);
        if (frame_start < 0) {
          return MakeError(ErrorCode::persistence_error, "cannot locate journal frame", errno);
        }
        std::array<std::uint8_t, kHeaderSize> header{};
        const auto header_size = Read(header);
        if (!header_size) {
          return header_size.error();
        }
        if (header_size.value() == 0U) {
          return events;
        }
        if (header_size.value() != header.size()) {
          if (::ftruncate(descriptor_, frame_start) != 0 ||
              (options_.synchronize && ::fsync(descriptor_) != 0)) {
            return MakeError(ErrorCode::persistence_error,
                             "cannot repair incomplete journal header", errno);
          }
          return events;
        }
        const auto magic = Get32(header.data());
        const auto version = Get16(header.data() + 4U);
        const auto reserved = Get16(header.data() + 6U);
        const auto size = Get32(header.data() + 8U);
        const auto checksum = Get32(header.data() + 12U);
        if (magic != kMagic || version != kFormatVersion || reserved != 0U ||
            size > options_.maximum_record_size) {
          return MakeError(ErrorCode::persistence_error, "journal frame header is invalid");
        }
        std::vector<std::uint8_t> payload(size);
        const auto payload_size = Read(payload);
        if (!payload_size) {
          return payload_size.error();
        }
        if (payload_size.value() != payload.size()) {
          if (::ftruncate(descriptor_, frame_start) != 0 ||
              (options_.synchronize && ::fsync(descriptor_) != 0)) {
            return MakeError(ErrorCode::persistence_error,
                             "cannot repair incomplete journal record", errno);
          }
          return events;
        }
        if (Crc32(payload) != checksum) {
          return MakeError(ErrorCode::persistence_error, "journal frame checksum mismatch");
        }
        auto decoded = Decode(payload);
        if (!decoded) {
          return decoded.error();
        }
        events.push_back(std::move(decoded).value());
      }
    } catch (...) {
      return MakeError(ErrorCode::resource_exhausted, "cannot allocate journal replay");
    }
  }

private:
  Result<std::size_t> Read(std::span<std::uint8_t> bytes) noexcept {
    std::size_t offset{};
    while (offset < bytes.size()) {
      const auto count = ::read(descriptor_, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return MakeError(ErrorCode::persistence_error, "cannot read transition journal", errno);
      }
      if (count == 0) {
        break;
      }
      offset += static_cast<std::size_t>(count);
    }
    return offset;
  }

  int descriptor_;
  FileJournalOptions options_;
  std::mutex mutex_;
};

} // namespace

Result<std::unique_ptr<TransitionJournal>> OpenFileTransitionJournal(FileJournalOptions options) {
  if (options.path.empty() || options.path.front() != '/' ||
      options.maximum_record_size < kMinimumMaximumRecordSize ||
      options.maximum_record_size > std::numeric_limits<std::uint32_t>::max()) {
    return MakeError(ErrorCode::invalid_argument, "invalid file journal options");
  }
  const int descriptor = ::open(options.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0640);
  if (descriptor < 0) {
    return MakeError(ErrorCode::persistence_error, "cannot open transition journal", errno);
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const auto error = errno;
    ::close(descriptor);
    return MakeError(ErrorCode::busy, "transition journal already has a writer", error);
  }
  return std::unique_ptr<TransitionJournal>{
      new FileTransitionJournal(descriptor, std::move(options))};
}

} // namespace ovf::exec::detail
