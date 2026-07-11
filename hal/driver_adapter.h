#pragma once

#include "interface/capture_request.h"
#include "interface/capture_result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace minicam {

// Driver-facing completion event after a queued capture buffer is done.
// V4L2 DQBUF will eventually produce this data. CameraDeviceSession consumes it
// to map low-level buffer completion back to CaptureResult.
struct DriverCompletion {
  int output_index = -1;
  int stream_id = 0;
  int buffer_id = -1;
  uint64_t frame_number = 0;
  size_t bytes_used = 0;
  std::chrono::steady_clock::time_point timestamp =
      std::chrono::steady_clock::now();
  int width = 0;
  int height = 0;
  FramePayloadFormat payload_format = FramePayloadFormat::None;
  int buffer_fd = -1;
  size_t buffer_size = 0;
};

struct DriverOutputBuffer {
  uint64_t frame_number = 0;
  int buffer_id = -1;
  OutputBufferTarget target;
  int output_index = -1;
};

// Generic driver-facing adapter used by CameraDeviceSession. Different capture
// backends can implement this interface: V4L2 vivid, USB webcam, fake driver,
// or a future vendor-specific adapter.
class DriverAdapter {
 public:
  virtual ~DriverAdapter() = default;

  // Prepares the driver stream resources. Implementations may open a device,
  // set format, request/mmap buffers, and prepare queue state.
  virtual bool configure_stream() = 0;

  virtual bool start_streaming() = 0;
  virtual size_t required_output_buffer_size(
      const OutputBufferTarget& output) const = 0;
  virtual bool start_continuous_stream(
      const StreamConfig& stream,
      std::vector<DriverOutputBuffer> buffers) = 0;
  virtual void stop_streaming() = 0;
  virtual void close() = 0;

  // Submits one output target from a framework request to the driver. The
  // output_index is the request output vector index used to map completion back
  // to CaptureResult.
  virtual bool submit_capture(DriverOutputBuffer output) = 0;

  // Dequeues one completed driver buffer after ready_fd becomes ready.
  virtual std::optional<DriverCompletion> dequeue_completion(int ready_fd) = 0;

  // Returns a standalone streaming buffer after the result callback has
  // finished reading it. Request-driven captures do not use this path.
  virtual bool return_stream_buffer(const DriverCompletion& completion) {
    (void)completion;
    return true;
  }

  // File descriptors registered with CameraHalRuntime's event loop.
  virtual std::vector<int> event_fds() const = 0;
  virtual const std::string& last_error() const = 0;
};

}  // namespace minicam
