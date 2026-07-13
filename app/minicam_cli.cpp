#include "app/buffer_tracker.h"
#include "app/fence_aware_result_writer.h"
#include "app/jpeg_encoder_processor.h"
#include "hal/camera_device_session.h"
#include "hal/camera_hal_runtime.h"
#include "hal/driver_adapter.h"
#include "hal/mock_driver_adapter.h"
#include "hal/v4l2_driver_adapter.h"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace {

struct CliOptions {
  bool use_v4l2 = false;
  std::string device_path = "/dev/video0";
  std::optional<std::string> preview_device_path;
  std::optional<std::string> still_device_path;
  std::optional<std::string> preview_output_path;
  size_t preview_frame_count = 0;
  int width = 640;
  int height = 480;
  size_t buffer_count = 4;
};

minicam::DmaBufLease acquire_output_buffer(minicam::DmaBufPool& pool) {
  auto buffer = pool.acquire();
  if (!buffer) {
    std::cerr << "dma-buf acquire failed: " << pool.last_error() << '\n';
  }
  return buffer;
}

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

void signal_fence(int write_fd) {
  const uint8_t byte = 1;
  (void)::write(write_fd, &byte, sizeof(byte));
  close_fd(write_fd);
}

void close_result_release_fences(const minicam::CaptureResult& result) {
  for (const auto& buffer : result.completed_output_buffers) {
    close_fd(buffer.release_fence_fd);
  }
}

int make_signaled_acquire_fence() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) < 0) {
    return -1;
  }

  const uint8_t byte = 1;
  if (::write(fds[1], &byte, sizeof(byte)) < 0) {
    close_fd(fds[0]);
    close_fd(fds[1]);
    return -1;
  }
  close_fd(fds[1]);
  return fds[0];
}

std::unique_ptr<minicam::DriverAdapter> make_driver(
    const CliOptions& options) {
  if (!options.use_v4l2) {
    return std::make_unique<minicam::MockDriverAdapter>();
  }

  if (options.preview_device_path && options.still_device_path) {
    return std::make_unique<minicam::V4L2MultiStreamDriverAdapter>(
        std::vector<minicam::V4L2StreamEndpointConfig>{
            minicam::V4L2StreamEndpointConfig{
                .stream_type = minicam::StreamType::Preview,
                .stream =
                    minicam::V4L2StreamConfig{
                        .device_path = *options.preview_device_path,
                        .width = options.width,
                        .height = options.height,
                        .buffer_count = options.buffer_count,
                    },
            },
            minicam::V4L2StreamEndpointConfig{
                .stream_type = minicam::StreamType::Still,
                .stream =
                    minicam::V4L2StreamConfig{
                        .device_path = *options.still_device_path,
                        .width = options.width,
                        .height = options.height,
                        .buffer_count = options.buffer_count,
                    },
            },
        });
  }

  const std::string single_device_path =
      options.still_device_path.value_or(
          options.preview_device_path.value_or(options.device_path));
  return std::make_unique<minicam::V4L2DriverAdapter>(
      minicam::V4L2StreamConfig{
          .device_path = single_device_path,
          .width = options.width,
          .height = options.height,
          .buffer_count = options.buffer_count,
      });
}

CliOptions parse_args(int argc, char** argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mock") {
      options.use_v4l2 = false;
    } else if (arg == "--v4l2" && i + 1 < argc) {
      options.use_v4l2 = true;
      options.device_path = argv[++i];
    } else if (arg == "--preview-v4l2" && i + 1 < argc) {
      options.use_v4l2 = true;
      options.preview_device_path = argv[++i];
    } else if (arg == "--still-v4l2" && i + 1 < argc) {
      options.use_v4l2 = true;
      options.still_device_path = argv[++i];
    } else if (arg == "--preview-out" && i + 1 < argc) {
      options.preview_output_path = argv[++i];
    } else if (arg == "--preview-frames" && i + 1 < argc) {
      options.preview_frame_count = static_cast<size_t>(std::stoul(argv[++i]));
    } else if (arg == "--width" && i + 1 < argc) {
      options.width = std::stoi(argv[++i]);
    } else if (arg == "--height" && i + 1 < argc) {
      options.height = std::stoi(argv[++i]);
    } else if (arg == "--buffers" && i + 1 < argc) {
      options.buffer_count = static_cast<size_t>(std::stoul(argv[++i]));
    }
  }
  return options;
}

std::optional<std::string> parse_capture_path(const std::string& line) {
  std::istringstream input(line);
  std::string command;
  input >> command;
  if (command.empty()) {
    return std::nullopt;
  }
  if (command == "quit" || command == "exit") {
    return std::string{};
  }
  if (command == "capture" || command == "snap") {
    std::string path;
    input >> path;
    if (!path.empty()) {
      return path;
    }
    return std::nullopt;
  }
  return command;
}

