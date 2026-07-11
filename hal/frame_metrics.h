#pragma once

#include "hal/latency_tracker.h"

#include <cstdint>

namespace minicam {

// Per-session frame counters. Successes feed latency stats; failures are
// counted separately because they may not have a completed frame timestamp.
class FrameMetrics {
 public:
  void record_success(std::chrono::microseconds latency);
  void record_failure();
  uint64_t completed() const;
  uint64_t failed() const;
  std::chrono::microseconds average_latency() const;

 private:
  uint64_t completed_ = 0;
  uint64_t failed_ = 0;
  LatencyTracker latency_;
};

}  // namespace minicam
