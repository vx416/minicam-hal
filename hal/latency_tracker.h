#pragma once

#include <chrono>
#include <cstdint>

namespace minicam {

// Running average helper for request-to-result latency. CameraDeviceSession
// updates this under its session lock.
class LatencyTracker {
 public:
  void record(std::chrono::microseconds latency);
  uint64_t count() const;
  std::chrono::microseconds average() const;

 private:
  uint64_t count_ = 0;
  std::chrono::microseconds total_{0};
};

}  // namespace minicam
