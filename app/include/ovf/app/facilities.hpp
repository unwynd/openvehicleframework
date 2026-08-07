// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/crypto/crypto.hpp"
#include "ovf/per/per.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace ovf::app {

// RuntimeHandle keeps optional facilities out of the common application link
// closure. The generated facility facade supplies the deleter in the target
// that links the corresponding runtime; applications that do not declare the
// facility need only its public type declarations.
template <typename Runtime> class RuntimeHandle final {
public:
  using Deleter = void (*)(Runtime*) noexcept;

  RuntimeHandle() = default;
  RuntimeHandle(Runtime* runtime, Deleter deleter) noexcept
      : runtime_(runtime), deleter_(deleter) {}
  ~RuntimeHandle() { Reset(); }
  RuntimeHandle(RuntimeHandle const&) = delete;
  RuntimeHandle& operator=(RuntimeHandle const&) = delete;
  RuntimeHandle(RuntimeHandle&& other) noexcept
      : runtime_(std::exchange(other.runtime_, nullptr)), deleter_(other.deleter_) {}
  RuntimeHandle& operator=(RuntimeHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      runtime_ = std::exchange(other.runtime_, nullptr);
      deleter_ = other.deleter_;
    }
    return *this;
  }

  [[nodiscard]] Runtime* get() const noexcept { return runtime_; }

private:
  void Reset() noexcept {
    if (runtime_ != nullptr && deleter_ != nullptr)
      deleter_(runtime_);
    runtime_ = nullptr;
  }
  Runtime* runtime_{};
  Deleter deleter_{};
};

using PersistenceRuntimeHandle = RuntimeHandle<ovf::per::Runtime>;
using CryptoRuntimeHandle = RuntimeHandle<ovf::crypto::Runtime>;
using PersistenceRuntimeStartup = std::optional<ovf::per::Result<PersistenceRuntimeHandle>>;
using CryptoRuntimeStartup = std::optional<ovf::crypto::Result<CryptoRuntimeHandle>>;

} // namespace ovf::app
