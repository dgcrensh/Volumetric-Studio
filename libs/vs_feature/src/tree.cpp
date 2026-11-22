#include "vs/feature/tree.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

namespace vs::feature {

std::string_view to_string(PrincipalPlane plane) {
  switch (plane) {
    case PrincipalPlane::XY: return "xy";
    case PrincipalPlane::YZ: return "yz";
    case PrincipalPlane::ZX: return "zx";
  }
  return "xy";
}

std::string_view to_string(PrincipalAxis axis) {
  switch (axis) {
    case PrincipalAxis::X: return "x";
    case PrincipalAxis::Y: return "y";
    case PrincipalAxis::Z: return "z";
  }
  return "x";
}

std::string_view to_string(Operation op) { return op == Operation::Add ? "add" : "cut"; }

std::string_view to_string(Extent extent) {
  switch (extent) {
    case Extent::Blind: return "blind";
    case Extent::ThroughAll: return "through_all";
    case Extent::UpToPlane: return "up_to_plane";
    case Extent::Symmetric: return "symmetric";
  }
  return "blind";
}

std::string_view to_string(FeatureKind kind) {
  switch (kind) {
    case FeatureKind::Sketch: return "Sketch";
    case FeatureKind::DatumPlane: return "Plane";
    case FeatureKind::DatumAxis: return "Axis";
    case FeatureKind::DatumPoint: return "Point";
    case FeatureKind::Extrude: return "Extrude";
    case FeatureKind::Revolve: return "Revolve";
    case FeatureKind::Sweep: return "Sweep";
    case FeatureKind::Loft: return "Loft";
    case FeatureKind::Shell: return "Shell";
    case FeatureKind::Fillet: return "Fillet";
    case FeatureKind::Chamfer: return "Chamfer";
    case FeatureKind::LinearPattern: return "Linear pattern";
    case FeatureKind::CircularPattern: return "Circular pattern";
  }
  return "Feature";
}

std::string_view to_string(SweepOrientation orientation) {
  return orientation == SweepOrientation::Fixed ? "fixed" : "follow_path";
}

FeatureKind Feature::kind() const {
  return static_cast<FeatureKind>(definition.index());
}

Id FeatureTree::add(FeatureDefinition definition, std::string name, Id id) {
  Feature f;
  if (id.is_null()) {
    if (const auto* s = std::get_if<SketchFeature>(&definition)) id = s->sketch.id();
  }
  f.id = id;
  f.name = std::move(name);
  f.definition = std::move(definition);
  return insert(features_.size(), std::move(f));
}

Id FeatureTree::insert(std::size_t index, Feature feature) {
  if (feature.id.is_null() || find(feature.id)) feature.id = Id::random();
  const Id id = feature.id;
  index = std::min(index, features_.size());
  features_.insert(features_.begin() + static_cast<std::ptrdiff_t>(index), std::move(feature));
  return id;
}

std::vector<Id> FeatureTree::append(std::vector<Feature> features) {
  std::unordered_set<Id> used;
  used.reserve(features_.size() + features.size());
  for (const auto& f : features_) used.insert(f.id);
  std::vector<Id> ids;
  ids.reserve(features.size());
  features_.reserve(features_.size() + features.size());
  for (auto& f : features) {
    if (f.id.is_null()) {
      if (const auto* s = std::get_if<SketchFeature>(&f.definition)) f.id = s->sketch.id();
    }
    if (f.id.is_null() || used.contains(f.id)) f.id = Id::random();
    used.insert(f.id);
    ids.push_back(f.id);
    features_.push_back(std::move(f));
  }
  return ids;
}

bool FeatureTree::remove(const Id& id) {
  const auto i = index_of(id);
  if (!i) return false;
  features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(*i));
  return true;
}

bool FeatureTree::move(const Id& id, std::size_t index) {
  const auto i = index_of(id);
  if (!i) return false;
  Feature f = std::move(features_[*i]);
  features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(*i));
  index = std::min(index, features_.size());
  features_.insert(features_.begin() + static_cast<std::ptrdiff_t>(index), std::move(f));
  return true;
}

bool FeatureTree::set_suppressed(const Id& id, bool suppressed) {
  Feature* f = find(id);
  if (!f) return false;
  f->suppressed = suppressed;
  return true;
}

