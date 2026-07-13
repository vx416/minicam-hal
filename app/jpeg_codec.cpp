#include "app/jpeg_codec.h"

#include <jpge.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace minicam::app {
namespace {

uint8_t clamp_to_byte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void append_yuyv_as_rgb(std::span<const uint8_t> yuyv,
                        std::vector<uint8_t>* rgb) {
  if (rgb == nullptr) {
    return;
  }

  for (size_t i = 0; i + 3 < yuyv.size(); i += 4) {
    const int y0 = yuyv[i];
    const int u = yuyv[i + 1] - 128;
    const int y1 = yuyv[i + 2];
    const int v = yuyv[i + 3] - 128;

    const auto write_pixel = [&](int y) {
      const int c = y - 16;
      rgb->push_back(clamp_to_byte((298 * c + 409 * v + 128) >> 8));
      rgb->push_back(
          clamp_to_byte((298 * c - 100 * u - 208 * v + 128) >> 8));
      rgb->push_back(clamp_to_byte((298 * c + 516 * u + 128) >> 8));
    };

    write_pixel(y0);
    write_pixel(y1);
  }
}

std::optional<std::vector<uint8_t>> compress_rgb_to_jpeg(
    int width,
    int height,
    const std::vector<uint8_t>& rgb) {
  if (width <= 0 || height <= 0 || rgb.empty()) {
    return std::nullopt;
  }

  int jpeg_size = static_cast<int>(rgb.size() + 1024U);
  std::vector<uint8_t> encoded(static_cast<size_t>(jpeg_size));
  if (!jpge::compress_image_to_jpeg_file_in_memory(encoded.data(),
                                                   jpeg_size,
                                                   width,
                                                   height,
                                                   3,
                                                   rgb.data()) ||
      jpeg_size <= 0) {
    return std::nullopt;
  }

  encoded.resize(static_cast<size_t>(jpeg_size));
  return encoded;
}

}  // namespace

std::optional<std::vector<uint8_t>> encode_jpeg(
    const CaptureResult& result,
    const DmaBuf* output_buffer) {
  std::span<const uint8_t> image_bytes;
  if (output_buffer != nullptr && output_buffer->mapped_data() != nullptr) {
    image_bytes = std::span<const uint8_t>(
        static_cast<const uint8_t*>(output_buffer->mapped_data()),
        output_buffer->size());
  }

  if (result.width <= 0 || result.height <= 0 || image_bytes.empty()) {
    return std::nullopt;
  }

  std::vector<uint8_t> rgb;
  rgb.reserve(static_cast<size_t>(result.width) *
              static_cast<size_t>(result.height) * 3U);
  if (result.payload_format == FramePayloadFormat::Rgb24) {
    const size_t rgb_size = static_cast<size_t>(result.width) *
                            static_cast<size_t>(result.height) * 3U;
    if (image_bytes.size() < rgb_size) {
      return std::nullopt;
    }
    rgb.insert(rgb.end(), image_bytes.begin(),
               image_bytes.begin() + static_cast<ptrdiff_t>(rgb_size));
  } else if (result.payload_format == FramePayloadFormat::Yuyv422) {
    append_yuyv_as_rgb(image_bytes, &rgb);
  } else {
    return std::nullopt;
  }

  return compress_rgb_to_jpeg(result.width, result.height, rgb);
}

std::optional<std::vector<uint8_t>> encode_synthetic_jpeg(
    const CaptureResult& result) {
  if (result.width <= 0 || result.height <= 0) {
    return std::nullopt;
  }

  std::vector<uint8_t> rgb;
  rgb.reserve(static_cast<size_t>(result.width) *
              static_cast<size_t>(result.height) * 3U);
  for (int y = 0; y < result.height; ++y) {
    for (int x = 0; x < result.width; ++x) {
      rgb.push_back(static_cast<uint8_t>((x * 255) / result.width));
      rgb.push_back(static_cast<uint8_t>((y * 255) / result.height));
      rgb.push_back(static_cast<uint8_t>(
          ((x + y + static_cast<int>(result.frame_number)) * 255) /
          (result.width + result.height)));
    }
  }
  return compress_rgb_to_jpeg(result.width, result.height, rgb);
}

}  // namespace minicam::app
