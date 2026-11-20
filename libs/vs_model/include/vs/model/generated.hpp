#pragma once

// Generated layouts (L4): what a composite's generator emits, and how it
// lands in a document.
//
// A composite with a generator (CompositeContents::generator, a vs_proc
// graph) produces a GeneratedLayout: child volumes as axis-aligned boxes in
// model coordinates, each filled from one of the composite's templates;
// connection surfaces between two of them, as rectangles on axis-aligned
// planes; and which generated child meets each surface that joins the
// composite from outside. It is plain data, so vs_model can hold it
// without knowing graphs (vs_proc makes it; vs_eval expands it).
//
// expand() writes a layout into a copy of the document exactly as if it
// had been authored at the composite's level:
//
//   - for each child, two datum points appended to the composite's layout
//     and a volume with a box boundary between them, `parent` the
//     composite, `generated` set. The template's contents, parameters and
//     outputs are copied with every id in them derived from the child's
//     (the template's own id, which its tree uses for its boundary,
//     becoming the child's), a parametric copy offset from the template's
//     `origin` to the child's min corner (Volume::contents_offset), and
//     parameters the generator sets take its
//     values;
//   - for each surface, a datum plane appended to the composite's layout
//     and a datum patch on it joining the two children;
//   - each delegation appended to the composite's.
//
// Every id is derived (Id::derive) from the composite's and the
// generator's key for the child or surface, so the same layout always
// expands to the same document, and a child keeps its id (and its cache
// entry) while the generator keeps its key. Generated volumes are appended
// after every authored one, so authored volumes keep their indices.
//
// Codes (the composite first):
//   model.generated.not_generator  the volume isn't a composite with a generator
//   model.generated.template       a child names no template of the composite
//   model.generated.parameter      a child sets a parameter its template doesn't have
//   model.generated.child          a child's box is empty or not finite
//   model.generated.surface        a surface or delegation names a child that
//                                  isn't there, or its rectangle is empty

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/model/document.hpp"

namespace vs::model {

struct GeneratedChild {
  Id key;  // stable across regenerations: the child's id derives from it
  std::string name;
  std::string template_name;  // empty: a placeholder
  core::dbox3 box;            // model coordinates
  std::map<std::string, double, std::less<>> parameters;  // by the template's names
  std::uint64_t seed = 0;
};

// A rectangle on the plane where `axis` (0: x, 1: y, 2: z) equals
// `offset`, spanning `lo` to `hi` along the plane's other two model axes
// in increasing order (x: y then z; y: z then x; z: x then y), as the
// principal planes' frames run.
struct GeneratedSurface {
  Id key;
  std::string name;
  std::size_t a = 0, b = 0;  // children, by index in GeneratedLayout::children
  ConnectionType type = ConnectionType::Bonded;  // bonded or clearance
  double gap = 0.2;                              // clearance only
  int axis = 0;
  double offset = 0.0;
  std::array<double, 2> lo{}, hi{};
};

// The generated child that meets `surface`, a connection surface joining
// the composite from outside.
struct GeneratedDelegation {
  Id surface;
  std::size_t child = 0;
};

struct GeneratedLayout {
  std::vector<GeneratedChild> children;
  std::vector<GeneratedSurface> surfaces;
  std::vector<GeneratedDelegation> delegations;
};

void hash_append(core::Hasher& h, const GeneratedLayout& layout);

// The id expand() gives the child or surface with this key.
[[nodiscard]] Id generated_child_id(const Id& composite, const Id& key);
[[nodiscard]] Id generated_surface_id(const Id& composite, const Id& key);

// `document` with `layout` written into composite `composite`'s level.
core::Result<Document> expand(const Document& document, const Id& composite,
                              const GeneratedLayout& layout);

// `contents` of a template copied for the volume `child`: every id in it
// derived from `child`, `template_id` becoming `child` itself.
TemplateContents instantiate(const TemplateContents& contents, const Id& template_id,
                             const Id& child);

// The composite whose generator made `volume`; null when it isn't generated.
[[nodiscard]] Id generator_of(const Document& document, const Id& volume);

}  // namespace vs::model
