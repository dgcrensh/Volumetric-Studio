#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "fixtures.hpp"
#include "meshers.hpp"
#include "vs/core/settings.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/weld.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/containment.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::core::dvec3;
using vs::core::Id;
using namespace vs::model;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

// The area of a regular n-gon inscribed in a circle of radius r.
double ngon_area(int n, double r) {
  return 0.5 * n * r * r * std::sin(2 * std::numbers::pi / n);
}

}  // namespace

TEST_CASE("The bracket's bolted flange gets a seam with its bolt holes", "[mesh][seam_library]") {
  const bracket::Bracket b = bracket::make_bracket();
  const Document& doc = b.project.document;
  const DocumentBuild built = build(doc, kSettings, &b.project.blobs);
  REQUIRE(built.find_connection(b.flange)->ok);

  const vs::mesh::SeamLibrary lib = vs::mesh::build_seams(doc, built, kSettings);
  CHECK(lib.diagnostics.empty());
  REQUIRE(lib.seams.size() == 1);
  const vs::mesh::Seam& seam = lib.seams[0];
  CHECK(lib.find(b.flange) == &seam);
  CHECK(seam.name == "Flange");
  CHECK(seam.volumes == doc.find_connection(b.flange)->volumes);
  // Spacing derived from the 60 x 10 outline.
  CHECK_THAT(seam.spacing, WithinRel(std::sqrt(60.0 * 60 + 10 * 10) / 16, 1e-9));
  // Outer rectangle plus two M4 clearance holes (ISO 273 medium: 4.5 mm).
  REQUIRE(seam.loops.size() == 3);
  CHECK(seam.outline.loops[0].segments.size() == 4);
  for (int h = 1; h <= 2; ++h) {
    const auto& seg = seam.outline.loops[static_cast<std::size_t>(h)].segments;
    REQUIRE(seg.size() == 1);
    CHECK(seg[0].kind == vs::mesh::OutlineSegment::Kind::Arc);
    CHECK(seg[0].radius == 2.25);
    CHECK(seam.loops[static_cast<std::size_t>(h)].vertices.size() == 16);
  }
  CHECK_THAT(seam.area(), WithinRel(600.0 - 2 * ngon_area(16, 2.25), 1e-9));
  // Every vertex lies on the flange plane, z = base_h = 10, inside the patch.
  for (const auto& v : seam.vertices) {
    CHECK_THAT(v.position.z, WithinAbs(10.0, 1e-12));
    CHECK(v.position.x >= -1e-9);
    CHECK(v.position.x <= 60 + 1e-9);
    CHECK(v.position.y >= -1e-9);
    CHECK(v.position.y <= 10 + 1e-9);
    const auto ref = lib.find_vertex(v.id);
    REQUIRE(ref.has_value());
    CHECK(ref->seam == 0);
  }
  // Built twice, the seam is the same.
  const vs::mesh::SeamLibrary again = vs::mesh::build_seams(doc, build(doc, kSettings), kSettings);
  REQUIRE(again.seams.size() == 1);
  CHECK(again.seams[0].key == seam.key);
  CHECK(again.seams[0].triangles == seam.triangles);
}

TEST_CASE("A surface's spacing overrides the derived one", "[mesh][seam_library]") {
  bracket::Bracket b = bracket::make_bracket();
  Document& doc = b.project.document;
  doc.find_connection(b.flange)->seam.spacing = 1.5;
  const vs::mesh::SeamLibrary lib = vs::mesh::build_seams(doc, build(doc, kSettings), kSettings);
  REQUIRE(lib.seams.size() == 1);
  CHECK(lib.seams[0].spacing == 1.5);
  CHECK(lib.seams[0].loops[0].segment_starts == std::vector<std::uint32_t>{0, 40, 47, 87});

  doc.find_connection(b.flange)->seam.spacing = 1e-5;
  const vs::mesh::SeamLibrary bad = vs::mesh::build_seams(doc, build(doc, kSettings), kSettings);
  CHECK(bad.seams.empty());
  REQUIRE(bad.diagnostics.size() == 1);
  CHECK(bad.diagnostics[0].code == "mesh.seam.spacing");
  CHECK(bad.diagnostics[0].target()->id == b.flange);
}

