#include "hal/v4l2_driver_adapter.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace minicam {

#ifdef __linux__
namespace {

int xioctl(int fd, unsigned long request, void* arg) {
  int rc = 0;
  do {
    rc = ::ioctl(fd, request, arg);
  } while (rc < 0 && errno == EINTR);
  return rc;
}

std::chrono::steady_clock::time_point steady_timestamp_from(
    const timeval& timestamp) {
  const auto now_steady = std::chrono::steady_clock::now();
  const auto now_system = std::chrono::system_clock::now();
  const auto buffer_system =
      std::chrono::system_clock::time_point{
          std::chrono::seconds(timestamp.tv_sec)} +
      std::chrono::microseconds(timestamp.tv_usec);
  return now_steady +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
             buffer_system - now_system);
}

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

void close_fence(int fence_fd) {
  if (fence_fd >= 0) {
    ::close(fence_fd);
  }
}

}  // namespace
#endif

V4L2DriverAdapter::V4L2DriverAdapter(V4L2StreamConfig config)
    : config_(std::move(config)) {}

V4L2DriverAdapter::~V4L2DriverAdapter() {
  close();
}

bool V4L2DriverAdapter::configure_stream() {
#ifdef __linux__
  if (fd_ >= 0) {
    return true;
  }

  fd_ = ::open(config_.device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    set_error(std::string("open failed: ") + std::strerror(errno));
    return false;
  }

  v4l2_capability capability{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
    set_error(std::string("VIDIOC_QUERYCAP failed: ") + std::strerror(errno));
    close();
    return false;
  }

  const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                        ? capability.device_caps
                        : capability.capabilities;
  if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
    set_error("device does not support single-plane V4L2 capture");
    close();
    return false;
  }
  if ((caps & V4L2_CAP_STREAMING) == 0) {
    set_error("device does not support V4L2 streaming I/O");
    close();
    return false;
  }

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = static_cast<__u32>(config_.width);
  format.fmt.pix.height = static_cast<__u32>(config_.height);
  format.fmt.pix.pixelformat =
      config_.pixel_format == 0 ? V4L2_PIX_FMT_YUYV : config_.pixel_format;
  format.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
    set_error(std::string("VIDIOC_S_FMT failed: ") + std::strerror(errno));
    close();
    return false;
  }

  v4l2_requestbuffers request_buffers{};
  request_buffers.count = static_cast<__u32>(config_.buffer_count);
  request_buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request_buffers.memory = V4L2_MEMORY_DMABUF;
  if (xioctl(fd_, VIDIOC_REQBUFS, &request_buffers) < 0) {
    set_error(std::string("VIDIOC_REQBUFS failed: ") + std::strerror(errno));
    close();
    return false;
  }
  if (request_buffers.count == 0) {
    set_error("VIDIOC_REQBUFS returned zero buffers");
    close();
    return false;
  }

  buffer_size_ =
      format.fmt.pix.sizeimage == 0
          ? static_cast<size_t>(config_.width) *
                static_cast<size_t>(config_.height) * 2U
          : static_cast<size_t>(format.fmt.pix.sizeimage);
  queued_requests_.clear();
  queued_requests_.reserve(request_buffers.count);
  for (__u32 i = 0; i < request_buffers.count; ++i) {
    queued_requests_.push_back(V4L2QueuedRequest{
        .index = static_cast<int>(i),
        .frame_number = 0,
        .output_index = -1,
        .stream_id = 0,
        .buffer_id = -1,
        .buffer_fd = -1,
        .acquire_fence_fd = -1,
        .buffer_size = 0,
        .queued = false,
    });
  }

  return true;
#else
  set_error("V4L2 is only available on Linux");
  return false;
#endif
}

bool V4L2DriverAdapter::start_streaming() {
  if (fd_ < 0 || queued_requests_.empty()) {
    set_error("V4L2 stream is not configured");
    return false;
  }
  streaming_ = true;
  return true;
}

size_t V4L2DriverAdapter::required_output_buffer_size(
    const OutputBufferTarget& output) const {
  (void)output;
  return buffer_size_;
}

