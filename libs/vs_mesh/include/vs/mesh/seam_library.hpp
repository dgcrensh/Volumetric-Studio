#pragma once

// The seam library: one Seam per connection surface of a built document,
// the single source every mesher and the weld use (spec, "Seam
// conformity"; plan, M7).
//
// Which surfaces get a seam. Bonded and bolted-flange surfaces: the two
// volumes meet on the patch, so both must mesh it identically and the
// weld joins them there. A surface joining one volume and the outside gets
// one too, so the volume's mesh still follows the published division.
// Clearance surfaces get none: the volumes stay `gap` apart, so their
// faces never meet and nothing is welded across them.
//
// The outline is the built patch's boundary (model::ConnectionBuild::patch,
// read with kernel::face_loops()) in the surface's frame. A bolted flange
// adds each bolt's ISO 273 clearance hole as a circular hole loop, because
// the holes go through the faces both volumes share. Spacing:
// ConnectionSurface::seam.spacing when positive, else derive_spacing().
//
// Codes (the surface first), besides make_seam()'s:
//   mesh.seam.curve      an outline edge is neither a line nor a circular
//                        arc in the surface's plane
//   mesh.seam.thread     a bolted flange's thread size is unknown

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/mesh/seam.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::mesh {

struct SeamVertexRef {
  std::uint32_t seam = 0;    // index into SeamLibrary::seams
  std::uint32_t vertex = 0;  // index into that seam's vertices
};

struct SeamLibrary {
  std::vector<Seam> seams;  // in document order
  // Labels for volumes, for weld diagnostics (empty when built by hand).
  std::map<core::Id, std::string> volume_names;
  core::Diagnostics diagnostics;

  [[nodiscard]] const Seam* find(const core::Id& surface) const;
  [[nodiscard]] std::optional<SeamVertexRef> find_vertex(const core::Id& vertex) const;
  [[nodiscard]] core::SubjectRef volume_subject(const core::Id& volume) const;

  // Adds a seam (after the seams already there) and indexes its vertices.
  void add(Seam seam);

 private:
  std::unordered_map<core::Id, SeamVertexRef> vertices_;
};

// The exact outline of a built connection surface (see above).
core::Result<SeamOutline> seam_outline(const model::ConnectionSurface& surface,
                                       const model::ConnectionBuild& build);

// One seam for every surface that needs one and was built; surfaces whose
// build failed are skipped (model::build() has already said why).
SeamLibrary build_seams(const model::Document& document, const model::DocumentBuild& build,
                        const core::ProductSettings& settings);

}  // namespace vs::mesh
