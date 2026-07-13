#include "app/fence_aware_result_writer.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <optional>
#include <utility>

namespace minicam::app {
namespace {

bool wait_for_release_fence(int release_fence_fd) {
  if (release_fence_fd < 0) {
    return true;
  }

  pollfd fence_poll{
      .fd = release_fence_fd,
      .events = POLLIN,
      .revents = 0,
  };
  while (true) {
    const int rc = ::poll(&fence_poll, 1, -1);
    if (rc > 0) {
      return (fence_poll.revents & (POLLIN | POLLERR | POLLHUP)) != 0;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

void close_release_fence(int release_fence_fd) {
  if (release_fence_fd >= 0) {
    ::close(release_fence_fd);
  }
}

std::optional<CompletedOutputBuffer> first_completed_buffer(
    const CaptureResult& result) {
  if (result.completed_output_buffers.empty()) {
    return std::nullopt;
  }
  return result.completed_output_buffers.front();
}

}  // namespace

FenceAwareResultWriter::FenceAwareResultWriter(
    const BufferTracker& preview_buffers,
    BufferTracker& capture_buffers,
    std::shared_ptr<EncodedFrameStore> encoded_frames)
    : preview_buffers_(preview_buffers),
      capture_buffers_(capture_buffers),
      encoded_frames_(std::move(encoded_frames)) {}

void FenceAwareResultWriter::register_path(uint64_t frame_number,
                                           std::string output_path) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_writes_[frame_number].output_path = std::move(output_path);
  }
  ready_.notify_all();
}

void FenceAwareResultWriter::register_result(const CaptureResult& result) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_writes_[result.frame_number].result = result;
  }
  ready_.notify_all();
}

void FenceAwareResultWriter::cancel(uint64_t frame_number) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_writes_.erase(frame_number);
  }
  ready_.notify_all();
}

std::optional<WriteResult> FenceAwareResultWriter::block_wait_and_write(
    uint64_t frame_number,
    std::chrono::seconds timeout) {
  PendingWrite pending;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool registered = ready_.wait_for(lock, timeout, [this, frame_number] {
      const auto it = pending_writes_.find(frame_number);
      return it != pending_writes_.end() && !it->second.output_path.empty() &&
             it->second.result.has_value();
    });
    if (!registered) {
      return std::nullopt;
    }

    const auto it = pending_writes_.find(frame_number);
    if (it == pending_writes_.end()) {
      return std::nullopt;
    }
    pending = std::move(it->second);
    pending_writes_.erase(it);
  }

  const auto& result = *pending.result;
  auto completed = first_completed_buffer(result);
  if (!completed) {
    return WriteResult{
        .result = result,
        .wrote_image = false,
    };
  }

  const bool fence_ready = wait_for_release_fence(completed->release_fence_fd);
  close_release_fence(completed->release_fence_fd);

  const bool is_preview = result.message == "preview";
  std::optional<TrackedBuffer> captured_buffer;
  std::optional<TrackedBufferMetadata> tracked_metadata;
  if (is_preview) {
    tracked_metadata = preview_buffers_.resolve_metadata(completed->buffer_id);
  } else {
    captured_buffer = capture_buffers_.take_metadata(completed->buffer_id);
    if (captured_buffer) {
      tracked_metadata = captured_buffer->metadata;
    }
  }
  if (!tracked_metadata && completed->buffer_id == -1) {
    tracked_metadata = TrackedBufferMetadata{
        .buffer_id = completed->buffer_id,
        .buffer_fd = -1,
        .buffer_size = 0,
    };
  }

  const bool wrote_image =
      result.status == CaptureStatus::Ok && fence_ready && tracked_metadata &&
      !pending.output_path.empty() &&
      write_encoded_frame(pending.output_path,
                          result.frame_number,
                          completed->buffer_id);

  return WriteResult{
      .result = result,
      .wrote_image = wrote_image,
  };
}

bool FenceAwareResultWriter::write_encoded_frame(
    const std::string& output_path,
    uint64_t frame_number,
    int buffer_id) {
  if (!encoded_frames_) {
    return false;
  }

  auto encoded = encoded_frames_->take(EncodedFrameKey{
      .frame_number = frame_number,
      .buffer_id = buffer_id,
  });
  if (!encoded) {
    return false;
  }

  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(encoded->data()),
            static_cast<std::streamsize>(encoded->size()));
  return static_cast<bool>(out);
}

}  // namespace minicam::app
