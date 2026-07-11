#pragma once

#include "hal/bounded_task_queue.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace minicam {

struct TaskExecutorConfig {
  size_t shard_count = 4;
  size_t shard_queue_capacity = 256;
};

// Runtime-level task executor with one bounded ring queue and one worker per
// shard.
// Tasks are routed by key:
//
//   shard = key % shard_count
//
// Camera sessions use session_id as the key so submit/completion/flush/close
// tasks for the same session are serialized by the same shard worker, while
// different sessions can run in parallel across shards.
class TaskExecutor {
 public:
  explicit TaskExecutor(TaskExecutorConfig config = {});
  ~TaskExecutor();

  TaskExecutor(const TaskExecutor&) = delete;
  TaskExecutor& operator=(const TaskExecutor&) = delete;

  bool start();
  void stop();

  bool post(uint64_t key, RuntimeTask task);

  size_t shard_count() const;
  size_t shard_queue_capacity() const;

 private:
  class ShardedWorker {
   public:
    explicit ShardedWorker(size_t capacity);
    ~ShardedWorker();

    ShardedWorker(const ShardedWorker&) = delete;
    ShardedWorker& operator=(const ShardedWorker&) = delete;

    bool start();
    void stop();
    void request_stop();
    void join();
    bool post(RuntimeTask task);

   private:
    void run();

    BoundedTaskQueue queue_;
    bool started_ = false;

    std::thread worker_;
  };

  TaskExecutorConfig config_;
  std::vector<std::unique_ptr<ShardedWorker>> workers_;
  bool started_ = false;
};

}  // namespace minicam
