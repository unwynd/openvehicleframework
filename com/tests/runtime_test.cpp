// SPDX-License-Identifier: Apache-2.0

#include "ovf/com/runtime.hpp"
#include "ovf/com/transports/inproc.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(RuntimeTest, OwnsAndOrdersTransportLifecycle) {
  std::vector<std::string> messages;
  ovf::com::Runtime runtime(
      {"runtime-test",
       [&](auto, std::string_view message) { messages.emplace_back(message); },
       {}});

  EXPECT_EQ(runtime.LoadTransport("../invalid"), ovf::com::RuntimeError::invalid_argument);
  EXPECT_EQ(runtime.LoadTransport("provider-that-does-not-exist"),
            ovf::com::RuntimeError::not_found);

  const auto* factory = ovf_com_inproc_transport_query_v1();
  ASSERT_TRUE(factory != nullptr);
  ASSERT_TRUE(runtime.AddTransport(*factory) == ovf::com::RuntimeError::none);
  ASSERT_TRUE(runtime.AddTransport(*factory) == ovf::com::RuntimeError::duplicate_transport);
  ASSERT_TRUE(runtime.TransportNames() == std::vector<std::string>{"inproc"});
  std::vector<ovf::com::TransportHealthState> health;
  runtime.OnHealth([&](auto const& update) {
    EXPECT_EQ(update.provider, "inproc");
    health.push_back(update.state);
  });
  ASSERT_EQ(runtime.Health().size(), 1U);
  EXPECT_EQ(runtime.Health().front().state, ovf::com::TransportHealthState::stopped);

  ASSERT_TRUE(runtime.Start() == ovf::com::RuntimeError::none);
  ASSERT_TRUE(runtime.IsRunning());
  ASSERT_TRUE(runtime.Start() == ovf::com::RuntimeError::invalid_state);
  ASSERT_TRUE(runtime.AddTransport(*factory) == ovf::com::RuntimeError::invalid_state);

  runtime.Stop();
  ASSERT_TRUE(!runtime.IsRunning());
  ASSERT_GE(health.size(), 3U);
  EXPECT_EQ(health[health.size() - 2U], ovf::com::TransportHealthState::ready);
  EXPECT_EQ(health.back(), ovf::com::TransportHealthState::stopped);
  ASSERT_TRUE(messages ==
              std::vector<std::string>({"inproc transport started", "inproc transport stopped"}));
}
