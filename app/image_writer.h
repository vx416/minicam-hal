#pragma once

#include "hal/dma_buf_allocator.h"
#include "interface/capture_result.h"

#include <memory>
#include <string>

namespace minicam::app {

bool write_ppm(const std::string& path,
               const CaptureResult& result,
               const std::shared_ptr<DmaBuf>& output_buffer);

}  // namespace minicam::app
