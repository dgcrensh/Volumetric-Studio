#pragma once

// Published outputs measured on a mesh (static STL, 3MF or OBJ contents),
// as model::build() measures them on a solid. Sums run through the pool's
// ordered reduction, so they don't depend on the thread count.

#include <vector>

#include "vs/eval/thread_pool.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::eval::detail {

std::vector<model::OutputResult> measure_mesh(const model::Volume& volume, const mesh::Mesh& mesh,
                                              ThreadPool& pool);

}  // namespace vs::eval::detail
