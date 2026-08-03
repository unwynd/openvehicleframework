// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

ovf::per::StoreOptions Options() {
  return {.logical_name = "integration/installed-provider",
          .access = ovf::per::Access::read_write,
          .minimum_durability = ovf::per::Durability::process_crash,
          .capacity_bytes = 4096,
          .max_entries = 16,
          .max_key_size = 64,
          .max_value_size = 256};
}

int Fail(std::string_view operation, const ovf::per::Error& error) {
  std::cerr << operation << ": " << error.message << '\n';
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: installed_provider_e2e <provider-directory> <storage-root>\n";
    return 64;
  }
  const std::string configuration =
      "{\"root\":\"" + std::string(argv[2]) + "\",\"journal_mode\":\"wal\"}";
  {
    auto runtime = ovf::per::Runtime::LoadFrom(
        "sqlite", argv[1], ovf::per::RuntimeConfig{.configuration = configuration});
    if (!runtime) {
      return Fail("load installed provider", runtime.error());
    }
    auto store = runtime.value()->OpenStore(Options());
    if (!store) {
      return Fail("open store", store.error());
    }
    auto write = store.value().BeginWrite();
    if (!write) {
      return Fail("begin write", write.error());
    }
    auto put = write.value().Put(Bytes("boot"), Bytes("complete"));
    if (!put) {
      return Fail("put", put.error());
    }
    auto commit = write.value().Commit();
    if (!commit) {
      return Fail("commit", commit.error());
    }
  }
  auto runtime = ovf::per::Runtime::LoadFrom(
      "sqlite", argv[1], ovf::per::RuntimeConfig{.configuration = configuration});
  if (!runtime) {
    return Fail("reload installed provider", runtime.error());
  }
  auto store = runtime.value()->OpenStore(Options());
  if (!store) {
    return Fail("reopen store", store.error());
  }
  auto read = store.value().BeginRead();
  if (!read) {
    return Fail("begin read", read.error());
  }
  auto value = read.value().Get(Bytes("boot"));
  if (!value) {
    return Fail("get", value.error());
  }
  if (!value.value() || value.value()->size() != 8U) {
    std::cerr << "installed provider did not retain committed data\n";
    return 3;
  }
  std::cout << "PERSISTENCE_INSTALLED_PROVIDER_VERIFIED\n";
  return 0;
}
