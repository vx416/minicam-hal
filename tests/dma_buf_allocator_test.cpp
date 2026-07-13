#include "hal/camera_hal_runtime.h"
#include "hal/dma_buf_allocator.h"

#include <unistd.h>

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace minicam {

class DmaBufPoolTestAccess {
 public:
  static bool initialize(DmaBufPool& pool, std::vector<DmaBuf> buffers) {
    return pool.initialize_for_testing(std::move(buffers));
  }
};

namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int release() {
    return std::exchange(fd_, -1);
  }

 private:
  int fd_ = -1;
};

DmaBuf make_test_buffer(size_t size) {
  int fds[2] = {-1, -1};
  EXPECT_EQ(::pipe(fds), 0);
  ScopedFd read_fd(fds[0]);
  ScopedFd write_fd(fds[1]);
  return DmaBuf(read_fd.release(), size);
}

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

TEST(DmaBufPoolTest, LeaseDestructorReturnsBufferToPool) {
  DmaBufPool pool;
  std::vector<DmaBuf> buffers;
  buffers.push_back(make_test_buffer(4096));

  ASSERT_TRUE(DmaBufPoolTestAccess::initialize(pool, std::move(buffers)));
  EXPECT_EQ(pool.capacity(), 1U);
  EXPECT_EQ(pool.available(), 1U);

  {
    auto lease = pool.acquire();
    ASSERT_TRUE(lease);
    EXPECT_EQ(pool.available(), 0U);
  }

  EXPECT_EQ(pool.available(), 1U);
}

TEST(DmaBufPoolTest, MoveTransfersLeaseWithoutReturningBuffer) {
  DmaBufPool pool;
  std::vector<DmaBuf> buffers;
  buffers.push_back(make_test_buffer(4096));

  ASSERT_TRUE(DmaBufPoolTestAccess::initialize(pool, std::move(buffers)));

  auto lease = pool.acquire();
  ASSERT_TRUE(lease);
  EXPECT_EQ(pool.available(), 0U);

  {
    auto moved = std::move(lease);
    EXPECT_FALSE(lease);
    EXPECT_TRUE(moved);
    EXPECT_EQ(pool.available(), 0U);
  }

  EXPECT_EQ(pool.available(), 1U);
}

}  // namespace
}  // namespace minicam
