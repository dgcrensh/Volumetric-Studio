#include "dimension_drag.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

#include "reference_choices.hpp"
#include "sketch_editor.hpp"
#include "vs/feature/expression.hpp"
#include "vs/feature/variables.hpp"

namespace vs::app {

namespace {

using core::dvec2;
using core::dvec3;
using sketch::ConstraintKind;

dvec2 unit(dvec2 v) {
  const double l = std::sqrt(v.x * v.x + v.y * v.y);
  return l > 0 ? dvec2{v.x / l, v.y / l} : dvec2{1, 0};
}

core::Diagnostics error(std::string code, std::string message) {
  core::Diagnostics d;
  d.add(core::Diagnostic::error(std::move(code), std::move(message)));
  return d;
}

// Whether `expression` is exactly one variable's name.
bool is_name(const std::string& expression) { return feature::is_valid_name(expression); }

std::string feature_label(const feature::FeatureTree& tree, const Id& id) { return tree.label(id); }

}  // namespace

std::optional<SketchHandle> sketch_dimension_handle(const sketch::Sketch& sk, const sketch::Constraint& c) {
  if (!c.dimension) return std::nullopt;
  SketchHandle h;
  switch (c.kind) {
    case ConstraintKind::Distance:
      if (c.refs.size() == 1) {
        const auto l = sketch::line_geometry(sk, c.refs[0]);
        if (!l) return std::nullopt;
        h.anchor = (l->start + l->end) * 0.5;
        h.direction = unit(l->end - l->start);
        return h;
      }
      if (c.refs.size() == 2) {
        const auto a = sk.point(c.refs[0]);
        if (!a) return std::nullopt;
        if (const auto b = sk.point(c.refs[1])) {
          h.anchor = (*a + *b) * 0.5;
          h.direction = unit(*b - *a);
          return h;
        }
        if (const auto l = sketch::line_geometry(sk, c.refs[1])) {
          // From the line out to the point.
          const dvec2 d = unit(l->end - l->start);
          const dvec2 v = *a - l->start;
          const dvec2 foot = l->start + d * (v.x * d.x + v.y * d.y);
          h.anchor = *a;
          h.direction = unit(*a - foot);
          return h;
        }
      }
      return std::nullopt;
    case ConstraintKind::HorizontalDistance:
    case ConstraintKind::VerticalDistance: {
      if (c.refs.size() != 2) return std::nullopt;
      const auto a = sk.point(c.refs[0]), b = sk.point(c.refs[1]);
      if (!a || !b) return std::nullopt;
      h.anchor = (*a + *b) * 0.5;
      h.direction = c.kind == ConstraintKind::HorizontalDistance ? dvec2{1, 0} : dvec2{0, 1};
      return h;
    }
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter: {
      if (c.refs.size() != 1) return std::nullopt;
      double angle = 0.25 * 3.14159265358979;
      dvec2 centre{};
      double radius = 0;
      if (const auto circle = sketch::circle_geometry(sk, c.refs[0])) {
        centre = circle->center;
        radius = circle->radius;
      } else if (const auto arc = sketch::arc_geometry(sk, c.refs[0])) {
        centre = arc->center;
        radius = arc->radius;
        angle = (arc->start_angle + arc->end_angle) / 2;
      } else {
        return std::nullopt;
      }
      h.direction = {std::cos(angle), std::sin(angle)};
      h.anchor = centre + h.direction * radius;
      h.rate = c.kind == ConstraintKind::Diameter ? 2.0 : 1.0;
      return h;
    }
    default: return std::nullopt;
  }
}

std::vector<DimensionHandle> dimension_handles(const eval::Evaluation& evaluation, const Id& tree_id, const Id& feature) {
  std::vector<DimensionHandle> out;
  const feature::FeatureTree* tree = tree_of(evaluation.document, tree_id);
  const feature::RebuildResult* rebuild = rebuild_of(evaluation, tree_id);
  if (tree == nullptr || rebuild == nullptr) return out;
  const feature::Feature* f = tree->find(feature);
  if (f == nullptr) return out;
  const auto& values = rebuild->variables.values;

  // What drives a number: a variable (a bare name), the number itself, or
  // nothing draggable (a longer expression, or a name that isn't one of
  // this tree's variables).
  auto drive = [&](DimensionHandle& h, const std::string& expression, double value) {
    h.value = value;
    if (expression.empty()) return true;
    if (!is_name(expression)) return false;
    const feature::Variable* var = tree->variables().find(expression);
    if (var == nullptr) return false;
    const auto it = values.find(expression);
    if (it == values.end()) return false;
    h.target = DimensionHandle::Target::Variable;
    h.variable = var->id;
    h.value = it->second;
    return true;
  };

  auto sketch_handles = [&](const Id& sketch_feature) {
    const feature::Feature* sf = tree->find(sketch_feature);
    const auto* r = rebuild->find(sketch_feature);
    if (sf == nullptr || sf->as<feature::SketchFeature>() == nullptr || r == nullptr || !r->ok() || !r->sketch) return;
    const sketch::Sketch& placed = *r->sketch;
    const sketch::SketchPlane& plane = placed.plane();
    for (const auto& c : placed.constraints()) {
      const auto sh = sketch_dimension_handle(placed, c);
      if (!sh) continue;
      DimensionHandle h;
      h.target = DimensionHandle::Target::SketchDimension;
      h.tree = tree_id;
      h.feature = sketch_feature;
      h.constraint = c.id;
      if (!drive(h, c.dimension->expression, c.dimension->value)) continue;
      h.rate = sh->rate;
      h.signed_value = signed_dimension(c.kind);
      h.anchor = plane.to_model(sh->anchor);
      h.direction = normalize(plane.to_model(sh->anchor + sh->direction) - h.anchor);
      h.label = feature_label(*tree, sketch_feature) + ": " + placed.label(c.id);
      if (h.target == DimensionHandle::Target::Variable) h.label += " (" + c.dimension->expression + ")";
      out.push_back(std::move(h));
    }
  };

  if (f->as<feature::SketchFeature>() != nullptr) {
    sketch_handles(feature);
    return out;
  }
  if (const auto* e = f->as<feature::ExtrudeFeature>()) {
    const auto* r = rebuild->find(feature);
    const auto* sr = rebuild->find(e->sketch);
    if (r != nullptr && r->ok() && sr != nullptr && sr->sketch &&
        (e->extent == feature::Extent::Blind || e->extent == feature::Extent::Symmetric)) {
      const feature::NamedFace* end = nullptr;
      for (const auto& nf : r->faces) {
        if (nf.name == "end") end = &nf;
      }
      // The profile's middle, on the sketch plane.
      const sketch::Sketch& placed = *sr->sketch;
      dvec2 sum{};
      int n = 0;
      for (const auto& ent : placed.entities()) {
        if (const auto* p = ent.as<sketch::Point>(); p != nullptr && !ent.construction) {
          sum = sum + p->position;
          ++n;
        }
      }
      const dvec3 middle = placed.plane().to_model(n > 0 ? sum * (1.0 / n) : dvec2{});
      DimensionHandle h;
      h.target = DimensionHandle::Target::FeatureParam;
      h.tree = tree_id;
      h.feature = feature;
      if (drive(h, e->depth.expression, e->depth.value)) {
        dvec3 dir = placed.plane().normal();
        if (e->reverse) dir = dir * -1.0;
        const bool add = !r->operation || *r->operation == feature::Operation::Add;
        if (end != nullptr) dir = normalize(add ? end->plane.normal : end->plane.normal * -1.0);
        const bool symmetric = e->extent == feature::Extent::Symmetric;
        h.rate = symmetric ? 2.0 : 1.0;
        h.direction = dir;
        h.anchor = middle + dir * (symmetric ? h.value / 2 : h.value);
        if (end != nullptr) h.anchor = middle + dir * dot(end->plane.origin - middle, dir);
        h.label = feature_label(*tree, feature) + ": depth";
        if (h.target == DimensionHandle::Target::Variable) h.label += " (" + e->depth.expression + ")";
        out.push_back(std::move(h));
      }
    }
    sketch_handles(e->sketch);
  }
  return out;
}

double dragged_value(const DimensionHandle& handle, double distance, double minimum) {
  const double value = handle.value + handle.rate * distance;
  return handle.signed_value ? value : std::max(minimum, value);
}

Command drag_command(const DimensionHandle& h, double value, core::ProductSettings settings) {
  const std::string label = "Drag " + h.label;
  switch (h.target) {
    case DimensionHandle::Target::Variable:
      return {label, [h, value](model::Project& p) -> core::Diagnostics {
                feature::FeatureTree* tree = tree_of(p.document, h.tree);
                if (tree == nullptr) return error("app.edit.no_tree", "That volume has no feature tree to edit.");
                const feature::Variable* var = tree->variables().find(h.variable);
                if (var == nullptr) return error("app.edit.no_variable", "That variable is no longer there.");
                return tree->variables().set_expression(h.variable, fmt::format("{:.10g} mm", value));
              }};
    case DimensionHandle::Target::SketchDimension:
      return {label, [h, value, settings](model::Project& p) -> core::Diagnostics {
                feature::FeatureTree* tree = tree_of(p.document, h.tree);
                feature::Feature* f = tree != nullptr ? tree->find(h.feature) : nullptr;
                auto* sf = f != nullptr ? f->as<feature::SketchFeature>() : nullptr;
                if (sf == nullptr) return error("app.edit.no_feature", "That feature is no longer there.");
                // Solved from where the sketch is: M2's drag solving moves
                // as little as it can.
                SketchEditor editor(sf->sketch, settings);
                core::Diagnostics d = editor.set_dimension(h.constraint, value);
                if (d.has_errors()) return d;
                sf->sketch = editor.sketch();
                return {};
              }};
    case DimensionHandle::Target::FeatureParam:
      return {label, [h, value](model::Project& p) -> core::Diagnostics {
                feature::FeatureTree* tree = tree_of(p.document, h.tree);
                feature::Feature* f = tree != nullptr ? tree->find(h.feature) : nullptr;
                auto* e = f != nullptr ? f->as<feature::ExtrudeFeature>() : nullptr;
                if (e == nullptr) return error("app.edit.no_feature", "That feature is no longer there.");
                e->depth = feature::Param::of(value);
                return {};
              }};
  }
  return {label, [](model::Project&) { return core::Diagnostics{}; }};
}

}  // namespace vs::app
