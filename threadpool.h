#pragma once

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <thread>

#include "executor.h"
#include "fifo_executor.h"

namespace theta {

class Executor;

// The ScalingThreadpool can be configured to only allow running/prioritized
// tasks on a subset of available cores. A throttled task may run on any core.
// When a throttled task is a candidate to be promoted to a running/prioritized
// task, if it is running on one of the quiet cores, the scaler may use various
// heuristics and leave it in a throttled state.
class ScalingThreadpool {
  friend class Executor;
  friend class Impl;
  using Func = Task::Func;

 public:
  class ConfigureOpts {
   public:
    // The number of cores where the only running threadpool threads will have
    // a nice value of 19.
    size_t nice_cores() const { return nice_cores_; }
    ConfigureOpts& set_nice_cores(size_t val) {
      nice_cores_ = val;
      return *this;
    }

    size_t thread_limit() const { return thread_limit_; }
    ConfigureOpts& set_thread_limit(size_t val) {
      thread_limit_ = val;
      return *this;
    }

    std::chrono::milliseconds throttle_interval() const {
      return throttle_interval_;
    }
    ConfigureOpts& set_throttle_interval(std::chrono::milliseconds val) {
      throttle_interval_ = val;
      return *this;
    }

    static ConfigureOpts defaultOpts();

   private:
    size_t nice_cores_{0};
    size_t thread_limit_{0};
    std::chrono::milliseconds throttle_interval_{0};
  };

  static ScalingThreadpool& getInstance();

  ~ScalingThreadpool();

  ScalingThreadpool(const ScalingThreadpool&) = delete;
  void operator=(const ScalingThreadpool&) = delete;

  void configure(const ConfigureOpts& opts);

  Executor create(Executor::Opts opts);

 private:
  ScalingThreadpool();

  void task_loop();

  std::shared_mutex shared_mutex_;
  ConfigureOpts opts_;

  std::vector<std::unique_ptr<Worker>> workers_;
  TaskQueues queues_;

  std::vector<std::unique_ptr<ExecutorImpl>> executors_;

  std::shared_ptr<Task> maybe_run_immediately(ExecutorStats* stats,
                                              std::shared_ptr<Task> task);
};

}  // namespace theta
