// build_layout() with a ParallelFor (decision 0033) builds exactly what it
// builds on the calling thread: same boundaries (by shape hash), named
// faces, surfaces, parameters and diagnostics, in the same order, on the
// shared fixtures, a generated grid and documents with failures.

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "bracket.hpp"
#include "curved.hpp"
#include "nested.hpp"
#include "rack.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/model/build.hpp"
#include "vs/model/generated.hpp"

using namespace vs::model;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::Id;

namespace {

const auto kSettings = ProductSettings::defaults();

std::string hash_of(const vs::kernel::Shape& s) {
  return s.is_empty() ? "empty" : vs::kernel::shape_hash(s).to_hex();
}

std::string text(const vs::core::Diagnostics& ds) {
  std::string out;
  for (const auto& d : ds) {
    out += d.code + " | " + d.message;
    for (const auto& s : d.subjects) out += " | " + s.id.to_string() + " " + s.label;
    out += "\n";
  }
  return out;
}

std::string text(const dvec3& p) { return fmt::format("({} {} {})", p.x, p.y, p.z); }

// Everything build_layout() makes, as text; numbers in full.
std::string describe(const DocumentBuild& b) {
  std::string out = "layout ok " + std::to_string(b.layout.ok()) + "\n";
  for (const auto& v : b.volumes) {
    out += fmt::format("volume {} status {} boundary {}\n", v.volume.to_string(),
                       to_string(v.status), hash_of(v.boundary));
    for (const auto& f : v.boundary_faces) {
      out += "  face " + f.name + " " + text(f.plane.origin) + " " + text(f.plane.normal);
      if (f.cylinder) out += fmt::format(" r {}", f.cylinder->radius);
      out += "\n";
    }
    for (const auto& [name, value] : v.parameters) out += fmt::format("  {} = {}\n", name, value);
    out += text(v.diagnostics);
  }
  for (const auto& c : b.connections) {
    out += fmt::format("surface {} ok {} patch {} area {} frame {} {} {}\n", c.surface.to_string(),
                       c.ok, hash_of(c.patch), c.area, text(c.frame.origin), text(c.frame.x_axis),
                       text(c.frame.y_axis));
    for (const auto& h : c.hole_centres) out += "  hole " + text(h) + "\n";
    if (c.gap) out += fmt::format("  gap {}\n", *c.gap);
    if (c.min_wall) out += fmt::format("  wall {}\n", *c.min_wall);
    out += text(c.diagnostics);
  }
  return out + text(b.diagnostics);
}

// Tasks on four threads at once, each taking every fourth index.
void four_threads(std::size_t n, const std::function<void(std::size_t)>& task) {
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (std::size_t i = t; i < n; i += 4) task(i);
    });
  }
  for (auto& t : threads) t.join();
}

// Tasks one after another, last first.
void backwards(std::size_t n, const std::function<void(std::size_t)>& task) {
  for (std::size_t i = n; i-- > 0;) task(i);
}

void same_both_ways(const Document& doc) {
  const std::string serial = describe(build_layout(doc, kSettings));
  CHECK(describe(build_layout(doc, kSettings, four_threads)) == serial);
  CHECK(describe(build_layout(doc, kSettings, backwards)) == serial);
}

// The rack with an nx x ny grid of cells written in, neighbours bonded.
Document rack_grid(int nx, int ny) {
  const rack::Rack r = rack::make_rack();
  GeneratedLayout g;
  const double w = 60.0 / nx;
  const double h = 40.0 / ny;
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      GeneratedChild c;
      c.key = Id::derive(Id{}, fmt::format("{} {}", i, j));
      c.name = fmt::format("Cell {} {}", i, j);
      c.template_name = "Cell";
      c.box = dbox3::from_min_max({i * w, j * h, 0}, {(i + 1) * w, (j + 1) * h, 10});
      g.children.push_back(c);
    }
  }
  const auto at = [&](int i, int j) { return static_cast<std::size_t>(j * nx + i); };
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      if (i + 1 < nx) {
        GeneratedSurface s;
        s.key = Id::derive(Id{}, fmt::format("x {} {}", i, j));
        s.name = "x";
        s.a = at(i, j);
        s.b = at(i + 1, j);
        s.axis = 0;
        s.offset = (i + 1) * w;
        s.lo = {j * h + 1, 1};
        s.hi = {(j + 1) * h - 1, 9};
        g.surfaces.push_back(s);
      }
      if (j + 1 < ny) {
        GeneratedSurface s;
        s.key = Id::derive(Id{}, fmt::format("y {} {}", i, j));
        s.name = "y";
        s.a = at(i, j);
        s.b = at(i, j + 1);
        s.axis = 1;
        s.offset = (j + 1) * h;
        s.lo = {1, i * w + 1};
        s.hi = {9, (i + 1) * w - 1};
        g.surfaces.push_back(s);
      }
    }
  }
  g.delegations = {{r.seat, 0}};
  auto e = expand(r.project.document, r.rack, g);
  REQUIRE(e);
  return *e;
}

}  // namespace

TEST_CASE("a layout built in parallel is the one built on the calling thread", "[build][parallel]") {
  SECTION("bracket") { same_both_ways(bracket::make_bracket().project.document); }
  SECTION("curved") { same_both_ways(curved::make_curved().project.document); }
  SECTION("nested") { same_both_ways(nested::make_nested().project.document); }
  SECTION("generated grid") {
    const Document doc = rack_grid(6, 4);
    const DocumentBuild b = build_layout(doc, kSettings, four_threads);
    CHECK(b.ok());
    CHECK(b.volumes.size() == 2 + 24);
    same_both_ways(doc);
  }
  SECTION("failures") {
    // A cell whose corners no longer exist, and a surface that is too small.
    Document doc = rack_grid(3, 2);
    Volume& cell = doc.volumes.back();
    std::get<BoxBoundary>(cell.boundary).max = vs::feature::FeatureRef{Id::random()};
    auto& patch = std::get<DatumPatch>(doc.connections.back().geometry);
    patch.width = vs::feature::Param::of(1e-6);
    const DocumentBuild b = build_layout(doc, kSettings);
    CHECK(b.diagnostics.has_errors());
    same_both_ways(doc);
  }
}
