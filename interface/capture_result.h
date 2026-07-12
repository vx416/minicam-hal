#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace minicam {

// High-level completion status reported to the client for one frame.
enum class CaptureStatus {
  Ok,
  Flushed,
  SensorError,
  ProcessingError,
};

enum class FramePayloadFormat {
  None,
  Rgb24,
  Yuyv422,
};

struct CompletedOutputBuffer {
  int stream_id = 0;
  int buffer_id = -1;
  int output_index = -1;
  CaptureStatus status = CaptureStatus::Ok;
  int release_fence_fd = -1;
};

// App/framework-facing result for one frame.
struct CaptureResult {
  uint64_t frame_number = 0;
  CaptureStatus status = CaptureStatus::Ok;
  std::vector<CompletedOutputBuffer> completed_output_buffers;
  std::string message;
  std::chrono::microseconds latency{0};

  int width = 0;
  int height = 0;
  FramePayloadFormat payload_format = FramePayloadFormat::None;
};

}  // namespace minicam
