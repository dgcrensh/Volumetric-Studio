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
