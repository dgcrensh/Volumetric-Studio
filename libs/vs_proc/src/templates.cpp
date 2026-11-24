#include "vs/proc/templates.hpp"

#include <map>
#include <utility>

#include <fmt/format.h>

namespace vs::proc {

using core::Diagnostic;

namespace {

// Builds a graph whose node ids derive from the template's name.
class Builder {
 public:
  explicit Builder(std::string_view name)
      : ns_(Id::derive(Id::derive(Id::null(), "vstudio.template"), name)) {}

  void parameter(std::string name, double value) {
    graph_.parameters.push_back({std::move(name), value});
  }

  Id node(std::string type, std::string name,
          std::map<std::string, Setting, std::less<>> settings = {}) {
    Node n;
    n.id = Id::derive(ns_, name);
    n.type = std::move(type);
    n.name = std::move(name);
    n.settings = std::move(settings);
    graph_.nodes.push_back(std::move(n));
    return graph_.nodes.back().id;
  }

  // A parameter node reading `name`.
  Id read(const std::string& name) {
    return node("input.parameter", name, {{"name", Setting(name)}});
  }

  void link(const Id& from, std::string from_port, const Id& to, std::string to_port) {
    graph_.link(from, std::move(from_port), to, std::move(to_port));
  }

  Graph finish() { return std::move(graph_); }

