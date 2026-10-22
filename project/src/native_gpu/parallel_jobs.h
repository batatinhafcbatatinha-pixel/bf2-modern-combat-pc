#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <system_error>
#include <vector>

namespace bf2n {
// Persistent workers for CPU-only reads. Run is synchronous; resources may be updated only
// after it returns. D3D11 context calls remain on the renderer thread.
class ParallelJobs {
 public:
  explicit ParallelJobs(unsigned total_threads = 4) {
    workers_.reserve(total_threads ? total_threads - 1 : 0);
    for (unsigned i=1;i<total_threads;++i) {
      try { workers_.emplace_back([this] { Worker(); }); }
      catch (const std::exception&) { break; }  // remaining workers (or the caller) suffice
    }
  }
  ~ParallelJobs() {
    { std::lock_guard lock(mutex_); stop_=true; }
    wake_.notify_all();
    for (auto& thread:workers_) thread.join();
  }
  ParallelJobs(const ParallelJobs&) = delete;
  ParallelJobs& operator=(const ParallelJobs&) = delete;
  void Run(size_t count, const std::function<void(size_t)>& job) {
    if (count<32 || workers_.empty()) {
      for (size_t i=0;i<count;++i) job(i);
      return;
    }
    {
      std::lock_guard lock(mutex_);
      count_=count; job_=job; next_=0; remaining_=workers_.size(); ++generation_;
    }
    wake_.notify_all();
    Work();
    std::unique_lock lock(mutex_);
    done_.wait(lock,[this] { return remaining_==0; });
    job_={};
  }
 private:
  void Work() {
    for (;;) {
      size_t index=next_.fetch_add(1,std::memory_order_relaxed);
      if (index>=count_) break;
      job_(index);
    }
  }
  void Worker() {
    size_t observed=0;
    for (;;) {
      std::unique_lock lock(mutex_);
      wake_.wait(lock,[&] { return stop_ || generation_!=observed; });
      if (stop_) return;
      observed=generation_;
      lock.unlock();
      Work();
      lock.lock();
      if (--remaining_==0) done_.notify_one();
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_,done_;
  std::vector<std::thread> workers_;
  std::function<void(size_t)> job_;
  std::atomic<size_t> next_{0};
  size_t count_=0,remaining_=0,generation_=0;
  bool stop_=false;
};
}
