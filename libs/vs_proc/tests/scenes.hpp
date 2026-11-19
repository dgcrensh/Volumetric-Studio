#pragma once

// Projects and graphs for the vs_proc suites, built in code.
//
// boxes(graphs): one procedural box volume per graph, 40 x 30 x 20 mm, in
// a row along x with no gaps (volume i spans x from 40 i to 40 i + 40), each
// with a parameter `wall` (2 mm) and seed i. Each pair of neighbours is
// joined by a bonded connection surface: a 10 x 10 patch on the plane
// x = 40 (i + 1), centred at y = 10, z = 10.
//
// shell_and_ball(): the acceptance graph: the boundary shelled by the
// graph parameter `wall` (default 2), unioned with a sphere of radius 6 at
// the box's centre (20, 15, 10), sampled with 1 mm cells.

#include <cstdint>
#include <string>
#include <vector>

#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/inputs.hpp"

namespace scenes {

using vs::core::Id;

struct Boxes {
  vs::model::Project project;
  std::vector<Id> volumes;
  std::vector<Id> wall;      // each volume's `wall` parameter
  std::vector<Id> surfaces;  // between volume i and i + 1
};

Boxes boxes(const std::vector<vs::proc::Graph>& graphs);

struct ShellAndBall {
  vs::proc::Graph graph;
  Id boundary, wall, shell, ball, join, output;
};
ShellAndBall shell_and_ball();

// The analytic value of shell_and_ball() at p for a box from `lo` to `hi`.
double shell_and_ball_value(const vs::core::dvec3& p, const vs::core::dvec3& lo,
                            const vs::core::dvec3& hi, double wall);

// Signed distance to the axis-aligned box from `lo` to `hi`.
double box_distance(const vs::core::dvec3& p, const vs::core::dvec3& lo, const vs::core::dvec3& hi);

// A graph whose contents are field.constant fed by `value_port` of node
// `source`: for testing scalar nodes through evaluate_graph().
vs::proc::Id constant_output(vs::proc::Graph& graph, const Id& source,
                             const std::string& value_port = "value");

// Inputs with no boundary or surfaces: for graphs that don't read them.
vs::proc::GraphInputs bare_inputs(std::uint64_t seed = 0);

// The codes of a diagnostics list, for INFO().
std::string codes(const vs::core::Diagnostics& diagnostics);

}  // namespace scenes
