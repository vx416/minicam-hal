#include "hal/camera_hal_runtime.h"
#include "hal/dma_buf_allocator.h"

#include <gtest/gtest.h>

namespace minicam {
namespace {

TEST(DmaBufPoolTest, RejectsZeroSizeInitialization) {
  DmaBufPool pool;

  EXPECT_FALSE(pool.initialize(0, 1));
  EXPECT_FALSE(pool.last_error().empty());
}

TEST(DmaBufPoolTest, ReportsMissingHeapPath) {
  DmaBufPool pool("/definitely/not/a/dma_heap");

  EXPECT_FALSE(pool.initialize(4096, 1));
  EXPECT_FALSE(pool.last_error().empty());
}

TEST(DmaBufPoolTest, RuntimeOwnsConfiguredPool) {
  CameraHalRuntime runtime{
      CameraHalRuntimeConfig{
          .executor = {},
          .dma_heap_path = "/tmp/minicam-test-dma-heap",
      },
  };

  EXPECT_EQ(runtime.dma_buf_pool().heap_path(), "/tmp/minicam-test-dma-heap");
}

}  // namespace
}  // namespace minicam
