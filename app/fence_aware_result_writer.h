#pragma once

#include "app/buffer_tracker.h"
#include "app/jpeg_encoder_processor.h"
#include "interface/capture_result.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace minicam::app {

struct WriteResult {
  CaptureResult result;
  bool wrote_image = false;
};

class FenceAwareResultWriter {
 public:
  FenceAwareResultWriter(const BufferTracker& preview_buffers,
                         BufferTracker& capture_buffers,
                         std::shared_ptr<EncodedFrameStore> encoded_frames);

  FenceAwareResultWriter(const FenceAwareResultWriter&) = delete;
  FenceAwareResultWriter& operator=(const FenceAwareResultWriter&) = delete;

  void register_path(uint64_t frame_number, std::string output_path);
  void register_result(const CaptureResult& result);
  void cancel(uint64_t frame_number);
  std::optional<WriteResult> block_wait_and_write(
      uint64_t frame_number,
      std::chrono::seconds timeout);

 private:
  struct PendingWrite {
    std::string output_path;
    std::optional<CaptureResult> result;
  };

  bool write_encoded_frame(const std::string& output_path,
                           uint64_t frame_number,
                           int buffer_id);

  const BufferTracker& preview_buffers_;
  BufferTracker& capture_buffers_;
  std::shared_ptr<EncodedFrameStore> encoded_frames_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::unordered_map<uint64_t, PendingWrite> pending_writes_;
};

}  // namespace minicam::app
