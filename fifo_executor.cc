#include "fifo_executor.h"

#include <glog/logging.h>

namespace theta {

FIFOExecutorImpl::~FIFOExecutorImpl() {}

void FIFOExecutorImpl::post(Executor::Func func) {
  auto task =
      std::make_shared<Task>(Task::Opts{}.set_func(func).set_executor(this));

  task = maybe_execute_immediately(std::move(task));
  if (task) {
    task->set_state(Task::State::kQueuedExecutor);
    queue_.push(std::move(task));
  }
}

std::shared_ptr<Task> FIFOExecutorImpl::pop() {
  printf("> FIFOExecutorImpl::pop()\n");
  return queue_.pop();
}

}  // namespace theta
