#pragma once

#include "hal/driver_adapter.h"
#include "hal/epoll_event_loop.h"
#include "hal/in_flight_request_tracker.h"
#include "hal/frame_metrics.h"
#include "hal/output_processor.h"
#include "interface/camera_device.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace minicam {

class CameraHalRuntime;

using StreamResultCallback =
    std::function<StreamBufferLease(const CaptureResult&, StreamBufferLease)>;

// Admission policy for framework-facing capture requests. This intentionally
// excludes caller-runs execution because running a session task on the framework
// thread can violate the per-session ordering guaranteed by the shard worker.
enum class CaptureRequestRejectionPolicy {
  ReturnError,
};

// Session construction parameters. This models the state created after the
// framework opens/configures one camera stream.
struct CameraDeviceSessionConfig {
  int session_id = 0;
  size_t buffer_count = 4;
  std::optional<StreamConfig> continuous_preview_config;
  std::vector<OutputBufferTarget> continuous_preview_buffers;

  // Backpressure policy for framework-facing capture request admission when
  // the shared executor shard is saturated. Keeping this explicit makes the
  // framework/HAL contract visible at the session boundary.
  CaptureRequestRejectionPolicy capture_request_rejection_policy =
      CaptureRequestRejectionPolicy::ReturnError;
};

enum class CameraSessionState {
  Created,
  Configured,
  Streaming,
  Flushing,
  Stopping,
  Stopped,
  Error,
};

// HAL session object. Think of this like a server-side connection object:
// CameraHalRuntime owns shared event resources, while CameraDeviceSession owns
// per-session camera semantics and hooks.
//
// Responsibilities:
// - accept framework CaptureRequest objects
// - assign/track output buffer slots
// - map frame_number <-> output buffer <-> V4L2 driver buffer completion
// - assemble CaptureResult and dispatch callbacks
// - coordinate flush/stop lifecycle
//
// Non-responsibilities:
// - owning a private epoll loop
// - owning a permanent per-session worker thread
// - implementing low-level V4L2 ioctl/mmap mechanics directly
class CameraDeviceSession final
    : public CameraDevice,
      public std::enable_shared_from_this<CameraDeviceSession> {
 public:
  CameraDeviceSession(CameraHalRuntime& runtime,
                      CameraDeviceSessionConfig config,
                      std::unique_ptr<DriverAdapter> driver,
                      ResultCallback result_callback,
                      std::vector<std::shared_ptr<OutputProcessor>>
                          output_processors = {},
                      StreamResultCallback stream_result_callback = {});
  ~CameraDeviceSession();

  CameraDeviceSession(const CameraDeviceSession&) = delete;
  CameraDeviceSession& operator=(const CameraDeviceSession&) = delete;

  int session_id() const;
  CameraSessionState state() const;

  // Configures the driver stream and HAL buffer tracking, then registers the
  // driver's fd with CameraHalRuntime.
  bool configure_streams() override;

  // Starts V4L2 streaming. This does not create a per-session event loop.
  bool start_streaming() override;

  // Framework-facing submit path. Thin HAL model:
  // validate -> record in-flight -> QBUF to driver -> return.
  bool process_capture_request(CaptureRequest request) override;

  // Runtime hook. CameraHalRuntime calls this when epoll reports fd readiness.
  void on_driver_fd_ready(int ready_fd, FdReadyEvents events);

  // Session semantic hook. Converts one driver completion into a framework
  // CaptureResult using the in-flight table and output buffer tracker.
  void on_driver_buffer_complete(const DriverCompletion& completion);

  // Completes pending/in-flight requests according to HAL flush semantics.
  void flush() override;

  // Stops streaming, unregisters from runtime, and rejects future requests.
  void close() override;

  FrameMetrics metrics() const;

 private:
  bool reject_request_locked() const;
  void submit_capture_request(CaptureRequest request);
  std::vector<InFlightOutput> make_in_flight_outputs(
      const std::vector<OutputBufferTarget>& outputs) const;
  bool start_in_flight_request_locked(
      const CaptureRequest& request,
      std::vector<InFlightOutput> tracked_outputs);
  bool submit_request_to_driver_locked(
      const CaptureRequest& request,
      const std::vector<OutputBufferTarget>& outputs,
      const std::vector<InFlightOutput>& in_flight_outputs);
  bool try_fill_result_from_completion_locked(
      const DriverCompletion& completion,
      CaptureResult* result);
  void fill_failed_result_locked(uint64_t frame_number,
                                 CaptureStatus status,
                                 std::string message,
                                 CaptureResult* result,
                                 std::vector<CompletedOutputBuffer>
                                     fallback_completed_buffers = {},
                                 std::chrono::steady_clock::time_point
                                     fallback_submitted_at =
                                         std::chrono::steady_clock::now());

  mutable std::mutex mutex_;
  CameraHalRuntime& runtime_;
  CameraDeviceSessionConfig config_;
  CameraSessionState state_ = CameraSessionState::Created;

  std::unique_ptr<DriverAdapter> driver_;
  std::vector<int> driver_fds_;
  InFlightRequestTracker in_flight_;
  ResultCallback result_callback_;
  StreamResultCallback stream_result_callback_;
  std::vector<std::shared_ptr<OutputProcessor>> output_processors_;
  FrameMetrics metrics_;
};

}  // namespace minicam
