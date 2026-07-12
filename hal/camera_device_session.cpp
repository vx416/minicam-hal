#include "hal/camera_device_session.h"

#include "hal/camera_hal_runtime.h"

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include <unistd.h>

namespace minicam {

namespace {

std::chrono::microseconds latency_since(
    std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);
}

CaptureResult make_result(uint64_t frame_number,
                          CaptureStatus status,
                          std::vector<CompletedOutputBuffer> completed_buffers,
                          std::string message,
                          std::chrono::microseconds latency) {
  return CaptureResult{
      .frame_number = frame_number,
      .status = status,
      .completed_output_buffers = std::move(completed_buffers),
      .message = std::move(message),
      .latency = latency,
      .width = 0,
      .height = 0,
      .payload_format = FramePayloadFormat::None,
  };
}

std::vector<OutputBufferTarget> request_outputs_for(
    const CaptureRequest& request) {
  if (!request.output_buffers.empty()) {
    return request.output_buffers;
  }
  return {
      OutputBufferTarget{
          .stream_id = 0,
          .buffer_id = -1,
          .buffer_fd = -1,
          .buffer_size = 0,
          .stream_type = StreamType::Still,
          .width = request.width,
          .height = request.height,
          .format = PixelFormat::Rgb24,
      },
  };
}

std::vector<CompletedOutputBuffer> completed_buffers_for(
    const std::vector<InFlightOutput>& buffers,
    CaptureStatus status) {
  std::vector<CompletedOutputBuffer> completed;
  completed.reserve(buffers.size());
  for (const auto& buffer : buffers) {
    completed.push_back(CompletedOutputBuffer{
        .stream_id = buffer.target.stream_id,
        .buffer_id = buffer.target.buffer_id,
        .output_index = buffer.output_index,
        .status = status,
        .release_fence_fd = buffer.release_fence_fd,
    });
  }
  return completed;
}

void close_fence_fd(int fence_fd) {
  if (fence_fd >= 0) {
    ::close(fence_fd);
  }
}

bool is_standalone_stream_completion(const DriverCompletion& completion) {
  return completion.output_index < 0;
}

CaptureResult capture_result_from_standalone_completion(
    const DriverCompletion& completion) {
  return CaptureResult{
      .frame_number = completion.frame_number,
      .status = CaptureStatus::Ok,
      .completed_output_buffers =
          {
              CompletedOutputBuffer{
                  .stream_id = completion.stream_id,
                  .buffer_id = completion.buffer_id,
                  .output_index = completion.output_index,
                  .status = CaptureStatus::Ok,
                  .release_fence_fd = completion.release_fence_fd,
              },
          },
      .message = "preview",
      .latency = std::chrono::microseconds{0},
      .width = completion.width,
      .height = completion.height,
      .payload_format = completion.payload_format,
  };
}

std::optional<CaptureResult> capture_result_from_output_completion(
    const OutputCompletionResult& output_completion,
    const DriverCompletion& completion) {
  if (output_completion.state != OutputCompletionState::RequestComplete ||
      !output_completion.request) {
    return std::nullopt;
  }

  const auto& request = *output_completion.request;
  const auto latency = latency_since(request.submitted_at);
  return CaptureResult{
      .frame_number = request.frame_number,
      .status = CaptureStatus::Ok,
      .completed_output_buffers = completed_buffers_for(
          request.output_buffers, CaptureStatus::Ok),
      .message = "ok",
      .latency = latency,
      .width = completion.width,
      .height = completion.height,
      .payload_format = completion.payload_format,
  };
}

}  // namespace

CameraDeviceSession::CameraDeviceSession(
    CameraHalRuntime& runtime,
    CameraDeviceSessionConfig config,
    std::unique_ptr<DriverAdapter> driver,
    ResultCallback result_callback,
    std::vector<std::shared_ptr<OutputProcessor>> output_processors)
    : runtime_(runtime),
      config_(config),
      driver_(std::move(driver)),
      result_callback_(std::move(result_callback)),
      output_processors_(std::move(output_processors)) {}