bool V4L2DriverAdapter::start_continuous_stream(
    const StreamConfig& stream,
    std::vector<DriverOutputBuffer> buffers) {
#ifdef __linux__
  (void)stream;
  if (!streaming_ || fd_ < 0) {
    set_error("V4L2 stream is not started");
    return false;
  }

  continuous_streaming_ = true;
  for (auto& buffer : buffers) {
    if (!queue_capture_buffer(next_continuous_frame_number_++,
                              buffer.target.stream_id,
                              buffer.buffer_id,
                              /*output_index=*/-1,
                              buffer.target.buffer_fd,
                              buffer.target.acquire_fence_fd,
                              buffer.target.buffer_size)) {
      continuous_streaming_ = false;
      return false;
    }
  }
  return true;
#else
  (void)stream;
  (void)buffers;
  set_error("V4L2 is only available on Linux");
  return false;
#endif
}

void V4L2DriverAdapter::stop_streaming() {
#ifdef __linux__
  if (fd_ >= 0 && device_streaming_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)xioctl(fd_, VIDIOC_STREAMOFF, &type);
  }
#endif
  reset_queued_requests();
  device_streaming_ = false;
  streaming_ = false;
  continuous_streaming_ = false;
}

void V4L2DriverAdapter::close() {
#ifdef __linux__
  stop_streaming();
  queued_requests_.clear();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#else
  queued_requests_.clear();
  fd_ = -1;
#endif
  device_streaming_ = false;
  streaming_ = false;
}

bool V4L2DriverAdapter::submit_capture(DriverOutputBuffer output) {
#ifdef __linux__
  return queue_capture_buffer(
      output.frame_number,
      output.target.stream_id,
      output.buffer_id,
      output.output_index,
      output.target.buffer_fd,
      output.target.acquire_fence_fd,
      output.target.buffer_size);
#else
  (void)output;
  set_error("V4L2 is only available on Linux");
  return false;
#endif
}

bool V4L2DriverAdapter::can_submit_capture_outputs(
    const std::vector<OutputBufferTarget>& outputs) const {
  if (!streaming_ || fd_ < 0 || outputs.size() > free_queued_request_count()) {
    return false;
  }
  for (const auto& output : outputs) {
    if (output.buffer_fd < 0 || output.buffer_size < buffer_size_) {
      return false;
    }
  }
  return true;
}

bool V4L2DriverAdapter::queue_capture_buffer(uint64_t frame_number,
                                             int stream_id,
                                             int buffer_id,
                                             int output_index,
                                             int buffer_fd,
                                             int acquire_fence_fd,
                                             size_t dma_buf_size) {
#ifdef __linux__
  if (!streaming_ || fd_ < 0) {
    set_error("V4L2 stream is not started");
    return false;
  }
  if (buffer_fd < 0) {
    set_error("V4L2 capture requires a valid dma-buf from the request");
    return false;
  }
  if (dma_buf_size < buffer_size_) {
    set_error("request dma-buf is smaller than the configured V4L2 frame");
    return false;
  }
  if (!wait_for_fence(acquire_fence_fd)) {
    close_fence(acquire_fence_fd);
    set_error(std::string("acquire fence wait failed: ") +
              std::strerror(errno));
    return false;
  }
  close_fence(acquire_fence_fd);
  acquire_fence_fd = -1;

  auto* queued_request =
      bind_free_queued_request(frame_number, stream_id, buffer_id, output_index,
                               buffer_fd, acquire_fence_fd, dma_buf_size);
  if (queued_request == nullptr) {
    set_error("V4L2 queue is full");
    return false;
  }
  if (queued_request->buffer_fd < 0) {
    set_error("V4L2 queued request binding failed");
    return false;
  }

  v4l2_buffer buffer{};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_DMABUF;
  buffer.index = static_cast<__u32>(queued_request->index);
  buffer.m.fd = queued_request->buffer_fd;
  if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
    clear_queued_request(buffer.index);
    set_error(std::string("VIDIOC_QBUF failed: ") + std::strerror(errno));
    return false;
  }

  if (!device_streaming_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      clear_queued_request(buffer.index);
      set_error(std::string("VIDIOC_STREAMON failed: ") +
                std::strerror(errno));
      return false;
    }
    device_streaming_ = true;
  }

  return true;
