#pragma once

#include "interface/capture_result.h"

#include <cstddef>
#include <cstdint>

namespace minicam {

struct OutputProcessRequest {
  uint64_t frame_number = 0;
  int output_index = -1;
  int stream_id = 0;
  int buffer_id = -1;
  int buffer_fd = -1;
  size_t buffer_size = 0;
  int input_acquire_fence_fd = -1;
  int width = 0;
  int height = 0;
  FramePayloadFormat payload_format = FramePayloadFormat::None;
};

struct OutputProcessResult {
  int release_fence_fd = -1;
};

// Optional HAL pipeline stage after driver completion and before the framework
// result callback. Implementations can return a release fence that represents
// asynchronous post-processing work for one output buffer.
class OutputProcessor {
 public:
  virtual ~OutputProcessor() = default;

  virtual OutputProcessResult process_output(
      OutputProcessRequest request) = 0;
};

}  // namespace minicam
