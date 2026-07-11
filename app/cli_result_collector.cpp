#include "app/cli_result_collector.h"

#include "app/image_writer.h"

#include <utility>

namespace minicam::app {

CliResultCollector::CliResultCollector(CliResultCollectorOptions options,
                                       const BufferTracker& preview_buffers)
    : options_(std::move(options)), preview_buffers_(preview_buffers) {}

ResultCallback CliResultCollector::callback() {
  return [this](const CaptureResult& result) {
    if (result.message == "preview") {
      handle_preview_result(result);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      results_[result.frame_number] = result;
    }
    ready_.notify_all();
  };
}

bool CliResultCollector::wait_for_preview_frames(
    std::chrono::seconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return ready_.wait_for(lock, timeout, [this] {
    return preview_frames_written_ >= options_.preview_frame_count;
  });
}

std::optional<CaptureResult> CliResultCollector::wait_for_frame(
    uint64_t frame_number,
    std::chrono::seconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  const bool completed = ready_.wait_for(lock, timeout, [this, frame_number] {
    return results_.find(frame_number) != results_.end();
  });
  if (!completed) {
    return std::nullopt;
  }

  auto result = std::move(results_[frame_number]);
  results_.erase(frame_number);
  return result;
}

size_t CliResultCollector::preview_frames_written() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return preview_frames_written_;
}

void CliResultCollector::handle_preview_result(const CaptureResult& result) {
  if (!options_.preview_output_path) {
    return;
  }

  if (result.completed_output_buffers.empty()) {
    return;
  }

  const int buffer_id = result.completed_output_buffers.front().buffer_id;
  auto output_buffer = preview_buffers_.resolve(buffer_id);
  const bool wrote_preview =
      write_ppm(*options_.preview_output_path, result, output_buffer);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wrote_preview) {
      ++preview_frames_written_;
    }
  }
  ready_.notify_all();
}

}  // namespace minicam::app
