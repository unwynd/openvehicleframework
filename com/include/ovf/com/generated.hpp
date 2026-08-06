// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ovf::com {

struct Uuid {
  std::array<std::uint8_t, 16> bytes;
};

template <class T, std::size_t Capacity> class BoundedVector {
public:
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  static constexpr auto capacity() noexcept -> std::size_t { return Capacity; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }
  constexpr auto values() noexcept -> std::span<T> { return {storage_.data(), size_}; }
  constexpr auto values() const noexcept -> std::span<const T> { return {storage_.data(), size_}; }
  constexpr auto push_back(T value) noexcept -> bool {
    if (size_ == Capacity)
      return false;
    storage_[size_++] = std::move(value);
    return true;
  }

private:
  std::array<T, Capacity> storage_{};
  std::size_t size_{};
};

template <std::size_t Capacity> class BoundedString {
public:
  constexpr auto assign(std::string_view value) noexcept -> bool {
    if (value.size() > Capacity)
      return false;
    for (std::size_t index = 0; index < value.size(); ++index)
      storage_[index] = value[index];
    size_ = value.size();
    return true;
  }
  constexpr auto view() const noexcept -> std::string_view { return {storage_.data(), size_}; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  static constexpr auto capacity() noexcept -> std::size_t { return Capacity; }

private:
  std::array<char, Capacity> storage_{};
  std::size_t size_{};
};

enum class CommunicationError : std::uint8_t {
  unavailable,
  incompatible,
  resource_exhausted,
  deadline_exceeded,
  cancelled,
  shutting_down,
  provider_failure
};
enum class SubscriptionState : std::uint8_t {
  requested,
  active,
  rejected,
  suspended,
  withdrawn,
};

template <class Value, class ApplicationError>
using MethodResult = std::variant<Value, ApplicationError, CommunicationError>;

struct ElementDescriptor {
  Uuid id;
  std::uint32_t tag;
  std::string_view name;
};
struct CallOptions {
  std::chrono::steady_clock::time_point deadline;
};

class Encoder {
public:
  struct CountOnly final {};
  Encoder() = default;
  explicit Encoder(CountOnly) noexcept : count_only_(true) {}
  explicit Encoder(std::span<std::byte> destination) noexcept : destination_(destination) {}
  template <class UInt> auto unsigned_integer(UInt value) -> void {
    for (std::size_t index = 0; index < sizeof(UInt); ++index)
      append(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
  auto raw(std::span<const std::byte> value) -> void {
    if (count_only_) {
      size_ += value.size();
      return;
    }
    if (destination_.empty()) {
      bytes_.insert(bytes_.end(), value.begin(), value.end());
      size_ += value.size();
      return;
    }
    if (size_ > destination_.size() || value.size() > destination_.size() - size_) {
      valid_ = false;
      return;
    }
    std::memcpy(destination_.data() + size_, value.data(), value.size());
    size_ += value.size();
  }
  auto take() && -> std::vector<std::byte> { return std::move(bytes_); }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto valid() const noexcept -> bool { return valid_; }

private:
  auto append(std::byte value) -> void {
    if (count_only_) {
      ++size_;
    } else if (destination_.empty()) {
      bytes_.push_back(value);
      ++size_;
    } else if (size_ < destination_.size()) {
      destination_[size_++] = value;
    } else {
      valid_ = false;
    }
  }
  std::vector<std::byte> bytes_;
  std::span<std::byte> destination_;
  std::size_t size_{};
  bool valid_{true};
  bool count_only_{};
};

class Decoder {
public:
  explicit Decoder(std::span<const std::byte> bytes) : bytes_(bytes) {}
  template <class UInt> auto unsigned_integer(UInt& value) -> bool {
    if (bytes_.size() < sizeof(UInt))
      return false;
    value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index)
      value |= static_cast<UInt>(std::to_integer<unsigned>(bytes_[index])) << (index * 8U);
    bytes_ = bytes_.subspan(sizeof(UInt));
    return true;
  }
  auto raw(std::size_t size, std::span<const std::byte>& value) -> bool {
    if (bytes_.size() < size)
      return false;
    value = bytes_.first(size);
    bytes_ = bytes_.subspan(size);
    return true;
  }
  auto empty() const noexcept -> bool { return bytes_.empty(); }
  auto remaining() const noexcept -> std::size_t { return bytes_.size(); }
  auto next_field(std::uint32_t& tag, Decoder& field) -> bool {
    std::uint32_t size{};
    std::span<const std::byte> bytes;
    if (!unsigned_integer(tag) || tag == 0U || !unsigned_integer(size) || !raw(size, bytes))
      return false;
    field = Decoder(bytes);
    return true;
  }

private:
  std::span<const std::byte> bytes_;
};

template <class T, class Enable = void> struct Codec;
template <class T, class Enable = void> struct MaximumEncodedSize;

template <class T>
struct Codec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static auto encode(Encoder& out, T value) -> bool {
    using UInt = std::make_unsigned_t<T>;
    out.unsigned_integer(static_cast<UInt>(value));
    return true;
  }
  static auto decode(Decoder& in, T& value) -> bool {
    using UInt = std::make_unsigned_t<T>;
    UInt raw{};
    if (!in.unsigned_integer(raw))
      return false;
    value = static_cast<T>(raw);
    return true;
  }
};
template <class T>
struct MaximumEncodedSize<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static constexpr std::optional<std::size_t> value{sizeof(T)};
};

