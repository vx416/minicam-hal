#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace minicam {

class DmaBufPool;
class DmaBufLease;

class DmaBuf {
 public:
  DmaBuf() = default;
  DmaBuf(int fd, size_t size);
  ~DmaBuf();

  DmaBuf(const DmaBuf&) = delete;
  DmaBuf& operator=(const DmaBuf&) = delete;

  DmaBuf(DmaBuf&& other) noexcept;
  DmaBuf& operator=(DmaBuf&& other) noexcept;

  bool valid() const;
  int fd() const;
  size_t size() const;

  bool map();
  void unmap();
  void* mapped_data() const;

  int release_fd();
  void reset();

 private:
  friend class DmaBufPool;

  void set_acquired(bool acquired);

  int fd_ = -1;
  size_t size_ = 0;
  void* mapped_data_ = nullptr;
  bool acquired_ = false;
};

class DmaBufLease {
 public:
  DmaBufLease() = default;
  ~DmaBufLease();

  DmaBufLease(const DmaBufLease&) = delete;
  DmaBufLease& operator=(const DmaBufLease&) = delete;

  DmaBufLease(DmaBufLease&& other) noexcept;
  DmaBufLease& operator=(DmaBufLease&& other) noexcept;

  explicit operator bool() const;
  DmaBuf* get() const;
  DmaBuf& operator*() const;
  DmaBuf* operator->() const;

  void reset();

 private:
  friend class DmaBufPool;

  DmaBufLease(DmaBufPool* pool, DmaBuf* buffer, uint64_t generation);

  DmaBufPool* pool_ = nullptr;
  DmaBuf* buffer_ = nullptr;
  uint64_t generation_ = 0;
};

class DmaBufPool {
 public:
  explicit DmaBufPool(std::string heap_path = "/dev/dma_heap/system");
  ~DmaBufPool();

  DmaBufPool(const DmaBufPool&) = delete;
  DmaBufPool& operator=(const DmaBufPool&) = delete;

  bool initialize(size_t buffer_size, size_t buffer_count, bool map = true);
  DmaBufLease acquire();
  void close();

  const std::string& heap_path() const;
  std::string last_error() const;
  size_t available() const;
  size_t capacity() const;

 private:
  bool initialize_for_testing(std::vector<DmaBuf> buffers);

  void release(DmaBuf* buffer, uint64_t generation);
  std::optional<DmaBuf> allocate_locked(size_t size, bool map);
  bool ensure_open_locked();
  void set_error(std::string error);

  friend class DmaBufLease;
  friend class DmaBufPoolTestAccess;

  mutable std::mutex mutex_;
  std::string heap_path_;
#ifdef __linux__
  int heap_fd_ = -1;
#endif
  std::string last_error_;
  size_t buffer_size_ = 0;
  uint64_t generation_ = 0;
  std::vector<std::unique_ptr<DmaBuf>> buffers_;
  std::deque<DmaBuf*> free_buffers_;
};

}  // namespace minicam
