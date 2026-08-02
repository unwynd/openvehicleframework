// SPDX-License-Identifier: Apache-2.0

#include "ovf/log/log.hpp"
#include "ovf/log/sink.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <vector>

namespace {

class CaptureSink final : public ovf::log::Sink {
public:
  bool Start(std::string_view application) noexcept override {
    application_ = application;
    return start_result;
  }
  bool Write(ovf::log::Record const& record) noexcept override {
    std::lock_guard lock(mutex_);
    records.push_back(record);
    condition_.notify_all();
    return write_result;
  }
  bool Flush(std::chrono::milliseconds) noexcept override { return true; }
  void Stop() noexcept override { stopped = true; }

  bool WaitFor(std::size_t size) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(1),
                               [&] { return records.size() >= size; });
  }

  bool start_result{true};
  bool write_result{true};
  bool stopped{};
  std::string application_;
  std::vector<ovf::log::Record> records;

private:
  std::mutex mutex_;
  std::condition_variable condition_;
};

constexpr ovf::log::Event kPublished{0x125A0041U, "frame_published", ovf::log::Level::info};

TEST(LogRuntimeTest, DeliversTypedStructuredRecord) {
  auto sink = std::make_unique<CaptureSink>();
  auto* capture = sink.get();
  auto runtime = ovf::log::Runtime::Create({.application_name = "camera"}, std::move(sink));
  ASSERT_NE(runtime, nullptr);
  auto logger = runtime->CreateLogger("camera.capture");
  EXPECT_EQ(logger.Event(kPublished, ovf::log::Field::Unsigned("sequence", 42),
                         ovf::log::Field::Text("class", "vehicle")),
            ovf::log::WriteResult::accepted);
  ASSERT_TRUE(capture->WaitFor(1));
  ASSERT_EQ(capture->records.size(), 1U);
  auto const& record = capture->records.front();
  EXPECT_EQ(record.event_id, kPublished.id);
  EXPECT_EQ(record.kind, ovf::log::RecordKind::event);
  EXPECT_EQ(std::string_view(record.text.data(), record.text_size), "frame_published");
  EXPECT_EQ(std::string_view(record.logger.data(), record.logger_size), "camera.capture");
  ASSERT_EQ(record.field_count, 2U);
  EXPECT_EQ(record.fields[0].value.unsigned_integer, 42U);
  EXPECT_EQ(record.sequence, 1U);
}

TEST(LogRuntimeTest, FiltersBeforeEncodingAndCountsOutcome) {
  auto sink = std::make_unique<CaptureSink>();
  auto runtime = ovf::log::Runtime::Create(
      {.application_name = "camera", .initial_level = ovf::log::Level::warning}, std::move(sink));
  ASSERT_NE(runtime, nullptr);
  auto logger = runtime->CreateLogger("camera.capture");
  EXPECT_FALSE(logger.Enabled(ovf::log::Level::info));
  EXPECT_EQ(logger.Event(kPublished), ovf::log::WriteResult::filtered);
  EXPECT_EQ(runtime->GetHealth().filtered, 1U);
}

TEST(LogRuntimeTest, RejectsInvalidIdentifiersAndOversizedValues) {
  auto sink = std::make_unique<CaptureSink>();
  auto runtime = ovf::log::Runtime::Create({.application_name = "camera"}, std::move(sink));
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(runtime->CreateLogger("").Event(kPublished), ovf::log::WriteResult::invalid);
  std::string oversized(ovf::log::kMaxStringValue + 1, 'x');
  EXPECT_EQ(
      runtime->CreateLogger("valid").Event(kPublished, ovf::log::Field::Text("value", oversized)),
      ovf::log::WriteResult::invalid);
  EXPECT_EQ(runtime->GetHealth().invalid, 1U);
}

TEST(LogRuntimeTest, ReportsBindingFailures) {
  auto sink = std::make_unique<CaptureSink>();
  sink->write_result = false;
  auto* capture = sink.get();
  auto runtime = ovf::log::Runtime::Create({.application_name = "camera"}, std::move(sink));
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(runtime->CreateLogger("camera.capture").Event(kPublished),
            ovf::log::WriteResult::accepted);
  ASSERT_TRUE(capture->WaitFor(1));
  ASSERT_TRUE(runtime->Flush(std::chrono::seconds(1)));
  EXPECT_EQ(runtime->GetHealth().binding_errors, 1U);
}

TEST(LogRuntimeTest, DeliversDiagnosticsWithoutInventingAnEventIdentifier) {
  auto sink = std::make_unique<CaptureSink>();
  auto* capture = sink.get();
  auto runtime = ovf::log::Runtime::Create({.application_name = "camera"}, std::move(sink));
  ASSERT_NE(runtime, nullptr);
  auto logger = runtime->CreateLogger("camera.capture");
  EXPECT_EQ(logger.Info("camera initialized", ovf::log::Field::Unsigned("device", 2)),
            ovf::log::WriteResult::accepted);
  ASSERT_TRUE(capture->WaitFor(1));
  auto const& record = capture->records.front();
  EXPECT_EQ(record.kind, ovf::log::RecordKind::diagnostic);
  EXPECT_EQ(record.event_id, 0U);
  EXPECT_EQ(record.level, ovf::log::Level::info);
  EXPECT_EQ(std::string_view(record.text.data(), record.text_size), "camera initialized");
}

TEST(LogRuntimeTest, ValidatesDiagnosticMessagesAndCountsInvalidInput) {
  auto runtime =
      ovf::log::Runtime::Create({.application_name = "camera"}, std::make_unique<CaptureSink>());
  ASSERT_NE(runtime, nullptr);
  auto logger = runtime->CreateLogger("camera.capture");
  EXPECT_EQ(logger.Warning(""), ovf::log::WriteResult::invalid);
  std::string oversized(ovf::log::kMaxDiagnosticMessage + 1, 'x');
  EXPECT_EQ(logger.Error(oversized), ovf::log::WriteResult::invalid);
  EXPECT_EQ(runtime->GetHealth().invalid, 2U);
}

TEST(LogRuntimeTest, RefusesInvalidRuntimeConfiguration) {
  EXPECT_EQ(ovf::log::Runtime::Create(
                {.application_name = "camera", .queue_capacity = 1, .critical_reserve = 1},
                std::make_unique<CaptureSink>()),
            nullptr);
}

} // namespace
