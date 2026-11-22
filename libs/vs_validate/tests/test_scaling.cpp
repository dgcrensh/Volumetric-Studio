// Scaling of the layout half of an evaluation with the number of generated
// children (decision 0029): a Rack of n x n cells expanded by hand, as a
// generator would, then check(), build_layout() and validate_layout()
// timed on their own. Hidden ([.scaling]); run
//
//   vs_validate_tests "[.scaling]"            (up to 2,500 cells)
//   VS_SCALING_CELLS=10000 vs_validate_tests "[.scaling]"
//
// to print one line per size (VS_SCALING_THREADS=N runs validate_layout()
// on N threads; VS_SCALING_SKIP_VALIDATE=1 leaves the layout phases out).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rack.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/generated.hpp"
#include "vs/validate/layout.hpp"

using namespace vs::model;
using vs::core::dbox3;
using vs::core::Id;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A grid of n x n cells of 20 x 20 x 10 mm, bonded to their +x and +y
// neighbours, the Seat delegated to the cell at the origin: what
// children.grid makes.
GeneratedLayout grid(int n, const Id& seat) {
  GeneratedLayout g;
  const auto at = [n](int i, int j) { return static_cast<std::size_t>(j * n + i); };
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      GeneratedChild c;
      c.key = Id::derive(Id{}, "cell " + std::to_string(i) + " " + std::to_string(j));
      c.name = "Cell";
      c.template_name = "Cell";
      c.box = dbox3::from_min_max({20.0 * i, 20.0 * j, 0}, {20.0 * i + 20, 20.0 * j + 20, 10});
      g.children.push_back(std::move(c));
    }
  }
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      for (int axis = 0; axis < 2; ++axis) {
        const int ni = axis == 0 ? i + 1 : i;
        const int nj = axis == 0 ? j : j + 1;
        if (ni >= n || nj >= n) continue;
        GeneratedSurface s;
        s.key = Id::derive(Id{}, "join " + std::to_string(i) + " " + std::to_string(j) + " " +
                                     std::to_string(axis));
        s.name = "Join";
        s.a = at(i, j);
        s.b = at(ni, nj);
        s.axis = axis;
        s.offset = axis == 0 ? 20.0 * ni : 20.0 * nj;
        // x plane: y then z; y plane: z then x.
        if (axis == 0) {
          s.lo = {20.0 * j + 1, 1};
          s.hi = {20.0 * j + 19, 9};
        } else {
          s.lo = {1, 20.0 * i + 1};
          s.hi = {9, 20.0 * i + 19};
        }
        g.surfaces.push_back(s);
      }
    }
  }
  g.delegations = {{seat, 0}};
  return g;
}

// The Rack (and the Base under it) made n cells square.
rack::Rack sized_rack(int n) {
  rack::Rack r = rack::make_rack();
  auto& layout = r.project.document.layout;
  for (const auto& f : std::vector<vs::feature::Feature>(layout.features())) {
    if (f.name != "Base high" && f.name != "Rack high") continue;
    auto* p = layout.find(f.id)->as<vs::feature::DatumPointFeature>();
    auto& at = std::get<vs::feature::PointAt>(p->definition);
    at.x = vs::feature::Param::of(20.0 * n);
    at.y = vs::feature::Param::of(20.0 * n);
  }
  return r;
}

// A ParallelFor on `threads` plain threads, taking tasks in turn.
vs::validate::ParallelFor threads_for(unsigned threads) {
  return [threads](std::size_t n, const std::function<void(std::size_t)>& task) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
      pool.emplace_back([&] {
        for (std::size_t k = next++; k < n; k = next++) task(k);
      });
    }
    for (auto& t : pool) t.join();
  };
}

