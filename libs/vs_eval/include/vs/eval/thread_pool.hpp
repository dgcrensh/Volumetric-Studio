#pragma once

// A deterministic thread pool (plan, M9).
//
// Results must not depend on how many threads ran them (spec, design rule
// 7). The pool guarantees that by construction rather than by care at each
// call site:
//
//   - run(n, task) calls task(i) once for each i in [0, n), in no
//     particular order and on any thread. Each task writes only its own
//     slot of an output, so the outcome is the same for any schedule.
//   - parallel_for() and parallel_reduce() split [0, n) into chunks of a
//     fixed size chosen by the caller, never by the thread count. A
//     reduction computes each chunk's partial result independently, then
//     folds the partials on the calling thread in chunk order. Floating-
//     point sums therefore come out bit-identical with 1, 4 or 16 threads.
//
// A pool of size 1 runs everything on the calling thread. Calls nest: a
// task may itself call run() (M12: a single procedural volume's sampling
// and meshing then use every thread, where M9 ran them inline). The
// calling thread works on its own call's tasks and then waits only for
// tasks already running on other threads, so nesting can't deadlock; idle
// workers take the newest call's tasks first. Where work happens never
// changes a result.
//
// Tasks may throw: the pool finishes the remaining tasks of the call, then
// rethrows the first exception (by task index) on the calling thread.
// Calls from several threads at once run side by side, sharing the workers.

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vs::eval {

// The most threads VS_THREADS may ask for.
inline constexpr std::size_t kMaxThreads = 1024;

// What a pool of 0 threads gets: the VS_THREADS environment variable when
// it is a whole number from 1 to kMaxThreads (CI runs the suites on 1, 4
// and 16 threads that way, plan H3), else one per hardware thread.
std::size_t default_thread_count();

class ThreadPool {
 public:
  // `threads` counts the calling thread: 1 starts no workers. 0 means
  // default_thread_count().
  explicit ThreadPool(std::size_t threads = 0);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // How many threads run tasks, the calling thread included.
  [[nodiscard]] std::size_t size() const { return workers_.size() + 1; }

  // Calls task(i) for every i in [0, n) and returns when all have finished.
  void run(std::size_t n, const std::function<void(std::size_t)>& task);

  // True on one of this pool's worker threads, or inside run() on the
  // thread that called it.
  [[nodiscard]] bool in_task() const;

 private:
  struct Batch;
  void worker_loop();
  static void work_on(Batch& batch);
  // The newest call with tasks no thread has taken yet (mutex_ held).
  [[nodiscard]] std::shared_ptr<Batch> open_batch() const;

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::shared_ptr<Batch>> batches_;  // calls in progress, oldest first
  bool stopping_ = false;
};

// Calls body(begin, end) for consecutive chunks of [0, n), each `chunk`
// long (the last may be shorter).
template <typename Body>
void parallel_for(ThreadPool& pool, std::size_t n, std::size_t chunk, Body&& body) {
  if (n == 0) return;
  if (chunk == 0) chunk = 1;
  const std::size_t chunks = (n + chunk - 1) / chunk;
  pool.run(chunks, [&](std::size_t c) {
    const std::size_t begin = c * chunk;
    const std::size_t end = begin + chunk < n ? begin + chunk : n;
    body(begin, end);
  });
}

// reduce(...reduce(reduce(init, map(chunk 0)), map(chunk 1))..., map(last)),
// where map(begin, end) is computed for each chunk of [0, n) in parallel
// and the folding runs on the calling thread, in chunk order. With a fixed
// `chunk` the result doesn't depend on the pool's size.
template <typename T, typename Map, typename Reduce>
T parallel_reduce(ThreadPool& pool, std::size_t n, std::size_t chunk, T init, Map&& map,
                  Reduce&& reduce) {
  if (n == 0) return init;
  if (chunk == 0) chunk = 1;
  const std::size_t chunks = (n + chunk - 1) / chunk;
  std::vector<T> partial(chunks, init);
  pool.run(chunks, [&](std::size_t c) {
    const std::size_t begin = c * chunk;
    const std::size_t end = begin + chunk < n ? begin + chunk : n;
    partial[c] = map(begin, end);
  });
  T out = std::move(init);
  for (auto& p : partial) out = reduce(std::move(out), std::move(p));
  return out;
}

}  // namespace vs::eval
