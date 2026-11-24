#pragma once

// The starter template library (plan, M12; spec, "Fill: ... attach a
// procedure from a template library").
//
//   gyroid_infill   the boundary shelled by `wall`, filled with a gyroid
//                   sheet (`cell`, `thickness`)
//   graded_lattice  the boundary shelled by `wall`, filled with a BCC strut
//                   lattice whose struts run from `thick` at the skin down
//                   to `thin` over `ramp` mm inward (graded by distance to
//                   the boundary)
//   bolt_bosses     the boundary shelled by `wall`, with a solid boss of
//                   `boss_radius` round every bolt hole of a bolted flange,
//                   reaching `boss_height` into the volume; publishes
//                   `boss_count`, the number of holes it found
//   voronoi_walls   the boundary shelled by `wall`, filled with Voronoi
//                   cell walls `thickness` thick round `count` seeds
//                   scattered on a connection surface (`spacing` apart)
//                   and moved `depth` into the volume along its normal,
//                   in `layers` layers `step` apart (decision 0042)
//
// make_template() returns a graph ready for a procedural volume: node ids
// derive from the template's name (Id::derive), so the same template with
// the same options is the same graph, byte for byte. Each exposes its
// numbers as graph parameters, so a volume parameter of the same name
// overrides them. `cell_size` is the output's grid (mm; the volume's
// generation cost grows with its inverse cube).
//
// Codes: proc.template.unknown (no such template), proc.template.surface
// (bolt_bosses needs the flange's surface id, voronoi_walls the surface it
// seeds on).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/proc/graph.hpp"

namespace vs::proc {

struct TemplateInfo {
  std::string name;
  std::string doc;
  bool needs_surface = false;  // reads a connection surface (a bolted flange, Voronoi seeds)
};

const std::vector<TemplateInfo>& templates();

struct TemplateOptions {
  std::optional<core::Id> surface;  // the connection surface it reads, if any
  double cell_size = 0.5;           // the output's grid, mm
};

core::Result<Graph> make_template(std::string_view name, const TemplateOptions& options = {});

}  // namespace vs::proc
