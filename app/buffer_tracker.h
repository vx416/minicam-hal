#pragma once

#include "hal/dma_buf_allocator.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace minicam::app {

class BufferTracker {
 public:
  bool register_buffer(int buffer_id, std::shared_ptr<DmaBuf> buffer);
  std::shared_ptr<DmaBuf> resolve(int buffer_id) const;
  std::shared_ptr<DmaBuf> take(int buffer_id);
  void release(int buffer_id);
  void release_all();

 private:
  mutable std::mutex mutex_;
  std::unordered_map<int, std::shared_ptr<DmaBuf>> buffers_;
};

}  // namespace minicam::app