template <> struct Codec<bool> {
  static auto encode(Encoder& out, bool value) -> bool {
    return Codec<std::uint8_t>::encode(out, value ? 1U : 0U);
  }
  static auto decode(Decoder& in, bool& value) -> bool {
    std::uint8_t raw{};
    if (!Codec<std::uint8_t>::decode(in, raw) || raw > 1U)
      return false;
    value = raw == 1U;
    return true;
  }
};
template <> struct MaximumEncodedSize<bool> {
  static constexpr std::optional<std::size_t> value{1U};
};

template <> struct Codec<float> {
  static auto encode(Encoder& out, float value) -> bool {
    return Codec<std::uint32_t>::encode(out, std::bit_cast<std::uint32_t>(value));
  }
  static auto decode(Decoder& in, float& value) -> bool {
    std::uint32_t raw{};
    if (!Codec<std::uint32_t>::decode(in, raw))
      return false;
    value = std::bit_cast<float>(raw);
    return true;
  }
};
template <> struct MaximumEncodedSize<float> {
  static constexpr std::optional<std::size_t> value{4U};
};
template <> struct Codec<double> {
  static auto encode(Encoder& out, double value) -> bool {
    return Codec<std::uint64_t>::encode(out, std::bit_cast<std::uint64_t>(value));
  }
  static auto decode(Decoder& in, double& value) -> bool {
    std::uint64_t raw{};
    if (!Codec<std::uint64_t>::decode(in, raw))
      return false;
    value = std::bit_cast<double>(raw);
    return true;
  }
};
template <> struct MaximumEncodedSize<double> {
  static constexpr std::optional<std::size_t> value{8U};
};
template <class T> auto encode_field(Encoder& out, std::uint32_t tag, T const& value) -> bool {
  Encoder counter(Encoder::CountOnly{});
  if (tag == 0U || !Codec<T>::encode(counter, value) || !counter.valid() ||
      counter.size() > std::numeric_limits<std::uint32_t>::max())
    return false;
  Codec<std::uint32_t>::encode(out, tag);
  Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(counter.size()));
  return Codec<T>::encode(out, value);
}

