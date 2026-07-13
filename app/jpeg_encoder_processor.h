#pragma once

#include "app/buffer_tracker.h"
#include "hal/output_processor.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minicam::app {

struct EncodedFrameKey {
  uint64_t frame_number = 0;
  int buffer_id = -1;

  bool operator==(const EncodedFrameKey& other) const {
    return frame_number == other.frame_number && buffer_id == other.buffer_id;
  }
};

struct EncodedFrameKeyHash {
  size_t operator()(const EncodedFrameKey& key) const {
    return std::hash<uint64_t>{}(key.frame_number) ^
           (std::hash<int>{}(key.buffer_id) << 1U);
  }
};

class EncodedFrameStore {
 public:
  void put(EncodedFrameKey key, std::vector<uint8_t> bytes);
  std::optional<std::vector<uint8_t>> take(EncodedFrameKey key);

 private:
  std::mutex mutex_;
  std::unordered_map<EncodedFrameKey,
                     std::vector<uint8_t>,
                     EncodedFrameKeyHash>
      frames_;
};

// Asynchronous JPEG encode stage. It converts the completed raw output buffer
// into encoded JPEG bytes in memory, then signals the returned release fence.
class JpegEncoderProcessor final : public OutputProcessor {
 public:
  JpegEncoderProcessor(const BufferTracker& preview_buffers,
                       const BufferTracker& capture_buffers,
                       std::shared_ptr<EncodedFrameStore> encoded_frames);
  ~JpegEncoderProcessor() override;

  JpegEncoderProcessor(const JpegEncoderProcessor&) = delete;
  JpegEncoderProcessor& operator=(const JpegEncoderProcessor&) = delete;

  OutputProcessResult process_output(OutputProcessRequest request) override;

 private:
  void add_worker(std::thread worker);
  void join_workers();
  DmaBuf* resolve_buffer(int buffer_id) const;

  const BufferTracker& preview_buffers_;
  const BufferTracker& capture_buffers_;
  std::shared_ptr<EncodedFrameStore> encoded_frames_;
  std::mutex mutex_;
  std::vector<std::thread> workers_;
};

}  // namespace minicam::app
