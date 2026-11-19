#include "vs/feature/tree.hpp"

#include <algorithm>

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

Diagnostics FeatureTree::validate() const {
  Diagnostics out;
  for (std::size_t i = 0; i < features_.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (features_[i].id == features_[j].id) {
        out.add(Diagnostic::error(
            "feature.tree.duplicate_id",
            fmt::format("Two features share the id {}", features_[i].id.to_string()),
            {subject(features_[i].id)}));
        break;
      }
    }
  }
  out.append(variables_.check());
  return out;
}

}  // namespace vs::feature
