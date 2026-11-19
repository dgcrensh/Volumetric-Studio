#include <atomic>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "vs/eval/thread_pool.hpp"

using vs::eval::parallel_for;
using vs::eval::parallel_reduce;
using vs::eval::ThreadPool;

TEST_CASE("The pool runs every task exactly once", "[eval][pool]") {
  for (const std::size_t threads : {1u, 4u, 16u}) {
    ThreadPool pool(threads);
    CHECK(pool.size() == threads);
    std::vector<std::atomic<int>> hits(1000);
    pool.run(hits.size(), [&](std::size_t i) { hits[i].fetch_add(1); });
    for (const auto& h : hits) CHECK(h.load() == 1);
    // Again, to reuse the workers.
    pool.run(hits.size(), [&](std::size_t i) { hits[i].fetch_add(1); });
    for (const auto& h : hits) CHECK(h.load() == 2);
  }
}

TEST_CASE("Ordered reductions don't depend on the thread count", "[eval][pool]") {
  // A sum whose value depends on its order in floating point.
  std::vector<double> values(100000);
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = std::sin(static_cast<double>(i)) * std::pow(10.0, static_cast<double>(i % 17) - 8);
  }
  std::vector<double> sums;
  for (const std::size_t threads : {1u, 2u, 4u, 16u}) {
    ThreadPool pool(threads);
    sums.push_back(parallel_reduce(
        pool, values.size(), 1000, 0.0,
        [&](std::size_t b, std::size_t e) {
          double s = 0;
          for (std::size_t i = b; i < e; ++i) s += values[i];
          return s;
        },
        [](double a, double b) { return a + b; }));
  }
  for (const double s : sums) CHECK(s == sums.front());  // bit for bit
}

TEST_CASE("parallel_for covers the range in chunks", "[eval][pool]") {
  ThreadPool pool(4);
  // Catch2 assertions aren't thread-safe: tasks only record, the test checks.
  std::vector<int> seen(1003, 0);
  std::atomic<bool> long_chunk{false};
  parallel_for(pool, seen.size(), 100, [&](std::size_t b, std::size_t e) {
    if (e - b > 100) long_chunk = true;
    for (std::size_t i = b; i < e; ++i) seen[i] += 1;
  });
  CHECK_FALSE(long_chunk.load());
  for (const int s : seen) CHECK(s == 1);
  bool called = false;
  parallel_for(pool, 0, 10, [&](std::size_t, std::size_t) { called = true; });
  CHECK_FALSE(called);
}

TEST_CASE("Nested calls run inline instead of deadlocking", "[eval][pool]") {
  ThreadPool pool(4);
  std::atomic<int> total{0};
  std::atomic<int> outside{0};
  pool.run(8, [&](std::size_t) {
    if (!pool.in_task()) outside.fetch_add(1);
    pool.run(10, [&](std::size_t) { total.fetch_add(1); });
  });
  CHECK(total.load() == 80);
  CHECK(outside.load() == 0);
  CHECK_FALSE(pool.in_task());
}

TEST_CASE("A throwing task is rethrown after the rest finish", "[eval][pool]") {
  ThreadPool pool(4);
  std::atomic<int> ran{0};
  CHECK_THROWS_AS(pool.run(100,
                           [&](std::size_t i) {
                             ran.fetch_add(1);
                             if (i == 7) throw std::runtime_error("seven");
                           }),
                  std::runtime_error);
  CHECK(ran.load() == 100);
  // The pool still works.
  std::atomic<int> after{0};
  pool.run(10, [&](std::size_t) { after.fetch_add(1); });
  CHECK(after.load() == 10);
}
