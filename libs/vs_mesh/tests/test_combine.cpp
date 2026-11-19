#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "fixtures.hpp"
#include "vs/core/settings.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/mesh/import.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/containment.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::core::Id;
using vs::mesh::Bytes;
using vs::mesh::Mesh;
using namespace vs::model;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

std::string describe(const vs::core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.to_string() + "\n";
  return s;
}

bool has(const vs::core::Diagnostics& ds, const std::string& code) {
  for (const auto& d : ds) {
    if (d.code == code) return true;
  }
  return false;
}

Bytes bytes_of(const std::string& text) {
  Bytes out;
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return bytes_of(text);
}

void check_closed(const Mesh& m, std::size_t shells = 1) {
  const vs::mesh::TopologyReport t = vs::mesh::analyse(m);
  CHECK(t.watertight());
  CHECK(t.manifold());
  CHECK(t.shells == shells);
}

vs::mesh::CombinedMesh combined_bracket(const bracket::Bracket& b) {
  const DocumentBuild built = build(b.project.document, kSettings, &b.project.blobs);
  REQUIRE(built.ok());
  return vs::mesh::combine(b.project.document, built, &b.project.blobs, kSettings);
}

}  // namespace

TEST_CASE("The bracket welds into one watertight, manifold solid", "[mesh][combine]") {
  const bracket::Bracket b = bracket::make_bracket();
  const DocumentBuild built = build(b.project.document, kSettings, &b.project.blobs);
  REQUIRE(built.ok());
  const vs::mesh::CombinedMesh c =
      vs::mesh::combine(b.project.document, built, &b.project.blobs, kSettings);
  INFO(describe(c.diagnostics));
  REQUIRE(c.ok());
  REQUIRE(c.volumes.size() == 2);
  CHECK(c.volumes[0].volume == b.base);
  CHECK(c.volumes[1].volume == b.upright);
  for (const auto& v : c.volumes) {
    CHECK(v.seams == std::vector<Id>{b.flange});
    check_closed(v.mesh);
    // Each volume's mesh stays inside its boundary (vs_validate's check).
    const auto contained = vs::validate::check_containment(
        built.find_volume(v.volume)->boundary, vs::mesh::view(v.mesh),
        b.project.document.volume_subject(v.volume), kSettings);
    CHECK(contained.contained);
  }
  check_closed(c.weld.mesh);
  CHECK(c.weld.topology.euler_characteristic() == 2);
  // The flange's triangles and both bolt holes' fills went in pairs.
  const vs::mesh::Seam* seam = c.seams.find(b.flange);
  REQUIRE(seam != nullptr);
  REQUIRE(seam->hole_fills.size() == 2);
  CHECK(c.weld.internal_triangles == 2 * (seam->triangles.size() + seam->hole_fills[0].size() +
                                          seam->hole_fills[1].size()));
  CHECK_THAT(vs::mesh::enclosed_volume(c.weld.mesh), WithinRel(60.0 * 40 * 10 + 60.0 * 10 * 40, 1e-6));
  CHECK(c.weld.mesh.volumes() == std::vector<Id>{b.base, b.upright});
}

TEST_CASE("The bracket's 3MF reads back as the same watertight solid", "[mesh][io][3mf]") {
  const bracket::Bracket b = bracket::make_bracket();
  const vs::mesh::CombinedMesh c = combined_bracket(b);
  REQUIRE(c.ok());

  SECTION("one object for the whole design") {
    const vs::mesh::ThreeMfObject whole[] = {{"Bracket", Id::null(), &c.weld.mesh}};
    const auto file = vs::mesh::write_3mf(whole, "Bracket");
    REQUIRE(file.ok());
    const auto back = vs::mesh::read_3mf(*file);
    REQUIRE(back.ok());
    CHECK(back->triangle_count() == c.weld.mesh.triangle_count());
    CHECK(back->vertex_count() == c.weld.mesh.vertex_count());
    CHECK(back->positions == c.weld.mesh.positions);  // shortest round-trip decimals
    check_closed(*back);
    // Writing is deterministic.
    CHECK(*vs::mesh::write_3mf(whole, "Bracket") == *file);
  }
  SECTION("one object per volume, with names and ids") {
    std::vector<vs::mesh::ThreeMfObject> objects;
    for (const auto& v : c.volumes) objects.push_back({v.name, v.volume, &v.mesh});
    const auto file = vs::mesh::write_3mf(objects, "Bracket");
    REQUIRE(file.ok());
    const auto back = vs::mesh::read_3mf(*file);
    REQUIRE(back.ok());
    CHECK(back->triangle_count() == c.volumes[0].mesh.triangle_count() + c.volumes[1].mesh.triangle_count());
    check_closed(*back, 2);
  }
}

