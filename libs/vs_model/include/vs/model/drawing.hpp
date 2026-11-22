#pragma once

// Drawings in the document (L6, decision 0038): helpers over the data in
// document.hpp. vs_draw computes what a drawing shows from an evaluation;
// here are only the parts that need nothing built.
//
// check() (check.hpp) refuses drawings that name nothing or the wrong kind
// of thing (`model.drawing.*`); prune_drawings() drops what an edit
// removed, so deleting a volume or a surface is never refused because a
// drawing showed or dimensioned it.

#include <cstddef>

#include "vs/model/document.hpp"

namespace vs::model {

// Removes, from every drawing, the volumes a view shows that are no longer
// in the document (or are generated), and the dimensions whose view is
// gone or whose anchors name a volume or surface that is gone. Returns how
// many items it removed.
std::size_t prune_drawings(Document& document);

// A new drawing with a random id, A3 landscape and no views.
Drawing make_drawing(std::string name);

}  // namespace vs::model