#else
  (void)frame_number;
  (void)stream_id;
  (void)buffer_id;
  (void)output_index;
  (void)buffer_fd;
  (void)acquire_fence_fd;
  (void)dma_buf_size;
  set_error("V4L2 is only available on Linux");
  return false;
#endif
}

std::optional<DriverCompletion> V4L2DriverAdapter::dequeue_completion(
    int ready_fd) {
#ifdef __linux__
  if (ready_fd != fd_) {
    return std::nullopt;
  }
  if (!device_streaming_ || fd_ < 0) {
    return std::nullopt;
  }

  v4l2_buffer buffer{};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_DMABUF;
  if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
    if (errno == EAGAIN) {
      return std::nullopt;
    }
    set_error(std::string("VIDIOC_DQBUF failed: ") + std::strerror(errno));
    return std::nullopt;
  }

  auto* queued_request = queued_request_for_index(buffer.index);
  if (!queued_request) {
    set_error("VIDIOC_DQBUF returned an out-of-range buffer index");
    return std::nullopt;
  }

  if (!queued_request->queued) {
    set_error("VIDIOC_DQBUF returned a buffer that is not queued");
    return std::nullopt;
  }
  auto completion = convert_queued_request_to_completion(
      *queued_request,
      buffer.bytesused,
      steady_timestamp_from(buffer.timestamp));
  clear_queued_request(buffer.index);
  return completion;
#else
  (void)ready_fd;
  set_error("V4L2 is only available on Linux");
  return std::nullopt;
#endif
}

bool V4L2DriverAdapter::return_stream_buffer(
    const DriverCompletion& completion) {
#ifdef __linux__
  if (!continuous_streaming_ || completion.output_index >= 0) {
    return true;
  }
  return queue_capture_buffer(next_continuous_frame_number_++,
                              completion.stream_id,
                              completion.buffer_id,
                              /*output_index=*/-1,
                              completion.buffer_fd,
                              /*acquire_fence_fd=*/-1,
                              completion.buffer_size);
#else
  (void)completion;
  set_error("V4L2 is only available on Linux");
  return false;
#endif
}

int V4L2DriverAdapter::fd() const {
  return fd_;
}

std::vector<int> V4L2DriverAdapter::event_fds() const {
  if (fd_ < 0) {
    return {};
  }
  return {fd_};
}

const std::string& V4L2DriverAdapter::last_error() const {
  return last_error_;
}

V4L2DriverAdapter::V4L2QueuedRequest*
V4L2DriverAdapter::queued_request_for_index(uint32_t index) {
  for (auto& queued_request : queued_requests_) {
    if (queued_request.index == static_cast<int>(index)) {
      return &queued_request;
    }
  }
  return nullptr;
}

size_t V4L2DriverAdapter::free_queued_request_count() const {
  return static_cast<size_t>(std::count_if(
      queued_requests_.begin(), queued_requests_.end(),
      [](const V4L2QueuedRequest& queued_request) {
        return !queued_request.queued;
      }));
}

V4L2DriverAdapter::V4L2QueuedRequest*
V4L2DriverAdapter::bind_free_queued_request(uint64_t frame_number,
                                            int stream_id,
                                            int buffer_id,
                                            int output_index,
                                            int buffer_fd,
                                            int acquire_fence_fd,
                                            size_t buffer_size) {
  for (auto& queued_request : queued_requests_) {
    if (queued_request.queued) {
      continue;
    }
    queued_request.queued = true;
    queued_request.frame_number = frame_number;
    queued_request.stream_id = stream_id;
    queued_request.buffer_id = buffer_id;
    queued_request.output_index = output_index;
    queued_request.buffer_fd = buffer_fd;
    queued_request.acquire_fence_fd = acquire_fence_fd;
    queued_request.buffer_size = buffer_size;
    return &queued_request;
  }
  return nullptr;
}

