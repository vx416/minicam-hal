#include "app/jpeg_encoder_processor.h"

#include "app/jpeg_codec.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace minicam::app {
namespace {

bool wait_for_fence(int fence_fd) {
  if (fence_fd < 0) {
    return true;
  }

  pollfd fence_poll{
      .fd = fence_fd,
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

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

void signal_fence(int write_fd) {
  const uint8_t byte = 1;
  (void)::write(write_fd, &byte, sizeof(byte));
  close_fd(write_fd);
}

}  // namespace

void EncodedFrameStore::put(EncodedFrameKey key, std::vector<uint8_t> bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_[key] = std::move(bytes);
}

std::optional<std::vector<uint8_t>> EncodedFrameStore::take(
    EncodedFrameKey key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = frames_.find(key);
  if (it == frames_.end()) {
    return std::nullopt;
  }

  auto bytes = std::move(it->second);
  frames_.erase(it);
  return bytes;
}

JpegEncoderProcessor::JpegEncoderProcessor(
    const BufferTracker& preview_buffers,
    const BufferTracker& capture_buffers,
    std::shared_ptr<EncodedFrameStore> encoded_frames)
    : preview_buffers_(preview_buffers),
      capture_buffers_(capture_buffers),
      encoded_frames_(std::move(encoded_frames)) {}

JpegEncoderProcessor::~JpegEncoderProcessor() {
  join_workers();
}

OutputProcessResult JpegEncoderProcessor::process_output(
    OutputProcessRequest request) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) < 0) {
    close_fd(request.input_acquire_fence_fd);
    return OutputProcessResult{.release_fence_fd = -1};
  }

  const auto* output_buffer = resolve_buffer(request.buffer_id);
  const int read_fd = fds[0];
  const int write_fd = fds[1];
  add_worker(std::thread([this,
                          request = std::move(request),
                          output_buffer,
                          write_fd] {
    (void)wait_for_fence(request.input_acquire_fence_fd);
    close_fd(request.input_acquire_fence_fd);

    if (encoded_frames_) {
      auto result = CaptureResult{
          .frame_number = request.frame_number,
          .status = CaptureStatus::Ok,
          .completed_output_buffers = {},
          .message = "jpeg-encode",
          .latency = std::chrono::microseconds{0},
          .width = request.width,
          .height = request.height,
          .payload_format = request.payload_format,
      };
      auto encoded = output_buffer ? encode_jpeg(result, output_buffer)
                                   : encode_synthetic_jpeg(result);
      if (encoded) {
        encoded_frames_->put(EncodedFrameKey{
                                 .frame_number = request.frame_number,
                                 .buffer_id = request.buffer_id,
                             },
                             std::move(*encoded));
      }
    }

    signal_fence(write_fd);
  }));

  return OutputProcessResult{.release_fence_fd = read_fd};
}

void JpegEncoderProcessor::add_worker(std::thread worker) {
  std::lock_guard<std::mutex> lock(mutex_);
  workers_.push_back(std::move(worker));
}

DmaBuf* JpegEncoderProcessor::resolve_buffer(int buffer_id) const {
  if (auto* buffer = capture_buffers_.resolve(buffer_id)) {
    return buffer;
  }
  return preview_buffers_.resolve(buffer_id);
}

void JpegEncoderProcessor::join_workers() {
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workers.swap(workers_);
  }

  for (auto& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

}  // namespace minicam::app
