#include "vs/eval/thread_pool.hpp"

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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
std::string environment(const char* name) {
#ifdef _MSC_VER
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
#endif
}

}  // namespace

std::size_t default_thread_count() {
  const std::string text = environment("VS_THREADS");
  std::size_t n = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
  if (!text.empty() && error == std::errc{} && end == text.data() + text.size() && n >= 1 &&
      n <= kMaxThreads) {
    return n;
  }
  return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

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

void set_current_thread_priority(WorkerPriority priority) {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(),
                    priority == WorkerPriority::Low ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(__linux__)
  // Per thread on Linux; an unprivileged thread may only raise its nice
  // value, so Normal after Low stays low.
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), priority == WorkerPriority::Low ? 10 : 0);
#else
  (void)priority;
#endif
}

ThreadPool::ThreadPool(std::size_t threads, WorkerPriority priority) {
  if (threads == 0) threads = default_thread_count();
  workers_.reserve(threads - 1);
  for (std::size_t i = 1; i < threads; ++i) {
    workers_.emplace_back([this, priority] {
      if (priority != WorkerPriority::Normal) set_current_thread_priority(priority);
      worker_loop();
    });
  }
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

std::shared_ptr<ThreadPool::Batch> ThreadPool::open_batch() const {
  // The newest first: nested calls finish before their callers need them.
  for (auto it = batches_.rbegin(); it != batches_.rend(); ++it) {
    if ((*it)->next.load() < (*it)->n) return *it;
  }
  return nullptr;
}

void ThreadPool::worker_loop() {
  for (;;) {
    std::shared_ptr<Batch> batch;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] {
        if (stopping_) return true;
        batch = open_batch();
        return batch != nullptr;
      });
      if (stopping_) return;
    }
    work_on(*batch);
  }
}

void ThreadPool::run(std::size_t n, const std::function<void(std::size_t)>& task) {
  if (n == 0) return;
  if (workers_.empty() || n == 1) {
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

  auto batch = std::make_shared<Batch>();
  batch->n = n;
  batch->task = &task;
  batch->pool = this;
  {
    const std::lock_guard lock(mutex_);
    batches_.push_back(batch);
  }
  wake_.notify_all();
  // This thread takes tasks too, then waits only for tasks other threads
  // are already running (they can't be waiting on this one: a thread that
  // calls run() from a task works on the new batch, never on this one).
  work_on(*batch);
  {
    std::unique_lock lock(batch->mutex);
    batch->done.wait(lock, [&] { return batch->finished.load() == batch->n; });
  }
  {
    const std::lock_guard lock(mutex_);
    batches_.erase(std::find(batches_.begin(), batches_.end(), batch));
  }
  if (batch->error) std::rethrow_exception(batch->error);
}

}  // namespace vs::eval