template <class T>
auto encode_optional_field(Encoder& out, std::uint32_t tag, std::optional<T> const& value) -> bool {
  return !value || encode_field(out, tag, *value);
}
template <std::size_t N> struct Codec<BoundedString<N>> {
  static auto encode(Encoder& out, BoundedString<N> const& value) -> bool {
    Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(value.size()));
    auto chars = std::as_bytes(std::span(value.view().data(), value.view().size()));
    out.raw(chars);
    return true;
  }
  static auto decode(Decoder& in, BoundedString<N>& value) -> bool {
    std::uint32_t size{};
    std::span<const std::byte> bytes;
    if (!Codec<std::uint32_t>::decode(in, size) || size > N || !in.raw(size, bytes))
      return false;
    return value.assign({reinterpret_cast<char const*>(bytes.data()), bytes.size()});
  }
};
template <std::size_t N> struct MaximumEncodedSize<BoundedString<N>> {
  static constexpr std::optional<std::size_t> value{4U + N};
};
template <class T, std::size_t N> struct Codec<BoundedVector<T, N>> {
  static auto encode(Encoder& out, BoundedVector<T, N> const& value) -> bool {
    Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(value.size()));
    for (auto const& item : value.values())
      if (!Codec<T>::encode(out, item))
        return false;
    return true;
  }
  static auto decode(Decoder& in, BoundedVector<T, N>& value) -> bool {
    std::uint32_t size{};
    if (!Codec<std::uint32_t>::decode(in, size) || size > N)
      return false;
    for (std::uint32_t index = 0; index < size; ++index) {
      T item{};
      if (!Codec<T>::decode(in, item) || !value.push_back(std::move(item)))
        return false;
    }
    return true;
  }
};
template <class T, std::size_t N> struct MaximumEncodedSize<BoundedVector<T, N>> {
  static constexpr std::optional<std::size_t> value = [] {
    if constexpr (MaximumEncodedSize<T>::value.has_value())
      return std::optional<std::size_t>{4U + N * *MaximumEncodedSize<T>::value};
    return std::optional<std::size_t>{};
  }();
};
template <class T, std::size_t N> struct Codec<std::array<T, N>> {
  static auto encode(Encoder& out, std::array<T, N> const& value) -> bool {
    for (auto const& item : value)
      if (!Codec<T>::encode(out, item))
        return false;
    return true;
  }
  static auto decode(Decoder& in, std::array<T, N>& value) -> bool {
    for (auto& item : value)
      if (!Codec<T>::decode(in, item))
        return false;
    return true;
  }
};
template <class T, std::size_t N> struct MaximumEncodedSize<std::array<T, N>> {
  static constexpr std::optional<std::size_t> value = [] {
    if constexpr (MaximumEncodedSize<T>::value.has_value())
      return std::optional<std::size_t>{N * *MaximumEncodedSize<T>::value};
    return std::optional<std::size_t>{};
  }();
};
template <> struct Codec<std::string> {
  static auto encode(Encoder& out, std::string const& value) -> bool {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
      return false;
    Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(value.size()));
    out.raw(std::as_bytes(std::span(value.data(), value.size())));
    return true;
  }
  static auto decode(Decoder& in, std::string& value) -> bool {
    std::uint32_t size{};
    std::span<const std::byte> bytes;
    if (!Codec<std::uint32_t>::decode(in, size) || size > in.remaining() || !in.raw(size, bytes))
      return false;
    value.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    return true;
  }
};
template <> struct MaximumEncodedSize<std::string> {
  static constexpr std::optional<std::size_t> value{};
};
template <class T> struct Codec<std::vector<T>> {
  static auto encode(Encoder& out, std::vector<T> const& value) -> bool {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
      return false;
    Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(value.size()));
    for (auto const& item : value)
      if (!Codec<T>::encode(out, item))
        return false;
    return true;
  }
  static auto decode(Decoder& in, std::vector<T>& value) -> bool {
    std::uint32_t size{};
    if (!Codec<std::uint32_t>::decode(in, size) || size > in.remaining())
      return false;
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
      T item{};
      if (!Codec<T>::decode(in, item))
        return false;
      value.push_back(std::move(item));
    }
    return true;
  }
};
template <class T> struct MaximumEncodedSize<std::vector<T>> {
  static constexpr std::optional<std::size_t> value{};
};
template <> struct Codec<std::span<const std::byte>> {
  static auto encode(Encoder& out, std::span<const std::byte> value) -> bool {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
      return false;
    Codec<std::uint32_t>::encode(out, static_cast<std::uint32_t>(value.size()));
    out.raw(value);
    return true;
  }
  static auto decode(Decoder& in, std::span<const std::byte>& value) -> bool {
    std::uint32_t size{};
    return Codec<std::uint32_t>::decode(in, size) && size <= in.remaining() && in.raw(size, value);
  }
};
template <> struct MaximumEncodedSize<std::span<const std::byte>> {
  static constexpr std::optional<std::size_t> value{};
};
template <class T> struct Codec<std::optional<T>> {
  static auto encode(Encoder& out, std::optional<T> const& value) -> bool {
    Codec<std::uint8_t>::encode(out, value ? 1U : 0U);
    return !value || Codec<T>::encode(out, *value);
  }
  static auto decode(Decoder& in, std::optional<T>& value) -> bool {
    std::uint8_t present{};
    if (!Codec<std::uint8_t>::decode(in, present) || present > 1)
      return false;
    if (!present) {
      value.reset();
      return true;
    }
    T decoded{};
    if (!Codec<T>::decode(in, decoded))
      return false;
    value = std::move(decoded);
    return true;
  }
};
template <class T> struct MaximumEncodedSize<std::optional<T>> : MaximumEncodedSize<T> {};

template <class T> constexpr auto maximum_encoded_size() -> std::optional<std::size_t> {
  return MaximumEncodedSize<T>::value;
}

template <class T> auto encode(T const& value) -> std::vector<std::byte> {
  Encoder encoder;
  if (!Codec<T>::encode(encoder, value))
    return {};
  return std::move(encoder).take();
}
template <class T> auto encoded_size(T const& value) -> std::size_t {
  Encoder encoder(Encoder::CountOnly{});
  return Codec<T>::encode(encoder, value) && encoder.valid() ? encoder.size() : 0;
}
template <class T> auto encode_into(T const& value, std::span<std::byte> destination) -> bool {
  Encoder encoder(destination);
  return Codec<T>::encode(encoder, value) && encoder.valid() &&
         encoder.size() == destination.size();
}
template <class T> auto decode(std::span<const std::byte> bytes, T& value) -> bool {
  Decoder decoder(bytes);
  return Codec<T>::decode(decoder, value) && decoder.empty();
}

