// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

extern "C" const ovf_per_backend_factory_v1* ovf_per_sqlite_backend_query_v1(void);

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string TestRoot(std::string_view name) {
  const char* directory = std::getenv("TEST_TMPDIR");
  EXPECT_NE(directory, nullptr);
  return std::string(directory == nullptr ? "." : directory) + "/" + std::string(name);
}

std::string Configuration(const std::string& root, std::string_view journal_mode = "persist") {
  return "{\"root\":\"" + root + "\",\"journal_mode\":\"" + std::string(journal_mode) +
         "\",\"busy_timeout_ms\":1000}";
}

ovf::per::StoreOptions Options(std::uint64_t capacity = 1024) {
  return {.logical_name = "vehicle/learned-state",
          .access = ovf::per::Access::read_write,
          .minimum_durability = ovf::per::Durability::process_crash,
          .capacity_bytes = capacity,
          .max_entries = 8,
          .max_key_size = 32,
          .max_value_size = 128};
}

TEST(PerSqliteProviderTest, PersistsCommittedGenerationAcrossRuntimeRestart) {
  const auto root = TestRoot("restart");
  {
    auto runtime_result = ovf::per::Runtime::Create(*ovf_per_sqlite_backend_query_v1(),
                                                    {.configuration = Configuration(root)});
    ASSERT_TRUE(runtime_result) << runtime_result.error().message;
    auto runtime = std::move(runtime_result).value();
    const auto capabilities = runtime->GetCapabilities();
    ASSERT_TRUE(capabilities);
    EXPECT_TRUE(capabilities.value().persistent);
    EXPECT_TRUE(capabilities.value().cross_process_leases);
    EXPECT_EQ(capabilities.value().maximum_durability, ovf::per::Durability::media);

    auto store_result = runtime->OpenStore(Options());
    ASSERT_TRUE(store_result) << store_result.error().message;
    auto store = std::move(store_result).value();
    auto write_result = store.BeginWrite(ovf::per::Durability::process_crash);
    ASSERT_TRUE(write_result) << write_result.error().message;
    auto write = std::move(write_result).value();
    ASSERT_TRUE(write.Put(Bytes("steering-offset"), Bytes("-17")));
    const auto commit = write.Commit();
    ASSERT_TRUE(commit) << commit.error().message;
    EXPECT_EQ(commit.value().generation, 1U);
    EXPECT_EQ(commit.value().achieved_durability, ovf::per::Durability::process_crash);
  }

  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_sqlite_backend_query_v1(),
                                                  {.configuration = Configuration(root)});
  ASSERT_TRUE(runtime_result) << runtime_result.error().message;
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options());
  ASSERT_TRUE(store_result) << store_result.error().message;
  auto store = std::move(store_result).value();
  auto read_result = store.BeginRead();
  ASSERT_TRUE(read_result) << read_result.error().message;
  auto read = std::move(read_result).value();
  EXPECT_EQ(read.generation(), 1U);
  const auto value = read.Get(Bytes("steering-offset"));
  ASSERT_TRUE(value) << value.error().message;
  ASSERT_TRUE(value.value().has_value());
  EXPECT_EQ(value.value()->size(), 3U);
}

TEST(PerSqliteProviderTest, FailedMutationDoesNotChangeTransactionOrCommittedState) {
  auto runtime_result = ovf::per::Runtime::Create(
      *ovf_per_sqlite_backend_query_v1(), {.configuration = Configuration(TestRoot("quota"))});
  ASSERT_TRUE(runtime_result) << runtime_result.error().message;
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options(8));
  ASSERT_TRUE(store_result) << store_result.error().message;
  auto store = std::move(store_result).value();
  auto write_result = store.BeginWrite(ovf::per::Durability::process_crash);
  ASSERT_TRUE(write_result);
  auto write = std::move(write_result).value();
  ASSERT_TRUE(write.Put(Bytes("a"), Bytes("1234")));
  const auto rejected = write.Put(Bytes("bb"), Bytes("5678"));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ovf::per::ErrorCode::quota_exceeded);
  const auto retained = write.Get(Bytes("a"));
  ASSERT_TRUE(retained);
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(retained.value()->size(), 4U);
  ASSERT_TRUE(write.Commit());
}

TEST(PerSqliteProviderTest, DeploymentMismatchCannotReinterpretExistingStore) {
  const auto root = TestRoot("deployment-mismatch");
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_sqlite_backend_query_v1(),
                                                  {.configuration = Configuration(root)});
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto first = runtime->OpenStore(Options());
  ASSERT_TRUE(first);
  first.value().Close();

  const auto incompatible = runtime->OpenStore(Options(2048));
  ASSERT_FALSE(incompatible);
  EXPECT_EQ(incompatible.error().code, ovf::per::ErrorCode::invalid_argument);
}

} // namespace
