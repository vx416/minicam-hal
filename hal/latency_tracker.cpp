#include "hal/latency_tracker.h"

namespace minicam {

void LatencyTracker::record(std::chrono::microseconds latency) {
  ++count_;
  total_ += latency;
}

uint64_t LatencyTracker::count() const {
  return count_;
}

std::chrono::microseconds LatencyTracker::average() const {
  if (count_ == 0) {
    return std::chrono::microseconds{0};
  }
  return total_ / count_;
}

}  // namespace minicam