const Feature* FeatureTree::find(const Id& id) const {
  for (const auto& f : features_) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

Feature* FeatureTree::find(const Id& id) {
  for (auto& f : features_) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

std::optional<std::size_t> FeatureTree::index_of(const Id& id) const {
  for (std::size_t i = 0; i < features_.size(); ++i) {
    if (features_[i].id == id) return i;
  }
  return std::nullopt;
}

sketch::Sketch* FeatureTree::sketch(const Id& feature) {
  Feature* f = find(feature);
  if (!f) return nullptr;
  auto* s = f->as<SketchFeature>();
  return s ? &s->sketch : nullptr;
}

const sketch::Sketch* FeatureTree::sketch(const Id& feature) const {
  const Feature* f = find(feature);
  if (!f) return nullptr;
  const auto* s = f->as<SketchFeature>();
  return s ? &s->sketch : nullptr;
}

std::string FeatureTree::label(const Id& id) const {
  const Feature* target = find(id);
  if (!target) return fmt::format("(missing feature {})", id.to_string().substr(0, 8));
  if (!target->name.empty()) return target->name;
  if (const auto* s = target->as<SketchFeature>(); s && !s->sketch.name().empty()) {
    return s->sketch.name();
  }
  int n = 0;
  for (const auto& f : features_) {
    if (f.kind() == target->kind()) ++n;
    if (f.id == id) break;
  }
  return fmt::format("{} {}", to_string(target->kind()), n);
}

core::SubjectRef FeatureTree::subject(const Id& id) const {
  return {core::SubjectKind::Feature, id, label(id)};
}

std::vector<core::SubjectRef> FeatureTree::subjects() const {
  // label() names the first feature with an id, counting the features of
  // its kind up to it; a repeated id takes its first feature's label.
  std::vector<core::SubjectRef> out;
  out.reserve(features_.size());
  std::map<FeatureKind, int> counts;
  std::unordered_map<Id, std::size_t> first;
  for (const auto& f : features_) {
    const int n = ++counts[f.kind()];
    const auto [it, fresh] = first.emplace(f.id, out.size());
    if (!fresh) {
      out.push_back(out[it->second]);
      continue;
    }
    std::string text;
    if (!f.name.empty()) {
      text = f.name;
    } else if (const auto* s = f.as<SketchFeature>(); s && !s->sketch.name().empty()) {
      text = s->sketch.name();
    } else {
      text = fmt::format("{} {}", to_string(f.kind()), n);
    }
    out.push_back({core::SubjectKind::Feature, f.id, std::move(text)});
  }
  return out;
}

Diagnostics FeatureTree::validate() const {
  Diagnostics out;
  std::unordered_set<Id> seen;
  for (const auto& f : features_) {
    if (!seen.insert(f.id).second) {
      out.add(Diagnostic::error("feature.tree.duplicate_id",
                                fmt::format("Two features share the id {}", f.id.to_string()),
                                {subject(f.id)}));
    }
  }
  out.append(variables_.check());
  return out;
}

// --- references_of --------------------------------------------------------------

namespace {

class ReferenceCollector {
 public:
  FeatureReferences out;

  void id(const Id& id) {
    if (id.is_null()) return;
    if (std::find(out.features.begin(), out.features.end(), id) == out.features.end()) {
      out.features.push_back(id);
    }
  }
  void entity(const SketchEntityRef& e) {
    id(e.sketch);
    if (std::find(out.sketch_entities.begin(), out.sketch_entities.end(), e) ==
        out.sketch_entities.end()) {
      out.sketch_entities.push_back(e);
    }
  }
  void expression(const std::string& text) {
    if (text.empty()) return;
    auto e = Expression::parse(text);
    if (!e) return;
    for (const auto& name : e->references()) names_.insert(name);
  }
  void param(const Param& p) { expression(p.expression); }

  void plane(const PlaneRef& ref) {
    if (const auto* f = std::get_if<FeatureRef>(&ref)) id(f->feature);
    if (const auto* f = std::get_if<FaceRef>(&ref)) id(f->feature);
  }
  void axis(const AxisRef& ref) {
    if (const auto* f = std::get_if<FeatureRef>(&ref)) id(f->feature);
    if (const auto* e = std::get_if<SketchEntityRef>(&ref)) entity(*e);
  }
  void point(const PointRef& ref) {
    if (const auto* f = std::get_if<FeatureRef>(&ref)) id(f->feature);
    if (const auto* e = std::get_if<SketchEntityRef>(&ref)) entity(*e);
  }
  void face(const FaceRef& ref) { id(ref.feature); }
  void edge(const EdgeRef& ref) {
    face(ref.face);
    if (ref.other) face(*ref.other);
  }

  void visit(const SketchFeature& f) {
    plane(f.plane);
    for (const auto& c : f.sketch.constraints()) {
      if (c.dimension) expression(c.dimension->expression);
    }
  }
  void visit(const DatumPlaneFeature& f) {
    std::visit(
        [this](const auto& d) {
          using T = std::decay_t<decltype(d)>;
          if constexpr (std::is_same_v<T, PlaneOffset>) {
            plane(d.base);
            param(d.distance);
          } else if constexpr (std::is_same_v<T, PlaneThroughPoints>) {
            point(d.a);
            point(d.b);
            point(d.c);
          } else {
            axis(d.axis);
            plane(d.base);
            param(d.angle);
          }
        },
        f.definition);
  }
  void visit(const DatumAxisFeature& f) {
    std::visit(
        [this](const auto& d) {
          using T = std::decay_t<decltype(d)>;
          if constexpr (std::is_same_v<T, AxisThroughPoints>) {
            point(d.a);
            point(d.b);
          } else if constexpr (std::is_same_v<T, AxisPlaneIntersection>) {
            plane(d.a);
            plane(d.b);
          } else {
            axis(d.axis);
          }
        },
        f.definition);
  }
  void visit(const DatumPointFeature& f) {
    std::visit(
        [this](const auto& d) {
          using T = std::decay_t<decltype(d)>;
          if constexpr (std::is_same_v<T, PointAt>) {
            param(d.x);
            param(d.y);
            param(d.z);
          } else if constexpr (std::is_same_v<T, PointAtRef>) {
            point(d.point);
          } else {
            axis(d.axis);
            plane(d.plane);
          }
        },
        f.definition);
  }
  void visit(const ExtrudeFeature& f) {
    id(f.sketch);
    param(f.depth);
    if (f.extent == Extent::UpToPlane) plane(f.up_to);
    param(f.draft);
  }
  void visit(const RevolveFeature& f) {
    id(f.sketch);
    axis(f.axis);
    param(f.angle);
  }
  void visit(const SweepFeature& f) {
    id(f.profile);
    id(f.path);
  }
  void visit(const LoftFeature& f) {
    for (const auto& s : f.sections) id(s);
  }
  void visit(const ShellFeature& f) {
    for (const auto& r : f.open) face(r);
    param(f.thickness);
  }
  void visit(const FilletFeature& f) {
    for (const auto& e : f.edges) edge(e);
    param(f.radius);
  }
  void visit(const ChamferFeature& f) {
    for (const auto& e : f.edges) edge(e);
    param(f.distance);
  }
  void visit(const LinearPatternFeature& f) {
    for (const auto& s : f.features) id(s);
    axis(f.direction);
    param(f.count);
    param(f.spacing);
  }
  void visit(const CircularPatternFeature& f) {
    for (const auto& s : f.features) id(s);
    axis(f.axis);
    param(f.count);
    param(f.angle);
  }

  FeatureReferences finish() {
    out.variables.assign(names_.begin(), names_.end());
    return std::move(out);
  }

 private:
  std::set<std::string> names_;
};

}  // namespace

FeatureReferences references_of(const Feature& feature) {
  ReferenceCollector c;
  std::visit([&c](const auto& def) { c.visit(def); }, feature.definition);
  return c.finish();
}

std::vector<TreeId> tree_ids(const FeatureTree& tree) {
  std::vector<TreeId> out;
  for (const auto& v : tree.variables().variables()) {
    out.push_back({v.id, TreeIdKind::Variable, {}, tree.variables().subject(v.id)});
  }
  const auto subjects = tree.subjects();
  for (std::size_t i = 0; i < tree.features().size(); ++i) {
    const auto& f = tree.features()[i];
    out.push_back({f.id, TreeIdKind::Feature, f.id, subjects[i]});
    const auto* s = f.as<SketchFeature>();
    if (s == nullptr) continue;
    if (!(s->sketch.id() == f.id)) {
      out.push_back({s->sketch.id(), TreeIdKind::Sketch, f.id, subjects[i]});
    }
    for (const auto& e : s->sketch.entities()) {
      out.push_back({e.id, TreeIdKind::SketchEntity, f.id, s->sketch.subject(e.id)});
    }
    for (const auto& c : s->sketch.constraints()) {
      out.push_back({c.id, TreeIdKind::Constraint, f.id, s->sketch.subject(c.id)});
    }
  }
  return out;
}

std::optional<TreeId> find_tree_id(const FeatureTree& tree, const Id& id) {
  if (id.is_null()) return std::nullopt;
  if (tree.variables().find(id)) {
    return TreeId{id, TreeIdKind::Variable, {}, tree.variables().subject(id)};
  }
  for (const auto& f : tree.features()) {
    if (f.id == id) return TreeId{id, TreeIdKind::Feature, f.id, tree.subject(f.id)};
    const auto* s = f.as<SketchFeature>();
    if (s == nullptr) continue;
    if (s->sketch.id() == id) return TreeId{id, TreeIdKind::Sketch, f.id, tree.subject(f.id)};
    if (s->sketch.find_entity(id)) {
      return TreeId{id, TreeIdKind::SketchEntity, f.id, s->sketch.subject(id)};
    }
    if (s->sketch.find_constraint(id)) {
      return TreeId{id, TreeIdKind::Constraint, f.id, s->sketch.subject(id)};
    }
  }
  return std::nullopt;
}

}  // namespace vs::feature
