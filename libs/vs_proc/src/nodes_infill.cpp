// Infill nodes (plan, M12): lattices, TPMS, thicken, clamp, instancing of
// fields, and output.publish.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "node_helpers.hpp"
#include "vs/proc/field.hpp"

namespace vs::proc::detail {

namespace {

// Cell size and (when not graded) thickness, checked against each other.
bool check_cell(NodeContext& ctx, double cell, double thickness, double most) {
  if (!check_size(ctx, "cell_size", cell)) return false;
  if (ctx.has("grade")) return true;  // the grade gives the thickness
  if (!check_size(ctx, "thickness", thickness)) return false;
  if (thickness >= cell * most) {
    ctx.infeasible({"thickness", "cell_size"},
                   fmt::format("a thickness of {} mm fills a {} mm cell; keep it under {} mm",
                               thickness, cell, cell * most));
    return false;
  }
  return true;
}

LatticeCell lattice_cell(const std::string& name) {
  if (name == "bcc") return LatticeCell::Bcc;
  if (name == "octet") return LatticeCell::Octet;
  return LatticeCell::Cubic;
}

TpmsSurface tpms_surface(const std::string& name) {
  if (name == "schwarz_p") return TpmsSurface::SchwarzP;
  if (name == "diamond") return TpmsSurface::Diamond;
  return TpmsSurface::Gyroid;
}

}  // namespace

void add_infill_nodes(NodeRegistry& r) {
  r.add(make(
      "field.lattice", "infill",
      "A strut lattice filling space: struts `thickness` thick on cubic, BCC or octet cells "
      "`cell_size` wide, a cell corner at `origin`. Connect `grade` (a field, mm) to vary the "
      "thickness point by point; intersect with the boundary to fill a volume",
      {in("cell_size", P::Scalar, num(10)), in("thickness", P::Scalar, num(1)),
       in("origin", P::Vector, vec(0, 0, 0)),
       optional_in("grade", P::Field, "the strut thickness at each point (overrides thickness)")},
      {out("field", P::Field)},
      [](NodeContext& ctx) {
        const double cell = ctx.scalar("cell_size");
        const double t = ctx.scalar("thickness");
        // Struts as thick as half the cell already merge into a solid.
        if (!check_cell(ctx, cell, t, 0.5)) return;
        ctx.set("field", lattice_field(lattice_cell(ctx.text("cell")), cell, ctx.vector("origin"), t,
                                       ctx.has("grade") ? ctx.field("grade") : nullptr));
      },
      {{"cell", PropertyKind::Choice, Setting(std::string("cubic")), {"cubic", "bcc", "octet"},
        "the unit cell"}}));

  r.add(make(
      "field.tpms", "infill",
      "A triply periodic minimal surface as a sheet `thickness` thick: gyroid, Schwarz P or "
      "diamond, with cells `cell_size` wide from `origin`. Connect `grade` (a field, mm) to vary "
      "the thickness; intersect with the boundary to fill a volume",
      {in("cell_size", P::Scalar, num(10)), in("thickness", P::Scalar, num(1)),
       in("origin", P::Vector, vec(0, 0, 0)),
       optional_in("grade", P::Field, "the sheet thickness at each point (overrides thickness)")},
      {out("field", P::Field)},
      [](NodeContext& ctx) {
        const double cell = ctx.scalar("cell_size");
        const double t = ctx.scalar("thickness");
        if (!check_cell(ctx, cell, t, 0.25)) return;
        ctx.set("field", tpms_field(tpms_surface(ctx.text("surface")), cell, ctx.vector("origin"), t,
                                    ctx.has("grade") ? ctx.field("grade") : nullptr));
      },
      {{"surface", PropertyKind::Choice, Setting(std::string("gyroid")),
        {"gyroid", "schwarz_p", "diamond"}, "the surface"}}));

  r.add(make(
      "field.voronoi", "infill",
      "Walls `thickness` thick on the faces of the Voronoi cells round the frames' origins (the "
      "seeds): each cell holds the points nearer its seed than any other. Seed from a surface "
      "with scatter.surface and frames.offset for cells that follow it; intersect with the "
      "boundary to fill a volume",
      {in("frames", P::Frames), in("thickness", P::Scalar, num(1))}, {out("field", P::Field)},
      [](NodeContext& ctx) {
        const double t = ctx.scalar("thickness");
        if (!check_size(ctx, "thickness", t)) return;
        const Frames& frames = ctx.frames("frames");
        if (frames->frames.size() < 2) {
          ctx.infeasible({"frames"}, fmt::format("Voronoi cells need at least 2 seeds; there are {}",
                                                 frames->frames.size()));
          return;
        }
        std::vector<std::array<float, 3>> at;
        at.reserve(frames->frames.size());
        for (const auto& f : frames->frames) {
          at.push_back({f.translation.x, f.translation.y, f.translation.z});
        }
        std::sort(at.begin(), at.end());
        if (std::adjacent_find(at.begin(), at.end()) != at.end()) {
          ctx.infeasible({"frames"}, "two seeds are at the same place, so their cells have no wall "
                                     "between them; space them apart (scatter's spacing)");
          return;
        }
        ctx.set("field", voronoi_field(*frames, t));
      }));

  r.add(make("field.thicken", "field",
             "A wall `thickness` thick centred on a field's surface (turns a surface or a sheet's "
             "mid-surface into a solid)",
             {in("field", P::Field), in("thickness", P::Scalar, num(1))}, {out("field", P::Field)},
             [](NodeContext& ctx) {
               const double t = ctx.scalar("thickness");
               if (!check_size(ctx, "thickness", t)) return;
               ctx.set("field", offset_field(abs_field(ctx.field("field")), 0.5 * t));
             }));

  r.add(make("field.clamp", "field", "Clamps a field's values to [min, max] (for grading fields)",
             {in("field", P::Field), in("min", P::Scalar, num(0)), in("max", P::Scalar, num(1))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               const double lo = ctx.scalar("min");
               const double hi = ctx.scalar("max");
               if (!(lo <= hi) || !std::isfinite(lo) || !std::isfinite(hi)) {
                 ctx.infeasible({"min", "max"}, fmt::format("min {} is above max {}", lo, hi));
                 return;
               }
               ctx.set("field", clamp_field(ctx.field("field"), lo, hi));
             }));

  r.add(make(
      "field.instances", "instance",
      "Places a template field at every frame (their union): a small parametric template, "
      "built from field nodes and parameters. `radius` (mm), when set, promises the template "
      "lies within that distance of its origin, which lets far copies be skipped",
      {in("template", P::Field), in("frames", P::Frames), in("radius", P::Scalar, num(0))},
      {out("field", P::Field)}, [](NodeContext& ctx) {
        const double radius = ctx.scalar("radius");
        if (!(radius >= 0.0) || !std::isfinite(radius)) {
          ctx.infeasible({"radius"}, "the radius can't be negative");
          return;
        }
        const Frames& frames = ctx.frames("frames");
        if (frames->frames.empty()) {
          ctx.infeasible({"frames"}, "there are no frames to place the template at");
          return;
        }
        ctx.set("field", instances_field(ctx.field("template"), *frames, radius));
      }));

  r.add(make("output.publish", "output",
             "Publishes a number as the volume's published output of quantity 'published' with "
             "the same name (a count, say)",
             {in("value", P::Scalar, num(0))}, {}, [](NodeContext& ctx) {
               if (!std::isfinite(ctx.scalar("value"))) {
                 ctx.infeasible({"value"}, "a published value must be finite");
               }
             },
             {{"name", PropertyKind::Name, std::nullopt, {}, "the published output's name"}}));
}

}  // namespace vs::proc::detail
