#include "hal/dma_buf_allocator.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#define MINICAM_HAS_DMA_HEAP 1
#else
#define MINICAM_HAS_DMA_HEAP 0
#endif

#include <cerrno>
#include <cstring>
#endif

#include <algorithm>
#include <utility>

namespace minicam {

DmaBuf::DmaBuf(int fd, size_t size) : fd_(fd), size_(size) {}

DmaBuf::~DmaBuf() {
  reset();
}

DmaBuf::DmaBuf(DmaBuf&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      size_(std::exchange(other.size_, 0)),
      mapped_data_(std::exchange(other.mapped_data_, nullptr)) {}

DmaBuf& DmaBuf::operator=(DmaBuf&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  fd_ = std::exchange(other.fd_, -1);
  size_ = std::exchange(other.size_, 0);
  mapped_data_ = std::exchange(other.mapped_data_, nullptr);
  return *this;
}

bool DmaBuf::valid() const {
  return fd_ >= 0;
}

int DmaBuf::fd() const {
  return fd_;
}

size_t DmaBuf::size() const {
  return size_;
}

bool DmaBuf::map() {
#ifdef __linux__
  if (!valid() || size_ == 0) {
    return false;
  }
  if (mapped_data_ != nullptr) {
    return true;
  }
  void* mapped =
      ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapped == MAP_FAILED) {
    return false;
  }
  mapped_data_ = mapped;
  return true;
#else
  return false;
#endif
}

void DmaBuf::unmap() {
#ifdef __linux__
  if (mapped_data_ != nullptr) {
    ::munmap(mapped_data_, size_);
    mapped_data_ = nullptr;
  }
#endif
}

void* DmaBuf::mapped_data() const {
  return mapped_data_;
}

void DmaBuf::release() {
  if (pool_ == nullptr) {
    return;
  }
  pool_->release(shared_from_this());
}

int DmaBuf::release_fd() {
  unmap();
  size_ = 0;
  return std::exchange(fd_, -1);
}

void DmaBuf::reset() {
  unmap();
#ifdef __linux__
  if (fd_ >= 0) {
    ::close(fd_);
  }
#endif
  fd_ = -1;
  size_ = 0;
}

void DmaBuf::set_pool(DmaBufPool* pool) {
  pool_ = pool;
}

void DmaBuf::set_acquired(bool acquired) {
  acquired_ = acquired;
}

DmaBufPool::DmaBufPool(std::string heap_path)
    : heap_path_(std::move(heap_path)) {}

DmaBufPool::~DmaBufPool() {
  close();
}

bool DmaBufPool::initialize(size_t buffer_size,
                            size_t buffer_count,
                            bool map) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (buffer_size == 0) {
    set_error("dma-buf allocation size must be non-zero");
    return false;
  }
  if (buffer_count == 0) {
    set_error("dma-buf pool buffer count must be non-zero");
    return false;
  }

  for (auto& buffer : buffers_) {
    if (buffer) {
      buffer->set_pool(nullptr);
      buffer->set_acquired(false);
    }
  }
  buffers_.clear();
  free_buffers_.clear();
  buffer_size_ = buffer_size;
  buffers_.reserve(buffer_count);
  for (size_t i = 0; i < buffer_count; ++i) {
    auto buffer = allocate_locked(buffer_size, map);
    if (!buffer) {
      buffers_.clear();
      free_buffers_.clear();
      return false;
    }
    auto shared = std::make_shared<DmaBuf>(std::move(*buffer));
    shared->set_pool(this);
    buffers_.push_back(shared);
    free_buffers_.push_back(std::move(shared));
  }
  return true;
}

std::shared_ptr<DmaBuf> DmaBufPool::acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_buffers_.empty()) {
    set_error("no free dma-buf available");
    return nullptr;
  }
  auto buffer = free_buffers_.front();
  free_buffers_.pop_front();
  buffer->set_acquired(true);
  return buffer;
}

std::optional<DmaBuf> DmaBufPool::allocate_locked(size_t size, bool map) {
#if defined(__linux__) && MINICAM_HAS_DMA_HEAP
  if (!ensure_open_locked()) {
    return std::nullopt;
  }

  dma_heap_allocation_data allocation{};
  allocation.len = size;
  allocation.fd_flags = O_RDWR | O_CLOEXEC;
  if (::ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
    set_error(std::string("DMA_HEAP_IOCTL_ALLOC failed: ") +
              std::strerror(errno));
    return std::nullopt;
  }

  DmaBuf buffer{static_cast<int>(allocation.fd), size};
  if (map && !buffer.map()) {
    set_error(std::string("mmap dma-buf failed: ") + std::strerror(errno));
    return std::nullopt;
  }
  return buffer;
#else
  (void)size;
  (void)map;
  set_error("dma-buf heap allocation is only available on Linux with "
            "<linux/dma-heap.h>");
  return std::nullopt;
#endif
}

void DmaBufPool::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& buffer : buffers_) {
    if (buffer) {
      buffer->set_pool(nullptr);
      buffer->set_acquired(false);
    }
  }
  free_buffers_.clear();
  buffers_.clear();
  buffer_size_ = 0;
#ifdef __linux__
  if (heap_fd_ >= 0) {
    ::close(heap_fd_);
    heap_fd_ = -1;
  }
#endif
}

const std::string& DmaBufPool::heap_path() const {
  return heap_path_;
}

std::string DmaBufPool::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

size_t DmaBufPool::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return free_buffers_.size();
}

size_t DmaBufPool::capacity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.size();
}

void DmaBufPool::release(std::shared_ptr<DmaBuf> buffer) {
  if (!buffer) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find(buffers_.begin(), buffers_.end(), buffer);
  if (it == buffers_.end() || !buffer->acquired_) {
    return;
  }
  buffer->set_acquired(false);
  free_buffers_.push_back(std::move(buffer));
}

bool DmaBufPool::ensure_open_locked() {
#if defined(__linux__) && MINICAM_HAS_DMA_HEAP
  if (heap_fd_ >= 0) {
    return true;
  }

  heap_fd_ = ::open(heap_path_.c_str(), O_RDWR | O_CLOEXEC);
  if (heap_fd_ < 0) {
    set_error(std::string("open dma heap failed: ") + std::strerror(errno));
    return false;
  }
  return true;
#else
  return false;
#endif
}

void DmaBufPool::set_error(std::string error) {
  last_error_ = std::move(error);
}

}  // namespace minicam