CameraDeviceSession::~CameraDeviceSession() {
  close();
}

int CameraDeviceSession::session_id() const {
  return config_.session_id;
}

CameraSessionState CameraDeviceSession::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool CameraDeviceSession::configure_streams() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (driver_ == nullptr || state_ != CameraSessionState::Created) {
    return false;
  }

  if (!driver_->configure_stream()) {
    state_ = CameraSessionState::Error;
    return false;
  }

  driver_fds_ = driver_->event_fds();
  if (!runtime_.register_session(config_.session_id,
                                 weak_from_this(),
                                 driver_fds_)) {
    state_ = CameraSessionState::Error;
    return false;
  }

  state_ = CameraSessionState::Configured;
  return true;
}

bool CameraDeviceSession::start_streaming() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (driver_ == nullptr || state_ != CameraSessionState::Configured) {
    return false;
  }

  if (!driver_->start_streaming()) {
    state_ = CameraSessionState::Error;
    return false;
  }

  if (config_.continuous_preview_config.has_value()) {
    if (config_.continuous_preview_buffers.empty()) {
      state_ = CameraSessionState::Error;
      return false;
    }

    std::vector<DriverOutputBuffer> preview_buffers;
    preview_buffers.reserve(config_.continuous_preview_buffers.size());
    for (const auto& output : config_.continuous_preview_buffers) {
      preview_buffers.push_back(DriverOutputBuffer{
          .frame_number = 0,
          .buffer_id = output.buffer_id,
          .target = output,
          .output_index = -1,
      });
    }

    if (!driver_->start_continuous_stream(
            *config_.continuous_preview_config, std::move(preview_buffers))) {
      state_ = CameraSessionState::Error;
      return false;
    }
  }

  state_ = CameraSessionState::Streaming;
  return true;
}

bool CameraDeviceSession::process_capture_request(CaptureRequest request) {
  auto self = weak_from_this().lock();
  if (!self) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reject_request_locked()) {
      return false;
    }
  }

  // Request admission is intentionally non-blocking. Queue saturation is
  // reported to the framework as a submit failure; the framework can retry or
  // apply its own pacing policy.
  return runtime_.post_session_task(
      config_.session_id,
      [self = std::move(self), request = std::move(request)]() mutable {
        self->submit_capture_request(std::move(request));
      });
}

void CameraDeviceSession::on_driver_fd_ready(int ready_fd,
                                             FdReadyEvents events) {
  if (!events.readable && !events.error && !events.hangup) {
    return;
  }

  while (true) {
    std::optional<DriverCompletion> completion;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (driver_ == nullptr || state_ != CameraSessionState::Streaming) {
        return;
      }
      completion = driver_->dequeue_completion(ready_fd);
    }

    if (!completion) {
      return;
    }
    on_driver_buffer_complete(*completion);
  }
}

void CameraDeviceSession::on_driver_buffer_complete(
    const DriverCompletion& completion) {
  CaptureResult result;
  bool should_dispatch = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    should_dispatch =
        try_fill_result_from_completion_locked(completion, &result);
  }

  if (should_dispatch && result_callback_) {
    result_callback_(result);
  }

  if (should_dispatch && is_standalone_stream_completion(completion)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (driver_ != nullptr && state_ == CameraSessionState::Streaming &&
        !driver_->return_stream_buffer(completion)) {
      metrics_.record_failure();
    }
  }
}

void CameraDeviceSession::flush() {
  std::vector<CaptureResult> results;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != CameraSessionState::Streaming &&
        state_ != CameraSessionState::Configured) {
      return;
    }

    state_ = CameraSessionState::Flushing;
    if (driver_ != nullptr) {
      driver_->stop_streaming();
    }

    for (auto& request : in_flight_.flush_all()) {
      metrics_.record_failure();
      results.push_back(make_result(request.frame_number,
                                    CaptureStatus::Flushed,
                                    completed_buffers_for(
                                        request.output_buffers,
                                        CaptureStatus::Flushed),
                                    "request flushed",
                                    latency_since(request.submitted_at)));
    }

    state_ = CameraSessionState::Configured;
  }

  for (const auto& result : results) {
    if (result_callback_) {
      result_callback_(result);
    }
  }
}

