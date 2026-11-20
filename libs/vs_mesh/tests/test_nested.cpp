// L3: seams follow delegations, so leaves at different depths weld.

#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nested.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/mesh/weld.hpp"
#include "vs/model/build.hpp"

using Catch::Matchers::WithinRel;
using vs::core::Id;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

std::string describe(const vs::core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.code + ": " + d.message + "\n";
  return s;
}

}  // namespace

TEST_CASE("Seams pair the volumes that actually meet them", "[mesh][seam][nested]") {
  const auto n = nested::make_nested();
  const auto& doc = n.project.document;
  const auto b = vs::model::build_layout(doc, kSettings);
  REQUIRE(b.ok());
  const auto lib = vs::mesh::build_seams(doc, b, kSettings);
  INFO(describe(lib.diagnostics));
  REQUIRE(lib.seams.size() == 3);
  for (const auto& s : lib.seams) {
    if (s.surface == n.seat) CHECK(s.volumes == std::vector<Id>{n.base, n.core});
    if (s.surface == n.middle) CHECK(s.volumes == std::vector<Id>{n.core, n.right});
    if (s.surface == n.joint) CHECK(s.volumes == std::vector<Id>{n.core, n.cap});
  }
  // A leaf's own library has every surface it meets, delegated or not.
  CHECK(vs::mesh::build_seams(doc, b, kSettings, n.core).seams.size() == 3);
  CHECK(vs::mesh::build_seams(doc, b, kSettings, n.cap).seams.size() == 1);
  CHECK(vs::mesh::build_seams(doc, b, kSettings, n.housing).seams.size() == 1);
}

TEST_CASE("Leaves at every depth weld into one closed mesh", "[mesh][combine][nested]") {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  const auto& doc = n.project.document;
  const auto b = vs::model::build(doc, kSettings);
  INFO(describe(b.diagnostics));
  REQUIRE(b.ok());
  const auto m = vs::mesh::combine(doc, b, &n.project.blobs, kSettings);
  INFO(describe(m.diagnostics));
  CHECK(m.ok());
  // The four leaves; composites have no mesh of their own.
  REQUIRE(m.volumes.size() == 4);
  for (const auto& vt : m.volumes) {
    CHECK_FALSE(vt.volume == n.housing);
    CHECK_FALSE(vt.volume == n.left);
  }
  CHECK(m.weld.topology.watertight());
  CHECK(m.weld.topology.manifold());
  CHECK(m.weld.topology.shells == 1);
  CHECK_THAT(vs::mesh::enclosed_volume(m.weld.mesh), WithinRel(40.0 * 20 * 30, 1e-9));
}

TEST_CASE("A surface a composite doesn't pass on reaches none of its children",
          "[mesh][combine][nested]") {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  auto& left = std::get<vs::model::CompositeContents>(
      n.project.document.find_volume(n.left)->contents);
  left.delegations.erase(left.delegations.begin());  // Seat stops at Left
  const auto& doc = n.project.document;
  const auto b = vs::model::build(doc, kSettings);
  REQUIRE(b.ok());
  const auto m = vs::mesh::combine(doc, b, &n.project.blobs, kSettings);
  INFO(describe(m.diagnostics));
  for (const auto& s : m.seams.seams) {
    if (s.surface == n.seat) CHECK(s.volumes == std::vector<Id>{n.base, n.left});
  }
  // Base keeps its side of the seam and Core its own face: two shells that
  // touch, as with no surface at all (check() warns model.composite.undelegated).
  CHECK(m.ok());
  CHECK(m.weld.topology.watertight());
  CHECK(m.weld.topology.shells == 2);
}
