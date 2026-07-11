#include "hal/camera_device_session.h"
#include "hal/camera_hal_runtime.h"
#include "hal/mock_driver_adapter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace minicam {
namespace {

class ResultCollector {
 public:
  ResultCallback callback() {
    return [this](const CaptureResult& result) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        results_[result.frame_number] = result;
      }
      ready_.notify_all();
    };
  }

  std::optional<CaptureResult> wait_for(uint64_t frame_number) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool completed = ready_.wait_for(lock, std::chrono::seconds(2), [&] {
      return results_.contains(frame_number);
    });
    if (!completed) {
      return std::nullopt;
    }
    auto result = results_[frame_number];
    results_.erase(frame_number);
    return result;
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::unordered_map<uint64_t, CaptureResult> results_;
};

class CameraDeviceSessionTest : public ::testing::Test {
 protected:
  CameraHalRuntime runtime{
      CameraHalRuntimeConfig{
          .executor =
              {
                  .shard_count = 1,
                  .shard_queue_capacity = 16,
              },
      },
  };
  ResultCollector results;
  MockDriverAdapter* driver = nullptr;
  std::shared_ptr<CameraDeviceSession> session;

  bool StartStreamingSession(
      size_t buffer_count = 2,
      std::optional<StreamConfig> continuous_preview = std::nullopt,
      std::vector<OutputBufferTarget> continuous_preview_buffers = {}) {
    if (!runtime.start()) {
      return false;
    }

    auto mock_driver = std::make_unique<MockDriverAdapter>();
    driver = mock_driver.get();
    session = std::make_shared<CameraDeviceSession>(
        runtime,
        CameraDeviceSessionConfig{
            .session_id = 7,
            .buffer_count = buffer_count,
            .continuous_preview_config = std::move(continuous_preview),
            .continuous_preview_buffers = std::move(continuous_preview_buffers),
        },
        std::move(mock_driver),
        results.callback());
    return session->configure_streams() && session->start_streaming();
  }

  bool StartRuntimeOnlySession(size_t buffer_count = 1) {
    if (!runtime.start()) {
      return false;
    }

    auto mock_driver = std::make_unique<MockDriverAdapter>();
    driver = mock_driver.get();
    session = std::make_shared<CameraDeviceSession>(
        runtime,
        CameraDeviceSessionConfig{
            .session_id = 7,
            .buffer_count = buffer_count,
            .continuous_preview_config = std::nullopt,
            .continuous_preview_buffers = {},
        },
        std::move(mock_driver),
        results.callback());
    return true;
  }

  void TearDown() override {
    if (session) {
      session->close();
    }
    runtime.stop();
  }
};

TEST_F(CameraDeviceSessionTest, RejectsRequestsBeforeStreaming) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartRuntimeOnlySession());

  EXPECT_FALSE(session->process_capture_request(
      CaptureRequest{.frame_number = 1, .output_buffers = {}}));
#endif
}

TEST_F(CameraDeviceSessionTest, DispatchesSuccessfulCaptureResult) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartStreamingSession());

  EXPECT_TRUE(session->process_capture_request(CaptureRequest{
      .frame_number = 10,
      .width = 16,
      .height = 8,
      .output_buffers = {},
  }));

  auto result = results.wait_for(10);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, CaptureStatus::Ok);
  EXPECT_EQ(result->frame_number, 10U);
  ASSERT_EQ(result->completed_output_buffers.size(), 1U);
  EXPECT_EQ(result->completed_output_buffers[0].buffer_id, -1);
  EXPECT_EQ(result->completed_output_buffers[0].status, CaptureStatus::Ok);
  EXPECT_EQ(result->width, 16);
  EXPECT_EQ(result->height, 8);
  EXPECT_EQ(result->payload_format, FramePayloadFormat::Rgb24);

  const auto metrics = session->metrics();
  EXPECT_EQ(metrics.completed(), 1U);
  EXPECT_EQ(metrics.failed(), 0U);
#endif
}

