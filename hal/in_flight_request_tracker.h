#pragma once

#include "hal/driver_adapter.h"
#include "interface/capture_request.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace minicam {

// Lifecycle state for a request after CameraDeviceSession has accepted it into
// HAL accounting. Runtime executor tasks that have not run yet are outside this
// table.
enum class InFlightState {
  Submitted,
  QueuedToDriver,
  Completed,
  Failed,
  Flushed,
};

// Per-frame metadata that HAL returns alongside image buffer completion. This is
// intentionally tiny; real camera results include AE/AWB/AF state, timestamps,
// sensor settings, stream information, and partial result state.
struct CaptureMetadata {
  std::chrono::steady_clock::time_point sensor_timestamp =
      std::chrono::steady_clock::now();
  std::chrono::microseconds exposure_time{0};
  float analog_gain = 1.0F;
};

struct InFlightOutput {
  OutputBufferTarget target;
  int output_index = -1;
  bool completed = false;
  int release_fence_fd = -1;
};

// Accounting row for one active frame. This is the key HAL bookkeeping object:
// it connects the app-facing frame number to the original request, output
// slots, metadata, and completion state.
struct InFlightRequest {
  uint64_t frame_number = 0;
  CaptureRequest request;
  std::vector<InFlightOutput> output_buffers;
  CaptureMetadata metadata;
  InFlightState state = InFlightState::Submitted;
  std::chrono::steady_clock::time_point submitted_at =
      std::chrono::steady_clock::now();
};

struct InFlightStreamingOutput {
  uint64_t frame_number = 0;
  OutputBufferTarget target;
  StreamType stream_type = StreamType::Preview;
};

struct DriverOutputContext {
  uint64_t frame_number = 0;
  int output_index = -1;
};

struct ResolvedDriverOutput {
  uint64_t frame_number = 0;
  int output_index = -1;
  OutputBufferTarget target;
  std::optional<InFlightStreamingOutput> streaming_output;
};

enum class OutputCompletionState {
  NotFound,
  WaitingForMoreOutputs,
  RequestComplete,
};

struct OutputCompletionResult {
  OutputCompletionState state = OutputCompletionState::NotFound;
  std::optional<InFlightRequest> request;
};

// Tracks active frames that have left the pending queue but have not yet sent a
// final CaptureResult. Completion/failure/flush remove rows from the table so
// leaks are visible in tests.
class InFlightRequestTracker {
 public:
  // Starts one frame. Duplicate frame numbers are rejected because result
  // callbacks must be unambiguous.
  bool start(CaptureRequest request, std::vector<InFlightOutput> output_buffers);

  // Marks the frame as handed to the backend/driver.
  bool mark_queued_to_driver(uint64_t frame_number);

  // Completes and removes one frame.
  std::optional<InFlightRequest> complete(
      uint64_t frame_number, CaptureMetadata metadata = {});

  // Marks one output buffer complete. Returns the full request only when every
  // output buffer for the frame is complete.
  OutputCompletionResult complete_output(
      uint64_t frame_number,
      int output_index,
      int release_fence_fd = -1,
      CaptureMetadata metadata = {});

  bool register_streaming_output(InFlightStreamingOutput output);

  bool bind_driver_output(DriverToken token, DriverOutputContext context);
  std::optional<ResolvedDriverOutput> take_driver_output_context(
      DriverToken token);
  void clear_driver_outputs_for_frame(uint64_t frame_number);
  void clear_streaming_outputs();

  // Fails and removes one frame.
  std::optional<InFlightRequest> fail(uint64_t frame_number);

  // Marks every active frame flushed, returns them to the caller, and clears the
  // table. The session is responsible for dispatching flushed results.
  std::vector<InFlightRequest> flush_all();

  bool contains(uint64_t frame_number) const;
  size_t size() const;

 private:
  std::unordered_map<uint64_t, InFlightRequest> requests_;
  std::unordered_map<uint64_t, InFlightStreamingOutput> streaming_outputs_;
  std::unordered_map<uint64_t, DriverOutputContext> driver_outputs_;

  std::optional<InFlightStreamingOutput> take_streaming_output(
      uint64_t frame_number);
  std::optional<OutputBufferTarget> output_target(uint64_t frame_number,
                                                  int output_index) const;
};

}  // namespace minicam
