#pragma once

// The tree of volumes (L3): levels, children and how connection surfaces
// pass down through composites.
//
// A **level** is the root (a null id) or a composite volume (its id). Each
// level has a layout (the root layout, or the composite's own feature
// tree) and owns its children: the volumes whose `parent` it is, and the
// connection surfaces, keep-outs and links between them, whose geometry and
// expressions are built from that layout. With no composites, every volume
// is at the root level and everything here reduces to what the MVP did.
//
// A connection surface **joins** a volume when it names it, or when it
// joins the volume's composite parent and the parent delegates it to that
// volume (CompositeContents::delegations): a composite passes a surface it
// receives down to the child lying against it, and that child takes the
// same surface object as a fixed input. Contents' inputs (parametric
// trees, procedures, cache keys, seams) all go by joins(); `seam_sides()`
// gives the volumes whose contents actually meet a surface.
//
// Every function here tolerates a malformed document (a missing parent, a
// loop of parents): check() reports those as model.volume.parent, and
// these functions stop walking instead of looping.

#include <cstddef>
#include <string>
#include <vector>

#include "vs/model/document.hpp"

namespace vs::model {

// The layout of `level`: the document's layout for the root (null), a
// composite's own tree; null when `level` isn't a composite volume.
[[nodiscard]] const feature::FeatureTree* level_layout(const Document& document, const Id& level);

// The volumes whose parent is `level`, as indices in document order.
[[nodiscard]] std::vector<std::size_t> children(const Document& document, const Id& level);

// The parents of `volume`, nearest first, not including the root. Stops
// at a missing parent or a loop.
[[nodiscard]] std::vector<Id> ancestors(const Document& document, const Id& volume);

// True when `level` is the root, or a strict ancestor of `volume`.
[[nodiscard]] bool is_within(const Document& document, const Id& volume, const Id& level);

// How deep `volume` sits: 0 directly under the root.
[[nodiscard]] std::size_t depth(const Document& document, const Id& volume);

// Of the volumes at `level`, the one that is `volume` or holds it; null when
// `volume` isn't under `level`.
[[nodiscard]] Id child_toward(const Document& document, const Id& level, const Id& volume);

// Every volume reachable from the root through valid parents, parents
// before their children (by depth, then document order), as indices.
// Volumes under a missing or non-composite parent, or in a loop, are left
// out.
[[nodiscard]] std::vector<std::size_t> level_order(const Document& document);

// The level a surface, keep-out or link belongs to: the parent of the
// (first) volume it names, or of the link's source. Null (the root) when
// that volume is missing.
[[nodiscard]] Id level_of(const Document& document, const ConnectionSurface& surface);
[[nodiscard]] Id level_of(const Document& document, const KeepOut& keep_out);
[[nodiscard]] Id level_of(const Document& document, const Link& link);

// The child a composite passes `surface` down to; null when it doesn't (or
// `composite` isn't one).
[[nodiscard]] Id delegate_of(const Document& document, const Id& composite, const Id& surface);

// Whether `surface` is an input of `volume`: it names the volume, or
// joins its parent, which delegates it to the volume.
[[nodiscard]] bool joins(const Document& document, const ConnectionSurface& surface,
                         const Id& volume);

// The connection surfaces joining `volume` (joins()), in document order.
[[nodiscard]] std::vector<const ConnectionSurface*> joining_surfaces(const Document& document,
                                                                     const Id& volume);

// The volumes whose contents meet `surface`: each volume it names, followed
// down its delegations as far as they go. A composite that doesn't delegate
// it stays in the list (and has no contents of its own to meet it with).
[[nodiscard]] std::vector<Id> seam_sides(const Document& document,
                                         const ConnectionSurface& surface);

// A volume's name with its parents', from the top: "Housing / Carrier".
[[nodiscard]] std::string volume_path(const Document& document, const Id& volume,
                                      std::string_view separator = " / ");

}  // namespace vs::model
