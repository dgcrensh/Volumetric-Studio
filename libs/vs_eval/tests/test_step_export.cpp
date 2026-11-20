// M17: STEP export of an evaluated design.

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/import.hpp"
#include "vs/eval/step_export.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/io.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

// A closed box mesh from `lo` to `hi`, wound outward.
mesh::Mesh box_mesh(core::fvec3 lo, core::fvec3 hi) {
  mesh::Mesh m;
  for (int i = 0; i < 8; ++i) {
    m.add_vertex({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  }
  const core::Id v = core::Id::random();
  const std::uint32_t quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : quads) {
    m.add_triangle(q[0], q[1], q[2], v);
    m.add_triangle(q[0], q[2], q[3], v);
  }
  return m;
}

std::vector<std::string> names(const std::vector<kernel::NamedShape>& parts) {
  std::vector<std::string> out;
  for (const auto& p : parts) out.push_back(p.name);
  return out;
}

}  // namespace

TEST_CASE("The bracket exports as named B-rep parts", "[eval][step]") {
  const auto b = bracket::make_bracket();
  eval::Engine engine(kSettings, {.threads = 2});
  const auto ev = engine.evaluate(b.project);
  REQUIRE(ev.ok());

  const auto plain = eval::step_parts(ev);
  REQUIRE(plain.ok());
  CHECK(names(plain.parts) == std::vector<std::string>{"Base", "Upright"});
  CHECK(plain.breps == 2);

  eval::StepExportOptions all;
  all.boundaries = true;
  all.connection_surfaces = true;
  const auto more = eval::step_parts(ev, all);
  REQUIRE(more.ok());
  CHECK(names(more.parts) ==
        std::vector<std::string>{"Base", "Upright", "Base boundary", "Upright boundary", "Flange"});

  scenes::TempDir dir;
  const auto file = std::filesystem::path(dir.path) / "bracket.step";
  const auto written = eval::export_step(ev, file, all);
  REQUIRE(written.ok());
  const auto back = kernel::read_step_parts(file);
  REQUIRE(back.ok());
  CHECK(names(*back) == names(more.parts));
  // The Base's contents fill its boundary: 60 x 40 x 10.
  const auto base = kernel::Solid::from(back->front().shape);
  REQUIRE(base.ok());
  CHECK(kernel::mass_properties(*base)->volume > 23999.0);
}

TEST_CASE("Mesh contents export as mesh bodies, placeholders as boundaries", "[eval][step]") {
  auto r = scenes::row(2);
  // Volume 0 gets an STL that fits; volume 1 becomes a placeholder.
  const auto stl = mesh::write_stl(box_mesh({2, 2, 2}, {18, 18, 18}));
  REQUIRE(stl.ok());
  REQUIRE_FALSE(eval::import_contents(r.project, r.volumes[0], *stl, "stl", kSettings).has_errors());
  r.project.document.volumes[1].contents = model::ParametricContents{};
  eval::Engine engine(kSettings, {.threads = 2});
  const auto ev = engine.evaluate(r.project);
  REQUIRE(ev.ok());

  const auto parts = eval::step_parts(ev);
  REQUIRE(parts.ok());
  CHECK(parts.meshes == 1);
  CHECK(parts.boundaries == 1);
  CHECK(kernel::stored_triangle_count(parts.parts[0].shape) == 12);
  CHECK(bracket::codes(parts.diagnostics).find("eval.export.placeholder") != std::string::npos);

  scenes::TempDir dir;
  const auto file = std::filesystem::path(dir.path) / "row.step";
  REQUIRE(eval::export_step(ev, file).ok());
  const auto back = kernel::read_step_parts(file);
  REQUIRE(back.ok());
  REQUIRE(back->size() == 2);
  CHECK(kernel::stored_triangle_count((*back)[0].shape) == 12);
  CHECK(kernel::Solid::from((*back)[1].shape).ok());
}

TEST_CASE("Only current results export", "[eval][step]") {
  auto r = scenes::row(2);
  // A STEP file bigger than volume 0: it fails containment.
  const auto big = bracket::step_box(-5, -5, -5, 25, 25, 25);
  REQUIRE_FALSE(eval::import_contents(r.project, r.volumes[0], big, "step", kSettings).has_errors());
  eval::Engine engine(kSettings, {.threads = 2});
  const auto ev = engine.evaluate(r.project);
  const auto parts = eval::step_parts(ev);
  CHECK_FALSE(parts.ok());
  CHECK(parts.parts.empty());
  CHECK(bracket::codes(parts.diagnostics).find("eval.export.not_current") != std::string::npos);
  REQUIRE_FALSE(parts.diagnostics.items().empty());
  CHECK(parts.diagnostics.items().front().subjects.front().id == r.volumes[0]);
}
