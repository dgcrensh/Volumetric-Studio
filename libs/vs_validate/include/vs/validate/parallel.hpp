#pragma once

#include <cstddef>
#include <functional>

namespace vs::validate {

// Runs task(0) .. task(n - 1), in any order, on any threads (vs_eval's
// ThreadPool::run, say; vs_validate sits below it). Checks that take one
// split their work into fixed chunks and fold the results in chunk order,
// so what they report never depends on the threads; empty runs the chunks
// on the calling thread.
using ParallelFor = std::function<void(std::size_t n, const std::function<void(std::size_t)>& task)>;

}  // namespace vs::validate