void CameraDeviceSession::close() {
  std::vector<CaptureResult> results;
  std::vector<int> driver_fds;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CameraSessionState::Stopped) {
      return;
    }

    state_ = CameraSessionState::Stopping;
    if (driver_ != nullptr) {
      driver_fds = driver_fds_;
      driver_->stop_streaming();
    }

    for (auto& request : in_flight_.flush_all()) {
      metrics_.record_failure();
      results.push_back(make_result(request.frame_number,
                                    CaptureStatus::Flushed,
                                    completed_buffers_for(
                                        request.output_buffers,
                                        CaptureStatus::Flushed),
                                    "session closed",
                                    latency_since(request.submitted_at)));
    }

    if (driver_ != nullptr) {
      driver_->close();
    }
    driver_fds_.clear();
    state_ = CameraSessionState::Stopped;
  }

  if (!driver_fds.empty()) {
    runtime_.unregister_session(config_.session_id, driver_fds);
  }

  for (const auto& result : results) {
    if (result_callback_) {
      result_callback_(result);
    }
  }
}

FrameMetrics CameraDeviceSession::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
}

bool CameraDeviceSession::reject_request_locked() const {
  return state_ != CameraSessionState::Streaming || driver_ == nullptr;
}

void CameraDeviceSession::submit_capture_request(CaptureRequest request) {
  CaptureResult result;
  auto dispatch_result = [this, &result] {
    if (result_callback_) {
      result_callback_(result);
    }
  };

  std::unique_lock<std::mutex> lock(mutex_);
  if (reject_request_locked()) {
    result = make_result(request.frame_number,
                         CaptureStatus::Flushed,
                         {},
                         "session is not accepting requests",
                         latency_since(request.submitted_at));
    metrics_.record_failure();
    lock.unlock();
    dispatch_result();
    return;
  }

  const auto outputs = request_outputs_for(request);
  auto in_flight_outputs = make_in_flight_outputs(outputs);
  const auto driver_outputs = in_flight_outputs;
  auto failed_buffers =
      completed_buffers_for(in_flight_outputs, CaptureStatus::ProcessingError);

  if (!driver_->can_submit_capture_outputs(outputs)) {
    fill_failed_result_locked(request.frame_number,
                              CaptureStatus::SensorError,
                              driver_->last_error().empty()
                                  ? "driver cannot accept every output"
                                  : driver_->last_error(),
                              &result,
                              std::move(failed_buffers),
                              request.submitted_at);
    lock.unlock();
    dispatch_result();
    return;
  }

  if (!start_in_flight_request_locked(request, std::move(in_flight_outputs))) {
    fill_failed_result_locked(request.frame_number,
                              CaptureStatus::ProcessingError,
                              "duplicate frame number",
                              &result,
                              std::move(failed_buffers),
                              request.submitted_at);
    lock.unlock();
    dispatch_result();
    return;
  }

  if (!submit_request_to_driver_locked(request, outputs, driver_outputs)) {
    fill_failed_result_locked(request.frame_number,
                              CaptureStatus::SensorError,
                              driver_ != nullptr ? driver_->last_error()
                                                 : "driver is not available",
                              &result);
    lock.unlock();
    dispatch_result();
    return;
  }
}

std::vector<InFlightOutput>
CameraDeviceSession::make_in_flight_outputs(
    const std::vector<OutputBufferTarget>& outputs) const {
  std::vector<InFlightOutput> in_flight_outputs;
  in_flight_outputs.reserve(outputs.size());
  for (size_t i = 0; i < outputs.size(); ++i) {
    in_flight_outputs.push_back(InFlightOutput{
        .target = outputs[i],
        .output_index = static_cast<int>(i),
    });
  }
  return in_flight_outputs;
}

