// H4 (plan docs/plans/H-hardening.md, decision 0026): project files that
// failed on main before their fixes.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
namespace fs = std::filesystem;

namespace {

struct Scratch {
  fs::path dir;
  Scratch() {
    dir = fs::temp_directory_path() / ("vs_model_h4_" + Id::random().to_string());
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

TEST_CASE("A blob path naming a folder is refused, not read", "[model][project][hardening]") {
  const auto b = bracket::make_bracket();
  Scratch scratch;
  const fs::path path = scratch.dir / "bracket.vsproj";
  REQUIRE_FALSE(save_project(b.project, path, ProjectLayout::Folder).has_errors());
  const fs::path manifest = path / "project.json";
  const std::string original = read_text(manifest);
  const std::string blob = b.project.blobs.begin()->first;

  for (const std::string folder : {"blobs/.", "blobs/sub"}) {
    CAPTURE(folder);
    fs::create_directories(path / "blobs" / "sub");
    std::string text = original;
    replace_once(text, "\"path\": \"" + blob + "\"", "\"path\": \"" + folder + "\"");
    write_text(manifest, text);
    const auto r = load_project(path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "model.project.blob_missing");
    // The cache folder's own entries are read the same way.
    CHECK(load_project_cache(path).empty());
  }
}

TEST_CASE("Format versions out of int range are read as numbers", "[model][project][hardening]") {
  const auto b = bracket::make_bracket();
  Scratch scratch;
  const fs::path path = scratch.dir / "bracket.vsproj";
  REQUIRE_FALSE(save_project(b.project, path, ProjectLayout::Folder).has_errors());
  const fs::path manifest = path / "project.json";
  const std::string original = read_text(manifest);
  const auto load_with = [&](const std::string& version) {
    std::string text = original;
    replace_once(text, "\"version\": 1", "\"version\": " + version);
    write_text(manifest, text);
    return load_project(path);
  };
  // 2^32 + 1 used to read as version 1.
  CHECK(load_with("4294967297").error().code == "model.project.version");
  CHECK(load_with("18446744073709551615").error().code == "model.project.version");
  CHECK(load_with("-4294967295").error().code == "model.json.bad_value");
  CHECK(load_with("1.0").error().code == "model.json.bad_value");
  CHECK(load_with("\"1\"").error().code == "model.json.bad_value");
  CHECK(load_with("1").ok());
}

TEST_CASE("Saves of one file from two threads both land whole", "[model][project][hardening]") {
  // Each write goes to its own temporary file: two saves racing for one
  // path no longer write into the same temporary and rename it from
  // under each other.
  const auto b = bracket::make_bracket();
  Project other = b.project;
  other.document.name = "Other bracket";
  Scratch scratch;
  const fs::path path = scratch.dir / "shared.vsproj";
  std::vector<std::string> failures[2];
  const auto save_many = [&](const Project& p, std::vector<std::string>& failed) {
    for (int i = 0; i < 40; ++i) {
      for (const auto& d : save_project(p, path, ProjectLayout::Zip)) failed.push_back(d.to_string());
    }
  };
  std::thread t0(save_many, std::cref(b.project), std::ref(failures[0]));
  std::thread t1(save_many, std::cref(other), std::ref(failures[1]));
  t0.join();
  t1.join();
  CHECK(failures[0].empty());
  CHECK(failures[1].empty());
  const auto loaded = load_project(path);
  REQUIRE(loaded.ok());
  CHECK((loaded->document.name == b.project.document.name || loaded->document.name == "Other bracket"));
  // No temporary file is left behind.
  std::size_t files = 0;
  for (const auto& entry : fs::directory_iterator(scratch.dir)) {
    (void)entry;
    ++files;
  }
  CHECK(files == 1);
}
