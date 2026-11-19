// M5 acceptance: a sample "bracket" project (layout, two sibling volumes, one
// bolted-flange connection) saves and loads with identical hashes.

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using bracket::codes;
namespace fs = std::filesystem;

namespace {

// A fresh scratch folder, removed at the end of the test.
struct Scratch {
  fs::path dir;
  Scratch() {
    dir = fs::temp_directory_path() / ("vs_model_test_" + Id::random().to_string());
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

void replace_once(std::string& s, const std::string& from, const std::string& to) {
  const auto at = s.find(from);
  REQUIRE(at != std::string::npos);
  s.replace(at, from.size(), to);
}

}  // namespace

TEST_CASE("The bracket saves and loads with identical hashes", "[model][project][acceptance]") {
  const auto b = bracket::make_bracket();
  const auto hash = content_hash(b.project);
  Scratch scratch;

  for (const auto layout : {ProjectLayout::Folder, ProjectLayout::Zip}) {
    const bool folder = layout == ProjectLayout::Folder;
    DYNAMIC_SECTION((folder ? "as a folder" : "as a zip")) {
      const fs::path path = scratch.dir / (folder ? "bracket.vsproj" : "bracket-zip.vsproj");
      const auto saved = save_project(b.project, path, layout);
      INFO(codes(saved));
      REQUIRE_FALSE(saved.has_errors());
      CHECK(fs::is_directory(path) == folder);

      auto loaded = load_project(path);
      INFO((loaded ? std::string() : loaded.error().to_string()));
      REQUIRE(loaded.ok());
      CHECK(content_hash(*loaded) == hash);
      CHECK(content_hash(loaded->document) == content_hash(b.project.document));
      CHECK(*loaded == b.project);

      // And again: saving what was loaded changes nothing.
      REQUIRE_FALSE(save_project(*loaded, path, layout).has_errors());
      auto again = load_project(path);
      REQUIRE(again.ok());
      CHECK(content_hash(*again) == hash);

      // The loaded project builds as the original does.
      const auto r = build(loaded->document, ProductSettings::defaults(), &loaded->blobs);
      INFO(codes(r.diagnostics));
      CHECK(r.ok());
      CHECK(r.find_volume(b.upright)->status == VolumeStatus::Current);
    }
  }
}

TEST_CASE("Hashes see every change", "[model][project]") {
  const auto b = bracket::make_bracket();
  const auto hash = content_hash(b.project);
  auto p = b.project;
  p.document.find_volume(b.base)->seed = 43;
  CHECK(content_hash(p) != hash);
  p = b.project;
  std::get<BoltedFlangeParams>(p.document.find_connection(b.flange)->params).thread = "M5";
  CHECK(content_hash(p) != hash);
  p = b.project;
  p.blobs.begin()->second.push_back(std::byte{0});
  CHECK(content_hash(p) != hash);
  CHECK(content_hash(b.project) == hash);
}

TEST_CASE("The document's JSON round-trips every kind of thing", "[model][project]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const auto diags = edit(doc, [&](Document& d) {
    // Procedural and placeholder volumes, a clearance and a bonded surface,
    // a box boundary, a keep-out, a datum patch with a bolt circle.
    const Id lo = d.layout.add(vs::feature::DatumPointFeature{vs::feature::PointAt{
                                   Param::of(100), Param::of(0), Param::of(0)}},
                               "Lo");
    const Id hi = d.layout.add(vs::feature::DatumPointFeature{vs::feature::PointAt{
                                   Param::of(120), Param::expr("width / 3"), Param::of(30)}},
                               "Hi");
    Volume lattice;
    lattice.id = Id::random();
    lattice.name = "Lattice";
    lattice.boundary = BoxBoundary{vs::feature::FeatureRef{lo}, vs::feature::FeatureRef{hi}};
    lattice.contents = ProceduralContents{R"({"nodes": [], "edges": []})"};
    lattice.seed = 0xfeedfacecafebeefull;
    lattice.parameters.push_back({Id::random(), "cell", Param::expr("width / 12")});
    lattice.outputs.push_back({Id::random(), "cog", OutputQuantity::CentreOfGravity, 1.0});
    lattice.outputs.push_back({Id::random(), "area", OutputQuantity::SurfaceArea, 1.0});
    d.volumes.push_back(lattice);

    Volume hole;
    hole.id = Id::random();
    hole.name = "Hole";
    hole.keep_out = true;
    hole.boundary = BoxBoundary{vs::feature::FeatureRef{lo}, vs::feature::FeatureRef{hi}};
    d.volumes.push_back(hole);
    d.keep_outs.push_back({Id::random(), "Hole cut", {hole.id, b.upright_block}, {lattice.id}});

    ConnectionSurface gap;
    gap.id = Id::random();
    gap.name = "Gap";
    gap.volumes = {lattice.id};
    gap.geometry = DatumPatch{vs::feature::FaceRef{b.base_block, "end"}, Param::of(1),
                              Param::expr("depth / 4"), Param::of(5), Param::of(6)};
    gap.params = ClearanceParams{Param::expr("0.1 mm * 3")};
    gap.seam.spacing = 0.75;
    d.connections.push_back(gap);

    ConnectionSurface glue;
    glue.id = Id::random();
    glue.name = "Glue";
    glue.volumes = {b.upright};
    DatumPatch zx;
    zx.plane = vs::feature::PrincipalPlane::ZX;
    glue.geometry = zx;
    glue.params = BondedParams{};
    d.connections.push_back(glue);

    BoltedFlangeParams circle;
    circle.holes.layout = BoltCircle{Param::of(6), Param::expr("width / 2"), Param::of(0.1)};
    circle.thread = "M2.5";
    d.find_connection(b.flange)->params = circle;
  });
  INFO(codes(diags));
  REQUIRE_FALSE(diags.has_errors());

