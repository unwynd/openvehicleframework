// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/per.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

extern "C" const ovf_per_backend_factory_v1* ovf_per_backend_query_v1(void);

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

ovf::per::Entry Entry(std::string_view key, std::string_view value) {
  return {{Bytes(key).begin(), Bytes(key).end()}, {Bytes(value).begin(), Bytes(value).end()}};
}

std::string Text(const std::vector<std::byte>& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

ovf::per::StoreOptions Options(std::uint64_t capacity = 128) {
  return {.logical_name = "vehicle/calibration",
          .access = ovf::per::Access::read_write,
          .minimum_durability = ovf::per::Durability::buffered,
          .capacity_bytes = capacity,
          .max_entries = 4,
          .max_key_size = 16,
          .max_value_size = 32};
}

TEST(PerMemoryProviderTest, CommitsAtomicallyAndKeepsReadSnapshotsStable) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result) << runtime_result.error().message;
  auto runtime = std::move(runtime_result).value();

  const auto capabilities = runtime->GetCapabilities();
  ASSERT_TRUE(capabilities);
  EXPECT_FALSE(capabilities.value().persistent);
  EXPECT_EQ(capabilities.value().maximum_durability, ovf::per::Durability::buffered);

  auto store_result = runtime->OpenStore(Options());
  ASSERT_TRUE(store_result) << store_result.error().message;
  auto store = std::move(store_result).value();

  auto initial_result = store.BeginRead();
  ASSERT_TRUE(initial_result);
  auto initial = std::move(initial_result).value();
  EXPECT_EQ(initial.generation(), 0U);
  const auto missing = initial.Get(Bytes("offset"));
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing.value().has_value());

  auto write_result = store.BeginWrite(ovf::per::Durability::buffered);
  ASSERT_TRUE(write_result);
  auto write = std::move(write_result).value();
  ASSERT_TRUE(write.Put(Bytes("offset"), Bytes("12")));
  const auto commit = write.Commit();
  ASSERT_TRUE(commit) << commit.error().message;
  EXPECT_EQ(commit.value().generation, 1U);

  const auto still_missing = initial.Get(Bytes("offset"));
  ASSERT_TRUE(still_missing);
  EXPECT_FALSE(still_missing.value().has_value());

  auto current_result = store.BeginRead();
  ASSERT_TRUE(current_result);
  auto current = std::move(current_result).value();
  EXPECT_EQ(current.generation(), 1U);
  const auto value = current.Get(Bytes("offset"));
  ASSERT_TRUE(value);
  ASSERT_TRUE(value.value().has_value());
  EXPECT_EQ(value.value().value(), (std::vector<std::byte>{std::byte{'1'}, std::byte{'2'}}));
}

TEST(PerMemoryProviderTest, AbortsByDefaultAndPreservesStateOnQuotaFailure) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options(8));
  ASSERT_TRUE(store_result);
  auto store = std::move(store_result).value();

  {
    auto write_result = store.BeginWrite(ovf::per::Durability::buffered);
    ASSERT_TRUE(write_result);
    auto write = std::move(write_result).value();
    ASSERT_TRUE(write.Put(Bytes("a"), Bytes("1")));
  }

  auto read_result = store.BeginRead();
  ASSERT_TRUE(read_result);
  auto read = std::move(read_result).value();
  const auto absent = read.Get(Bytes("a"));
  ASSERT_TRUE(absent);
  EXPECT_FALSE(absent.value().has_value());
  read.Close();

  auto write_result = store.BeginWrite(ovf::per::Durability::buffered);
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
}

TEST(PerMemoryProviderTest, RejectsUnsupportedDurabilityAndConcurrentWriters) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options());
  ASSERT_TRUE(store_result);
  auto store = std::move(store_result).value();

  const auto durable = store.BeginWrite(ovf::per::Durability::media);
  ASSERT_FALSE(durable);
  EXPECT_EQ(durable.error().code, ovf::per::ErrorCode::unsupported);

  auto first_result = store.BeginWrite(ovf::per::Durability::buffered);
  ASSERT_TRUE(first_result);
  auto first = std::move(first_result).value();
  const auto second = store.BeginWrite(ovf::per::Durability::buffered);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, ovf::per::ErrorCode::busy);
}

TEST(PerMemoryProviderTest, ReplacesBlobsAtomicallyWithBoundedStreaming) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options());
  ASSERT_TRUE(store_result);
  auto store = std::move(store_result).value();

  auto writer_result = store.BeginBlobReplace(Bytes("map"), 6, ovf::per::Durability::buffered);
  ASSERT_TRUE(writer_result) << writer_result.error().message;
  auto writer = std::move(writer_result).value();
  ASSERT_TRUE(writer.Write(Bytes("abc")));
  ASSERT_TRUE(writer.Write(Bytes("def")));
  const auto committed = writer.Commit();
  ASSERT_TRUE(committed) << committed.error().message;
  EXPECT_EQ(committed.value().generation, 1U);

  auto reader_result = store.OpenBlob(Bytes("map"));
  ASSERT_TRUE(reader_result);
  auto reader = std::move(reader_result).value();
  EXPECT_EQ(reader.size(), 6U);
  std::array<std::byte, 3> tail{};
  const auto read = reader.Read(3, tail);
  ASSERT_TRUE(read);
  EXPECT_EQ(tail, (std::array<std::byte, 3>{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}}));
  reader.Close();

  {
    auto aborted_result = store.BeginBlobReplace(Bytes("map"), 3, ovf::per::Durability::buffered);
    ASSERT_TRUE(aborted_result);
    auto aborted = std::move(aborted_result).value();
    ASSERT_TRUE(aborted.Write(Bytes("new")));
  }
  auto retained_result = store.OpenBlob(Bytes("map"));
  ASSERT_TRUE(retained_result);
  EXPECT_EQ(retained_result.value().size(), 6U);
}

TEST(PerMemoryProviderTest, SupportsOrderedCursorsGenerationGuardsAndExplicitReset) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore(Options());
  ASSERT_TRUE(store_result);
  auto store = std::move(store_result).value();

  const std::array initial{Entry("config/z", "last"), Entry("config/a", "first"),
                           Entry("other", "ignored")};
  const auto reset = store.Reset(initial, ovf::per::Durability::buffered);
  ASSERT_TRUE(reset) << reset.error().message;
  const auto stale = store.BeginWriteAt(0, ovf::per::Durability::buffered);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, ovf::per::ErrorCode::conflict);

  auto read_result = store.BeginRead();
  ASSERT_TRUE(read_result);
  auto read = std::move(read_result).value();
  auto cursor_result = read.Iterate(Bytes("config/"));
  ASSERT_TRUE(cursor_result);
  auto cursor = std::move(cursor_result).value();
  const auto first = cursor.Next();
  const auto second = cursor.Next();
  const auto end = cursor.Next();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(end);
  ASSERT_TRUE(first.value());
  ASSERT_TRUE(second.value());
  EXPECT_FALSE(end.value());
  EXPECT_EQ(Text(first.value()->key), "config/a");
  EXPECT_EQ(Text(second.value()->key), "config/z");

  const auto status = store.GetStatus();
  ASSERT_TRUE(status);
  EXPECT_EQ(status.value().generation, reset.value().generation);
  EXPECT_EQ(status.value().recovery_state, ovf::per::RecoveryState::reset);
  EXPECT_EQ(status.value().recovery_count, 1U);
  EXPECT_GE(status.value().rejected_operations, 1U);
}

} // namespace
