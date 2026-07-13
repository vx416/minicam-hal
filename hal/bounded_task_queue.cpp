#include "hal/bounded_task_queue.h"

#include <algorithm>
#include <cassert>
#include <thread>

namespace minicam {

BoundedTaskQueue::BoundedTaskQueue(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 1)),
      ring_(capacity_),
      sequences_(capacity_) {
  reset();
}

bool BoundedTaskQueue::push(RuntimeTask task) {
  if (!task) {
    return false;
  }

  while (true) {
    if (stopped_.load(std::memory_order_acquire)) {
      return false;
    }

    uint64_t current_tail = tail_.load(std::memory_order_relaxed);
    const uint64_t current_head = head_.load(std::memory_order_acquire);
    if (current_tail - current_head >= capacity_) {
      return false;
    }

    if (tail_.compare_exchange_weak(current_tail,
                                    current_tail + 1,
                                    std::memory_order_acq_rel,
                                    std::memory_order_relaxed)) {
      const size_t slot = static_cast<size_t>(current_tail % capacity_);
      uint64_t slot_sequence =
          sequences_[slot].load(std::memory_order_acquire);
      assert(slot_sequence == current_tail);
      // The capacity check plus successful CAS should reserve a reusable slot.
      // Keep the wait as a defensive guard if that invariant is violated by a
      // future memory-ordering or consumer-side change.
      while (slot_sequence != current_tail) {
        std::this_thread::yield();
        slot_sequence = sequences_[slot].load(std::memory_order_acquire);
      }

      ring_[slot] = std::move(task);
      sequences_[slot].store(current_tail + 1, std::memory_order_release);
      not_empty_.notify_one();
      return true;
    }
  }
}

bool BoundedTaskQueue::wait_pop(RuntimeTask* task) {
  if (task == nullptr) {
    return false;
  }

  while (true) {
    if (try_pop(task)) {
      return true;
    }
    if (stopped_.load(std::memory_order_acquire) && has_no_pending_task()) {
      return false;
    }

    std::unique_lock<std::mutex> lock(wait_mutex_);
    not_empty_.wait(lock, [&] {
      return has_available_task() ||
             (stopped_.load(std::memory_order_acquire) &&
              has_no_pending_task());
    });
  }
}

void BoundedTaskQueue::stop() {
  stopped_.store(true, std::memory_order_release);
  not_empty_.notify_all();
}

void BoundedTaskQueue::reset() {
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  stopped_.store(false, std::memory_order_relaxed);
  for (auto& task : ring_) {
    task = nullptr;
  }
  for (size_t i = 0; i < capacity_; ++i) {
    sequences_[i].store(static_cast<uint64_t>(i), std::memory_order_relaxed);
  }
}

size_t BoundedTaskQueue::capacity() const {
  return capacity_;
}

size_t BoundedTaskQueue::size() const {
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  const uint64_t head = head_.load(std::memory_order_acquire);
  return static_cast<size_t>(std::min<uint64_t>(tail - head, capacity_));
}

bool BoundedTaskQueue::stopped() const {
  return stopped_.load(std::memory_order_acquire);
}

bool BoundedTaskQueue::try_pop(RuntimeTask* task) {
  const uint64_t current_head = head_.load(std::memory_order_relaxed);
  const size_t slot = static_cast<size_t>(current_head % capacity_);
  if (sequences_[slot].load(std::memory_order_acquire) != current_head + 1) {
    return false;
  }

  *task = std::move(ring_[slot]);
  ring_[slot] = nullptr;
  sequences_[slot].store(current_head + capacity_, std::memory_order_release);
  head_.store(current_head + 1, std::memory_order_release);
  return true;
}

bool BoundedTaskQueue::has_available_task() const {
  const uint64_t current_head = head_.load(std::memory_order_relaxed);
  const size_t slot = static_cast<size_t>(current_head % capacity_);
  return sequences_[slot].load(std::memory_order_acquire) == current_head + 1;
}

bool BoundedTaskQueue::has_no_pending_task() const {
  const uint64_t head = head_.load(std::memory_order_acquire);
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  return head == tail;
}

}  // namespace minicam
