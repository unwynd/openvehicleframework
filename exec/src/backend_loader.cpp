// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/backend_loader.hpp"

#include <dlfcn.h>

#include <utility>

namespace ovf::exec::detail {

class BackendLibrary::Impl final {
public:
  Impl(void* handle, const ovf_exec_backend_factory_v1* factory)
      : handle_(handle), factory_(factory) {}
  ~Impl() {
    if (handle_ != nullptr) {
      ::dlclose(handle_);
    }
  }

  void* handle_{};
  const ovf_exec_backend_factory_v1* factory_{};
};

BackendLibrary::BackendLibrary(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BackendLibrary::~BackendLibrary() = default;
BackendLibrary::BackendLibrary(BackendLibrary&&) noexcept = default;
BackendLibrary& BackendLibrary::operator=(BackendLibrary&&) noexcept = default;

const ovf_exec_backend_factory_v1& BackendLibrary::Factory() const noexcept {
  return *impl_->factory_;
}

Result<BackendLibrary> LoadBackendLibrary(const std::string& absolute_path) {
  if (absolute_path.empty() || absolute_path.front() != '/') {
    return MakeError(ErrorCode::invalid_argument, "backend library path must be absolute");
  }
  void* handle = ::dlopen(absolute_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* error = ::dlerror();
    return MakeError(ErrorCode::backend_unavailable,
                     error == nullptr
                         ? "cannot load execution backend library"
                         : std::string{"cannot load execution backend library: "} + error);
  }
  ::dlerror();
  auto* symbol = ::dlsym(handle, "ovf_exec_backend_query_v1");
  const char* symbol_error = ::dlerror();
  if (symbol == nullptr || symbol_error != nullptr) {
    ::dlclose(handle);
    return MakeError(ErrorCode::incompatible_abi, "execution backend query symbol is missing");
  }
  const auto query = reinterpret_cast<ovf_exec_backend_query_fn_v1>(symbol);
  const auto* factory = query();
  if (factory == nullptr) {
    ::dlclose(handle);
    return MakeError(ErrorCode::incompatible_abi, "execution backend returned no factory");
  }
  try {
    return BackendLibrary{std::make_unique<BackendLibrary::Impl>(handle, factory)};
  } catch (...) {
    ::dlclose(handle);
    return MakeError(ErrorCode::resource_exhausted, "cannot allocate backend library handle");
  }
}

} // namespace ovf::exec::detail
