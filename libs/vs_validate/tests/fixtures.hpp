#pragma once

// Fixture projects for vs_validate, one per check, built in code. Each is
// also saved under tests/data/<name>.vsproj (regenerate them with
// `vs_validate_tests "[.generate]"`), which `vstudio-cli validate` reads in
// ctest.
//
//   clean        two stacked blocks bolted together; the top one is a STEP
//                file that fills its boundary. No diagnostics.
//   overlap      two blocks 10 mm into each other.
//   contact      three blocks: two side by side face to face, and a third
//                touching the second along an edge only, none connected.
//   mismatch     two stacked blocks bolted with M6 holes too close to the
//                flange's edge and to each other.
//   loop         a block whose published volume drives its own height.
//   containment  a block whose STEP contents stick 5 mm out of it.

#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/vec.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace fixtures {

using vs::core::dvec3;
using vs::core::Id;

struct Fixture {
  std::string name;
  vs::model::Project project;
  std::vector<std::string> expected;  // codes, in order
};

// A volume whose boundary is the box between two datum points.
Id add_box(vs::model::Document& doc, const std::string& name, dvec3 lo, dvec3 hi);

// A rectangular datum patch on the plane z = `z`, centred at (u, v).
Id add_patch(vs::model::Document& doc, const std::string& name, std::vector<Id> volumes, double z,
             double u, double v, double width, double height, vs::model::ConnectionParams params);

vs::model::BoltedFlangeParams bolts(const std::string& thread, double min_wall, int columns,
                                    double pitch, double u, double v);

// The STEP file of an axis-aligned box.
vs::model::Blob step_box(dvec3 lo, dvec3 hi);

std::vector<Fixture> all();
Fixture by_name(const std::string& name);

std::string codes(const vs::core::Diagnostics& diagnostics);
std::string joined(const std::vector<std::string>& codes);

}  // namespace fixtures