TEST_CASE("STL out and in", "[mesh][io][stl]") {
  const bracket::Bracket b = bracket::make_bracket();
  const vs::mesh::CombinedMesh c = combined_bracket(b);
  REQUIRE(c.ok());
  const auto file = vs::mesh::write_stl(c.weld.mesh);
  REQUIRE(file.ok());
  CHECK(file->size() == 84 + 50 * c.weld.mesh.triangle_count());
  const auto back = vs::mesh::read_stl(*file);
  REQUIRE(back.ok());
  CHECK(back->triangle_count() == c.weld.mesh.triangle_count());
  CHECK(back->vertex_count() == c.weld.mesh.vertex_count());
  check_closed(*back);
  CHECK_THAT(vs::mesh::enclosed_volume(*back), WithinRel(vs::mesh::enclosed_volume(c.weld.mesh), 1e-12));

  CHECK(vs::mesh::write_stl(Mesh{}).error().code == "mesh.io.empty");
  CHECK(vs::mesh::read_stl(bytes_of("not a mesh at all")).error().code == "mesh.io.stl");
  CHECK(vs::mesh::read_stl(bytes_of("solid x\n facet normal 0 0 1\n outer loop\n vertex 0 0 nan\n"))
            .error()
            .code == "mesh.io.stl");
}

TEST_CASE("OBJ in: polygons, slashes and negative indices", "[mesh][io][obj]") {
  // A unit cube: quads, one written with relative indices.
  const std::string obj =
      "# cube\no cube\n"
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
      "vn 0 0 1\nvt 0 0\n"
      "f 1 4 3 2\nf 5/1 6/1 7/1 8/1\nf 1//1 2//1 6//1 5//1\nf 2/1/1 3/1/1 7/1/1 6/1/1\n"
      "f -5 -1 -2 -6\nf 4 1 5 8\n";
  const auto m = vs::mesh::read_obj(bytes_of(obj));
  REQUIRE(m.ok());
  CHECK(m->triangle_count() == 12);
  check_closed(*m);
  CHECK_THAT(vs::mesh::enclosed_volume(*m), WithinRel(1.0, 1e-12));
  CHECK(vs::mesh::read_obj(bytes_of("v 0 0 0\nf 1 2 3\n")).error().code == "mesh.io.obj");
  CHECK(vs::mesh::read_mesh(bytes_of(obj), "OBJ").ok());
  CHECK(vs::mesh::read_mesh(bytes_of(obj), "ply").error().code == "mesh.io.format");
}

