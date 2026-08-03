// SPDX-License-Identifier: Apache-2.0

#include "ovf_persistence.hpp"

#include "ovf/per/backend_abi.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

extern "C" const ovf_per_backend_factory_v1* ovf_per_backend_query_v1(void);

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string Text(std::span<const std::byte> value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

int Fail(std::string_view operation, const ovf::per::Error& error) {
  std::cerr << operation << ": " << error.message << '\n';
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: per_e2e_app <root> <initialize|crash|verify-old|update|verify-new>\n";
    return 64;
  }
  const auto* factory = ovf_per_backend_query_v1();
  auto runtime_result = ovf::deployment::per_e2e::CreateRuntime(
      *factory, ovf::per::RuntimeConfig{.configuration = "{\"root\":\"" + std::string(argv[1]) +
                                                         "\",\"journal_mode\":\"wal\","
                                                         "\"busy_timeout_ms\":250}"});
  if (!runtime_result) {
    return Fail("create runtime", runtime_result.error());
  }
  auto runtime = std::move(runtime_result).value();
  auto store_result = ovf::deployment::per_e2e::OpenOperationalState(*runtime);
  if (!store_result) {
    return Fail("open store", store_result.error());
  }
  auto store = std::move(store_result).value();
  const std::string_view mode(argv[2]);
  if (mode == "initialize" || mode == "update") {
    const std::string_view expected = mode == "initialize" ? "ready" : "active";
    auto transaction_result = store.BeginWrite();
    if (!transaction_result) {
      return Fail("begin write", transaction_result.error());
    }
    auto transaction = std::move(transaction_result).value();
    auto put = transaction.Put(Bytes("mode"), Bytes(expected));
    if (!put) {
      return Fail("put", put.error());
    }
    auto commit = transaction.Commit();
    if (!commit) {
      return Fail("commit", commit.error());
    }
    const std::string_view blob = mode == "initialize" ? "baseline-map" : "updated-map";
    auto writer_result = store.BeginBlobReplace(Bytes("map"), blob.size());
    if (!writer_result) {
      return Fail("begin blob", writer_result.error());
    }
    auto writer = std::move(writer_result).value();
    auto written = writer.Write(Bytes(blob));
    if (!written) {
      return Fail("write blob", written.error());
    }
    auto blob_commit = writer.Commit();
    if (!blob_commit) {
      return Fail("commit blob", blob_commit.error());
    }
    std::cout << "COMMITTED generation=" << blob_commit.value().generation << '\n';
    return 0;
  }
  if (mode == "crash") {
    auto writer_result = store.BeginBlobReplace(Bytes("map"), 12);
    if (!writer_result) {
      return Fail("begin crash blob", writer_result.error());
    }
    auto writer = std::move(writer_result).value();
    const auto written = writer.Write(Bytes("partial"));
    if (!written) {
      return Fail("partial blob write", written.error());
    }
    std::_Exit(23);
  }
  const bool old = mode == "verify-old";
  if (!old && mode != "verify-new") {
    std::cerr << "unknown mode\n";
    return 64;
  }
  auto read_result = store.BeginRead();
  if (!read_result) {
    return Fail("begin read", read_result.error());
  }
  auto value = read_result.value().Get(Bytes("mode"));
  if (!value || !value.value().has_value()) {
    return value ? 3 : Fail("get", value.error());
  }
  const std::string expected_value = old ? "ready" : "active";
  if (Text(*value.value()) != expected_value) {
    std::cerr << "unexpected value\n";
    return 4;
  }
  auto blob_result = store.OpenBlob(Bytes("map"));
  if (!blob_result) {
    return Fail("open blob", blob_result.error());
  }
  auto blob = std::move(blob_result).value();
  std::array<std::byte, 32> buffer{};
  auto count = blob.Read(0, std::span(buffer).first(blob.size()));
  if (!count) {
    return Fail("read blob", count.error());
  }
  const std::string expected_blob = old ? "baseline-map" : "updated-map";
  if (Text(std::span(buffer).first(count.value())) != expected_blob) {
    std::cerr << "unexpected blob\n";
    return 5;
  }
  std::cout << "VERIFIED generation=" << blob.generation() << '\n';
  return 0;
}