bool CameraDeviceSession::start_in_flight_request_locked(
    const CaptureRequest& request,
    std::vector<InFlightOutput> tracked_outputs) {
  return in_flight_.start(request, std::move(tracked_outputs));
}

bool CameraDeviceSession::submit_request_to_driver_locked(
    const CaptureRequest& request,
    const std::vector<OutputBufferTarget>& outputs,
    const std::vector<InFlightOutput>& in_flight_outputs) {
  bool submitted_all = true;
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (!driver_->submit_capture(DriverOutputBuffer{
            .frame_number = request.frame_number,
            .buffer_id = outputs[i].buffer_id,
            .target = outputs[i],
            .output_index = in_flight_outputs[i].output_index,
        })) {
      submitted_all = false;
      break;
    }
  }

  return submitted_all && in_flight_.mark_queued_to_driver(request.frame_number);
}

bool CameraDeviceSession::try_fill_result_from_completion_locked(
    const DriverCompletion& completion,
    CaptureResult* result) {
  if (result == nullptr) {
    return false;
  }

  CaptureMetadata metadata{
      .sensor_timestamp = completion.timestamp,
  };
  int release_fence_fd = completion.release_fence_fd;
  for (const auto& output_processor : output_processors_) {
    if (output_processor == nullptr) {
      continue;
    }

    release_fence_fd =
        output_processor
            ->process_output(OutputProcessRequest{
                .frame_number = completion.frame_number,
                .output_index = completion.output_index,
                .stream_id = completion.stream_id,
                .buffer_id = completion.buffer_id,
                .buffer_fd = completion.buffer_fd,
                .buffer_size = completion.buffer_size,
                .input_acquire_fence_fd = release_fence_fd,
                .width = completion.width,
                .height = completion.height,
                .payload_format = completion.payload_format,
            })
            .release_fence_fd;
  }

  if (is_standalone_stream_completion(completion)) {
    metrics_.record_success(std::chrono::microseconds{0});
    auto processed_completion = completion;
    processed_completion.release_fence_fd = release_fence_fd;
    *result = capture_result_from_standalone_completion(processed_completion);
    return true;
  }

  auto output_completion = in_flight_.complete_output(
      completion.frame_number,
      completion.output_index,
      release_fence_fd,
      metadata);
  if (output_completion.state == OutputCompletionState::WaitingForMoreOutputs) {
    return false;
  }
  if (output_completion.state == OutputCompletionState::NotFound ||
      !output_completion.request) {
    close_fence_fd(release_fence_fd);
    metrics_.record_failure();
    return false;
  }

  auto converted_result = capture_result_from_output_completion(
      output_completion, completion);
  if (!converted_result) {
    close_fence_fd(release_fence_fd);
    metrics_.record_failure();
    return false;
  }
  metrics_.record_success(converted_result->latency);
  *result = std::move(*converted_result);
  return true;
}

void CameraDeviceSession::fill_failed_result_locked(
    uint64_t frame_number,
    CaptureStatus status,
    std::string message,
    CaptureResult* result,
    std::vector<CompletedOutputBuffer> fallback_completed_buffers,
    std::chrono::steady_clock::time_point fallback_submitted_at) {
  if (result == nullptr) {
    return;
  }

  auto request = in_flight_.fail(frame_number);
  auto completed_buffers = std::move(fallback_completed_buffers);
  auto submitted_at = fallback_submitted_at;
  if (request) {
    completed_buffers =
        completed_buffers_for(request->output_buffers, status);
    submitted_at = request->submitted_at;
  }

  metrics_.record_failure();
  *result = make_result(
      frame_number, status, std::move(completed_buffers),
      std::move(message),
      latency_since(submitted_at));
}

}  // namespace minicam
