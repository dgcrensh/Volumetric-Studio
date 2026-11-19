#pragma once

// Published outputs measured on a mesh (static STL, 3MF or OBJ contents,
// and generated meshes: vs_proc measures its own, M12), as model::build()
// measures them on a solid. Sums run through the pool's ordered
// reduction, so they don't depend on the thread count. A closed mesh
// wound counter-clockwise from outside gives its volume, mass, area,
// centre of gravity and envelope; Published outputs are left without a
// value (only a procedure knows them).

#include <vector>

#include "vs/eval/thread_pool.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::eval {

std::vector<model::OutputResult> measure_mesh(const model::Volume& volume, const mesh::Mesh& mesh,
                                              ThreadPool& pool);

}  // namespace vs::eval