 private:
  Id ns_;
  Graph graph_;
};

// boundary shelled by the `wall` parameter; returns the shell node.
Id shell(Builder& b, const Id& boundary) {
  const Id wall = b.read("wall");
  const Id s = b.node("field.shell", "Skin");
  b.link(boundary, "field", s, "field");
  b.link(wall, "value", s, "thickness");
  return s;
}

// (infill ∩ boundary) ∪ skin, to the output.
void finish(Builder& b, const Id& boundary, const Id& skin, const Id& infill,
            const TemplateOptions& options) {
  const Id clip = b.node("field.intersection", "Infill in boundary");
  b.link(infill, "field", clip, "a");
  b.link(boundary, "field", clip, "b");
  const Id all = b.node("field.union", "Skin and infill");
  b.link(skin, "field", all, "a");
  b.link(clip, "field", all, "b");
  const Id out = b.node("output", "Contents", {{"cell_size", Setting(options.cell_size)}});
  b.link(all, "field", out, "contents");
}

Graph gyroid_infill(const TemplateOptions& o) {
  Builder b("gyroid_infill");
  b.parameter("wall", 1.2);
  b.parameter("cell", 8.0);
  b.parameter("thickness", 0.8);
  const Id boundary = b.node("input.boundary", "Boundary");
  const Id skin = shell(b, boundary);
  const Id gyroid = b.node("field.tpms", "Gyroid", {{"surface", Setting(std::string("gyroid"))}});
  b.link(b.read("cell"), "value", gyroid, "cell_size");
  b.link(b.read("thickness"), "value", gyroid, "thickness");
  finish(b, boundary, skin, gyroid, o);
  return b.finish();
}

Graph graded_lattice(const TemplateOptions& o) {
  Builder b("graded_lattice");
  b.parameter("wall", 1.2);
  b.parameter("cell", 6.0);
  b.parameter("thick", 1.6);
  b.parameter("thin", 0.8);
  b.parameter("ramp", 6.0);
  const Id boundary = b.node("input.boundary", "Boundary");
  const Id skin = shell(b, boundary);
  const Id thick = b.read("thick");
  const Id thin = b.read("thin");
  // grade = clamp(thick - (thick - thin) / ramp * distance, thin, thick)
  const Id span = b.node("math.subtract", "Thick less thin");
  b.link(thick, "value", span, "a");
  b.link(thin, "value", span, "b");
  const Id slope = b.node("math.divide", "Per mm");
  b.link(span, "value", slope, "a");
  b.link(b.read("ramp"), "value", slope, "b");
  const Id down = b.node("math.negate", "Thinner inward");
  b.link(slope, "value", down, "a");
  const Id distance = b.node("query.distance_to_boundary", "Depth");
  const Id scaled = b.node("field.scale", "Depth times slope");
  b.link(distance, "distance", scaled, "field");
  b.link(down, "value", scaled, "factor");
  const Id base = b.node("field.constant", "Thick everywhere");
  b.link(thick, "value", base, "value");
  const Id sum = b.node("field.add", "Thickness");
  b.link(base, "field", sum, "a");
  b.link(scaled, "field", sum, "b");
  const Id grade = b.node("field.clamp", "Thickness clamped");
  b.link(sum, "field", grade, "field");
  b.link(thin, "value", grade, "min");
  b.link(thick, "value", grade, "max");
  const Id lattice = b.node("field.lattice", "Lattice", {{"cell", Setting(std::string("bcc"))}});
  b.link(b.read("cell"), "value", lattice, "cell_size");
  b.link(grade, "field", lattice, "grade");
  finish(b, boundary, skin, lattice, o);
  return b.finish();
}

Graph bolt_bosses(const TemplateOptions& o) {
  Builder b("bolt_bosses");
  b.parameter("wall", 1.2);
  b.parameter("boss_radius", 4.0);
  b.parameter("boss_height", 8.0);
  const Id boundary = b.node("input.boundary", "Boundary");
  const Id skin = shell(b, boundary);
  const Id flange =
      b.node("input.surface", "Flange", {{"surface", Setting(o.surface->to_string())}});
  const Id radius = b.read("boss_radius");
  const Id height = b.read("boss_height");
  // A cylinder centred on the hole, reaching boss_height to each side of
  // the flange: the boundary clips the outer half.
  const Id twice = b.node("math.multiply", "Both sides", {{"b", Setting(2.0)}});
  b.link(height, "value", twice, "a");
  const Id boss = b.node("field.cylinder", "Boss");
  b.link(radius, "value", boss, "radius");
  b.link(twice, "value", boss, "height");
  const Id reach = b.node("math.add", "Reach");
  b.link(radius, "value", reach, "a");
  b.link(height, "value", reach, "b");
  const Id bosses = b.node("field.instances", "Bosses");
  b.link(boss, "field", bosses, "template");
  b.link(flange, "holes", bosses, "frames");
  b.link(reach, "value", bosses, "radius");
  const Id count = b.node("frames.count", "Holes");
  b.link(flange, "holes", count, "frames");
  const Id publish = b.node("output.publish", "Boss count",
                            {{"name", Setting(std::string("boss_count"))}});
  b.link(count, "count", publish, "value");
  finish(b, boundary, skin, bosses, o);
  return b.finish();
}

Graph voronoi_walls(const TemplateOptions& o) {
  Builder b("voronoi_walls");
  b.parameter("wall", 1.0);
  b.parameter("count", 200.0);
  b.parameter("spacing", 0.0);
  b.parameter("depth", 2.0);
  b.parameter("layers", 1.0);
  b.parameter("step", 5.0);
  b.parameter("thickness", 0.8);
  const Id boundary = b.node("input.boundary", "Boundary");
  const Id skin = shell(b, boundary);
  // Seeds on the surface, moved into the volume along its normal, so the
  // cells follow its shape; layers stack further in.
  const Id seeds = b.node("scatter.surface", "Seeds", {{"surface", Setting(o.surface->to_string())}});
  b.link(b.read("count"), "value", seeds, "count");
  b.link(b.read("spacing"), "value", seeds, "spacing");
  const Id inward = b.node("frames.offset", "Inward");
  b.link(seeds, "frames", inward, "frames");
  b.link(b.read("depth"), "value", inward, "distance");
  b.link(b.read("layers"), "value", inward, "layers");
  b.link(b.read("step"), "value", inward, "step");
  const Id thickness = b.read("thickness");
  b.link(thickness, "value", inward, "margin");
  const Id cells = b.node("field.voronoi", "Cells");
  b.link(inward, "frames", cells, "frames");
  b.link(thickness, "value", cells, "thickness");
  finish(b, boundary, skin, cells, o);
  return b.finish();
}

}  // namespace

const std::vector<TemplateInfo>& templates() {
  static const std::vector<TemplateInfo> list{
      {"gyroid_infill", "Gyroid infill with a shell: wall, cell, thickness", false},
      {"graded_lattice",
       "BCC lattice with a shell, struts graded from thick at the skin to thin inside: wall, "
       "cell, thick, thin, ramp",
       false},
      {"bolt_bosses",
       "A shell with a solid boss round each bolt hole of a bolted flange; publishes "
       "boss_count: wall, boss_radius, boss_height",
       true},
      {"voronoi_walls",
       "A shell with Voronoi cell walls seeded on a connection surface and moved inward, so the "
       "cells follow it: wall, count, spacing, depth, layers, step, thickness",
       true},
  };
  return list;
}

core::Result<Graph> make_template(std::string_view name, const TemplateOptions& options) {
  if (name == "gyroid_infill") return gyroid_infill(options);
  if (name == "graded_lattice") return graded_lattice(options);
  if (name == "bolt_bosses") {
    if (!options.surface) {
      return Diagnostic::error("proc.template.surface",
                               "bolt_bosses reads a bolted flange: give its surface id");
    }
    return bolt_bosses(options);
  }
  if (name == "voronoi_walls") {
    if (!options.surface) {
      return Diagnostic::error("proc.template.surface",
                               "voronoi_walls seeds its cells on a connection surface: give its id");
    }
    return voronoi_walls(options);
  }
  return Diagnostic::error("proc.template.unknown",
                           fmt::format("There's no template '{}'", name));
}

}  // namespace vs::proc
