#pragma once

// Layout checks: what the parent verifies about its children's boundaries
// and connection surfaces before anything generates (spec, "Validation and
// diagnostics"). They need only the built layout (model::build()), never
// contents.
//
// Overlap. Every pair of sibling boundaries whose boxes meet is intersected
// in FP64 (kernel common, fuzzy at the linear tolerance). A region deeper
// than the tolerance is an error naming both boundaries, the region's box,
// its volume and its depth. The depth is the smallest side of the region's
// bounding box: for the box-like overlaps layouts make, how far one volume
// must move to clear the other. Penetration up to the tolerance counts as
// touching (spec, "Structural adjacency"). Keep-out volumes take part like
// any other: a keep-out that sits in a volume without cutting it overlaps it.
//
// Undeclared contact. Siblings that share a bonded or bolted connection
// surface are adjacent by definition, with no distance test; a clearance
// surface keeps its volumes apart, so it excuses no contact. For every other
// pair (keep-out
// volumes aside: touching one is what it is for) that comes within the
// tolerance, each pair of faces lying against each other over an area is a
// warning naming both faces, by the boundary's face names (model build.hpp:
// "<feature>:<name>" or "-x".."+z"; "face <k>" for a face with no name, such
// as one a keep-out cut). Pairs that touch only along an edge or at a point
// get one warning for the pair.
//
// Connection type rules, per surface, naming the surface and the parameters
// in conflict:
//   every type     the patch must lie on each joined volume's boundary (for
//                  a clearance, within its gap of it), and no point of it
//                  may lie inside the volume farther than that from its
//                  surface (a patch floating inside or poking through; H2)
//   clearance      the gap must be at least the minimum feature size
//   bolted flange  the minimum wall must be at least the minimum feature
//                  size; every clearance hole (ISO 273, medium fit) must sit
//                  inside the patch with the minimum wall to its edge, and
//                  neighbouring holes must leave the minimum wall between them
//
// Codes (registry.hpp has severities and routing):
//   validate.overlap                    error    both boundaries
//   validate.contact                    warning  both boundaries, faces in the labels
//   validate.contact.edge               warning  both boundaries
//   validate.connection.off_boundary    error    surface, then the boundary
//   validate.connection.gap             error    surface
//   validate.connection.min_wall        error    surface
//   validate.connection.edge_wall       error    surface
//   validate.connection.hole_spacing    error    surface
//   validate.undecided                  error    the kernel couldn't decide a check
//
// A boundary or surface that didn't build is left out of these checks;
// model::build() already reported why.

#include <string>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/settings.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/validate/parallel.hpp"

namespace vs::validate {

using core::dbox3;
using core::Diagnostic;
using core::Diagnostics;
using core::Id;
using core::SubjectRef;
using core::dvec3;
using core::ProductSettings;

struct Overlap {
  Id a, b;           // volumes, in document order
  dbox3 region;      // the overlap's bounding box
  double volume = 0; // mm³
  double depth = 0;  // mm, the region box's smallest side
};

struct Contact {
  Id a, b;              // volumes, in document order
  std::string face_a;   // face names (see above); empty for an edge or point touch
  std::string face_b;
  double area = 0;      // mm², 0 for an edge or point touch
  dbox3 region;         // where they touch
};


struct LayoutReport {
  std::vector<Overlap> overlaps;
  std::vector<Contact> contacts;
  Diagnostics diagnostics;  // overlaps, contacts, then connection rules, in that order
  // Pairs of boundaries whose boxes met, so were looked at more closely.
  std::size_t pairs_tested = 0;
};

// The checks above. `build` must come from model::build(document, ...).
// `parallel` (optional) runs the pairs of boundaries and the connection
// surfaces in fixed chunks on several threads, each chunk on its own deep
// copies of the boundaries it reads; the report is the same either way.
LayoutReport validate_layout(const model::Document& document, const model::DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel = {});

// The connection type rules alone.
Diagnostics check_connections(const model::Document& document, const model::DocumentBuild& build,
                              const ProductSettings& settings, const ParallelFor& parallel = {});

}  // namespace vs::validate
