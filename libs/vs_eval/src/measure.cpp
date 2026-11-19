#include "vs/eval/measure.hpp"

namespace vs::eval {

using core::dvec3;

namespace {

// Triangles per chunk of the reductions: fixed, so sums don't depend on
// the thread count.
constexpr std::size_t kChunk = 4096;

struct Sums {
  double volume = 0;  // 6x the signed volume
  double area = 0;    // 2x the area
  dvec3 moment{0, 0, 0};  // 24x the first moment of volume
};

Sums operator+(const Sums& a, const Sums& b) {
  return {a.volume + b.volume, a.area + b.area, a.moment + b.moment};
}

}  // namespace

std::vector<model::OutputResult> measure_mesh(const model::Volume& volume, const mesh::Mesh& mesh,
                                              ThreadPool& pool) {
  const Sums sums = parallel_reduce(
      pool, mesh.triangle_count(), kChunk, Sums{},
      [&](std::size_t begin, std::size_t end) {
        Sums s;
        for (std::size_t t = begin; t < end; ++t) {
          const auto tri = mesh.triangle(t);
          const dvec3 a = core::to_fp64(mesh.positions[tri[0]]);
          const dvec3 b = core::to_fp64(mesh.positions[tri[1]]);
          const dvec3 c = core::to_fp64(mesh.positions[tri[2]]);
          const double v6 = dot(a, cross(b, c));  // 6x the tetrahedron to the origin
          s.volume += v6;
          s.area += length(cross(b - a, c - a));
          s.moment = s.moment + (a + b + c) * v6;  // centroid (a+b+c)/4, times 6v, times 4
        }
        return s;
      },
      [](Sums a, Sums b) { return a + b; });

  const double vol = sums.volume / 6.0;
  const double area = sums.area / 2.0;
  const auto box = mesh.bounds();
  std::vector<model::OutputResult> out;
  for (const auto& o : volume.outputs) {
    model::OutputResult r{o.id, std::nullopt};
    if (!mesh.empty()) {
      switch (o.quantity) {
        case model::OutputQuantity::Volume: r.value = vol; break;
        case model::OutputQuantity::Mass: r.value = vol * 1e-3 * o.density; break;
        case model::OutputQuantity::SurfaceArea: r.value = area; break;
        case model::OutputQuantity::CentreOfGravity:
          if (vol != 0.0) r.value = sums.moment * (1.0 / (4.0 * sums.volume));
          break;
        case model::OutputQuantity::Envelope:
          r.value = core::dbox3::from_min_max(core::to_fp64(box.min), core::to_fp64(box.max));
          break;
        case model::OutputQuantity::Published:
          break;  // the procedure's own numbers (vs_proc)

      }
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace vs::eval
