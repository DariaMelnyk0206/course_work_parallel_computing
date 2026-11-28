#include "thread_pool.h"

#include <exception>
#include <string>
#include <utility>

#include "logger.h"

namespace search_engine {

ThreadPool::ThreadPool(size_t num_threads, size_t capacity)
    : capacity_(capacity) {
  if (num_threads == 0) num_threads = 1;
  workers_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::enqueue(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    if (capacity_ > 0 && tasks_.size() >= capacity_) return false;
    tasks_.push(std::move(fn));
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  // Workers drain the remaining queue before exiting; join them here.
  for (std::jthread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

void ThreadPool::WorkerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty())
        return;  // stopping_ is true and the queue is drained.
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    try {
      task();
    } catch (const std::exception& e) {
      Logger::Instance().Error(std::string("Task threw an exception: ") +
                               e.what());
    } catch (...) {
      Logger::Instance().Error("Task threw an unknown exception");
    }
  }
}

}  // namespace search_engine
