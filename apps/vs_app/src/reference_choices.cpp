#include "reference_choices.hpp"

#include "vs/model/hierarchy.hpp"

#include <fmt/format.h>

#include <cctype>
#include <variant>

namespace vs::app {

namespace {

using feature::FaceRef;
using feature::FeatureRef;
using feature::SketchEntityRef;

// The sketch whose curves name a solid feature's side faces.
Id profile_sketch(const feature::Feature& f) {
  if (const auto* e = f.as<feature::ExtrudeFeature>()) return e->sketch;
  if (const auto* r = f.as<feature::RevolveFeature>()) return r->sketch;
  if (const auto* s = f.as<feature::SweepFeature>()) return s->profile;
  return {};
}

// "end", "side Line 3", "copy 2 of Boss end".
std::string face_label(const feature::FeatureTree& tree, const Id& feature, const std::string& name) {
  const feature::Feature* f = tree.find(feature);
  if (name.rfind("side:", 0) == 0 && f != nullptr) {
    if (const auto curve = Id::parse(name.substr(5))) {
      if (const auto* sk = tree.sketch(profile_sketch(*f)); sk != nullptr && sk->find_entity(*curve) != nullptr) {
        return "side " + sk->label(*curve);
      }
    }
    return "side";
  }
  // Pattern copies: "<k>:<feature id>:<name>".
  const auto a = name.find(':');
  if (a != std::string::npos) {
    const auto b = name.find(':', a + 1);
    if (b != std::string::npos) {
      if (const auto seed = Id::parse(name.substr(a + 1, b - a - 1)); seed && tree.find(*seed) != nullptr) {
        return fmt::format("copy {} of {} {}", name.substr(0, a), tree.label(*seed),
                           face_label(tree, *seed, name.substr(b + 1)));
      }
    }
  }
  return name;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool is_solid_feature(const feature::Feature& f) {
  switch (f.kind()) {
    case feature::FeatureKind::Extrude:
    case feature::FeatureKind::Revolve:
    case feature::FeatureKind::Sweep:
    case feature::FeatureKind::Loft: return true;
    default: return false;
  }
}

// The features of `tree` above `index`, with what they offer.
void add_tree(const feature::FeatureTree& tree, const feature::RebuildResult* rebuild,
              std::optional<std::size_t> index, ReferenceChoices& out) {
  const auto& features = tree.features();
  const std::size_t end = index ? std::min(*index, features.size()) : features.size();
  for (std::size_t i = 0; i < end; ++i) {
    const feature::Feature& f = features[i];
    const std::string label = tree.label(f.id);
    switch (f.kind()) {
      case feature::FeatureKind::Sketch: {
        out.sketches.push_back({label, f.id});
        out.planes.push_back({label + " (plane)", FeatureRef{f.id}});
        const auto& sk = f.as<feature::SketchFeature>()->sketch;
        for (const auto& e : sk.entities()) {
          if (e.kind() == sketch::EntityKind::Line) {
            out.axes.push_back({label + ": " + sk.label(e.id), SketchEntityRef{f.id, e.id}});
          } else if (e.kind() == sketch::EntityKind::Point) {
            out.points.push_back({label + ": " + sk.label(e.id), SketchEntityRef{f.id, e.id}});
          }
        }
        break;
      }
      case feature::FeatureKind::DatumPlane: out.planes.push_back({label, FeatureRef{f.id}}); break;
      case feature::FeatureKind::DatumAxis: out.axes.push_back({label, FeatureRef{f.id}}); break;
      case feature::FeatureKind::DatumPoint: out.points.push_back({label, FeatureRef{f.id}}); break;
      default: break;
    }
    if (is_solid_feature(f)) out.solid_features.push_back({label, f.id});
    if (rebuild != nullptr) {
      if (const auto* r = rebuild->find(f.id); r != nullptr && r->ok()) {
        for (const auto& face : r->faces) {
          const std::string fl = label + " " + face_label(tree, f.id, face.name);
          out.faces.push_back({fl, FaceRef{f.id, face.name}});
          if (!face.curved()) out.planes.push_back({fl, FaceRef{f.id, face.name}});
        }
      }
    }
  }
}

template <typename Ref>
std::string find_label(const std::vector<Choice<Ref>>& choices, const Ref& ref) {
  for (const auto& c : choices) {
    if (c.ref == ref) return c.label;
  }
  return {};
}

}  // namespace

std::string ReferenceChoices::label_of(const feature::PlaneRef& ref) const { return find_label(planes, ref); }
std::string ReferenceChoices::label_of(const feature::AxisRef& ref) const { return find_label(axes, ref); }
std::string ReferenceChoices::label_of(const feature::PointRef& ref) const { return find_label(points, ref); }
std::string ReferenceChoices::label_of(const feature::FaceRef& ref) const { return find_label(faces, ref); }

const feature::FeatureTree* tree_of(const model::Document& document, const Id& volume) {
  if (volume.is_null()) return &document.layout;
  const model::Volume* v = document.find_volume(volume);
  return v != nullptr ? v->tree() : nullptr;
}

feature::FeatureTree* tree_of(model::Document& document, const Id& volume) {
  if (volume.is_null()) return &document.layout;
  model::Volume* v = document.find_volume(volume);
  return v != nullptr ? v->tree() : nullptr;
}

const feature::RebuildResult* rebuild_of(const eval::Evaluation& evaluation, const Id& volume) {
  if (volume.is_null()) return &evaluation.build.layout;
  if (const auto* r = evaluation.find(volume); r != nullptr && r->contents && r->contents->rebuild) {
    return &*r->contents->rebuild;
  }
  if (const auto* vb = evaluation.build.find_volume(volume); vb != nullptr && vb->contents_rebuild) {
    return &*vb->contents_rebuild;
  }
  return nullptr;
}

ReferenceChoices reference_choices(const eval::Evaluation& evaluation, const Id& volume,
                                   std::optional<std::size_t> index) {
  ReferenceChoices out;
  const model::Document& doc = evaluation.document;
  if (volume.is_null()) {
    for (const auto p : {feature::PrincipalPlane::XY, feature::PrincipalPlane::YZ, feature::PrincipalPlane::ZX}) {
      out.planes.push_back({upper(feature::to_string(p)) + " plane", p});
    }
    for (const auto a : {feature::PrincipalAxis::X, feature::PrincipalAxis::Y, feature::PrincipalAxis::Z}) {
      out.axes.push_back({upper(feature::to_string(a)) + " axis", a});
    }
    add_tree(doc.layout, &evaluation.build.layout, index, out);
    for (const auto& v : doc.layout.variables().variables()) out.variables.push_back(v.name);
    return out;
  }

  const model::Volume* v = doc.find_volume(volume);
  if (v == nullptr) return out;
  // The boundary's named faces: "<layout feature id>:<name>" or "-x".."+z".
  if (const auto* vb = evaluation.build.find_volume(volume)) {
    for (const auto& face : vb->boundary_faces) {
      std::string what = face.name;
      if (const auto colon = face.name.find(':'); colon != std::string::npos) {
        // The boundary's features are in the layout of its level (L3).
        const feature::FeatureTree* lay = model::level_layout(doc, v->parent);
        if (const auto fid = Id::parse(face.name.substr(0, colon)); fid && lay != nullptr && lay->find(*fid) != nullptr) {
          what = lay->label(*fid) + " " + face_label(*lay, *fid, face.name.substr(colon + 1));
        }
      }
      const std::string label = "Boundary: " + what;
      out.faces.push_back({label, FaceRef{volume, face.name}});
      if (!face.curved()) out.planes.push_back({label, FaceRef{volume, face.name}});
    }
  }
  for (const auto& s : doc.connections) {
    if (!model::joins(doc, s, volume)) continue;
    const std::string label = "Surface: " + s.name;
    out.planes.push_back({label, FeatureRef{s.id}});
    out.faces.push_back({label + " patch", FaceRef{s.id, "patch"}});
  }
  if (const auto* tree = v->tree()) {
    add_tree(*tree, rebuild_of(evaluation, volume), index, out);
    for (const auto& var : tree->variables().variables()) out.variables.push_back(var.name);
  }
  for (const auto& p : v->parameters) out.variables.push_back(p.name);
  return out;
}

ReferenceChoices layout_choices(const eval::Evaluation& evaluation, const Id& level) {
  return reference_choices(evaluation, level, std::nullopt);
}

}  // namespace vs::app
