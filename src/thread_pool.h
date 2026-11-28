#ifndef SEARCH_ENGINE_THREAD_POOL_H_
#define SEARCH_ENGINE_THREAD_POOL_H_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "httplib.h"

namespace search_engine {

// Own thread pool implementing httplib::TaskQueue. No OS scheduler
// primitives beyond std::thread/mutex/condition_variable/future are used
// (Requirement 1.7, 1.8). Workers block on a condition_variable while the
// queue is empty -- no busy polling (Requirement 4.2).
class ThreadPool final : public httplib::TaskQueue {
 public:
  // capacity == 0 means an unbounded queue; capacity > 0 bounds the number
  // of queued-but-not-started tasks (Requirement 4.3).
  ThreadPool(size_t num_threads, size_t capacity);
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ~ThreadPool() override;  // Calls shutdown() (Requirement 4.7).

  // httplib::TaskQueue interface. Non-blocking: returns false immediately
  // if the queue is full or the pool is shutting down (Requirement 4.4,
  // 4.5). Exceptions thrown by `fn` are caught in the worker thread, logged,
  // and do not terminate the worker (Requirement 4.10).
  bool enqueue(std::function<void()> fn) override;

  // Idempotent: the first call stops intake, drains the queue and joins the
  // workers; later calls are no-ops. This matters because httplib calls
  // shutdown() itself when the server stops and then destroys the pool
  // through its owning unique_ptr, so the destructor calls shutdown()
  // a second time (Requirement 4.6, 4.7).
  void shutdown() override;

  // Submits a task and returns a future for its result or exception
  // (Requirement 4.8, 4.9). Used by IndexBuilder for per-file tasks.
  // std::function<void()> requires a copyable callable, but
  // std::packaged_task is move-only, so the task is owned through a
  // std::shared_ptr and the queued lambda copies only the shared_ptr.
  // Defined in the header because it is a template. A packaged_task stores
  // any exception in the future, so the queued lambda itself never throws.
  // Throws std::runtime_error if the pool rejects the task (stopped, or
  // bounded queue full); index_pool is unbounded and never rejects while
  // it is alive.
  template <class F, class R = std::invoke_result_t<F>>
  std::future<R> Submit(F task) {
    auto packaged = std::make_shared<std::packaged_task<R()>>(std::move(task));
    std::future<R> future = packaged->get_future();
    if (!enqueue([packaged]() { (*packaged)(); })) {
      throw std::runtime_error("ThreadPool rejected the task");
    }
    return future;
  }

 private:
  void WorkerLoop();

  std::vector<std::jthread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  size_t capacity_;  // 0 == unbounded.
  bool stopping_ = false;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_THREAD_POOL_H_
