#include "app/buffer_tracker.h"

#include <utility>
#include <vector>

namespace minicam::app {

bool BufferTracker::register_buffer(int buffer_id,
                                    std::shared_ptr<DmaBuf> buffer) {
  if (!buffer) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.emplace(buffer_id, std::move(buffer)).second;
}

std::shared_ptr<DmaBuf> BufferTracker::resolve(int buffer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return nullptr;
  }
  return it->second;
}

std::shared_ptr<DmaBuf> BufferTracker::take(int buffer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return nullptr;
  }

  auto buffer = std::move(it->second);
  buffers_.erase(it);
  return buffer;
}

void BufferTracker::release(int buffer_id) {
  auto buffer = take(buffer_id);
  if (buffer) {
    buffer->release();
  }
}

void BufferTracker::release_all() {
  std::vector<std::shared_ptr<DmaBuf>> buffers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers.reserve(buffers_.size());
    for (auto& [_, buffer] : buffers_) {
      buffers.push_back(std::move(buffer));
    }
    buffers_.clear();
  }

  for (auto& buffer : buffers) {
    if (buffer) {
      buffer->release();
    }
  }
}

}  // namespace minicam::app
