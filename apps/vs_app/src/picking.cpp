#include "picking.hpp"

#include <cmath>
#include <variant>

#include "vs/kernel/queries.hpp"

namespace vs::app {

namespace {

using core::dvec3;

constexpr double kCosTolerance = 1e-6;  // about 0.08 degrees

bool parallel(const dvec3& a, const dvec3& b) { return std::abs(dot(normalize(a), normalize(b))) > 1.0 - kCosTolerance; }

double distance_to_line(const dvec3& p, const kernel::Axis& axis) {
  const dvec3 d = normalize(axis.direction);
  const dvec3 v = p - axis.origin;
  return length(v - d * dot(v, d));
}

std::optional<NamedSurface> from_named(const feature::NamedFace& f) {
  NamedSurface s;
  s.plane = f.plane;
  s.cylinder = f.cylinder;
  return s;
}

const feature::NamedFace* find_named(const std::vector<feature::NamedFace>& faces, const std::string& name) {
  for (const auto& f : faces) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

// The face of `item` a face index names, if the item has faces.
const DisplayMesh::FacePart* face_part(const SceneItem& item, std::size_t index) {
  if (index >= item.mesh.faces.size()) return nullptr;
  return &item.mesh.faces[index];
}

// Names one face of `item`: the first face choice on its surface.
std::optional<Choice<feature::FaceRef>> name_face(const SceneItem& item, std::size_t index,
                                                  const eval::Evaluation& evaluation, const Id& tree_volume,
                                                  const ReferenceChoices& choices, std::string& why) {
  if (item.role == ItemRole::ConnectionSurface) {
    const feature::FaceRef ref{item.subject, "patch"};
    for (const auto& c : choices.faces) {
      if (c.ref == ref) return c;
    }
    why = "That connection surface doesn't join the volume being edited.";
    return std::nullopt;
  }
  const auto* part = face_part(item, index);
  if (part == nullptr) {
    why = "Only faces of modelled solids can be picked; generated contents have none.";
    return std::nullopt;
  }
  const auto surface = face_surface(part->face);
  if (!surface) {
    why = "That face is neither planar nor cylindrical, so no reference names it.";
    return std::nullopt;
  }
  for (const auto& c : choices.faces) {
    const auto named = named_surface(evaluation, tree_volume, c.ref);
    if (named && same_surface(*named, *surface)) return c;
  }
  why = "No face reference offered here lies on that face (it may belong to a feature further down).";
  return std::nullopt;
}

}  // namespace

std::vector<std::pair<Id, ItemRole>> pickable_items(const eval::Evaluation& evaluation, const Id& context_volume) {
  std::vector<std::pair<Id, ItemRole>> out;
  const model::Document& doc = evaluation.document;
  if (context_volume.is_null()) {
    for (const auto& v : doc.volumes) out.emplace_back(v.id, ItemRole::Boundary);
    for (const auto& f : doc.layout.features()) out.emplace_back(f.id, ItemRole::LayoutSolid);
    return out;
  }
  out.emplace_back(context_volume, ItemRole::Boundary);
  out.emplace_back(context_volume, ItemRole::Contents);
  for (const auto& s : doc.connections) {
    if (s.joins(context_volume)) out.emplace_back(s.id, ItemRole::ConnectionSurface);
  }
  return out;
}

std::optional<NamedSurface> named_surface(const eval::Evaluation& evaluation, const Id& tree_volume,
                                          const feature::FaceRef& ref) {
  const model::Document& doc = evaluation.document;
  // A connection surface's patch.
  if (ref.name == "patch") {
    if (const auto* cb = evaluation.build.find_connection(ref.feature); cb != nullptr && cb->ok) {
      NamedSurface s;
      s.plane.origin = cb->frame.origin;
      s.plane.normal = cb->frame.normal();
      if (cb->cylinder) {  // L2: a curved patch
        s.cylinder = kernel::Cylinder{{cb->frame.origin, cb->frame.normal()}, cb->cylinder->radius, false};
      }
      s.patch = true;
      return s;
    }
  }
  // The boundary of the volume being edited: "<feature>:<name>" or "-x".."+z".
  if (!tree_volume.is_null() && ref.feature == tree_volume) {
    if (const auto* vb = evaluation.build.find_volume(tree_volume)) {
      if (const auto* f = find_named(vb->boundary_faces, ref.name)) return from_named(*f);
    }
    return std::nullopt;
  }
  // A feature of the tree being edited.
  const feature::RebuildResult* rebuild = rebuild_of(evaluation, tree_volume);
  if (rebuild == nullptr) return std::nullopt;
  const auto* r = rebuild->find(ref.feature);
  if (r == nullptr || !r->ok()) return std::nullopt;
  if (const auto* f = find_named(r->faces, ref.name)) return from_named(*f);
  (void)doc;
  return std::nullopt;
}

bool same_surface(const NamedSurface& a, const NamedSurface& b, double tolerance) {
  if (a.cylinder.has_value() != b.cylinder.has_value()) return false;
  if (a.cylinder) {
    const auto& ca = *a.cylinder;
    const auto& cb = *b.cylinder;
    return parallel(ca.axis.direction, cb.axis.direction) && distance_to_line(cb.axis.origin, ca.axis) < tolerance &&
           std::abs(ca.radius - cb.radius) < tolerance &&
           (ca.inward == cb.inward || a.patch || b.patch);  // a patch faces both ways
  }
  const dvec3 na = normalize(a.plane.normal), nb = normalize(b.plane.normal);
  const double c = dot(na, nb);
  const bool facing = c > 1.0 - kCosTolerance || ((a.patch || b.patch) && c < -1.0 + kCosTolerance);
  if (!facing) return false;
  return std::abs(dot(b.plane.origin - a.plane.origin, na)) < tolerance;
}

std::optional<NamedSurface> face_surface(const kernel::Face& face) {
  NamedSurface s;
  if (const auto c = kernel::face_cylinder(face)) {
    s.cylinder = *c;
    return s;
  }
  if (const auto p = kernel::face_plane(face)) {
    s.plane = *p;
    return s;
  }
  return std::nullopt;
}

PickResolution resolve_pick(const Scene& scene, const eval::Evaluation& evaluation, const Id& tree_volume,
                            const ReferenceChoices& choices, const PickedPart& part) {
  PickResolution out;
  const SceneItem* item = scene.find(part.subject, part.role);
  if (item == nullptr) {
    out.why = "That item is no longer shown.";
    return out;
  }
  if (part.kind == PickedPart::Kind::Face) {
    const auto named = name_face(*item, part.index, evaluation, tree_volume, choices, out.why);
    if (!named) return out;
    out.face = named->ref;
    out.label = named->label;
    if (item->role == ItemRole::ConnectionSurface) {
      // As a plane, a connection surface is offered as the surface itself.
      const feature::PlaneRef plane = feature::FeatureRef{item->subject};
      for (const auto& c : choices.planes) {
        if (c.ref == plane) out.plane = plane;
      }
    } else {
      const feature::PlaneRef plane = named->ref;
      for (const auto& c : choices.planes) {
        if (c.ref == plane) out.plane = plane;
      }
    }
    return out;
  }
  // An edge: the two faces it lies between.
  if (part.index >= item->mesh.edge_faces.size()) {
    out.why = "Only edges of modelled solids can be picked.";
    return out;
  }
  const auto [fa, fb] = item->mesh.edge_faces[part.index];
  if (fa < 0) {
    out.why = "That edge belongs to no face.";
    return out;
  }
  const auto a = name_face(*item, static_cast<std::size_t>(fa), evaluation, tree_volume, choices, out.why);
  if (!a) return out;
  if (fb < 0 || fb == fa) {
    out.why = "That edge is the seam of a closed face; pick an edge where two faces meet.";
    return out;
  }
  const auto b = name_face(*item, static_cast<std::size_t>(fb), evaluation, tree_volume, choices, out.why);
  if (!b) return out;
  if (a->ref == b->ref) {
    out.why = "Both sides of that edge lie on the same face reference.";
    return out;
  }
  out.edge = feature::EdgeRef{a->ref, b->ref};
  out.label = a->label + " / " + b->label;
  out.why.clear();
  return out;
}

}  // namespace vs::app
