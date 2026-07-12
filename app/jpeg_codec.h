#pragma once

#include "hal/dma_buf_allocator.h"
#include "interface/capture_result.h"

#include <memory>
#include <optional>
#include <vector>

namespace minicam::app {

std::optional<std::vector<uint8_t>> encode_jpeg(
    const CaptureResult& result,
    const std::shared_ptr<DmaBuf>& output_buffer);

std::optional<std::vector<uint8_t>> encode_synthetic_jpeg(
    const CaptureResult& result);

}  // namespace minicam::app
