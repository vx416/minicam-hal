#pragma once

#include "hal/dma_buf_allocator.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace minicam::app {

struct TrackedBuffer {
  int buffer_id = -1;
  int buffer_fd = -1;
  size_t buffer_size = 0;
  std::shared_ptr<DmaBuf> buffer;
};

class BufferTracker {
 public:
  bool register_buffer(int buffer_id, std::shared_ptr<DmaBuf> buffer);
  std::optional<TrackedBuffer> resolve_metadata(int buffer_id) const;
  std::optional<TrackedBuffer> take_metadata(int buffer_id);
  std::shared_ptr<DmaBuf> resolve(int buffer_id) const;
  std::shared_ptr<DmaBuf> take(int buffer_id);
  void release(int buffer_id);
  void release_all();

 private:
  mutable std::mutex mutex_;
  std::unordered_map<int, TrackedBuffer> buffers_;
};

}  // namespace minicam::app
