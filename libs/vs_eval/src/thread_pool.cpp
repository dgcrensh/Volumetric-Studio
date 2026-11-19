#include "vs/eval/thread_pool.hpp"

#include <atomic>

namespace vs::eval {

namespace {
// The pool whose task this thread is running, if any.
thread_local const ThreadPool* t_current_pool = nullptr;

struct PoolScope {
  const ThreadPool* previous;
  explicit PoolScope(const ThreadPool* pool) : previous(t_current_pool) { t_current_pool = pool; }
  ~PoolScope() { t_current_pool = previous; }
  PoolScope(const PoolScope&) = delete;
  PoolScope& operator=(const PoolScope&) = delete;
};
}  // namespace

struct ThreadPool::Batch {
  std::size_t n = 0;
  const std::function<void(std::size_t)>* task = nullptr;
  const ThreadPool* pool = nullptr;
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> finished{0};
  std::mutex mutex;
  std::condition_variable done;
  // The first exception by task index.
  std::size_t error_index = static_cast<std::size_t>(-1);
  std::exception_ptr error;
};

ThreadPool::ThreadPool(std::size_t threads) {
  if (threads == 0) threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
  workers_.reserve(threads - 1);
  for (std::size_t i = 1; i < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  for (auto& w : workers_) w.join();
}

bool ThreadPool::in_task() const { return t_current_pool == this; }

void ThreadPool::work_on(Batch& batch) {
  const PoolScope scope(batch.pool);
  for (;;) {
    const std::size_t i = batch.next.fetch_add(1);
    if (i >= batch.n) return;
    try {
      (*batch.task)(i);
    } catch (...) {
      const std::lock_guard lock(batch.mutex);
      if (i < batch.error_index) {
        batch.error_index = i;
        batch.error = std::current_exception();
      }
    }
    if (batch.finished.fetch_add(1) + 1 == batch.n) {
      const std::lock_guard lock(batch.mutex);
      batch.done.notify_all();
    }
  }
}

void ThreadPool::worker_loop() {
  std::uint64_t seen = 0;
  for (;;) {
    std::shared_ptr<Batch> batch;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || (batch_ && generation_ != seen); });
      if (stopping_) return;
      seen = generation_;
      batch = batch_;
    }
    work_on(*batch);
  }
}

void ThreadPool::run(std::size_t n, const std::function<void(std::size_t)>& task) {
  if (n == 0) return;
  if (workers_.empty() || in_task() || n == 1) {
    // Inline: in order, on this thread. Same contract, same results.
    const PoolScope scope(this);
    std::exception_ptr error;
    for (std::size_t i = 0; i < n; ++i) {
      try {
        task(i);
      } catch (...) {
        if (!error) error = std::current_exception();
      }
    }
    if (error) std::rethrow_exception(error);
    return;
  }

  const std::lock_guard run_lock(run_mutex_);
  auto batch = std::make_shared<Batch>();
  batch->n = n;
  batch->task = &task;
  batch->pool = this;
  {
    const std::lock_guard lock(mutex_);
    batch_ = batch;
    ++generation_;
  }
  wake_.notify_all();
  work_on(*batch);
  {
    std::unique_lock lock(batch->mutex);
    batch->done.wait(lock, [&] { return batch->finished.load() == batch->n; });
  }
  {
    const std::lock_guard lock(mutex_);
    batch_.reset();
  }
  if (batch->error) std::rethrow_exception(batch->error);
}

}  // namespace vs::eval
