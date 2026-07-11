#include "hal/task_executor.h"

#include <algorithm>

namespace minicam {

TaskExecutor::ShardedWorker::ShardedWorker(size_t capacity) : queue_(capacity) {}

TaskExecutor::ShardedWorker::~ShardedWorker() {
  stop();
}

bool TaskExecutor::ShardedWorker::start() {
  if (started_) {
    return true;
  }
  queue_.reset();
  started_ = true;
  worker_ = std::thread(&ShardedWorker::run, this);
  return true;
}

void TaskExecutor::ShardedWorker::stop() {
  if (!started_) {
    return;
  }
  request_stop();
  join();
}

void TaskExecutor::ShardedWorker::request_stop() {
  if (started_) {
    queue_.stop();
  }
}

void TaskExecutor::ShardedWorker::join() {
  if (!started_) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  started_ = false;
}

bool TaskExecutor::ShardedWorker::post(RuntimeTask task) {
  if (!task) {
    return false;
  }

  if (!started_) {
    return false;
  }
  return queue_.push(std::move(task));
}

void TaskExecutor::ShardedWorker::run() {
  RuntimeTask task;
  while (queue_.wait_pop(&task)) {
    task();
    task = nullptr;
  }
}

TaskExecutor::TaskExecutor(TaskExecutorConfig config)
    : config_{
          .shard_count = std::max<size_t>(config.shard_count, 1),
          .shard_queue_capacity =
              std::max<size_t>(config.shard_queue_capacity, 1),
      } {
  workers_.reserve(config_.shard_count);
  for (size_t i = 0; i < config_.shard_count; ++i) {
    workers_.push_back(
        std::make_unique<ShardedWorker>(config_.shard_queue_capacity));
  }
}

TaskExecutor::~TaskExecutor() {
  stop();
}

bool TaskExecutor::start() {
  if (started_) {
    return true;
  }

  for (auto& worker : workers_) {
    if (!worker->start()) {
      stop();
      return false;
    }
  }
  started_ = true;
  return true;
}

void TaskExecutor::stop() {
  for (auto& worker : workers_) {
    worker->request_stop();
  }
  for (auto& worker : workers_) {
    worker->join();
  }
  started_ = false;
}

bool TaskExecutor::post(uint64_t key, RuntimeTask task) {
  if (!started_ || workers_.empty()) {
    return false;
  }
  return workers_[key % workers_.size()]->post(std::move(task));
}

size_t TaskExecutor::shard_count() const {
  return workers_.size();
}

size_t TaskExecutor::shard_queue_capacity() const {
  return config_.shard_queue_capacity;
}

}  // namespace minicam