TEST_F(CameraDeviceSessionTest, CompletesMultipleOutputBuffersForOneRequest) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartStreamingSession(/*buffer_count=*/3));

  EXPECT_TRUE(session->process_capture_request(CaptureRequest{
      .frame_number = 11,
      .width = 16,
      .height = 8,
      .output_buffers =
          {
              OutputBufferTarget{
                  .stream_id = 1,
                  .buffer_id = 101,
                  .stream_type = StreamType::Preview,
                  .width = 16,
                  .height = 8,
                  .format = PixelFormat::Rgb24,
              },
              OutputBufferTarget{
                  .stream_id = 2,
                  .buffer_id = 102,
                  .stream_type = StreamType::Still,
                  .width = 16,
                  .height = 8,
                  .format = PixelFormat::Rgb24,
              },
          },
  }));

  auto result = results.wait_for(11);
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(driver, nullptr);
  EXPECT_EQ(driver->submit_count(), 2U);
  EXPECT_EQ(result->status, CaptureStatus::Ok);
  ASSERT_EQ(result->completed_output_buffers.size(), 2U);
  EXPECT_EQ(result->completed_output_buffers[0].stream_id, 1);
  EXPECT_EQ(result->completed_output_buffers[0].buffer_id, 101);
  EXPECT_EQ(result->completed_output_buffers[0].status, CaptureStatus::Ok);
  EXPECT_EQ(result->completed_output_buffers[1].stream_id, 2);
  EXPECT_EQ(result->completed_output_buffers[1].buffer_id, 102);
  EXPECT_EQ(result->completed_output_buffers[1].status, CaptureStatus::Ok);

  EXPECT_TRUE(session->process_capture_request(CaptureRequest{
      .frame_number = 12,
      .width = 4,
      .height = 4,
      .output_buffers = {},
  }));
  auto next_result = results.wait_for(12);
  ASSERT_TRUE(next_result.has_value());
  EXPECT_EQ(next_result->status, CaptureStatus::Ok);
  ASSERT_EQ(next_result->completed_output_buffers.size(), 1U);
  EXPECT_EQ(next_result->completed_output_buffers[0].buffer_id, -1);
#endif
}

TEST_F(CameraDeviceSessionTest, ContinuousPreviewDispatchesWithoutCaptureRequest) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartStreamingSession(
      /*buffer_count=*/2,
      StreamConfig{
          .stream_id = 9,
          .stream_type = StreamType::Preview,
          .width = 8,
          .height = 4,
          .format = PixelFormat::Rgb24,
      },
      {
          OutputBufferTarget{
              .stream_id = 9,
              .buffer_id = 901,
              .stream_type = StreamType::Preview,
              .width = 8,
              .height = 4,
              .format = PixelFormat::Rgb24,
          },
      }));

  auto preview = results.wait_for(1000000000000ULL);
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->status, CaptureStatus::Ok);
  EXPECT_EQ(preview->message, "preview");
  ASSERT_EQ(preview->completed_output_buffers.size(), 1U);
  EXPECT_EQ(preview->completed_output_buffers[0].stream_id, 9);
  EXPECT_EQ(preview->completed_output_buffers[0].buffer_id, 901);
  EXPECT_EQ(preview->width, 8);
  EXPECT_EQ(preview->height, 4);

  EXPECT_TRUE(session->process_capture_request(CaptureRequest{
      .frame_number = 31,
      .width = 4,
      .height = 4,
      .output_buffers = {},
  }));
  auto still = results.wait_for(31);
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(still->status, CaptureStatus::Ok);
  EXPECT_EQ(still->message, "ok");
#endif
}

TEST_F(CameraDeviceSessionTest, ContinuousPreviewRequiresAppBuffers) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  EXPECT_FALSE(StartStreamingSession(
      /*buffer_count=*/2,
      StreamConfig{
          .stream_id = 9,
          .stream_type = StreamType::Preview,
          .width = 8,
          .height = 4,
          .format = PixelFormat::Rgb24,
      }));
  ASSERT_TRUE(session != nullptr);
  EXPECT_EQ(session->state(), CameraSessionState::Error);
#endif
}

TEST_F(CameraDeviceSessionTest, SubmitFailureReturnsErrorAndReleasesBuffer) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartStreamingSession(/*buffer_count=*/1));
  ASSERT_NE(driver, nullptr);

  driver->set_fail_submit(true);
  EXPECT_TRUE(session->process_capture_request(
      CaptureRequest{.frame_number = 20, .output_buffers = {}}));

  auto failed = results.wait_for(20);
  ASSERT_TRUE(failed.has_value());
  EXPECT_EQ(failed->status, CaptureStatus::SensorError);
  ASSERT_EQ(failed->completed_output_buffers.size(), 1U);
  EXPECT_EQ(failed->completed_output_buffers[0].status,
            CaptureStatus::SensorError);
  EXPECT_EQ(failed->message, "mock submit failure");

  driver->set_fail_submit(false);
  EXPECT_TRUE(session->process_capture_request(CaptureRequest{
      .frame_number = 21,
      .width = 4,
      .height = 4,
      .output_buffers = {},
  }));

  auto recovered = results.wait_for(21);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->status, CaptureStatus::Ok);
  ASSERT_EQ(recovered->completed_output_buffers.size(), 1U);
  EXPECT_EQ(recovered->completed_output_buffers[0].buffer_id, -1);

  const auto metrics = session->metrics();
  EXPECT_EQ(metrics.completed(), 1U);
  EXPECT_EQ(metrics.failed(), 1U);
#endif
}

TEST_F(CameraDeviceSessionTest, CloseRejectsFutureRequests) {
#ifndef __linux__
  GTEST_SKIP() << "CameraHalRuntime uses epoll";
#else
  ASSERT_TRUE(StartStreamingSession());

  session->close();
  EXPECT_EQ(session->state(), CameraSessionState::Stopped);
  EXPECT_FALSE(session->process_capture_request(
      CaptureRequest{.frame_number = 30, .output_buffers = {}}));

#endif
}

}  // namespace
}  // namespace minicam
