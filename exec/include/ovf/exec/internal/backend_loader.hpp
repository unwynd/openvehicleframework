// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/backend_abi.h"
#include "ovf/exec/error.hpp"

#include <memory>
#include <string>

namespace ovf::exec::detail {

class BackendLibrary final {
public:
  ~BackendLibrary();
  BackendLibrary(BackendLibrary&&) noexcept;
  BackendLibrary& operator=(BackendLibrary&&) noexcept;
  BackendLibrary(const BackendLibrary&) = delete;
  BackendLibrary& operator=(const BackendLibrary&) = delete;

  [[nodiscard]] const ovf_exec_backend_factory_v1& Factory() const noexcept;

private:
  class Impl;
  explicit BackendLibrary(std::unique_ptr<Impl> impl);
  friend Result<BackendLibrary> LoadBackendLibrary(const std::string&);
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<BackendLibrary> LoadBackendLibrary(const std::string& absolute_path);

} // namespace ovf::exec::detail
