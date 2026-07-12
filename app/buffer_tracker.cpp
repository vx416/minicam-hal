#include "app/buffer_tracker.h"

#include <utility>
#include <vector>

namespace minicam::app {

bool BufferTracker::register_buffer(int buffer_id,
                                    std::shared_ptr<DmaBuf> buffer) {
  if (!buffer) {
    return false;
  }

  auto tracked = TrackedBuffer{
      .buffer_id = buffer_id,
      .buffer_fd = buffer->fd(),
      .buffer_size = buffer->size(),
      .buffer = std::move(buffer),
  };

  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.emplace(buffer_id, std::move(tracked)).second;
}

std::optional<TrackedBuffer> BufferTracker::resolve_metadata(
    int buffer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<TrackedBuffer> BufferTracker::take_metadata(int buffer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return std::nullopt;
  }

  auto tracked = std::move(it->second);
  buffers_.erase(it);
  return tracked;
}

std::shared_ptr<DmaBuf> BufferTracker::resolve(int buffer_id) const {
  auto tracked = resolve_metadata(buffer_id);
  if (!tracked) {
    return nullptr;
  }
  return tracked->buffer;
}

std::shared_ptr<DmaBuf> BufferTracker::take(int buffer_id) {
  auto tracked = take_metadata(buffer_id);
  if (!tracked) {
    return nullptr;
  }
  return std::move(tracked->buffer);
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
    for (auto& [_, tracked] : buffers_) {
      buffers.push_back(std::move(tracked.buffer));
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
