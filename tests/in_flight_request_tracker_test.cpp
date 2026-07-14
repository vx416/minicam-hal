#include "hal/in_flight_request_tracker.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

namespace minicam {
namespace {

std::vector<InFlightOutput> OutputBuffers(
    std::initializer_list<int> output_indexes) {
  std::vector<InFlightOutput> buffers;
  int stream_id = 0;
  for (int output_index : output_indexes) {
    buffers.push_back(InFlightOutput{
        .target =
            {
                .stream_id = stream_id++,
                .buffer_id = output_index,
                .stream_type = StreamType::Still,
            },
        .output_index = output_index,
        .completed = false,
    });
  }
  return buffers;
}

TEST(InFlightRequestTrackerTest, RejectsDuplicateFrameNumbers) {
  InFlightRequestTracker tracker;

  EXPECT_TRUE(tracker.start(CaptureRequest{.frame_number = 10,
                                           .output_buffers = {}},
                            OutputBuffers({3})));
  EXPECT_FALSE(tracker.start(CaptureRequest{.frame_number = 10,
                                            .output_buffers = {}},
                             OutputBuffers({4})));
  EXPECT_EQ(tracker.size(), 1U);
}

TEST(InFlightRequestTrackerTest, RequiresSubmittedStateBeforeQueued) {
  InFlightRequestTracker tracker;

  EXPECT_FALSE(tracker.mark_queued_to_driver(1));
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 1,
                                           .output_buffers = {}},
                            OutputBuffers({2})));
  EXPECT_TRUE(tracker.mark_queued_to_driver(1));
  EXPECT_FALSE(tracker.mark_queued_to_driver(1));
}

TEST(InFlightRequestTrackerTest, CompleteRemovesRequestAndPreservesMetadata) {
  InFlightRequestTracker tracker;
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 7,
                                           .output_buffers = {}},
                            OutputBuffers({1, 2})));

  CaptureMetadata metadata{
      .exposure_time = std::chrono::microseconds{3333},
      .analog_gain = 2.0F,
  };
  auto completed = tracker.complete(7, metadata);

  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->frame_number, 7U);
  ASSERT_EQ(completed->output_buffers.size(), 2U);
  EXPECT_EQ(completed->output_buffers[0].output_index, 1);
  EXPECT_EQ(completed->output_buffers[1].output_index, 2);
  EXPECT_EQ(completed->state, InFlightState::Completed);
  EXPECT_EQ(completed->metadata.exposure_time.count(), 3333);
  EXPECT_FLOAT_EQ(completed->metadata.analog_gain, 2.0F);
  EXPECT_FALSE(tracker.contains(7));
  EXPECT_EQ(tracker.size(), 0U);
}

TEST(InFlightRequestTrackerTest, CompleteOutputWaitsForEveryOutputBuffer) {
  InFlightRequestTracker tracker;
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 8,
                                           .output_buffers = {}},
                            OutputBuffers({1, 2})));

  auto first_output = tracker.complete_output(8, 1);
  EXPECT_EQ(first_output.state, OutputCompletionState::WaitingForMoreOutputs);
  EXPECT_FALSE(first_output.request.has_value());
  EXPECT_TRUE(tracker.contains(8));

  auto completed = tracker.complete_output(8, 2);
  EXPECT_EQ(completed.state, OutputCompletionState::RequestComplete);
  ASSERT_TRUE(completed.request.has_value());
  EXPECT_EQ(completed.request->frame_number, 8U);
  ASSERT_EQ(completed.request->output_buffers.size(), 2U);
  EXPECT_TRUE(completed.request->output_buffers[0].completed);
  EXPECT_TRUE(completed.request->output_buffers[1].completed);
  EXPECT_FALSE(tracker.contains(8));
}

TEST(InFlightRequestTrackerTest, MapsDriverTokenToRequestOutputContext) {
  InFlightRequestTracker tracker;
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 9,
                                           .output_buffers = {}},
                            OutputBuffers({1})));

  EXPECT_TRUE(tracker.bind_driver_output(
      DriverToken{.value = 42},
      DriverOutputContext{
          .frame_number = 9,
          .output_index = 1,
      }));

  auto resolved =
      tracker.take_driver_output_context(DriverToken{.value = 42});
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->frame_number, 9U);
  EXPECT_EQ(resolved->output_index, 1);
  EXPECT_EQ(resolved->target.buffer_id, 1);
  EXPECT_FALSE(resolved->streaming_output.has_value());
  EXPECT_FALSE(
      tracker.take_driver_output_context(DriverToken{.value = 42}).has_value());
}

TEST(InFlightRequestTrackerTest, MapsDriverTokenToStreamingOutput) {
  InFlightRequestTracker tracker;
  ASSERT_TRUE(tracker.register_streaming_output(InFlightStreamingOutput{
      .frame_number = 100,
      .target =
          OutputBufferTarget{
              .stream_id = 4,
              .buffer_id = 44,
              .stream_type = StreamType::Preview,
          },
      .stream_type = StreamType::Preview,
  }));
  ASSERT_TRUE(tracker.bind_driver_output(
      DriverToken{.value = 77},
      DriverOutputContext{
          .frame_number = 100,
          .output_index = -1,
      }));

  auto resolved =
      tracker.take_driver_output_context(DriverToken{.value = 77});
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->frame_number, 100U);
  EXPECT_EQ(resolved->output_index, -1);
  EXPECT_EQ(resolved->target.stream_id, 4);
  EXPECT_EQ(resolved->target.buffer_id, 44);
  ASSERT_TRUE(resolved->streaming_output.has_value());
  EXPECT_EQ(resolved->streaming_output->target.buffer_id, 44);
  EXPECT_FALSE(
      tracker.take_driver_output_context(DriverToken{.value = 77}).has_value());
}

TEST(InFlightRequestTrackerTest, FlushAllReturnsAndClearsActiveRequests) {
  InFlightRequestTracker tracker;
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 1,
                                           .output_buffers = {}},
                            OutputBuffers({0})));
  ASSERT_TRUE(tracker.start(CaptureRequest{.frame_number = 2,
                                           .output_buffers = {}},
                            OutputBuffers({1, 2})));

  auto flushed = tracker.flush_all();

  EXPECT_EQ(flushed.size(), 2U);
  EXPECT_EQ(tracker.size(), 0U);
  for (const auto& request : flushed) {
    EXPECT_EQ(request.state, InFlightState::Flushed);
  }
}

}  // namespace
}  // namespace minicam
