#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace minicam {

enum class StreamType {
  Preview,
  Still,
  Video,
};

enum class PixelFormat {
  Rgb24,
  Yuyv422,
};

struct StreamConfig {
  int stream_id = 0;
  StreamType stream_type = StreamType::Preview;
  int width = 640;
  int height = 480;
  PixelFormat format = PixelFormat::Rgb24;
};

// Framework-facing output target for one capture request. Real Android HAL
// receives concrete buffer handles from gralloc/BufferQueue. The demo keeps
// the memory handle app-owned and refers to it by buffer_id.
struct OutputBufferTarget {
  int stream_id = 0;
  int buffer_id = -1;
  int buffer_fd = -1;
  size_t buffer_size = 0;
  StreamType stream_type = StreamType::Still;
  int width = 640;
  int height = 480;
  PixelFormat format = PixelFormat::Rgb24;
};

// App/framework-facing request for one frame. Real Android requests include many
// more controls; this project models frame identity, output stream targets, and
// latency.
struct CaptureRequest {
  uint64_t frame_number = 0;
  int width = 640;
  int height = 480;
  std::vector<OutputBufferTarget> output_buffers;
  std::chrono::steady_clock::time_point submitted_at =
      std::chrono::steady_clock::now();
};

}  // namespace minicam
