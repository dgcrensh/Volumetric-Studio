// M17 acceptance: the spec's five workflows, end to end, through the
// document API (spec, "Core workflows"; plan, M17).
//
//   1. Block out  layout variables, two box boundaries joined by a bonded
//                 connection surface, contents left as placeholders.
//   2. Fill       Body modelled (a feature tree inside), Infill from the
//                 gyroid template, and two imports (a STEP block and an
//                 STL puck) as static volumes.
//   3. Iterate    a layout dimension changes: the preview shows every
//                 boundary at once, then only the volumes it touches
//                 regenerate.
//   4. Swap       Infill procedural -> baked -> procedural, Body modelled
//                 -> placeholder: nothing outside the volume changes.
//   5. Ship       validate, then STEP (procedural as mesh bodies), 3MF and
//                 STL; the project saves with its caches and a new engine
//                 regenerates nothing that still matches.
//
// docs/manual-tests/mvp-workflows.md walks the same workflows in the app.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/import.hpp"
#include "vs/eval/step_export.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/templates.hpp"
#include "vs/sketch/sketch.hpp"
#include "vs/validate/validate.hpp"

using namespace vs;
using core::Id;
using feature::FeatureRef;
using feature::Param;
using model::VolumeStatus;
namespace fs = std::filesystem;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

