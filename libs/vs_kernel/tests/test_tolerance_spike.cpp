// The M1 tolerance spike, as assertions. The measured numbers and what they
// mean for the product settings are in
// docs/decisions/0001-working-space-and-tolerance.md; `vstudio-cli
// tolerance-spike` prints the same table.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using vs::core::ProductSettings;

namespace {

const SpikeCase& case_named(const std::vector<SpikeCase>& cases, const std::string& name) {
  for (const auto& c : cases) {
    if (c.name == name) return c;
  }
  FAIL("no spike case named " << name);
  return cases.front();
}

}  // namespace

TEST_CASE("tolerance spike at the edge of the working space", "[kernel][spike]") {
  const ProductSettings settings = ProductSettings::defaults();
  const auto cases = run_tolerance_spike(settings);
  REQUIRE(cases.size() == 20);

  for (const auto& c : cases) {
    INFO(c.name << ": " << c.description);
    for (const auto& d : c.diagnostics) INFO(d.to_string());
    REQUIRE(c.built);
    CHECK(c.valid);
    CHECK(c.audit.vertices.count > 0);
  }

  SECTION("volumes match analytic values wherever the part is placed") {
    for (const auto& c : cases) {
      INFO(c.name << " volume " << c.volume << " expected " << c.expected_volume);
      CHECK(c.volume_relative_error() < 1e-6);
    }
  }

  SECTION("placement at the far corner doesn't change the outcome") {
    for (const std::string scenario :
         {"box", "drilled", "drilled-25", "sphere-union", "crossed-cylinders-union",
          "crossed-cylinders-common", "min-feature-boss", "fillet-12", "near-coincident",
          "near-coincident-fuzzy"}) {
      const std::string origin_name = scenario + "@origin";
      const std::string corner_name = scenario + "@corner";
      const auto& at_origin = case_named(cases, origin_name);
      const auto& at_corner = case_named(cases, corner_name);
      INFO(scenario);
      CHECK(at_origin.solids == at_corner.solids);
      CHECK(at_origin.audit.faces.count == at_corner.audit.faces.count);
      CHECK(at_origin.audit.edges.count == at_corner.audit.edges.count);
    }
  }

  SECTION("no kernel tolerance grows past the product tolerance") {
    // Every entity keeps OCCT's default 1e-7 mm, even after booleans of
    // curved faces at the corner of the working space.
    for (const auto& c : cases) {
      INFO(c.name << " max tolerance " << c.audit.max_tolerance());
      CHECK(c.audit.within_limit());
    }
  }

  SECTION("a gap below the product tolerance stays a gap unless the boolean is fuzzy") {
    for (const std::string where : {"@origin", "@corner"}) {
      CHECK(case_named(cases, "near-coincident" + where).solids == 2);
      CHECK(case_named(cases, "near-coincident-fuzzy" + where).solids == 1);
    }
  }
}
