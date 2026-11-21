// M17: Validate before export (the Qt-free half; the app's own walk is
// test_ship_ui.cpp).

#include <catch2/catch_test_macros.hpp>

#include "samples.hpp"
#include "ship.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

}  // namespace

TEST_CASE("Validate sums up every check before export", "[app][ship]") {
  SECTION("a clean design is ready") {
    const auto project = samples::load("bracket.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    INFO(samples::codes(check.diagnostics));
    CHECK(check.ok());
    CHECK(check.volumes == 2);
    CHECK(check.current == 2);
    CHECK(app::summary(check).starts_with("Ready to export"));
  }
  SECTION("overlap stops it, once, with the reason") {
    const auto project = samples::load("overlap.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    CHECK_FALSE(check.ok());
    // Both validate() and the engine see the overlap: it is listed once.
    CHECK(check.diagnostics.with_code("validate.overlap").size() == 1);
    CHECK(app::summary(check).starts_with("Not ready to export"));
  }
  SECTION("generated contents that stick out are caught") {
    const auto project = samples::load("containment.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    CHECK_FALSE(check.diagnostics.with_code("validate.containment").empty());
    CHECK_FALSE(check.ok());
  }
  SECTION("a result that isn't current is a warning") {
    const auto project = samples::load("shell_and_ball.vsproj");
    eval::Engine engine(kSettings, {.threads = 2});
    const auto preview = engine.preview(project);  // no generator: nothing generated
    const auto check = app::check_for_export(project, preview, kSettings);
    CHECK_FALSE(check.diagnostics.with_code("app.validate.stale").empty());
    CHECK(check.warnings >= 1);
  }
}
