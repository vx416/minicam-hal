#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace minicam {

using RuntimeTask = std::function<void()>;

// Bounded MPSC task queue implemented as a sequence-barrier ring buffer.
//
// Multiple producer threads may call push(); one shard worker calls wait_pop().
//
// Producers claim tail positions with CAS, wait for the target slot sequence to
// indicate that the slot is reusable, write the task, then publish by advancing
// the slot sequence. The single consumer reads published slots in head order and
// releases each slot for the next generation. The condition_variable is only a
// parking mechanism for the consumer; it does not protect ring correctness.
class BoundedTaskQueue {
 public:
  explicit BoundedTaskQueue(size_t capacity);
  ~BoundedTaskQueue() = default;

  BoundedTaskQueue(const BoundedTaskQueue&) = delete;
  BoundedTaskQueue& operator=(const BoundedTaskQueue&) = delete;

  bool push(RuntimeTask task);
  bool wait_pop(RuntimeTask* task);
  void stop();
  void reset();

  size_t capacity() const;
  size_t size() const;
  bool stopped() const;

 private:
  bool try_pop(RuntimeTask* task);
  bool has_available_task() const;
  bool has_no_pending_task() const;

  const size_t capacity_;
  std::vector<RuntimeTask> ring_;
  std::vector<std::atomic<uint64_t>> sequences_;
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};
  std::atomic<bool> stopped_{false};

  mutable std::mutex wait_mutex_;
  std::condition_variable not_empty_;
};

}  // namespace minicam
