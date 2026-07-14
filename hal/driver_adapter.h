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

struct DriverToken {
  uint64_t value = 0;

  friend bool operator==(DriverToken lhs, DriverToken rhs) {
    return lhs.value == rhs.value;
  }
};

struct DriverSubmission {
  DriverToken token;
};

// Driver-facing completion event after a queued capture buffer is done. The
// token is opaque to the session; InFlightRequestTracker maps it back to the
// request or streaming output context that submitted the buffer.
struct DriverCompletion {
  DriverToken token;
  size_t bytes_used = 0;
  std::chrono::steady_clock::time_point timestamp =
      std::chrono::steady_clock::now();
};

struct DriverOutputBuffer {
  OutputBufferTarget target;
};

struct StreamBufferLease {
  uint64_t frame_number = 0;
  int stream_id = 0;
  StreamType stream_type = StreamType::Preview;
  int buffer_id = -1;
  int buffer_fd = -1;
  size_t buffer_size = 0;
  int consumer_release_fence_fd = -1;
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
  virtual std::optional<std::vector<DriverSubmission>> start_continuous_stream(
      const StreamConfig& stream,
      std::vector<DriverOutputBuffer> buffers) = 0;
  virtual void stop_streaming() = 0;
  virtual void close() = 0;

  // CameraDeviceSession uses this to avoid partially queueing a multi-output
  // request. A backend should return true only when it can submit every output
  // in the request without first queueing a subset.
  virtual bool can_submit_capture_outputs(
      const std::vector<OutputBufferTarget>& outputs) const = 0;

  // Submits one output target from a framework request to the driver.
  virtual std::optional<DriverSubmission> submit_capture(
      DriverOutputBuffer output) = 0;

  // Dequeues one completed driver buffer after ready_fd becomes ready.
  virtual std::optional<DriverCompletion> dequeue_completion(int ready_fd) = 0;

  // Returns a standalone streaming buffer. Request-driven captures do not use
  // this path. The driver must not overwrite the buffer until
  // consumer_release_fence_fd has signaled.
  virtual std::optional<DriverSubmission> return_stream_buffer(
      StreamBufferLease lease) {
    (void)lease;
    return DriverSubmission{};
  }

  // File descriptors registered with CameraHalRuntime's event loop.
  virtual std::vector<int> event_fds() const = 0;
  virtual const std::string& last_error() const = 0;
};

}  // namespace minicam
