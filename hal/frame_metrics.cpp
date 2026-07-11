#include "hal/frame_metrics.h"

namespace minicam {

void FrameMetrics::record_success(std::chrono::microseconds latency) {
  ++completed_;
  latency_.record(latency);
}

void FrameMetrics::record_failure() {
  ++failed_;
}

uint64_t FrameMetrics::completed() const {
  return completed_;
}

uint64_t FrameMetrics::failed() const {
  return failed_;
}

std::chrono::microseconds FrameMetrics::average_latency() const {
  return latency_.average();
}

}  // namespace minicam
