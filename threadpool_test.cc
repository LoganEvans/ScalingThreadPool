#include "threadpool.h"

#include <glog/logging.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "gtest/gtest.h"

namespace theta {

using namespace std::chrono_literals;

TEST(Executor, ctor) {
  Executor executor{Executor::Opts{}
                        .set_priority_policy(PriorityPolicy::FIFO)
                        .set_thread_weight(5)
                        .set_worker_limit(2)
                        .set_require_low_latency(true)};

  EXPECT_EQ(executor.get_opts().priority_policy(), PriorityPolicy::FIFO);
  EXPECT_EQ(executor.get_opts().thread_weight(), 5);
  EXPECT_EQ(executor.get_opts().worker_limit(), 2);
  EXPECT_EQ(executor.get_opts().require_low_latency(), true);
}

TEST(Executor, post) {
  std::condition_variable cv;
  std::mutex mu;

  Executor executor{Executor::Opts{}};

  std::unique_lock<std::mutex> lock{mu};
  auto now = Executor::Clock::now();
  std::atomic<bool> jobRan{false};

  executor.post([&]() {
    jobRan.store(true, std::memory_order_release);
    cv.notify_all();
  });

  cv.wait_until(lock, now + 1s,
                [&]() { return jobRan.load(std::memory_order_acquire); });

  EXPECT_TRUE(jobRan.load(std::memory_order_acquire));
}

}  // namespace theta