void close_output_fences(std::vector<minicam::OutputBufferTarget>& outputs) {
  for (auto& output : outputs) {
    if (output.acquire_fence_fd >= 0) {
      ::close(output.acquire_fence_fd);
      output.acquire_fence_fd = -1;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  const CliOptions options = parse_args(argc, argv);

  minicam::CameraHalRuntime runtime({
      .executor =
          {
              .shard_count = 2,
              .shard_queue_capacity = 16,
          },
  });
  if (!runtime.start()) {
    std::cerr << "failed to start runtime: " << runtime.last_error() << '\n';
    return EXIT_FAILURE;
  }

  if (options.use_v4l2) {
    const size_t buffer_size =
        static_cast<size_t>(options.width) *
        static_cast<size_t>(options.height) * 2U;
    const size_t pool_size =
        options.preview_device_path ? options.buffer_count + 1
                                    : options.buffer_count;
    if (!runtime.dma_buf_pool().initialize(buffer_size, pool_size)) {
      std::cerr << "failed to initialize dma-buf pool: "
                << runtime.dma_buf_pool().last_error() << '\n';
      runtime.stop();
      return EXIT_FAILURE;
    }
  }

  std::vector<minicam::OutputBufferTarget> preview_buffers;
  minicam::app::BufferTracker preview_buffer_tracker;
  minicam::app::BufferTracker capture_buffer_tracker;
  if (options.use_v4l2 && options.preview_device_path) {
    preview_buffers.reserve(options.buffer_count);
    for (size_t i = 0; i < options.buffer_count; ++i) {
      auto buffer = acquire_output_buffer(runtime.dma_buf_pool());
      if (!buffer) {
        runtime.stop();
        return EXIT_FAILURE;
      }
      const int buffer_id = static_cast<int>(i);
      const int buffer_fd = buffer->fd();
      const size_t buffer_size = buffer->size();
      if (!preview_buffer_tracker.register_buffer(buffer_id,
                                                  std::move(buffer))) {
        runtime.stop();
        return EXIT_FAILURE;
      }
      preview_buffers.push_back(minicam::OutputBufferTarget{
          .stream_id = 1,
          .buffer_id = buffer_id,
          .buffer_fd = buffer_fd,
          .acquire_fence_fd = make_signaled_acquire_fence(),
          .buffer_size = buffer_size,
          .stream_type = minicam::StreamType::Preview,
          .width = options.width,
          .height = options.height,
          .format = minicam::PixelFormat::Yuyv422,
      });
    }
  }

  auto encoded_frames = std::make_shared<minicam::app::EncodedFrameStore>();
  minicam::app::FenceAwareResultWriter result_writer(
      preview_buffer_tracker,
      capture_buffer_tracker,
      encoded_frames);
  std::mutex preview_mutex;
  std::condition_variable preview_ready;
  size_t preview_frames_written = 0;
  std::mutex preview_workers_mutex;
  std::vector<std::thread> preview_workers;

  minicam::ResultCallback result_callback =
      [&](const minicam::CaptureResult& result) {
        result_writer.register_result(result);
      };

  minicam::StreamResultCallback stream_result_callback =
      [&](const minicam::CaptureResult& result,
          minicam::StreamBufferLease lease) {
        if (!options.preview_output_path) {
          close_result_release_fences(result);
          return lease;
        }

        {
          std::lock_guard<std::mutex> lock(preview_mutex);
          if (preview_frames_written >= options.preview_frame_count) {
            close_result_release_fences(result);
            return lease;
          }
        }

        result_writer.register_path(result.frame_number,
                                    *options.preview_output_path);
        result_writer.register_result(result);
        int fds[2] = {-1, -1};
        if (::pipe(fds) < 0) {
          const auto written_preview =
              result_writer.block_wait_and_write(result.frame_number,
                                                 std::chrono::seconds(5));
          {
            std::lock_guard<std::mutex> lock(preview_mutex);
            if (written_preview && written_preview->wrote_image) {
              ++preview_frames_written;
            }
          }
          preview_ready.notify_all();
          return lease;
        }
        lease.consumer_release_fence_fd = fds[0];
        std::thread worker([&result_writer,
                            &preview_mutex,
                            &preview_ready,
                            &preview_frames_written,
                            frame_number = result.frame_number,
                            write_fd = fds[1]] {
          const auto written_preview =
              result_writer.block_wait_and_write(frame_number,
                                                 std::chrono::seconds(5));
          {
            std::lock_guard<std::mutex> lock(preview_mutex);
            if (written_preview && written_preview->wrote_image) {
              ++preview_frames_written;
            }
          }
          preview_ready.notify_all();
          signal_fence(write_fd);
        });
        {
          std::lock_guard<std::mutex> lock(preview_workers_mutex);
          preview_workers.push_back(std::move(worker));
        }
        return lease;
      };

  auto join_preview_workers = [&] {
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(preview_workers_mutex);
      workers.swap(preview_workers);
    }
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  };

  auto output_processor =
      std::make_shared<minicam::app::JpegEncoderProcessor>(
          preview_buffer_tracker,
          capture_buffer_tracker,
          encoded_frames);

  auto session = std::make_shared<minicam::CameraDeviceSession>(
      runtime,
      minicam::CameraDeviceSessionConfig{
          .session_id = 1,
          .buffer_count = options.buffer_count,
          .continuous_preview_config =
              options.preview_device_path
                  ? std::optional<minicam::StreamConfig>{
                        minicam::StreamConfig{
                            .stream_id = 1,
                            .stream_type = minicam::StreamType::Preview,
                            .width = options.width,
                            .height = options.height,
                            .format = minicam::PixelFormat::Yuyv422,
                        }}
                  : std::nullopt,
          .continuous_preview_buffers = preview_buffers,
      },
      make_driver(options),
      result_callback,
      std::vector<std::shared_ptr<minicam::OutputProcessor>>{
          output_processor,
      },
      stream_result_callback);

  if (!session->configure_streams()) {
    std::cerr << "failed to configure session\n";
    runtime.stop();
    return EXIT_FAILURE;
  }
  if (!session->start_streaming()) {
    std::cerr << "failed to start session streaming\n";
    session->close();
    runtime.stop();
    return EXIT_FAILURE;
  }

  std::cout << "MiniCam HAL CLI\n";
  std::cout << "commands: capture <path.jpg>, <path.jpg>, quit\n";

  if (options.preview_output_path && options.preview_frame_count > 0) {
    std::unique_lock<std::mutex> lock(preview_mutex);
    const bool completed = preview_ready.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] {
          return preview_frames_written >= options.preview_frame_count;
        });
    lock.unlock();
    if (!completed) {
      std::cerr << "timed out waiting for preview frames\n";
      join_preview_workers();
      session->close();
      runtime.stop();
      return EXIT_FAILURE;
    }

    std::cout << "saved " << preview_frames_written
              << " preview frames to " << *options.preview_output_path << '\n';
    join_preview_workers();
    session->close();
    preview_buffer_tracker.release_all();
    runtime.stop();
    return EXIT_SUCCESS;
  }

  uint64_t next_frame = 1;
  std::string line;
  while (true) {
    std::cout << "capture path> " << std::flush;
    if (!std::getline(std::cin, line)) {
      break;
    }

    auto parsed = parse_capture_path(line);
    if (!parsed) {
      std::cout << "usage: capture <path.jpg>\n";
      continue;
    }
    if (parsed->empty()) {
      break;
    }

    const uint64_t frame_number = next_frame++;
    std::vector<minicam::OutputBufferTarget> outputs;
    if (options.use_v4l2) {
      auto buffer = acquire_output_buffer(runtime.dma_buf_pool());
      if (!buffer) {
        continue;
      }
      const int buffer_id = static_cast<int>(frame_number);
      const int buffer_fd = buffer->fd();
      const size_t buffer_size = buffer->size();
      if (!capture_buffer_tracker.register_buffer(buffer_id,
                                                  std::move(buffer))) {
        continue;
      }
      outputs = {
          minicam::OutputBufferTarget{
              .stream_id =
                  options.preview_device_path && options.still_device_path
                      ? 2
                      : 0,
              .buffer_id = buffer_id,
              .buffer_fd = buffer_fd,
              .acquire_fence_fd = make_signaled_acquire_fence(),
              .buffer_size = buffer_size,
              .stream_type = minicam::StreamType::Still,
              .width = options.width,
              .height = options.height,
              .format = minicam::PixelFormat::Yuyv422,
          },
      };
    }

    result_writer.register_path(frame_number, *parsed);
    auto request_outputs = std::move(outputs);
    if (!session->process_capture_request(minicam::CaptureRequest{
            .frame_number = frame_number,
            .width = options.width,
            .height = options.height,
            .output_buffers = std::move(request_outputs),
    })) {
      std::cerr << "request rejected by HAL\n";
      close_output_fences(request_outputs);
      result_writer.cancel(frame_number);
      capture_buffer_tracker.release(static_cast<int>(frame_number));
      continue;
    }

    auto written = result_writer.block_wait_and_write(frame_number,
                                                      std::chrono::seconds(5));
    if (!written) {
      std::cerr << "timed out waiting for frame " << frame_number << '\n';
      continue;
    }

    const auto& capture_result = written->result;

    if (capture_result.status != minicam::CaptureStatus::Ok) {
      std::cerr << "capture failed: " << capture_result.message << '\n';
      continue;
    }

    if (written->wrote_image) {
      std::cout << "saved " << *parsed << " (" << capture_result.width << "x"
                << capture_result.height << ", "
                << capture_result.latency.count()
                << " us)\n";
    } else {
      std::cerr << "failed to write " << *parsed << '\n';
    }
  }

  join_preview_workers();
  session->close();
  preview_buffer_tracker.release_all();
  capture_buffer_tracker.release_all();
  runtime.stop();
  return EXIT_SUCCESS;
}
