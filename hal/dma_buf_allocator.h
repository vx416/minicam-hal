#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace minicam {

class DmaBufPool;

class DmaBuf : public std::enable_shared_from_this<DmaBuf> {
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

  void release();
  int release_fd();
  void reset();

 private:
  friend class DmaBufPool;

  void set_pool(DmaBufPool* pool);
  void set_acquired(bool acquired);

  DmaBufPool* pool_ = nullptr;
  int fd_ = -1;
  size_t size_ = 0;
  void* mapped_data_ = nullptr;
  bool acquired_ = false;
};

class DmaBufPool {
 public:
  explicit DmaBufPool(std::string heap_path = "/dev/dma_heap/system");
  ~DmaBufPool();

  DmaBufPool(const DmaBufPool&) = delete;
  DmaBufPool& operator=(const DmaBufPool&) = delete;

  bool initialize(size_t buffer_size, size_t buffer_count, bool map = true);
  std::shared_ptr<DmaBuf> acquire();
  void close();

  const std::string& heap_path() const;
  std::string last_error() const;
  size_t available() const;
  size_t capacity() const;

 private:
  void release(std::shared_ptr<DmaBuf> buffer);
  std::optional<DmaBuf> allocate_locked(size_t size, bool map);
  bool ensure_open_locked();
  void set_error(std::string error);

  friend class DmaBuf;

  mutable std::mutex mutex_;
  std::string heap_path_;
#ifdef __linux__
  int heap_fd_ = -1;
#endif
  std::string last_error_;
  size_t buffer_size_ = 0;
  std::vector<std::shared_ptr<DmaBuf>> buffers_;
  std::deque<std::shared_ptr<DmaBuf>> free_buffers_;
};

}  // namespace minicam
