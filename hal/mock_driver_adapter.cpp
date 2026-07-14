#include "hal/mock_driver_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace minicam {

namespace {

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

MockDriverAdapter::MockDriverAdapter() = default;

MockDriverAdapter::~MockDriverAdapter() {
  close();
}

bool MockDriverAdapter::configure_stream() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_pipe()) {
    return false;
  }
  configured_ = true;
  return true;
}

bool MockDriverAdapter::start_streaming() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!configured_) {
    set_error("mock driver is not configured");
    return false;
  }
  streaming_ = true;
  return true;
}

size_t MockDriverAdapter::required_output_buffer_size(
    const OutputBufferTarget& output) const {
  return static_cast<size_t>(output.width) * static_cast<size_t>(output.height) *
         3U;
}

std::optional<std::vector<DriverSubmission>>
MockDriverAdapter::start_continuous_stream(
    const StreamConfig& stream,
    std::vector<DriverOutputBuffer> buffers) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!streaming_) {
    set_error("mock driver is not streaming");
    return std::nullopt;
  }

  const size_t count = buffers.empty() ? 1 : buffers.size();
  std::vector<DriverSubmission> submissions;
  submissions.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const auto fallback_target = OutputBufferTarget{
        .stream_id = stream.stream_id,
        .buffer_id = -1,
        .stream_type = stream.stream_type,
        .width = stream.width,
        .height = stream.height,
        .format = stream.format,
    };
    const auto& target = buffers.empty() ? fallback_target : buffers[i].target;
    DriverSubmission submission{
        .token = DriverToken{.value = next_token_++},
    };
    submissions.push_back(submission);
    completions_.push(DriverCompletion{
        .token = submission.token,
        .bytes_used = static_cast<size_t>(target.width * target.height * 3),
        .timestamp = std::chrono::steady_clock::now(),
    });

    const uint8_t byte = 1;
    if (::write(write_fd_, &byte, sizeof(byte)) < 0 && errno != EAGAIN) {
      set_error(std::string("mock wake write failed: ") + std::strerror(errno));
      return std::nullopt;
    }
  }
  return submissions;
}

void MockDriverAdapter::stop_streaming() {
  std::lock_guard<std::mutex> lock(mutex_);
  streaming_ = false;
}

void MockDriverAdapter::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (read_fd_ >= 0) {
    ::close(read_fd_);
    read_fd_ = -1;
  }
  if (write_fd_ >= 0) {
    ::close(write_fd_);
    write_fd_ = -1;
  }
  configured_ = false;
  streaming_ = false;
  while (!completions_.empty()) {
    completions_.pop();
  }
}

bool MockDriverAdapter::can_submit_capture_outputs(
    const std::vector<OutputBufferTarget>& outputs) const {
  (void)outputs;
  return can_submit_capture_outputs_;
}

std::optional<DriverSubmission> MockDriverAdapter::submit_capture(
    DriverOutputBuffer output) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!streaming_) {
    set_error("mock driver is not streaming");
    return std::nullopt;
  }
  if (fail_submit_) {
    set_error("mock submit failure");
    return std::nullopt;
  }
  ++submit_count_;
  DriverSubmission submission{
      .token = DriverToken{.value = next_token_++},
  };
  completions_.push(DriverCompletion{
      .token = submission.token,
      .bytes_used = static_cast<size_t>(output.target.width *
                                        output.target.height * 3),
      .timestamp = std::chrono::steady_clock::now(),
  });

  const uint8_t byte = 1;
  if (::write(write_fd_, &byte, sizeof(byte)) < 0 && errno != EAGAIN) {
    set_error(std::string("mock wake write failed: ") + std::strerror(errno));
    return std::nullopt;
  }
  return submission;
}

std::optional<DriverCompletion> MockDriverAdapter::dequeue_completion(
    int ready_fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ready_fd != read_fd_) {
    return std::nullopt;
  }
  if (completions_.empty()) {
    return std::nullopt;
  }

  uint8_t byte = 0;
  (void)::read(read_fd_, &byte, sizeof(byte));

  auto completion = completions_.front();
  completions_.pop();
  return completion;
}

std::optional<DriverSubmission> MockDriverAdapter::return_stream_buffer(
    StreamBufferLease lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!streaming_) {
    set_error("mock driver is not streaming");
    return std::nullopt;
  }
  ++returned_stream_buffer_count_;
  last_returned_stream_buffer_ = lease;
  return DriverSubmission{
      .token = DriverToken{.value = next_token_++},
  };
}

std::vector<int> MockDriverAdapter::event_fds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (read_fd_ < 0) {
    return {};
  }
  return {read_fd_};
}

const std::string& MockDriverAdapter::last_error() const {
  return last_error_;
}

void MockDriverAdapter::set_can_submit_capture_outputs(bool can_submit) {
  std::lock_guard<std::mutex> lock(mutex_);
  can_submit_capture_outputs_ = can_submit;
  if (!can_submit_capture_outputs_) {
    set_error("mock driver cannot accept every output");
  }
}

void MockDriverAdapter::set_fail_submit(bool fail_submit) {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_submit_ = fail_submit;
}

size_t MockDriverAdapter::submit_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return submit_count_;
}

size_t MockDriverAdapter::returned_stream_buffer_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return returned_stream_buffer_count_;
}

std::optional<StreamBufferLease>
MockDriverAdapter::last_returned_stream_buffer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_returned_stream_buffer_;
}

bool MockDriverAdapter::open_pipe() {
  if (read_fd_ >= 0 && write_fd_ >= 0) {
    return true;
  }

  int fds[2] = {-1, -1};
  if (::pipe(fds) < 0) {
    set_error(std::string("pipe failed: ") + std::strerror(errno));
    return false;
  }

  read_fd_ = fds[0];
  write_fd_ = fds[1];
  if (!set_nonblocking(read_fd_) || !set_nonblocking(write_fd_)) {
    set_error(std::string("fcntl O_NONBLOCK failed: ") + std::strerror(errno));
    ::close(read_fd_);
    ::close(write_fd_);
    read_fd_ = -1;
    write_fd_ = -1;
    return false;
  }
  return true;
}

void MockDriverAdapter::set_error(std::string error) {
  last_error_ = std::move(error);
}

}  // namespace minicam
