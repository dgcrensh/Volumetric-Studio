// Acceptance: validating a 50-volume layout takes well under a second.

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/layout.hpp"

using namespace vs::validate;
using namespace vs::model;

TEST_CASE("Validating a 50-volume layout takes well under a second",
          "[validate][performance]") {
  const auto settings = vs::core::ProductSettings::defaults();
  // A 10 x 5 grid of 20 mm blocks. Along x, neighbours touch through
  // bonded connection surfaces; along y the rows touch with no surface
  // (undeclared contact) except the last row, which stands 5 mm apart.
  Document doc = make_document("Grid");
  Id ids[5][10];
  for (int j = 0; j < 5; ++j) {
    const double y0 = j < 4 ? 20.0 * j : 20.0 * j + 5.0;
    for (int i = 0; i < 10; ++i) {
      ids[j][i] = fixtures::add_box(doc, "B" + std::to_string(j) + "_" + std::to_string(i),
                                    {20.0 * i, y0, 0}, {20.0 * i + 20, y0 + 20, 20});
    }
  }
  for (int j = 0; j < 5; ++j) {
    const double y0 = j < 4 ? 20.0 * j : 20.0 * j + 5.0;
    for (int i = 0; i + 1 < 10; ++i) {
      // A patch on the blocks' tops (z = 20), straddling the joint: it lies
      // on both, and joining them makes them adjacent.
      fixtures::add_patch(doc, "J" + std::to_string(j) + "_" + std::to_string(i),
                          {ids[j][i], ids[j][i + 1]}, 20, 20.0 * (i + 1), y0 + 10, 10, 10,
                          BondedParams{});
    }
  }
  REQUIRE(doc.volumes.size() == 50);

  const auto t0 = std::chrono::steady_clock::now();
  const auto b = build(doc, settings);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(b.ok());
  const auto report = validate_layout(doc, b, settings);
  const auto t2 = std::chrono::steady_clock::now();

  const double build_s = std::chrono::duration<double>(t1 - t0).count();
  const double validate_s = std::chrono::duration<double>(t2 - t1).count();
  WARN("50 volumes: build " << build_s << " s, validate " << validate_s << " s, "
                            << report.pairs_tested << " pairs looked at");

  // 3 rows of 10 touching the row above: 30 face contacts, plus the
  // diagonal neighbours touching along edges: 2 x 9 per row pair.
  CHECK(report.overlaps.empty());
  CHECK(report.diagnostics.with_code("validate.contact").size() == 30);
  CHECK(report.diagnostics.with_code("validate.contact.edge").size() == 3 * 18);
  CHECK_FALSE(report.diagnostics.has_errors());
#ifdef NDEBUG
  CHECK(validate_s < 1.0);
#else
  CHECK(validate_s < 5.0);  // unoptimised builds of OCCT and the studio
#endif
}