struct RawResult {
  CommunicationError error;
  bool has_error;
  bool application_error;
  std::vector<std::byte> payload;
};

class RawOperation {
public:
  virtual ~RawOperation() = default;
  virtual auto wait(std::chrono::steady_clock::time_point deadline) -> RawResult = 0;
  virtual auto cancel() noexcept -> void = 0;
};
class RawSubscription {
public:
  using Callback = std::function<void(std::span<const std::byte>)>;
  using StateCallback = std::function<void(SubscriptionState, std::optional<CommunicationError>)>;
  virtual ~RawSubscription() = default;
  virtual auto set_callback(Callback) -> void = 0;
  virtual auto set_state_callback(StateCallback) -> void = 0;
  virtual auto close() noexcept -> void = 0;
};
class ClientBinding {
public:
  virtual ~ClientBinding() = default;
  virtual auto invoke(ElementDescriptor const&, std::span<const std::byte>, CallOptions)
      -> std::shared_ptr<RawOperation> = 0;
  virtual auto invoke_loaned(ElementDescriptor const&, std::size_t,
                             std::function<bool(std::span<std::byte>)>, CallOptions)
      -> std::shared_ptr<RawOperation> = 0;
  virtual auto subscribe(ElementDescriptor const&) -> std::shared_ptr<RawSubscription> = 0;
};

struct RawServerResult {
  CommunicationError error{CommunicationError::provider_failure};
  bool has_error{};
  bool application_error{};
  std::vector<std::byte> payload;
  std::size_t encoded_payload_size{};
  std::function<bool(std::span<std::byte>)> encode_payload;
};

template <class T> auto make_raw_result(T value) -> RawServerResult {
  auto stored = std::make_shared<T>(std::move(value));
  auto size = encoded_size(*stored);
  return {{}, false, false, {}, size, [stored = std::move(stored)](std::span<std::byte> output) {
            return encode_into(*stored, output);
          }};
}
template <class T>
auto make_application_raw_result(std::uint8_t index, T value) -> RawServerResult {
  auto stored = std::make_shared<T>(std::move(value));
  auto size = encoded_size(*stored) + 1;
  return {
      {}, false, true, {}, size, [index, stored = std::move(stored)](std::span<std::byte> output) {
        if (output.empty())
          return false;
        output.front() = static_cast<std::byte>(index);
        return encode_into(*stored, output.subspan(1));
      }};
}

class ServerBinding {
public:
  using MethodHandler = std::function<RawServerResult(std::span<const std::byte>,
                                                      std::chrono::steady_clock::time_point)>;
  virtual ~ServerBinding() = default;
  virtual auto add_method(ElementDescriptor const&, MethodHandler) -> bool = 0;
  virtual auto add_event(ElementDescriptor const&) -> bool = 0;
  virtual auto publish(ElementDescriptor const&, std::span<const std::byte>)
      -> std::optional<CommunicationError> = 0;
  virtual auto publish_loaned(ElementDescriptor const&, std::size_t,
                              std::function<bool(std::span<std::byte>)>)
      -> std::optional<CommunicationError> = 0;
  virtual auto close() noexcept -> void = 0;
};

template <class T> class Operation {
public:
  using Decode = T (*)(RawResult const&);
  Operation(std::shared_ptr<RawOperation> state, Decode decode)
      : state_(std::move(state)), decode_(decode) {}
  Operation(Operation&&) noexcept = default;
  Operation(Operation const&) = delete;
  auto get(CallOptions options) -> T { return decode_(state_->wait(options.deadline)); }
  auto cancel() noexcept -> void { state_->cancel(); }

private:
  std::shared_ptr<RawOperation> state_;
  Decode decode_;
};

template <class T> class EventSubscription {
public:
  using Callback = std::function<void(T const&)>;
  using StateCallback = RawSubscription::StateCallback;
  using Decode = bool (*)(std::span<const std::byte>, T&);
  EventSubscription(std::shared_ptr<RawSubscription> state, Decode decode)
      : state_(std::move(state)), decode_(decode) {}
  EventSubscription(EventSubscription&&) noexcept = default;
  EventSubscription(EventSubscription const&) = delete;
  [[nodiscard]] auto valid() const noexcept -> bool { return static_cast<bool>(state_); }
  auto on_sample(Callback callback) -> void {
    if (!state_)
      return;
    auto decode = decode_;
    state_->set_callback([decode, callback = std::move(callback)](auto bytes) {
      T value{};
      if (decode(bytes, value))
        callback(value);
    });
  }
  auto on_state(StateCallback callback) -> void {
    if (state_)
      state_->set_state_callback(std::move(callback));
  }
  auto close() noexcept -> void {
    if (state_)
      state_->close();
  }

private:
  std::shared_ptr<RawSubscription> state_;
  Decode decode_;
};

} // namespace ovf::com
