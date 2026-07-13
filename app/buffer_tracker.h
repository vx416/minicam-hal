#pragma once

#include "hal/dma_buf_allocator.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace minicam::app {

struct TrackedBufferMetadata {
  int buffer_id = -1;
  int buffer_fd = -1;
  size_t buffer_size = 0;
};

struct TrackedBuffer {
  TrackedBufferMetadata metadata;
  DmaBufLease lease;
};

class BufferTracker {
 public:
  bool register_buffer(int buffer_id, DmaBufLease lease);
  std::optional<TrackedBufferMetadata> resolve_metadata(int buffer_id) const;
  std::optional<TrackedBuffer> take_metadata(int buffer_id);
  DmaBuf* resolve(int buffer_id) const;
  void release(int buffer_id);
  void release_all();

 private:
  mutable std::mutex mutex_;
  std::unordered_map<int, TrackedBuffer> buffers_;
};

}  // namespace minicam::app
