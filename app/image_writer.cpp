#include "app/image_writer.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

namespace minicam::app {
namespace {

uint8_t clamp_to_byte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

std::vector<uint8_t> yuyv_to_rgb(std::span<const uint8_t> yuyv,
                                 int width,
                                 int height) {
  std::vector<uint8_t> rgb;
  rgb.resize(static_cast<size_t>(width * height * 3));

  size_t rgb_offset = 0;
  for (size_t i = 0; i + 3 < yuyv.size(); i += 4) {
    const int y0 = yuyv[i];
    const int u = yuyv[i + 1] - 128;
    const int y1 = yuyv[i + 2];
    const int v = yuyv[i + 3] - 128;

    const auto write_pixel = [&](int y) {
      const int c = y - 16;
      rgb[rgb_offset++] = clamp_to_byte((298 * c + 409 * v + 128) >> 8);
      rgb[rgb_offset++] =
          clamp_to_byte((298 * c - 100 * u - 208 * v + 128) >> 8);
      rgb[rgb_offset++] = clamp_to_byte((298 * c + 516 * u + 128) >> 8);
    };

    write_pixel(y0);
    if (rgb_offset < rgb.size()) {
      write_pixel(y1);
    }
  }

  return rgb;
}

}  // namespace

bool write_ppm(const std::string& path,
               const CaptureResult& result,
               const std::shared_ptr<DmaBuf>& output_buffer) {
  std::span<const uint8_t> image_bytes;
  if (output_buffer && output_buffer->mapped_data() != nullptr) {
    image_bytes = std::span<const uint8_t>(
        static_cast<const uint8_t*>(output_buffer->mapped_data()),
        output_buffer->size());
  }

  if (result.width <= 0 || result.height <= 0 || image_bytes.empty()) {
    std::cerr << "result has no image payload\n";
    return false;
  }

  std::vector<uint8_t> rgb;
  if (result.payload_format == FramePayloadFormat::Rgb24) {
    rgb.assign(image_bytes.begin(), image_bytes.end());
  } else if (result.payload_format == FramePayloadFormat::Yuyv422) {
    rgb = yuyv_to_rgb(image_bytes, result.width, result.height);
  } else {
    std::cerr << "unsupported result payload format\n";
    return false;
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cerr << "failed to open output path: " << path << '\n';
    return false;
  }

  out << "P6\n" << result.width << ' ' << result.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
  return static_cast<bool>(out);
}

}  // namespace minicam::app
