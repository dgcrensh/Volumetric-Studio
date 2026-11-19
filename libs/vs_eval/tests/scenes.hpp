#pragma once

// Projects for the vs_eval suites, built in code.
//
// row(n): n box volumes 20 x 20 x 20 mm in a row along x, 5 mm apart. Each
// is parametric: a 16 x 16 rectangle sketched on its "+z" face, extruded
// down into it by its parameter `depth` (10 mm). Each publishes `volume`
// (mm³; 2560 at the default depth).
//
// chain(): row(3), named A, B and C, except that B's extrusion is
// `feed / 256` deep, where B's parameter `feed` is driven by a link from
// A's published volume (so B depends on A; C is independent). At the
// defaults every block is 10 mm deep.

#include <string>
#include <vector>

#include "vs/core/id.hpp"
#include "vs/model/project.hpp"

namespace scenes {

using vs::core::Id;

struct Row {
  vs::model::Project project;
  std::vector<Id> volumes;
  std::vector<Id> depth;   // each volume's `depth` parameter (B's `feed` in chain())
  std::vector<Id> output;  // each volume's `volume` output
};

Row row(std::size_t n);
Row chain();

// Sets a volume parameter's value.
void set_parameter(vs::model::Project& project, const Id& volume, const Id& parameter,
                   double value);

// A temporary directory, removed when the object goes.
struct TempDir {
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  std::string path;
};

}  // namespace scenes
