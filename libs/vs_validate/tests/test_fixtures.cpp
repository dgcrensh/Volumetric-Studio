// Acceptance: the fixture projects for each check (overlap, contact,
// mismatch, loop, containment) produce exactly the expected diagnostics,
// both as built in code and as saved under tests/data.

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "vs/model/project.hpp"
#include "vs/validate/registry.hpp"
#include "vs/validate/validate.hpp"

using namespace vs::validate;
using fixtures::Fixture;
namespace fs = std::filesystem;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

fs::path data_path(const std::string& name) {
  return fs::path(VS_VALIDATE_TEST_DATA) / (name + ".vsproj");
}

// Where each fixture's diagnostics send the user.
ActOn expected_target(const std::string& name) {
  if (name == "mismatch") return ActOn::ConnectionSurface;
  if (name == "containment") return ActOn::Contents;
  return ActOn::Layout;
}

void check_report(const std::string& name, const vs::model::Project& project,
                  const std::vector<std::string>& expected) {
  const auto report = validate(project.document, kSettings, &project.blobs);
  INFO(name);
  for (const auto& d : report.diagnostics) INFO(d.to_string());
  CHECK(fixtures::codes(report.diagnostics) == fixtures::joined(expected));
  // Contact is a warning; everything else is an error.
  CHECK(report.ok() == (name == "clean" || name == "contact"));
  REQUIRE(report.routed.size() == report.diagnostics.size());
  for (const auto& r : report.routed) {
    CHECK(r.target.on == expected_target(name));
    // Every validate.* code is registered, with the severity it is raised at.
    if (r.diagnostic.code.starts_with("validate.")) {
      const CodeInfo* info = find_code(r.diagnostic.code);
      REQUIRE(info != nullptr);
      CHECK(info->severity == r.diagnostic.severity);
    }
  }
}

}  // namespace

TEST_CASE("Fixture projects give exactly the expected diagnostics", "[validate][fixtures]") {
  for (const auto& f : fixtures::all()) {
    DYNAMIC_SECTION(f.name) { check_report(f.name, f.project, f.expected); }
  }
}

TEST_CASE("Saved fixture projects give exactly the expected diagnostics",
          "[validate][fixtures]") {
  for (const auto& f : fixtures::all()) {
    DYNAMIC_SECTION(f.name) {
      // Unchecked, so the loop fixture loads and validate() reports it.
      auto loaded = vs::model::load_project(data_path(f.name), /*check_document=*/false);
      INFO("regenerate with: vs_validate_tests \"[.generate]\"");
      REQUIRE(loaded.ok());
      check_report(f.name, *loaded, f.expected);
    }
  }
}

TEST_CASE("The loop fixture is refused by a checked load", "[validate][fixtures]") {
  auto loaded = vs::model::load_project(data_path("loop"));
  REQUIRE_FALSE(loaded.ok());
  CHECK(loaded.error().code == "model.dependency.loop");
}

TEST_CASE("Diagnostics carry what the spec asks them to name", "[validate][fixtures]") {
  SECTION("overlap: both volumes, the region and its depth") {
    const auto f = fixtures::by_name("overlap");
    const auto r = validate(f.project.document, kSettings);
    REQUIRE(r.layout.overlaps.size() == 1);
    const auto& o = r.layout.overlaps.front();
    CHECK(o.a == f.project.document.volumes[0].id);
    CHECK(o.b == f.project.document.volumes[1].id);
    CHECK(std::abs(o.depth - 10.0) < 1e-6);
    CHECK(std::abs(o.volume - 10.0 * 40 * 20) < 1e-3);
    CHECK(std::abs(o.region.min.x - 30.0) < 1e-6);
    CHECK(std::abs(o.region.max.x - 40.0) < 1e-6);
    const auto& d = r.diagnostics[0];
    REQUIRE(d.subjects.size() == 2);
    CHECK(d.subjects[0].kind == vs::core::SubjectKind::Boundary);
    CHECK(d.subjects[1].id == o.b);
  }
  SECTION("contact: both faces") {
    const auto f = fixtures::by_name("contact");
    const auto r = validate(f.project.document, kSettings);
    REQUIRE(r.layout.contacts.size() == 2);
    const auto& c = r.layout.contacts[0];
    CHECK(c.face_a == "+x");
    CHECK(c.face_b == "-x");
    CHECK(std::abs(c.area - 400.0) < 1e-6);
    const auto& d = r.diagnostics[0];
    CHECK(d.severity == vs::core::Severity::Warning);
    REQUIRE(d.subjects.size() == 2);
    CHECK(d.subjects[0].label.find("face '+x'") != std::string::npos);
    CHECK(d.subjects[1].label.find("face '-x'") != std::string::npos);
    CHECK(r.layout.contacts[1].area == 0.0);  // the edge touch
  }
  SECTION("mismatch: the surface and the conflicting parameters") {
    const auto f = fixtures::by_name("mismatch");
    const auto r = validate(f.project.document, kSettings);
    for (const auto& d : r.diagnostics) {
      REQUIRE(d.subjects.size() == 1);
      CHECK(d.subjects[0].id == f.project.document.connections[0].id);
      CHECK(d.message.find("min_wall") != std::string::npos);
      CHECK(d.message.find("thread") != std::string::npos);
    }
  }
  SECTION("containment: the volume and the region outside") {
    const auto f = fixtures::by_name("containment");
    const auto r = validate(f.project.document, kSettings, &f.project.blobs);
    REQUIRE(r.routed.size() == 1);
    CHECK(r.routed[0].target.volume == f.project.document.volumes[0].id);
    CHECK(r.diagnostics[0].message.find("reach 5 mm outside") != std::string::npos);
  }
}

TEST_CASE("Regenerate the fixture projects", "[.generate]") {
  for (const auto& f : fixtures::all()) {
    const fs::path path = data_path(f.name);
    std::error_code ec;
    fs::remove_all(path, ec);
    REQUIRE_FALSE(
        vs::model::save_project(f.project, path, vs::model::ProjectLayout::Folder).has_errors());
  }
}
