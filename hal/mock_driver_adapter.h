#pragma once

#include "hal/driver_adapter.h"

#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace minicam {

// Test driver adapter with deterministic immediate completions. It exposes a
// real fd so CameraHalRuntime can exercise the same readiness path used by V4L2.
class MockDriverAdapter final : public DriverAdapter {
 public:
  MockDriverAdapter();
  ~MockDriverAdapter() override;

  MockDriverAdapter(const MockDriverAdapter&) = delete;
  MockDriverAdapter& operator=(const MockDriverAdapter&) = delete;

  bool configure_stream() override;
  bool start_streaming() override;
  size_t required_output_buffer_size(
      const OutputBufferTarget& output) const override;
  bool start_continuous_stream(const StreamConfig& stream,
                               std::vector<DriverOutputBuffer> buffers) override;
  void stop_streaming() override;
  void close() override;

  bool can_submit_capture_outputs(
      const std::vector<OutputBufferTarget>& outputs) const override;
  bool submit_capture(DriverOutputBuffer output) override;
  std::optional<DriverCompletion> dequeue_completion(int ready_fd) override;

  std::vector<int> event_fds() const override;
  const std::string& last_error() const override;

  void set_can_submit_capture_outputs(bool can_submit);
  void set_fail_submit(bool fail_submit);
  size_t submit_count() const;

 private:
  bool open_pipe();
  void set_error(std::string error);

  mutable std::mutex mutex_;
  std::queue<DriverCompletion> completions_;
  std::string last_error_;
  int read_fd_ = -1;
  int write_fd_ = -1;
  bool configured_ = false;
  bool streaming_ = false;
  bool can_submit_capture_outputs_ = true;
  bool fail_submit_ = false;
  size_t submit_count_ = 0;
};

}  // namespace minicam
