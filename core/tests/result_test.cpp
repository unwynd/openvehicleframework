// SPDX-License-Identifier: Apache-2.0

#include "ovf/core/result.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

struct TestError {
  int code;
  std::string message;
};

using IntResult = ovf::core::Result<int, TestError>;
using StringResult = ovf::core::Result<std::string, TestError>;
using VoidResult = ovf::core::Result<void, TestError>;

TEST(CoreResult, HoldsValue) {
  IntResult result{42};
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result.value(), 42);
}

TEST(CoreResult, HoldsError) {
  IntResult result{TestError{7, "nope"}};
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().code, 7);
  EXPECT_EQ(result.error().message, "nope");
}

TEST(CoreResult, MovesValueOut) {
  StringResult result{std::string{"payload"}};
  auto moved = std::move(result).value();
  EXPECT_EQ(moved, "payload");
}

TEST(CoreResult, MapTransformsValue) {
  IntResult result{5};
  auto mapped = std::move(result).map([](int value) { return value * 2; });
  EXPECT_TRUE(mapped.has_value());
  EXPECT_EQ(mapped.value(), 10);
}

TEST(CoreResult, MapPreservesError) {
  IntResult result{TestError{3, "bad"}};
  auto mapped = std::move(result).map([](int value) { return value * 2; });
  EXPECT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code, 3);
}

TEST(CoreResult, MapTransformsValueToVoid) {
  IntResult result{5};
  auto mapped = std::move(result).map([](int) {});
  EXPECT_TRUE(mapped.has_value());
}

TEST(CoreResult, MapToVoidPreservesError) {
  IntResult result{TestError{3, "bad"}};
  auto mapped = std::move(result).map([](int) {});
  EXPECT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code, 3);
}

TEST(CoreResult, AndThenChains) {
  IntResult result{5};
  auto next = std::move(result).and_then(
      [](int value) -> StringResult { return std::to_string(value * 3); });
  EXPECT_TRUE(next.has_value());
  EXPECT_EQ(next.value(), "15");
}

TEST(CoreResult, AndThenPropagatesError) {
  IntResult result{TestError{4, "propagate"}};
  auto next = std::move(result).and_then([](int) -> StringResult { return std::string{"never"}; });
  EXPECT_FALSE(next.has_value());
  EXPECT_EQ(next.error().code, 4);
}

TEST(CoreResult, OrElseRecovers) {
  IntResult result{TestError{1, "recover"}};
  auto recovered =
      std::move(result).or_else([](TestError err) -> IntResult { return err.code * 10; });
  EXPECT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered.value(), 10);
}

TEST(CoreResultVoid, EmptySuccess) {
  VoidResult result{};
  EXPECT_TRUE(result.has_value());
}

TEST(CoreResultVoid, HoldsError) {
  VoidResult result{TestError{9, "void"}};
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, 9);
}

TEST(CoreResultVoid, AndThenChains) {
  VoidResult result{};
  auto next = std::move(result).and_then([]() -> IntResult { return 7; });
  EXPECT_TRUE(next.has_value());
  EXPECT_EQ(next.value(), 7);
}

TEST(CoreResultVoid, OrElseRecovers) {
  VoidResult result{TestError{2, "boom"}};
  auto recovered = std::move(result).or_else([](TestError) -> VoidResult { return {}; });
  EXPECT_TRUE(recovered.has_value());
}

} // namespace
