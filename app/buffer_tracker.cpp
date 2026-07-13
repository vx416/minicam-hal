#include "app/buffer_tracker.h"

#include <utility>

namespace minicam::app {

bool BufferTracker::register_buffer(int buffer_id, DmaBufLease lease) {
  if (!lease) {
    return false;
  }

  auto tracked = TrackedBuffer{
      .metadata =
          TrackedBufferMetadata{
              .buffer_id = buffer_id,
              .buffer_fd = lease->fd(),
              .buffer_size = lease->size(),
          },
      .lease = std::move(lease),
  };

  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.emplace(buffer_id, std::move(tracked)).second;
}

std::optional<TrackedBufferMetadata> BufferTracker::resolve_metadata(
    int buffer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return std::nullopt;
  }
  return it->second.metadata;
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

DmaBuf* BufferTracker::resolve(int buffer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return nullptr;
  }
  return it->second.lease.get();
}

void BufferTracker::release(int buffer_id) {
  (void)take_metadata(buffer_id);
}

void BufferTracker::release_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  buffers_.clear();
}

}  // namespace minicam::app
