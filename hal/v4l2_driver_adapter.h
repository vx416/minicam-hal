#pragma once

#include "hal/driver_adapter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace minicam {

// Static V4L2 capture stream configuration.
struct V4L2StreamConfig {
  std::string device_path = "/dev/video0";
  int width = 640;
  int height = 480;
  uint32_t pixel_format = 0;  // V4L2 fourcc once linux/videodev2.h is included.
  size_t buffer_count = 4;
};

// V4L2 implementation of DriverAdapter for one capture node.
//
// This class owns V4L2 mechanics:
// - open/close
// - QUERYCAP/S_FMT/REQBUFS
// - QBUF/DQBUF
// - STREAMON/STREAMOFF
//
// It does not own HAL request ordering, in-flight accounting, or result
// callback dispatch.
class V4L2DriverAdapter final : public DriverAdapter {
 public:
  explicit V4L2DriverAdapter(
      V4L2StreamConfig config,
      std::function<DriverToken()> token_generator = {});
  ~V4L2DriverAdapter() override;

  V4L2DriverAdapter(const V4L2DriverAdapter&) = delete;
  V4L2DriverAdapter& operator=(const V4L2DriverAdapter&) = delete;

  bool configure_stream() override;
  bool start_streaming() override;
  size_t required_output_buffer_size(
      const OutputBufferTarget& output) const override;
  std::optional<std::vector<DriverSubmission>> start_continuous_stream(
      const StreamConfig& stream,
      std::vector<DriverOutputBuffer> buffers) override;
  void stop_streaming() override;
  void close() override;

  bool can_submit_capture_outputs(
      const std::vector<OutputBufferTarget>& outputs) const override;
  std::optional<DriverSubmission> submit_capture(
      DriverOutputBuffer output) override;
  std::optional<DriverCompletion> dequeue_completion(int ready_fd) override;
  std::optional<DriverSubmission> return_stream_buffer(
      StreamBufferLease lease) override;

  std::vector<int> event_fds() const override;
  const std::string& last_error() const override;

  int fd() const;

 private:
  struct V4L2QueuedRequest {
    int index = -1;
    DriverToken token;
    int buffer_fd = -1;
    int acquire_fence_fd = -1;
    size_t buffer_size = 0;
    bool queued = false;
  };

  std::optional<DriverSubmission> queue_capture_buffer(int buffer_fd,
                                                       int acquire_fence_fd,
                                                       size_t buffer_size);
  DriverCompletion convert_queued_request_to_completion(
      const V4L2QueuedRequest& queued_request,
      size_t bytes_used,
      std::chrono::steady_clock::time_point timestamp) const;
  void set_error(std::string error);

  V4L2QueuedRequest* queued_request_for_index(uint32_t index);
  size_t free_queued_request_count() const;
  V4L2QueuedRequest* bind_free_queued_request(
      DriverToken token,
      int buffer_fd,
      int acquire_fence_fd,
      size_t buffer_size);
  void clear_queued_request(uint32_t index);
  void reset_queued_requests();
  DriverToken next_token();

  V4L2StreamConfig config_;
  std::vector<V4L2QueuedRequest> queued_requests_;
  std::string last_error_;
  std::function<DriverToken()> token_generator_;
  int fd_ = -1;
  size_t buffer_size_ = 0;
#ifdef __linux__
  uint64_t next_token_ = 1;
#endif
  bool streaming_ = false;
  bool device_streaming_ = false;
  bool continuous_streaming_ = false;
};

struct V4L2StreamEndpointConfig {
  StreamType stream_type = StreamType::Still;
  V4L2StreamConfig stream;
};

// Driver adapter for a HAL configuration where different stream types are
// backed by different V4L2 video nodes/fds.
class V4L2MultiStreamDriverAdapter final : public DriverAdapter {
 public:
  explicit V4L2MultiStreamDriverAdapter(
      std::vector<V4L2StreamEndpointConfig> endpoints);
  ~V4L2MultiStreamDriverAdapter() override;

  V4L2MultiStreamDriverAdapter(const V4L2MultiStreamDriverAdapter&) = delete;
  V4L2MultiStreamDriverAdapter& operator=(
      const V4L2MultiStreamDriverAdapter&) = delete;

  bool configure_stream() override;
  bool start_streaming() override;
  size_t required_output_buffer_size(
      const OutputBufferTarget& output) const override;
  std::optional<std::vector<DriverSubmission>> start_continuous_stream(
      const StreamConfig& stream,
      std::vector<DriverOutputBuffer> buffers) override;
  void stop_streaming() override;
  void close() override;

  bool can_submit_capture_outputs(
      const std::vector<OutputBufferTarget>& outputs) const override;
  std::optional<DriverSubmission> submit_capture(
      DriverOutputBuffer output) override;
  std::optional<DriverCompletion> dequeue_completion(int ready_fd) override;
  std::optional<DriverSubmission> return_stream_buffer(
      StreamBufferLease lease) override;

  std::vector<int> event_fds() const override;
  const std::string& last_error() const override;

 private:
  struct Endpoint {
    StreamType stream_type = StreamType::Still;
    std::unique_ptr<V4L2DriverAdapter> adapter;
  };

  Endpoint* endpoint_for(StreamType stream_type);
  const Endpoint* endpoint_for(StreamType stream_type) const;
  Endpoint* endpoint_for_fd(int fd);
  const Endpoint* endpoint_for_fd(int fd) const;
  void set_error(std::string error);

  std::vector<Endpoint> endpoints_;
  std::string last_error_;
  uint64_t next_token_ = 1;
};

}  // namespace minicam