struct TempDir {
  fs::path path = fs::temp_directory_path() / ("vs_mvp_" + Id::random().to_string());
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

model::Blob read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  model::Blob out(s.size());
  std::transform(s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

Id point(model::Document& doc, const std::string& name, Param x, Param y, Param z) {
  return doc.layout.add(feature::DatumPointFeature{feature::PointAt{x, y, z}}, name);
}

// A closed box mesh, wound outward.
mesh::Mesh box_mesh(core::fvec3 lo, core::fvec3 hi) {
  mesh::Mesh m;
  for (int i = 0; i < 8; ++i) {
    m.add_vertex({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  }
  const Id v = Id::random();
  const std::uint32_t quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : quads) {
    m.add_triangle(q[0], q[1], q[2], v);
    m.add_triangle(q[0], q[2], q[3], v);
  }
  return m;
}

// Body's contents: one extrude filling its boundary, sketched on its top
// face; the rectangle's length follows the volume parameter `len`.
model::ParametricContents body_contents(const Id& body) {
  model::ParametricContents contents;
  sketch::Sketch s(Id::random(), "Outline");
  Id p[4];
  Id l[4];
  p[0] = s.add_point({0, 0});
  p[1] = s.add_point({40, 0});
  p[2] = s.add_point({40, 30});
  p[3] = s.add_point({0, 30});
  for (std::size_t i = 0; i < 4; ++i) l[i] = s.add_line(p[i], p[(i + 1) % 4]);
  s.fix(p[0]);
  s.horizontal(l[0]);
  s.horizontal(l[2]);
  s.vertical(l[1]);
  s.vertical(l[3]);
  const Id len = s.length(l[0], 40);
  s.find_constraint(len)->dimension->expression = "len";
  s.length(l[1], 30);
  const Id sk = contents.tree.add(feature::SketchFeature{s, feature::FaceRef{body, "+z"}});
  feature::ExtrudeFeature fill;
  fill.sketch = sk;
  fill.depth = Param::of(20);
  fill.reverse = true;
  contents.tree.add(fill, "Fill");
  return contents;
}

std::map<Id, core::Hash256> keys_of(const eval::Evaluation& ev) {
  std::map<Id, core::Hash256> out;
  for (const auto& v : ev.volumes) {
    if (v.key) out.emplace(v.volume, *v.key);
  }
  return out;
}

std::string codes(const core::Diagnostics& d) { return scenes::codes(d); }

}  // namespace

TEST_CASE("The MVP workflows run end to end: block out, fill, iterate, swap, ship",
          "[proc][mvp][acceptance]") {
  TempDir dir;
  model::Project project;
  model::Document& doc = project.document;
  doc = model::make_document("MVP walk");
  eval::Engine engine(kSettings, {.threads = 4});
  proc::install(engine);

  // --- 1. Block out ----------------------------------------------------------------
  const Id w = *doc.layout.variables().add("w", "40 mm");
  model::Volume body;
  body.id = Id::random();
  body.name = "Body";
  body.boundary = model::BoxBoundary{
      FeatureRef{point(doc, "Body min", Param::of(0), Param::of(0), Param::of(0))},
      FeatureRef{point(doc, "Body max", Param::expr("w"), Param::of(30), Param::of(20))}};
  body.parameters.push_back({Id::random(), "len", Param::expr("w")});
  body.outputs.push_back({Id::random(), "mass", model::OutputQuantity::Mass, 1.24});
  body.contents = model::ParametricContents{};  // a placeholder

  model::Volume infill;
  infill.id = Id::random();
  infill.name = "Infill";
  infill.boundary = model::BoxBoundary{
      FeatureRef{point(doc, "Infill min", Param::expr("w"), Param::of(0), Param::of(0))},
      FeatureRef{point(doc, "Infill max", Param::expr("w + 40 mm"), Param::of(30), Param::of(20))}};
  infill.seed = 3;
  infill.contents = model::ParametricContents{};

  const Id joint = doc.layout.add(
      feature::DatumPlaneFeature{feature::PlaneOffset{feature::PrincipalPlane::YZ, Param::expr("w")}},
      "Joint");
  model::ConnectionSurface bond;
  bond.id = Id::random();
  bond.name = "Bond";
  model::DatumPatch patch;
  patch.plane = FeatureRef{joint};
  patch.u = Param::of(15);
  patch.v = Param::of(10);
  patch.width = Param::of(10);
  patch.height = Param::of(10);
  bond.geometry = patch;
  bond.volumes = {body.id, infill.id};
  bond.params = model::BondedParams{};
  const Id body_id = body.id;
  const Id infill_id = infill.id;
  doc.volumes.push_back(std::move(body));
  doc.volumes.push_back(std::move(infill));
  doc.connections.push_back(std::move(bond));
  INFO(codes(model::check(doc)));
  REQUIRE_FALSE(model::check(doc).has_errors());

  {
    const auto report = validate::validate(doc, kSettings, &project.blobs);
    INFO(codes(report.diagnostics));
    CHECK(report.ok());
    const auto ev = engine.evaluate(project);
    REQUIRE(ev.ok());
    for (const auto& v : ev.volumes) CHECK(v.status == VolumeStatus::Current);  // placeholders
  }

  // --- 2. Fill ---------------------------------------------------------------------
  {
    const core::Diagnostics d = model::edit(doc, [&](model::Document& edited) {
      edited.find_volume(body_id)->contents = body_contents(body_id);
      proc::TemplateOptions opts;
      opts.cell_size = 1.0;
      edited.find_volume(infill_id)->contents = model::ProceduralContents{proc::to_json(*proc::make_template("gyroid_infill", opts))};
    });
    INFO(codes(d));
    REQUIRE_FALSE(d.has_errors());
  }
  // Imports: a STEP block and an STL puck, each a new static volume.
  const auto block_solid = kernel::make_box(core::dbox3::from_min_max({0, 50, 0}, {10, 60, 10}), kSettings);
  REQUIRE(block_solid.ok());
  REQUIRE(kernel::write_step(*block_solid, dir.path / "block.step").empty());
  const auto block = eval::import_volume(project, read_bytes(dir.path / "block.step"), "step", "Block", kSettings);
  REQUIRE(block.ok());
  const auto stl = mesh::write_stl(box_mesh({20, 50, 0}, {30, 60, 5}));
  REQUIRE(stl.ok());
  const auto puck = eval::import_volume(project, *stl, "stl", "Puck", kSettings);
  REQUIRE(puck.ok());

  eval::EvaluateOptions weld;
  weld.weld = true;
  const auto filled = engine.evaluate(project, weld);
  INFO(codes(filled.diagnostics));
  REQUIRE(filled.ok());
  REQUIRE(filled.volumes.size() == 4);
  for (const auto& v : filled.volumes) CHECK(v.status == VolumeStatus::Current);
  CHECK(filled.find(body_id)->contents->rebuild);
  REQUIRE(proc::contents_mesh(*filled.find(infill_id)) != nullptr);
  REQUIRE(filled.combined);
  CHECK(filled.combined->weld.topology.watertight());
  CHECK(filled.combined->weld.topology.manifold());
  const auto filled_keys = keys_of(filled);

  // --- 3. Iterate ------------------------------------------------------------------
  REQUIRE_FALSE(doc.layout.variables().set_expression(w, "45 mm").has_errors());
  {
    // The preview tier answers at once: every boundary, the untouched
    // volumes' contents from the cache, the changed ones stale.
    const auto preview = engine.preview(project);
    CHECK(preview.find(*block)->from_cache);
    CHECK(preview.find(body_id)->stale_contents);
    CHECK(preview.find(infill_id)->status == VolumeStatus::Stale);
  }
  const auto iterated = engine.evaluate(project, weld);
  INFO(codes(iterated.diagnostics));
  REQUIRE(iterated.ok());
  CHECK(iterated.find(body_id)->regenerated);
  CHECK(iterated.find(infill_id)->regenerated);
  CHECK_FALSE(iterated.find(*block)->regenerated);
  CHECK_FALSE(iterated.find(*puck)->regenerated);
  CHECK(keys_of(iterated).at(*block) == filled_keys.at(*block));
  CHECK(keys_of(iterated).at(body_id) != filled_keys.at(body_id));
  // The Body grew with the layout: 45 x 30 x 20 of PLA.
  const auto& mass = iterated.find(body_id)->contents->outputs.at(0);
  CHECK(std::get<double>(*mass.value) > 0.0);
  REQUIRE(iterated.combined);
  CHECK(iterated.combined->weld.topology.watertight());
  const auto iterated_keys = keys_of(iterated);
  const auto iterated_hash = iterated.output_hash();

  // --- 4. Swap ---------------------------------------------------------------------
  {
    const auto before = model::content_hash(project);
    const mesh::Mesh* generated = proc::contents_mesh(*iterated.find(infill_id));
    REQUIRE(generated != nullptr);
    const mesh::Mesh generated_mesh = *generated;
    REQUIRE_FALSE(proc::bake(project, infill_id, generated_mesh).has_errors());
    const auto baked = engine.evaluate(project, weld);
    REQUIRE(baked.ok());
    CHECK(doc.find_volume(infill_id)->kind() == model::VolumeKind::Static);
    const auto baked_keys = keys_of(baked);
    for (const auto& [id, key] : iterated_keys) {
      if (id != infill_id) CHECK(baked_keys.at(id) == key);
    }
    REQUIRE(baked.find(infill_id)->contents->mesh);
    // The same mesh as a file (3MF keeps every triangle; vertices may be reordered).
    CHECK(baked.find(infill_id)->contents->mesh->triangle_count() == generated_mesh.triangle_count());
    CHECK(mesh::enclosed_volume(*baked.find(infill_id)->contents->mesh) ==
          Catch::Approx(mesh::enclosed_volume(generated_mesh)).epsilon(1e-6));
    REQUIRE(baked.combined);
    CHECK(baked.combined->ok());

    REQUIRE_FALSE(proc::unbake(project, infill_id).has_errors());
    CHECK(model::content_hash(project) == before);

    // Body to a placeholder and back: only its own key moves.
    const model::Contents modelled = doc.find_volume(body_id)->contents;
    doc.find_volume(body_id)->contents = model::ParametricContents{};
    const auto cleared = engine.evaluate(project);
    REQUIRE(cleared.ok());
    for (const auto& [id, key] : keys_of(cleared)) {
      if (id != body_id) CHECK(key == iterated_keys.at(id));
    }
    doc.find_volume(body_id)->contents = modelled;
    CHECK(model::content_hash(project) == before);
  }

  // --- 5. Ship ---------------------------------------------------------------------
  const auto shipped = engine.evaluate(project, weld);
  REQUIRE(shipped.ok());
  CHECK(shipped.output_hash() == iterated_hash);  // the swaps came back exactly
  {
    const auto report = validate::validate(doc, kSettings, &project.blobs);
    INFO(codes(report.diagnostics));
    CHECK(report.ok());
  }

  // STEP: B-rep where there is B-rep, procedural and mesh contents as mesh bodies.
  const fs::path step = dir.path / "mvp.step";
  const auto step_out = eval::export_step(shipped, step);
  INFO(codes(step_out.diagnostics));
  REQUIRE(step_out.ok());
  CHECK(step_out.breps == 2);   // Body, Block
  CHECK(step_out.meshes == 2);  // Infill, Puck
  const auto parts = kernel::read_step_parts(step);
  REQUIRE(parts.ok());
  REQUIRE(parts->size() == 4);
  CHECK((*parts)[0].name == "Body");
  CHECK((*parts)[1].name == "Infill");
  CHECK(kernel::stored_triangle_count((*parts)[1].shape) ==
        proc::contents_mesh(*shipped.find(infill_id))->triangle_count());
  CHECK(kernel::Solid::from((*parts)[2].shape).ok());

  // 3MF, one object per volume, and STL, both from the welded mesh.
  REQUIRE(shipped.combined);
  const mesh::Mesh& welded = shipped.combined->weld.mesh;
  std::vector<mesh::Mesh> per_volume;
  for (const auto& v : doc.volumes) per_volume.push_back(mesh::extract_volume(welded, v.id));
  std::vector<mesh::ThreeMfObject> objects;
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    objects.push_back({doc.volumes[i].name, doc.volumes[i].id, &per_volume[i]});
  }
  const auto three_mf = mesh::write_3mf(objects, doc.name);
  REQUIRE(three_mf.ok());
  const auto read_back = mesh::read_3mf(*three_mf);
  REQUIRE(read_back.ok());
  CHECK(read_back->triangle_count() == welded.triangle_count());
  // One object (what a slicer prints as one part) is watertight; per volume
  // the pieces are open where seams were welded away.
  const mesh::ThreeMfObject whole{doc.name, Id::null(), &welded};
  const auto one = mesh::write_3mf(std::span(&whole, 1), doc.name);
  REQUIRE(one.ok());
  const auto one_back = mesh::read_3mf(*one);
  REQUIRE(one_back.ok());
  CHECK(mesh::analyse(*one_back).watertight());
  CHECK(mesh::analyse(*one_back).manifold());
  const auto stl_out = mesh::write_stl(welded);
  REQUIRE(stl_out.ok());
  CHECK(mesh::read_stl(*stl_out)->triangle_count() == welded.triangle_count());

  // The project with its caches: a fresh engine regenerates only Body
  // (parametric contents rebuild; everything else is a hit).
  const fs::path saved = dir.path / "mvp.vsproj";
  const model::CacheFiles cache = eval::project_cache_files(shipped);
  REQUIRE_FALSE(model::save_project(project, saved, model::ProjectLayout::Zip, &cache).has_errors());
  auto loaded = model::load_project(saved);
  REQUIRE(loaded.ok());
  CHECK(model::content_hash(*loaded) == model::content_hash(project));
  eval::Engine fresh(kSettings, {.threads = 4});
  proc::install(fresh);
  CHECK(fresh.cache().insert_files(model::load_project_cache(saved)) == 3);
  const auto reopened = fresh.evaluate(*loaded, weld);
  REQUIRE(reopened.ok());
  CHECK(reopened.stats.regenerated == 1);
  CHECK(reopened.find(body_id)->regenerated);
  CHECK(reopened.find(infill_id)->from_cache);
  CHECK(reopened.output_hash() == shipped.output_hash());
}
