#include "hal/task_executor.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <vector>

namespace minicam {
namespace {

class WaitGroup {
 public:
  explicit WaitGroup(size_t target) : target_(target) {}

  void done() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    ready_.notify_all();
  }

  bool wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_.wait_for(lock, std::chrono::seconds(2), [this] {
      return count_ >= target_;
    });
  }

 private:
  const size_t target_;
  size_t count_ = 0;
  std::mutex mutex_;
  std::condition_variable ready_;
};

TEST(TaskExecutorTest, ClampsInvalidConfigToAtLeastOneShardAndOneSlot) {
  TaskExecutor executor({
      .shard_count = 0,
      .shard_queue_capacity = 0,
  });

  EXPECT_EQ(executor.shard_count(), 1U);
  EXPECT_EQ(executor.shard_queue_capacity(), 1U);
}

TEST(TaskExecutorTest, RejectsPostBeforeStartAndAfterStop) {
  TaskExecutor executor({
      .shard_count = 1,
      .shard_queue_capacity = 1,
  });

  EXPECT_FALSE(executor.post(0, [] {}));
  ASSERT_TRUE(executor.start());
  executor.stop();
  EXPECT_FALSE(executor.post(0, [] {}));
}

TEST(TaskExecutorTest, RejectsEmptyTask) {
  TaskExecutor executor({
      .shard_count = 1,
      .shard_queue_capacity = 1,
  });
  ASSERT_TRUE(executor.start());

  EXPECT_FALSE(executor.post(0, RuntimeTask{}));
  executor.stop();
}

TEST(TaskExecutorTest, PreservesFifoOrderWithinSameShard) {
  TaskExecutor executor({
      .shard_count = 2,
      .shard_queue_capacity = 8,
  });
  ASSERT_TRUE(executor.start());

  std::mutex mutex;
  std::vector<int> order;
  WaitGroup done(4);

  for (int value = 0; value < 4; ++value) {
    ASSERT_TRUE(executor.post(3, [&, value] {
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(value);
      done.done();
    }));
  }

  ASSERT_TRUE(done.wait());
  executor.stop();

  EXPECT_EQ(order, std::vector<int>({0, 1, 2, 3}));
}

TEST(TaskExecutorTest, RoutesDifferentKeysToExpectedShards) {
  TaskExecutor executor({
      .shard_count = 3,
      .shard_queue_capacity = 4,
  });
  ASSERT_TRUE(executor.start());

  std::mutex mutex;
  std::vector<int> values;
  WaitGroup done(3);

  ASSERT_TRUE(executor.post(0, [&] {
    std::lock_guard<std::mutex> lock(mutex);
    values.push_back(0);
    done.done();
  }));
  ASSERT_TRUE(executor.post(3, [&] {
    std::lock_guard<std::mutex> lock(mutex);
    values.push_back(3);
    done.done();
  }));
  ASSERT_TRUE(executor.post(6, [&] {
    std::lock_guard<std::mutex> lock(mutex);
    values.push_back(6);
    done.done();
  }));

  ASSERT_TRUE(done.wait());
  executor.stop();

  EXPECT_EQ(values, std::vector<int>({0, 3, 6}));
}

TEST(TaskExecutorTest, StopDrainsAlreadyQueuedTasks) {
  TaskExecutor executor({
      .shard_count = 1,
      .shard_queue_capacity = 8,
  });
  ASSERT_TRUE(executor.start());

  WaitGroup done(4);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(executor.post(0, [&] { done.done(); }));
  }

  executor.stop();
  EXPECT_TRUE(done.wait());
}

TEST(TaskExecutorTest, RestartReusesWorkersAndQueues) {
  TaskExecutor executor({
      .shard_count = 1,
      .shard_queue_capacity = 2,
  });
  ASSERT_TRUE(executor.start());
  executor.stop();

  WaitGroup done(1);
  ASSERT_TRUE(executor.start());
  ASSERT_TRUE(executor.post(0, [&] { done.done(); }));

  EXPECT_TRUE(done.wait());
  executor.stop();
}

}  // namespace
}  // namespace minicam
