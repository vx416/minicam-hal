#include "hal/in_flight_request_tracker.h"

namespace minicam {

bool InFlightRequestTracker::start(
    CaptureRequest request,
    std::vector<InFlightOutput> output_buffers) {
  const auto frame_number = request.frame_number;
  // A duplicate frame number would make completion routing ambiguous.
  if (requests_.contains(frame_number) || output_buffers.empty()) {
    return false;
  }

  requests_.emplace(
      frame_number,
      InFlightRequest{
          .frame_number = frame_number,
          .request = request,
          .output_buffers = std::move(output_buffers),
          .metadata = {},
          .state = InFlightState::Submitted,
          .submitted_at = request.submitted_at,
      });
  return true;
}

bool InFlightRequestTracker::mark_queued_to_driver(uint64_t frame_number) {
  auto it = requests_.find(frame_number);
  if (it == requests_.end() || it->second.state != InFlightState::Submitted) {
    return false;
  }
  it->second.state = InFlightState::QueuedToDriver;
  return true;
}

std::optional<InFlightRequest> InFlightRequestTracker::complete(
    uint64_t frame_number, CaptureMetadata metadata) {
  auto it = requests_.find(frame_number);
  if (it == requests_.end()) {
    return std::nullopt;
  }
  it->second.metadata = metadata;
  it->second.state = InFlightState::Completed;
  auto request = it->second;
  requests_.erase(it);
  return request;
}

OutputCompletionResult InFlightRequestTracker::complete_output(
    uint64_t frame_number,
    int output_index,
    int release_fence_fd,
    CaptureMetadata metadata) {
  auto it = requests_.find(frame_number);
  if (it == requests_.end()) {
    return OutputCompletionResult{
        .state = OutputCompletionState::NotFound,
        .request = std::nullopt,
    };
  }

  bool matched = false;
  bool all_completed = true;
  for (auto& output : it->second.output_buffers) {
    if (output.output_index == output_index) {
      output.completed = true;
      output.release_fence_fd = release_fence_fd;
      matched = true;
    }
    all_completed = output.completed && all_completed;
  }

  if (!matched) {
    return OutputCompletionResult{
        .state = OutputCompletionState::NotFound,
        .request = std::nullopt,
    };
  }
  if (!all_completed) {
    return OutputCompletionResult{
        .state = OutputCompletionState::WaitingForMoreOutputs,
        .request = std::nullopt,
    };
  }

  it->second.metadata = metadata;
  it->second.state = InFlightState::Completed;
  auto request = it->second;
  requests_.erase(it);
  return OutputCompletionResult{
      .state = OutputCompletionState::RequestComplete,
      .request = std::move(request),
  };
}

std::optional<InFlightRequest> InFlightRequestTracker::fail(
    uint64_t frame_number) {
  auto it = requests_.find(frame_number);
  if (it == requests_.end()) {
    return std::nullopt;
  }
  it->second.state = InFlightState::Failed;
  auto request = it->second;
  requests_.erase(it);
  return request;
}

std::vector<InFlightRequest> InFlightRequestTracker::flush_all() {
  std::vector<InFlightRequest> flushed;
  flushed.reserve(requests_.size());
  // Copy flushed rows out before clearing so the session can dispatch results
  // and release any associated output slots.
  for (auto& [_, request] : requests_) {
    request.state = InFlightState::Flushed;
    flushed.push_back(request);
  }
  requests_.clear();
  return flushed;
}

bool InFlightRequestTracker::contains(uint64_t frame_number) const {
  return requests_.contains(frame_number);
}

size_t InFlightRequestTracker::size() const {
  return requests_.size();
}

}  // namespace minicam