std::string report_text(const vs::validate::LayoutReport& r) {
  std::string out = std::to_string(r.pairs_tested) + "\n";
  for (const auto& d : r.diagnostics) {
    out += d.code + " | " + d.message;
    for (const auto& s : d.subjects) out += " | " + s.id.to_string() + " " + s.label;
    out += "\n";
  }
  for (const auto& o : r.overlaps) {
    out += o.a.to_string() + o.b.to_string() + std::to_string(o.volume) + "\n";
  }
  for (const auto& c : r.contacts) {
    out += c.a.to_string() + c.b.to_string() + c.face_a + c.face_b + std::to_string(c.area) +
           std::to_string(c.region.min.x) + std::to_string(c.region.max.y) + "\n";
  }
  return out;
}

}  // namespace

TEST_CASE("validate_layout reports the same on several threads", "[validate][scaling]") {
  const auto settings = vs::core::ProductSettings::defaults();
  // 7 x 7 cells: diagonal neighbours touch along edges (contact.edge), and
  // a cell pushed into its neighbour overlaps it.
  const int n = 7;
  const rack::Rack r = sized_rack(n);
  GeneratedLayout g = grid(n, r.seat);
  g.children[10].box.max.x += 5;  // into cell 11
  auto expanded = expand(r.project.document, r.rack, g);
  REQUIRE(expanded);
  const DocumentBuild b = build_layout(*expanded, settings);
  REQUIRE(b.ok());
  const auto serial = vs::validate::validate_layout(*expanded, b, settings);
  CHECK(serial.diagnostics.with_code("validate.contact.edge").size() > 60);  // 72 less 2 near the overlap
  CHECK_FALSE(serial.overlaps.empty());
  for (const unsigned threads : {2u, 5u}) {
    const auto parallel =
        vs::validate::validate_layout(*expanded, b, settings, threads_for(threads));
    CHECK(report_text(parallel) == report_text(serial));
  }
}

TEST_CASE("Layout phases scale with generated children", "[.scaling]") {
  const auto settings = vs::core::ProductSettings::defaults();
  int largest = 2500;
  if (const char* env = std::getenv("VS_SCALING_CELLS")) largest = std::atoi(env);
  std::vector<int> sides;
  for (int side : {10, 32, 50, 71, 100, 141, 200}) {
    if (side * side <= largest) sides.push_back(side);
  }
  const bool validate = std::getenv("VS_SCALING_SKIP_VALIDATE") == nullptr;
  unsigned threads = 0;
  if (const char* env = std::getenv("VS_SCALING_THREADS")) threads = static_cast<unsigned>(std::atoi(env));
  const vs::validate::ParallelFor parallel = threads > 1 ? threads_for(threads) : nullptr;
  std::printf("%8s %10s %10s %10s %10s %10s\n", "cells", "expand", "check", "graph",
              "layout", "validate");
  for (const int n : sides) {
    const rack::Rack r = sized_rack(n);
    auto t = std::chrono::steady_clock::now();
    auto expanded = expand(r.project.document, r.rack, grid(n, r.seat));
    REQUIRE(expanded);
    const double expand_s = seconds_since(t);

    t = std::chrono::steady_clock::now();
    const Diagnostics found = check(*expanded);
    const double check_s = seconds_since(t);
    for (const auto& d : found) {
      if (d.severity == vs::core::Severity::Error) UNSCOPED_INFO(d.code << ": " << d.message);
    }
    REQUIRE_FALSE(found.has_errors());

    t = std::chrono::steady_clock::now();
    const DependencyGraph graph = DependencyGraph::build(*expanded);
    (void)graph.order();
    const double graph_s = seconds_since(t);

    double layout_s = 0, validate_s = 0;
    if (validate) {
      t = std::chrono::steady_clock::now();
      const DocumentBuild b = build_layout(*expanded, settings);
      layout_s = seconds_since(t);
      REQUIRE(b.ok());
      t = std::chrono::steady_clock::now();
      const auto report = vs::validate::validate_layout(*expanded, b, settings, parallel);
      validate_s = seconds_since(t);
      CHECK_FALSE(report.diagnostics.has_errors());
    }
    std::printf("%8d %10.3f %10.3f %10.3f %10.3f %10.3f\n", n * n, expand_s, check_s, graph_s,
                layout_s, validate_s);
    std::fflush(stdout);
  }
}
