#pragma once

// Face and edge picking in the viewport (M16, deferred from M14 by decision
// 0014). Plain data and rules, no Qt and no OCCT.
//
// B-rep items of the scene (boundaries, layout solids, parametric contents)
// keep each face's triangle range and each drawn edge's neighbouring faces
// (DisplayMesh::faces, DisplayMesh::edge_faces), so the viewport can pick a
// face or an edge as a part of an item. Generated and imported meshes have
// no faces and stay unpickable.
//
// A picked face is named as the feature dialogs name faces: the face's
// surface (its plane and outward normal, or its cylinder) is matched with
// the surfaces the offered references lie on, in the order the reference
// lists offer them (reference_choices.hpp). Since a FaceRef resolves by
// geometry (vs_feature rebuild.hpp), two references on the same surface,
// facing the same way, name the same faces of the body: the first in the
// list is the one a pick gives. A connection surface's patch names itself
// (FaceRef{surface, "patch"}; as a plane, FeatureRef{surface}). A picked
// edge is the pair of faces it lies between, each named as above: the
// EdgeRef a fillet or chamfer takes.
//
// What can be picked follows edit in context (decision 0014): in the layout,
// every boundary and layout solid; inside a volume, only that volume's
// boundary and B-rep contents and the connection surfaces joining it, so a
// pick can never name something the lists wouldn't offer.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "reference_choices.hpp"
#include "scene.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/rebuild.hpp"

namespace vs::app {

// A part of a scene item under the cursor.
struct PickedPart {
  enum class Kind { Face, Edge };
  Id subject;
  ItemRole role = ItemRole::Boundary;
  Kind kind = Kind::Face;
  std::size_t index = 0;  // into the item's DisplayMesh::faces or ::edges
  friend bool operator==(const PickedPart&, const PickedPart&) = default;
};

// What a pick names, for each kind of field that can take it. Empty fields
// (and `why`) when the pick names nothing the lists offer.
struct PickResolution {
  std::optional<feature::FaceRef> face;
  std::optional<feature::PlaneRef> plane;  // planar faces and patches only
  std::optional<feature::EdgeRef> edge;    // edge picks only
  std::string label;                       // as the lists label it
  std::string why;                         // why nothing resolved
  [[nodiscard]] bool ok() const { return face.has_value() || edge.has_value(); }
};

// The items a pick may land on: (subject, role) pairs. In the layout
// (`context_volume` null) every boundary and layout solid; inside a volume
// its boundary, its contents and the connection surfaces joining it.
std::vector<std::pair<Id, ItemRole>> pickable_items(const eval::Evaluation& evaluation, const Id& context_volume);

// The surface a named face lies on, in this evaluation: a plane (normal out
// of the material), a cylinder, or for any other surface (an extruded
// Bézier's side, decision 0040) sample points and normals on it. Nullopt
// when the reference doesn't resolve to a built face.
struct NamedSurface {
  kernel::Plane plane;
  std::optional<kernel::Cylinder> cylinder;
  std::vector<feature::SurfacePoint> samples;  // set: neither plane nor cylinder
  bool patch = false;  // a connection surface's patch: either side matches
};
std::optional<NamedSurface> named_surface(const eval::Evaluation& evaluation, const Id& tree_volume,
                                          const feature::FaceRef& ref);

// Whether two surfaces are the same (and face the same way), within
// `tolerance` mm and a matching angle tolerance. Sampled surfaces are the
// same when their samples are; neither is ever a plane or cylinder.
bool same_surface(const NamedSurface& a, const NamedSurface& b, double tolerance = 1e-4);

// The surface a face of a displayed B-rep lies on (nullopt for faces that
// are neither planar nor cylindrical).
std::optional<NamedSurface> face_surface(const kernel::Face& face);

// Names a picked part for a feature in `tree_volume`'s tree (null: the
// layout), choosing among `choices` (reference_choices() for that feature).
PickResolution resolve_pick(const Scene& scene, const eval::Evaluation& evaluation, const Id& tree_volume,
                            const ReferenceChoices& choices, const PickedPart& part);

}  // namespace vs::app
