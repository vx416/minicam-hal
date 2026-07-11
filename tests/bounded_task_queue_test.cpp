#include "hal/bounded_task_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace minicam {
namespace {

TEST(BoundedTaskQueueTest, PreservesFifoOrderForSingleProducer) {
  BoundedTaskQueue queue(4);
  ASSERT_TRUE(queue.push([] {}));
  ASSERT_TRUE(queue.push([] {}));
  EXPECT_EQ(queue.size(), 2U);

  int executed = 0;
  ASSERT_TRUE(queue.push([&] { executed = 3; }));

  RuntimeTask task;
  ASSERT_TRUE(queue.wait_pop(&task));
  task();
  EXPECT_EQ(executed, 0);

  ASSERT_TRUE(queue.wait_pop(&task));
  task();
  EXPECT_EQ(executed, 0);

  ASSERT_TRUE(queue.wait_pop(&task));
  task();
  EXPECT_EQ(executed, 3);
  EXPECT_EQ(queue.size(), 0U);
}

TEST(BoundedTaskQueueTest, RejectsWhenFullAndAcceptsAfterPop) {
  BoundedTaskQueue queue(2);

  EXPECT_TRUE(queue.push([] {}));
  EXPECT_TRUE(queue.push([] {}));
  EXPECT_FALSE(queue.push([] {}));
  EXPECT_EQ(queue.size(), 2U);

  RuntimeTask task;
  ASSERT_TRUE(queue.wait_pop(&task));
  task();

  EXPECT_TRUE(queue.push([] {}));
  EXPECT_EQ(queue.size(), 2U);
}

TEST(BoundedTaskQueueTest, StopWakesWaitingConsumerAndRejectsPush) {
  BoundedTaskQueue queue(1);
  std::atomic<bool> popped = true;

  std::thread consumer([&] {
    RuntimeTask task;
    popped = queue.wait_pop(&task);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.stop();
  consumer.join();

  EXPECT_FALSE(popped.load());
  EXPECT_TRUE(queue.stopped());
  EXPECT_FALSE(queue.push([] {}));
}

TEST(BoundedTaskQueueTest, StopDrainsAlreadyQueuedTasks) {
  BoundedTaskQueue queue(2);

  ASSERT_TRUE(queue.push([] {}));
  ASSERT_TRUE(queue.push([] {}));

  queue.stop();
  EXPECT_FALSE(queue.push([] {}));

  RuntimeTask task;
  EXPECT_TRUE(queue.wait_pop(&task));
  EXPECT_TRUE(static_cast<bool>(task));
  EXPECT_TRUE(queue.wait_pop(&task));
  EXPECT_TRUE(static_cast<bool>(task));
  EXPECT_FALSE(queue.wait_pop(&task));
}

TEST(BoundedTaskQueueTest, ResetReusesQueueAfterStop) {
  BoundedTaskQueue queue(1);
  queue.stop();
  ASSERT_FALSE(queue.push([] {}));

  queue.reset();
  EXPECT_FALSE(queue.stopped());

  bool ran = false;
  ASSERT_TRUE(queue.push([&] { ran = true; }));

  RuntimeTask task;
  ASSERT_TRUE(queue.wait_pop(&task));
  task();
  EXPECT_TRUE(ran);
}

TEST(BoundedTaskQueueTest, MultipleProducersDoNotLoseTasks) {
  constexpr int kProducerCount = 4;
  constexpr int kTasksPerProducer = 25;
  BoundedTaskQueue queue(128);
  std::atomic<int> executed = 0;
  std::vector<std::thread> producers;

  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&] {
      for (int i = 0; i < kTasksPerProducer; ++i) {
        while (!queue.push([&] { ++executed; })) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }

  for (int i = 0; i < kProducerCount * kTasksPerProducer; ++i) {
    RuntimeTask task;
    ASSERT_TRUE(queue.wait_pop(&task));
    task();
  }

  EXPECT_EQ(executed.load(), kProducerCount * kTasksPerProducer);
  EXPECT_EQ(queue.size(), 0U);
}

}  // namespace
}  // namespace minicam