TEST_CASE("Clearance surfaces get no seam; one-sided ones do", "[mesh][seam_library]") {
  Document doc = make_document("Seams");
  const Id lower = fixtures::add_box(doc, "Lower", {0, 0, 0}, {60, 40, 10});
  const Id upper = fixtures::add_box(doc, "Upper", {0, 0, 10.5}, {60, 40, 20});
  const Id gap = fixtures::add_patch(doc, "Gap", {lower, upper}, 10, 30, 20, 60, 40,
                                     ClearanceParams{Param::of(0.5)});
  const Id outside = fixtures::add_patch(doc, "Mount", {lower}, 0, 30, 20, 20, 20, BondedParams{});
  const DocumentBuild built = build(doc, kSettings);
  REQUIRE(built.ok());
  const vs::mesh::SeamLibrary lib = vs::mesh::build_seams(doc, built, kSettings);
  CHECK(lib.diagnostics.empty());
  CHECK(lib.find(gap) == nullptr);
  REQUIRE(lib.find(outside) != nullptr);
  CHECK(lib.find(outside)->volumes == std::vector<Id>{lower});
  CHECK(lib.volume_subject(lower).label == "Lower");
}

TEST_CASE("Two volumes of a document meshed against its seam weld into one solid",
          "[mesh][seam_library][weld]") {
  // A 60 x 40 x 10 block with a 40 x 20 x 20 block centred on it, bonded
  // over the smaller one's footprint.
  Document doc = make_document("Stack");
  const Id base = fixtures::add_box(doc, "Base", {0, 0, 0}, {60, 40, 10});
  const Id top = fixtures::add_box(doc, "Top", {10, 10, 10}, {50, 30, 30});
  const Id bond = fixtures::add_patch(doc, "Bond", {base, top}, 10, 30, 20, 40, 20, BondedParams{});
  const DocumentBuild built = build(doc, kSettings);
  REQUIRE(built.ok());
  const vs::mesh::SeamLibrary lib = vs::mesh::build_seams(doc, built, kSettings);
  REQUIRE(lib.diagnostics.empty());
  const vs::mesh::Seam* seam = lib.find(bond);
  REQUIRE(seam != nullptr);
  REQUIRE(std::abs(seam->normal().z) == 1.0);

  // The base sits on the side the normal points away from; the top on the other.
  const bool up = seam->normal().z > 0;
  REQUIRE(up);
  const vs::mesh::Mesh base_mesh = meshers::box_under(*seam, 10.0, 10.0, base);
  const vs::mesh::SeamOutline lid = [&] {
    vs::mesh::SeamOutline o = seam->outline;
    o.frame.origin = o.frame.origin + dvec3{0, 0, 20};
    return o;
  }();
  const vs::mesh::Mesh top_mesh = meshers::prism({seam}, {nullptr, &lid, 7.0}, top);

  // Each volume's mesh stays inside its boundary (vs_validate's check).
  const auto subject = doc.volume_subject(base);
  CHECK(vs::validate::check_containment(built.find_volume(base)->boundary,
                                        vs::mesh::view(base_mesh), subject, kSettings)
            .contained);
  CHECK(vs::validate::check_containment(built.find_volume(top)->boundary,
                                        vs::mesh::view(top_mesh), doc.volume_subject(top),
                                        kSettings)
            .contained);

  const vs::mesh::Mesh both[] = {base_mesh, top_mesh};
  const vs::mesh::WeldResult r = vs::mesh::weld(both, lib);
  CHECK(r.ok());
  CHECK(r.topology.watertight());
  CHECK(r.topology.manifold());
  CHECK(r.topology.shells == 1);
  CHECK_THAT(vs::mesh::enclosed_volume(r.mesh), WithinRel(60.0 * 40 * 10 + 40.0 * 20 * 20, 1e-6));
}