void V4L2DriverAdapter::clear_queued_request(uint32_t index) {
  auto* queued_request = queued_request_for_index(index);
  if (queued_request == nullptr) {
    return;
  }
  queued_request->queued = false;
  queued_request->frame_number = 0;
  queued_request->output_index = -1;
  queued_request->stream_id = 0;
  queued_request->buffer_id = -1;
  queued_request->buffer_fd = -1;
  queued_request->acquire_fence_fd = -1;
  queued_request->buffer_size = 0;
}

void V4L2DriverAdapter::reset_queued_requests() {
  for (auto& queued_request : queued_requests_) {
    queued_request.queued = false;
    queued_request.frame_number = 0;
    queued_request.output_index = -1;
    queued_request.stream_id = 0;
    queued_request.buffer_id = -1;
    queued_request.buffer_fd = -1;
    queued_request.acquire_fence_fd = -1;
    queued_request.buffer_size = 0;
  }
}

DriverCompletion V4L2DriverAdapter::convert_queued_request_to_completion(
    const V4L2QueuedRequest& queued_request,
    size_t bytes_used,
    std::chrono::steady_clock::time_point timestamp) const {
  return DriverCompletion{
      .output_index = queued_request.output_index,
      .stream_id = queued_request.stream_id,
      .buffer_id = queued_request.buffer_id,
      .frame_number = queued_request.frame_number,
      .bytes_used = bytes_used,
      .timestamp = timestamp,
      .width = config_.width,
      .height = config_.height,
      .payload_format = FramePayloadFormat::Yuyv422,
      .buffer_fd = queued_request.buffer_fd,
      .buffer_size = queued_request.buffer_size,
      .release_fence_fd = -1,
  };
}

void V4L2DriverAdapter::set_error(std::string error) {
  last_error_ = std::move(error);
}

V4L2MultiStreamDriverAdapter::V4L2MultiStreamDriverAdapter(
    std::vector<V4L2StreamEndpointConfig> endpoints) {
  endpoints_.reserve(endpoints.size());
  for (auto& endpoint : endpoints) {
    endpoints_.push_back(Endpoint{
        .stream_type = endpoint.stream_type,
        .adapter = std::make_unique<V4L2DriverAdapter>(
            std::move(endpoint.stream)),
    });
  }
}

V4L2MultiStreamDriverAdapter::~V4L2MultiStreamDriverAdapter() {
  close();
}

bool V4L2MultiStreamDriverAdapter::configure_stream() {
  if (endpoints_.empty()) {
    set_error("no V4L2 stream endpoints configured");
    return false;
  }

  for (auto& endpoint : endpoints_) {
    if (!endpoint.adapter->configure_stream()) {
      set_error(endpoint.adapter->last_error());
      close();
      return false;
    }
  }
  return true;
}

bool V4L2MultiStreamDriverAdapter::start_streaming() {
  for (auto& endpoint : endpoints_) {
    if (!endpoint.adapter->start_streaming()) {
      set_error(endpoint.adapter->last_error());
      return false;
    }
  }
  return true;
}

size_t V4L2MultiStreamDriverAdapter::required_output_buffer_size(
    const OutputBufferTarget& output) const {
  const auto* endpoint = endpoint_for(output.stream_type);
  if (endpoint == nullptr) {
    return 0;
  }
  return endpoint->adapter->required_output_buffer_size(output);
}

bool V4L2MultiStreamDriverAdapter::start_continuous_stream(
    const StreamConfig& stream,
    std::vector<DriverOutputBuffer> buffers) {
  auto* endpoint = endpoint_for(stream.stream_type);
  if (endpoint == nullptr) {
    set_error("no V4L2 endpoint for continuous stream type");
    return false;
  }

  if (!endpoint->adapter->start_continuous_stream(stream, std::move(buffers))) {
    set_error(endpoint->adapter->last_error());
    return false;
  }
  return true;
}

void V4L2MultiStreamDriverAdapter::stop_streaming() {
  for (auto& endpoint : endpoints_) {
    endpoint.adapter->stop_streaming();
  }
}

void V4L2MultiStreamDriverAdapter::close() {
  for (auto& endpoint : endpoints_) {
    endpoint.adapter->close();
  }
}

