// SPDX-License-Identifier: Apache-2.0

#include "typed_state/ovf_record.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

TEST(PerTypedContractTest, EncodesDeterministicallyAndRejectsAnIncompatibleEnvelope) {
  example::state::OperationalState state{};
  state.sequence = 42;
  ASSERT_TRUE(state.mode.assign("active"));
  example::state::ModeText reason;
  ASSERT_TRUE(reason.assign("startup"));
  state.reason = reason;

  const auto first = example::state::OperationalStatePersistent::Encode(state, 256);
  const auto second = example::state::OperationalStatePersistent::Encode(state, 256);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());

  const auto decoded = example::state::OperationalStatePersistent::Decode(first.value());
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded.value(), state);

  auto incompatible = first.value();
  incompatible[27] = std::byte{2};
  const auto rejected = example::state::OperationalStatePersistent::Decode(incompatible);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ovf::per::ErrorCode::corrupted);

  const auto too_small = example::state::OperationalStatePersistent::Encode(state, 16);
  ASSERT_FALSE(too_small);
  EXPECT_EQ(too_small.error().code, ovf::per::ErrorCode::quota_exceeded);
}

extern "C" const ovf_per_backend_factory_v1* ovf_per_backend_query_v1(void);

TEST(PerTypedContractTest, TypedFacadeWritesAndReadsWithoutDynamicKeys) {
  auto runtime_result = ovf::per::Runtime::Create(*ovf_per_backend_query_v1());
  ASSERT_TRUE(runtime_result);
  auto runtime = std::move(runtime_result).value();
  auto store_result = runtime->OpenStore({.logical_name = "typed-state",
                                          .access = ovf::per::Access::read_write,
                                          .minimum_durability = ovf::per::Durability::buffered,
                                          .capacity_bytes = 4096,
                                          .max_entries = 4,
                                          .max_key_size = 64,
                                          .max_value_size = 512});
  ASSERT_TRUE(store_result);
  auto store = std::move(store_result).value();

  example::state::OperationalState expected{};
  expected.sequence = 9;
  ASSERT_TRUE(expected.mode.assign("ready"));
  auto write_result = store.BeginWrite(ovf::per::Durability::buffered);
  ASSERT_TRUE(write_result);
  auto write = std::move(write_result).value();
  ASSERT_TRUE(example::state::OperationalStatePersistent::Put(write, expected, 512));
  ASSERT_TRUE(write.Commit());

  auto read_result = store.BeginRead();
  ASSERT_TRUE(read_result);
  auto read = std::move(read_result).value();
  const auto actual = example::state::OperationalStatePersistent::Get(read);
  ASSERT_TRUE(actual) << actual.error().message;
  ASSERT_TRUE(actual.value());
  EXPECT_EQ(*actual.value(), expected);
}