TEST_CASE("Importing the sample STL makes a static volume", "[mesh][import]") {
  Project project;
  project.document = make_document("Imported");
  const Bytes file = read_file(std::filesystem::path(VS_MESH_TEST_DATA) / "hex_prism.stl");
  const auto id = vs::mesh::import_mesh_volume(project, file, "stl", "Hex", kSettings);
  REQUIRE(id.ok());
  const Volume* v = project.document.find_volume(*id);
  REQUIRE(v != nullptr);
  CHECK(v->name == "Hex");
  REQUIRE(std::holds_alternative<StaticContents>(v->contents));
  CHECK(std::get<StaticContents>(v->contents).format == "stl");
  CHECK(project.blobs.size() == 1);

  // A hexagonal prism of radius 10 round (5, -3), from z 2 to 14.5.
  const double h = 10 * std::sqrt(3.0) / 2;
  const DocumentBuild built = build(project.document, kSettings, &project.blobs);
  INFO(describe(built.diagnostics));
  REQUIRE(built.ok());
  const auto boundary = vs::kernel::bounding_box(built.find_volume(*id)->boundary);
  CHECK_THAT(boundary.min.x, WithinAbs(-5, 1e-5));
  CHECK_THAT(boundary.min.y, WithinAbs(-3 - h, 1e-5));
  CHECK_THAT(boundary.min.z, WithinAbs(2, 1e-5));
  CHECK_THAT(boundary.max.x, WithinAbs(15, 1e-5));
  CHECK_THAT(boundary.max.y, WithinAbs(-3 + h, 1e-5));
  CHECK_THAT(boundary.max.z, WithinAbs(14.5, 1e-5));

  const vs::mesh::CombinedMesh c = vs::mesh::combine(project.document, built, &project.blobs, kSettings);
  INFO(describe(c.diagnostics));
  REQUIRE(c.ok());
  REQUIRE(c.volumes.size() == 1);
  const Mesh& m = c.volumes[0].mesh;
  CHECK(m.triangle_count() == 20);
  CHECK(m.vertex_count() == 12);
  const auto box = m.bounds();
  CHECK_THAT(box.min.x, WithinAbs(-5, 1e-5));
  CHECK_THAT(box.min.y, WithinAbs(-3 - h, 1e-5));
  CHECK_THAT(box.max.z, WithinAbs(14.5, 1e-5));
  check_closed(c.weld.mesh);
  CHECK_THAT(vs::mesh::enclosed_volume(m), WithinRel(1.5 * std::sqrt(3.0) * 100 * 12.5, 1e-6));

  // Saved and loaded, the project still has it.
  const auto dir = std::filesystem::temp_directory_path() / ("vs_mesh_import_" + Id::random().to_string());
  REQUIRE_FALSE(save_project(project, dir, ProjectLayout::Folder).has_errors());
  const auto loaded = load_project(dir);
  std::filesystem::remove_all(dir);
  REQUIRE(loaded.ok());
  CHECK(content_hash(*loaded) == content_hash(project));

  // Files that aren't meshes, or are flat, are refused and change nothing.
  const Project before = project;
  CHECK(vs::mesh::import_mesh_volume(project, bytes_of("garbage"), "stl", "Bad", kSettings)
            .error()
            .code == "mesh.io.stl");
  CHECK(vs::mesh::import_mesh_volume(project, bytes_of("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"),
                                     "obj", "Flat", kSettings)
            .error()
            .code == "mesh.import.flat");
  CHECK(project == before);
}

TEST_CASE("Placeholders are left out, or meshed as their boundaries", "[mesh][combine]") {
  Document doc = make_document("Stack");
  const Id base = fixtures::add_box(doc, "Base", {0, 0, 0}, {60, 40, 10});
  const Id top = fixtures::add_box(doc, "Top", {10, 10, 10}, {50, 30, 30});
  fixtures::add_patch(doc, "Bond", {base, top}, 10, 30, 20, 40, 20, BondedParams{});
  const DocumentBuild built = build(doc, kSettings);
  REQUIRE(built.ok());

  const vs::mesh::CombinedMesh none = vs::mesh::combine(doc, built, nullptr, kSettings);
  CHECK(has(none.diagnostics, "mesh.combine.skipped"));
  CHECK(has(none.diagnostics, "mesh.combine.empty"));
  CHECK_FALSE(none.ok());

  vs::mesh::CombineOptions options;
  options.placeholders = true;
  const vs::mesh::CombinedMesh c = vs::mesh::combine(doc, built, nullptr, kSettings, options);
  INFO(describe(c.diagnostics));
  CHECK(c.ok());
  check_closed(c.weld.mesh);
  CHECK_THAT(vs::mesh::enclosed_volume(c.weld.mesh), WithinRel(60.0 * 40 * 10 + 40.0 * 20 * 20, 1e-6));
}

TEST_CASE("A mesh volume can't follow a seam: the weld names the surface", "[mesh][combine]") {
  Project project;
  project.document = make_document("Mixed");
  Document& doc = project.document;
  const Id base = fixtures::add_box(doc, "Base", {-10, -20, -5}, {20, 10, 2});
  const Bytes file = read_file(std::filesystem::path(VS_MESH_TEST_DATA) / "hex_prism.stl");
  const auto hex = vs::mesh::import_mesh_volume(project, file, "stl", "Hex", kSettings);
  REQUIRE(hex.ok());
  const Id bond = fixtures::add_patch(doc, "Bond", {base, *hex}, 2, 5, -3, 8, 8, BondedParams{});
  const DocumentBuild built = build(doc, kSettings, &project.blobs);
  REQUIRE(built.ok());
  vs::mesh::CombineOptions options;
  options.placeholders = true;
  const vs::mesh::CombinedMesh c = vs::mesh::combine(doc, built, &project.blobs, kSettings, options);
  bool named = false;
  for (const auto& d : c.diagnostics) {
    if (d.code == "mesh.weld.nonconforming" && !d.subjects.empty() && d.subjects[0].id == bond) {
      named = d.subjects.size() > 1 && d.subjects[1].id == *hex;
    }
  }
  CHECK(named);
}
