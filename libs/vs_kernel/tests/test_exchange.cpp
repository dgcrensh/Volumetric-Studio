#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/core/id.hpp"
#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using Catch::Approx;
using vs::core::ProductSettings;
namespace fs = std::filesystem;

namespace {
const ProductSettings kSettings = ProductSettings::defaults();

// A fresh directory under the system temp directory, removed afterwards.
struct TempDir {
  fs::path path = fs::temp_directory_path() / ("vs_kernel_" + vs::core::Id::random().to_string());
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}
}  // namespace

TEST_CASE("the demo part has its analytic volume", "[kernel][exchange]") {
  const auto part = make_demo_part(kSettings);
  REQUIRE(part.ok());
  CHECK(part->solid_count() == 1);
  CHECK(mass_properties(*part)->volume == Approx(demo_part_volume()).epsilon(1e-9));
}

TEST_CASE("STEP round trip preserves volume to 1e-6 relative", "[kernel][exchange]") {
  const TempDir dir;
  const fs::path file = dir.path / "demo part.step";  // a space, on purpose
  const auto part = make_demo_part(kSettings);
  REQUIRE(part.ok());
  const double before = mass_properties(*part)->volume;

  const Diagnostics written = write_step(*part, file);
  REQUIRE(written.empty());
  REQUIRE(fs::file_size(file) > 0);
  // AP242, in millimetres.
  const std::string text = read_text(file);
  CHECK(text.find("AP242") != std::string::npos);
  CHECK(text.find(".MILLI.") != std::string::npos);

  const auto read = read_step(file);
  REQUIRE(read.ok());
  const auto body = Solid::from(*read);
  REQUIRE(body.ok());
  CHECK(body->solid_count() == 1);
  CHECK(check_validity(*body).empty());
  const double after = mass_properties(*body)->volume;
  CHECK(std::abs(after - before) / before <= 1e-6);
}

TEST_CASE("IGES round trip keeps the surface area", "[kernel][exchange]") {
  const TempDir dir;
  const fs::path file = dir.path / "demo.igs";
  const auto part = make_demo_part(kSettings);
  REQUIRE(part.ok());

  REQUIRE(write_iges(*part, file).empty());
  const auto read = read_iges(file);
  REQUIRE(read.ok());
  CHECK(faces(*read).size() == faces(*part).size());
  CHECK(surface_area(*read) == Approx(surface_area(*part)).epsilon(1e-6));
}

TEST_CASE("exchange failures are diagnostics", "[kernel][exchange]") {
  const TempDir dir;
  CHECK(read_step(dir.path / "missing.step").error().code == "kernel.step.not_found");
  CHECK(read_iges(dir.path / "missing.igs").error().code == "kernel.iges.not_found");

  const fs::path junk = dir.path / "junk.step";
  std::ofstream(junk) << "this is not a STEP file\n";
  const auto bad = read_step(junk);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().code.rfind("kernel.step.", 0) == 0);

  CHECK(write_step(Shape{}, dir.path / "empty.step").with_code("kernel.step.empty_shape").size() ==
        1);
  CHECK(write_step(make_demo_part(kSettings).value(), dir.path / "no-such-dir" / "x.step")
            .with_code("kernel.step.write_failed")
            .size() == 1);
}

// --- named parts and mesh shapes (M17) ---------------------------------------------

namespace {
// A closed tetrahedron, wound counter-clockwise from outside.
struct Tetra {
  std::vector<vs::core::fvec3> positions{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
  std::vector<std::uint32_t> indices{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
};
}  // namespace

TEST_CASE("mesh shapes keep their triangles and reject bad input", "[kernel][exchange]") {
  const Tetra t;
  const auto shape = mesh_shape(t.positions, t.indices);
  REQUIRE(shape.ok());
  CHECK(shape->kind() == ShapeKind::Face);
  CHECK(stored_triangle_count(*shape) == 4);

  const std::vector<std::uint32_t> out_of_range{0, 1, 4};
  CHECK(mesh_shape(t.positions, out_of_range).error().code == "kernel.mesh.invalid");
  const std::vector<std::uint32_t> partial{0, 1};
  CHECK(mesh_shape(t.positions, partial).error().code == "kernel.mesh.invalid");
  std::vector<vs::core::fvec3> nan = t.positions;
  nan[1].x = std::nanf("");
  CHECK(mesh_shape(nan, t.indices).error().code == "kernel.mesh.invalid");
}

TEST_CASE("STEP parts keep their names, B-rep solids and mesh triangles", "[kernel][exchange]") {
  const TempDir dir;
  const fs::path file = dir.path / "parts.step";
  const auto box = make_box(vs::core::dbox3::from_min_max({0, 0, 0}, {20, 10, 5}), kSettings);
  REQUIRE(box.ok());
  const Tetra t;
  const auto tetra = mesh_shape(t.positions, t.indices);
  REQUIRE(tetra.ok());

  // A name beyond ASCII, on purpose.
  const std::vector<NamedShape> parts{{"Base", *box}, {"Infill \xC3\xBC", *tetra}};
  const Diagnostics written = write_step(parts, file, "Bracket");
  INFO((written.empty() ? std::string() : written.items().front().message));
  REQUIRE(written.empty());
  const std::string text = read_text(file);
  CHECK(text.find("AP242") != std::string::npos);
  // The mesh goes out as tessellated geometry, not as B-rep faces.
  CHECK(text.find("TESSELLATED_SHAPE_REPRESENTATION") != std::string::npos);

  const auto back = read_step_parts(file);
  REQUIRE(back.ok());
  REQUIRE(back->size() == 2);
  CHECK((*back)[0].name == "Base");
  CHECK((*back)[1].name == "Infill \xC3\xBC");
  const auto solid = Solid::from((*back)[0].shape);
  REQUIRE(solid.ok());
  CHECK(mass_properties(*solid)->volume == Approx(1000.0).epsilon(1e-9));
  CHECK(stored_triangle_count((*back)[1].shape) == 4);
}

TEST_CASE("writing no parts, or an empty one, is refused", "[kernel][exchange]") {
  const TempDir dir;
  const std::vector<NamedShape> none;
  CHECK(write_step(none, dir.path / "none.step", "A").items().front().code == "kernel.step.empty_shape");
  const std::vector<NamedShape> empty{{"Empty", Shape{}}};
  CHECK(write_step(empty, dir.path / "empty.step", "A").items().front().code == "kernel.step.empty_shape");
  CHECK(read_step_parts(dir.path / "missing.step").error().code == "kernel.step.not_found");
}