  const std::string text = to_json(doc);
  auto back = document_from_json(text);
  INFO((back ? std::string() : back.error().to_string()));
  REQUIRE(back.ok());
  CHECK(*back == doc);
  CHECK(to_json(*back) == text);
  CHECK(content_hash(*back) == content_hash(doc));
}

TEST_CASE("Reading rejects what it can't trust", "[model][project]") {
  const auto b = bracket::make_bracket();
  Scratch scratch;
  const fs::path path = scratch.dir / "bracket.vsproj";
  REQUIRE_FALSE(save_project(b.project, path, ProjectLayout::Folder).has_errors());
  const fs::path manifest = path / "project.json";
  const std::string original = read_text(manifest);

  SECTION("a newer format version") {
    std::string text = original;
    replace_once(text, "\"version\": 1", "\"version\": 2");
    write_text(manifest, text);
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.project.version");
  }
  SECTION("a changed blob") {
    const fs::path blob = path / fs::path(b.project.blobs.begin()->first);
    std::string bytes = read_text(blob);
    bytes[bytes.size() / 2] ^= 1;
    write_text(blob, bytes);
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.project.blob_corrupt");
  }
  SECTION("a missing blob") {
    fs::remove(path / fs::path(b.project.blobs.begin()->first));
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.project.blob_missing");
  }
  SECTION("a document with a loop in it") {
    // Saving doesn't check; loading does. Point the link at the variable
    // the Base's boundary is sketched with.
    auto bad = b.project;
    bad.document.links[0].target = b.depth;
    REQUIRE_FALSE(save_project(bad, path, ProjectLayout::Folder).has_errors());
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.dependency.loop");
  }
  SECTION("a document that reaches into contents") {
    auto bad = b.project;
    bad.document.find_volume(b.upright)->boundary = FeatureBoundary{{b.inner_extrude}};
    const fs::path zip = scratch.dir / "bad.vsproj";
    REQUIRE_FALSE(save_project(bad, zip, ProjectLayout::Zip).has_errors());
    const auto r = load_project(zip);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.reference.contents");
  }
  SECTION("broken JSON") {
    write_text(manifest, original.substr(0, original.size() / 2));
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.json.syntax");
  }
  SECTION("not a project") {
    const fs::path empty = scratch.dir / "empty";
    fs::create_directories(empty);
    CHECK(load_project(empty).error().code == "model.project.not_project");
    write_text(scratch.dir / "junk.vsproj", "not a zip");
    CHECK(load_project(scratch.dir / "junk.vsproj").error().code == "model.project.not_project");
    CHECK(load_project(scratch.dir / "nothing.vsproj").error().code == "model.project.io");
  }
}

TEST_CASE("Saving a folder keeps caches and drops stale blobs", "[model][project]") {
  auto b = bracket::make_bracket();
  Scratch scratch;
  const fs::path path = scratch.dir / "bracket.vsproj";
  REQUIRE_FALSE(save_project(b.project, path, ProjectLayout::Folder).has_errors());
  fs::create_directories(path / "cache");
  write_text(path / "cache" / "anything.bin", "cached");

  // Replace the Upright's file.
  const std::string old_blob = b.project.blobs.begin()->first;
  b.project.blobs.clear();
  const std::string new_blob =
      b.project.add_blob(bracket::step_box(0, 0, 10, 60, 10, 49), "step");
  std::get<StaticContents>(b.project.document.find_volume(b.upright)->contents).blob = new_blob;
  REQUIRE_FALSE(save_project(b.project, path, ProjectLayout::Folder).has_errors());

  CHECK_FALSE(fs::exists(path / fs::path(old_blob)));
  CHECK(fs::exists(path / fs::path(new_blob)));
  CHECK(fs::exists(path / "cache" / "anything.bin"));
  auto loaded = load_project(path);
  REQUIRE(loaded.ok());
  CHECK(*loaded == b.project);
}

TEST_CASE("Blobs are named by their content", "[model][project]") {
  Project p;
  const Blob bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  const auto a = p.add_blob(bytes, "stl");
  const auto again = p.add_blob(bytes, "stl");
  CHECK(a == again);
  CHECK(p.blobs.size() == 1);
  CHECK(a.starts_with("blobs/"));
  CHECK(a.ends_with(".stl"));
  CHECK(a.size() == std::string("blobs/").size() + 32 + 4);
}

// The checked-in sample used by `vstudio-cli info`'s test. It is generated
// by the hidden test below and then kept as it is, so its ids stay put.
TEST_CASE("The checked-in sample bracket loads, checks and builds", "[model][project]") {
  const fs::path sample = fs::path(VS_MODEL_TEST_DATA) / "bracket.vsproj";
  auto p = load_project(sample);
  INFO((p ? std::string() : p.error().to_string()));
  REQUIRE(p.ok());
  CHECK(p->document.volumes.size() == 2);
  CHECK(p->document.connections.size() == 1);
  const auto r = build(p->document, ProductSettings::defaults(), &p->blobs);
  INFO(codes(r.diagnostics));
  CHECK(r.ok());
}

TEST_CASE("Write the sample bracket project", "[.generate]") {
  const auto b = bracket::make_bracket();
  const fs::path sample = fs::path(VS_MODEL_TEST_DATA) / "bracket.vsproj";
  std::error_code ec;
  fs::remove_all(sample, ec);
  REQUIRE_FALSE(save_project(b.project, sample, ProjectLayout::Folder).has_errors());
}
