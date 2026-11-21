// Plan H1: write_step() and read_step_parts() go through an XCAF document
// that the OCCT application keeps in its list of open documents until it is
// closed. The only test that looks at OCCT itself, to count them.

#include <XCAFApp_Application.hxx>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "vs/core/id.hpp"
#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
namespace fs = std::filesystem;

namespace {
const vs::core::ProductSettings kSettings = vs::core::ProductSettings::defaults();

int open_documents() { return XCAFApp_Application::GetApplication()->NbDocuments(); }
}  // namespace

TEST_CASE("STEP assemblies leave no XCAF document open", "[kernel][exchange][h1]") {
  const fs::path dir = fs::temp_directory_path() / ("vs_kernel_xcaf_" + vs::core::Id::random().to_string());
  fs::create_directories(dir);
  const fs::path path = dir / "parts.step";
  const Solid a = make_box(vs::core::dbox3::from_min_max({0, 0, 0}, {10, 10, 10}), kSettings).value();
  const Solid b = make_box(vs::core::dbox3::from_min_max({20, 0, 0}, {30, 10, 10}), kSettings).value();
  const std::vector<NamedShape> parts{{"A", a}, {"B", b}};

  const int before = open_documents();
  for (int i = 0; i < 3; ++i) {
    REQUIRE_FALSE(write_step(parts, path, "Assembly").has_errors());
    const auto read = read_step_parts(path);
    REQUIRE(read.ok());
    CHECK(read->size() == 2);
  }
  // Failing paths close theirs too.
  CHECK_FALSE(read_step_parts(dir / "missing.step").ok());
  fs::create_directories(dir / "not_a_file.step");
  CHECK(write_step(parts, dir / "not_a_file.step", "Assembly").has_errors());
  {
    std::ofstream junk(dir / "junk.step");
    junk << "ISO-10303-21;\nthis is not STEP\n";
  }
  CHECK_FALSE(read_step_parts(dir / "junk.step").ok());
  CHECK(open_documents() == before);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
