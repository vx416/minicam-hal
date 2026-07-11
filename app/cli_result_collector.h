#pragma once

#include "app/buffer_tracker.h"
#include "interface/camera_device.h"
#include "interface/capture_result.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace minicam::app {

struct CliResultCollectorOptions {
  std::optional<std::string> preview_output_path;
  size_t preview_frame_count = 0;
};

class CliResultCollector {
 public:
  CliResultCollector(CliResultCollectorOptions options,
                     const BufferTracker& preview_buffers);

  ResultCallback callback();

  bool wait_for_preview_frames(std::chrono::seconds timeout);
  std::optional<CaptureResult> wait_for_frame(uint64_t frame_number,
                                              std::chrono::seconds timeout);
  size_t preview_frames_written() const;

 private:
  void handle_preview_result(const CaptureResult& result);

  CliResultCollectorOptions options_;
  const BufferTracker& preview_buffers_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::unordered_map<uint64_t, CaptureResult> results_;
  size_t preview_frames_written_ = 0;
};

}  // namespace minicam::app