bool V4L2MultiStreamDriverAdapter::can_submit_capture_outputs(
    const std::vector<OutputBufferTarget>& outputs) const {
  std::unordered_map<const Endpoint*, std::vector<OutputBufferTarget>>
      outputs_by_endpoint;
  for (const auto& output : outputs) {
    const auto* endpoint = endpoint_for(output.stream_type);
    if (endpoint == nullptr) {
      return false;
    }
    outputs_by_endpoint[endpoint].push_back(output);
  }

  for (const auto& [endpoint, endpoint_outputs] : outputs_by_endpoint) {
    if (!endpoint->adapter->can_submit_capture_outputs(endpoint_outputs)) {
      return false;
    }
  }
  return true;
}

bool V4L2MultiStreamDriverAdapter::submit_capture(DriverOutputBuffer output) {
  auto* endpoint = endpoint_for(output.target.stream_type);
  if (endpoint == nullptr) {
    set_error("no V4L2 endpoint for requested stream type");
    return false;
  }

  if (!endpoint->adapter->submit_capture(std::move(output))) {
    set_error(endpoint->adapter->last_error());
    return false;
  }
  return true;
}

std::optional<DriverCompletion> V4L2MultiStreamDriverAdapter::
    dequeue_completion(int ready_fd) {
  auto* endpoint = endpoint_for_fd(ready_fd);
  if (endpoint == nullptr) {
    return std::nullopt;
  }

  auto completion = endpoint->adapter->dequeue_completion(ready_fd);
  if (!completion && !endpoint->adapter->last_error().empty()) {
    set_error(endpoint->adapter->last_error());
  }
  return completion;
}

bool V4L2MultiStreamDriverAdapter::return_stream_buffer(
    const DriverCompletion& completion) {
  if (completion.output_index >= 0) {
    return true;
  }

  auto* endpoint = endpoint_for(StreamType::Preview);
  if (endpoint == nullptr) {
    set_error("no V4L2 endpoint for continuous stream type");
    return false;
  }

  if (!endpoint->adapter->return_stream_buffer(completion)) {
    set_error(endpoint->adapter->last_error());
    return false;
  }
  return true;
}

std::vector<int> V4L2MultiStreamDriverAdapter::event_fds() const {
  std::vector<int> fds;
  fds.reserve(endpoints_.size());
  for (const auto& endpoint : endpoints_) {
    const auto endpoint_fds = endpoint.adapter->event_fds();
    fds.insert(fds.end(), endpoint_fds.begin(), endpoint_fds.end());
  }
  return fds;
}

const std::string& V4L2MultiStreamDriverAdapter::last_error() const {
  return last_error_;
}

V4L2MultiStreamDriverAdapter::Endpoint*
V4L2MultiStreamDriverAdapter::endpoint_for(StreamType stream_type) {
  for (auto& endpoint : endpoints_) {
    if (endpoint.stream_type == stream_type) {
      return &endpoint;
    }
  }
  return nullptr;
}

const V4L2MultiStreamDriverAdapter::Endpoint*
V4L2MultiStreamDriverAdapter::endpoint_for(StreamType stream_type) const {
  for (const auto& endpoint : endpoints_) {
    if (endpoint.stream_type == stream_type) {
      return &endpoint;
    }
  }
  return nullptr;
}

V4L2MultiStreamDriverAdapter::Endpoint*
V4L2MultiStreamDriverAdapter::endpoint_for_fd(int fd) {
  for (auto& endpoint : endpoints_) {
    if (endpoint.adapter->fd() == fd) {
      return &endpoint;
    }
  }
  return nullptr;
}

const V4L2MultiStreamDriverAdapter::Endpoint*
V4L2MultiStreamDriverAdapter::endpoint_for_fd(int fd) const {
  for (const auto& endpoint : endpoints_) {
    if (endpoint.adapter->fd() == fd) {
      return &endpoint;
    }
  }
  return nullptr;
}

void V4L2MultiStreamDriverAdapter::set_error(std::string error) {
  last_error_ = std::move(error);
}

}  // namespace minicam
