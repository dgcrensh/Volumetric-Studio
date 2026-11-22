#include "vs/model/drawing.hpp"

#include <algorithm>

namespace vs::model {

namespace {

bool anchor_exists(const Document& doc, const Anchor& a) {
  switch (a.on) {
    case AnchorOn::Origin: return true;
    case AnchorOn::VolumeBoundary:
    case AnchorOn::VolumeContents: {
      const Volume* v = doc.find_volume(a.target);
      return v != nullptr && !v->generated;
    }
    case AnchorOn::Surface: return doc.find_connection(a.target) != nullptr;
  }
  return false;
}

}  // namespace

std::size_t prune_drawings(Document& document) {
  std::size_t removed = 0;
  for (auto& d : document.drawings) {
    for (auto& v : d.views) {
      const auto before = v.volumes.size();
      std::erase_if(v.volumes, [&](const Id& id) {
        const Volume* vol = document.find_volume(id);
        return vol == nullptr || vol->generated;
      });
      removed += before - v.volumes.size();
    }
    const auto before = d.dimensions.size();
    std::erase_if(d.dimensions, [&](const DrawingDimension& m) {
      if (!d.find_view(m.view)) return true;
      if (!anchor_exists(document, m.from)) return true;
      return m.kind != DimensionKind::Diameter && !anchor_exists(document, m.to);
    });
    removed += before - d.dimensions.size();
  }
  return removed;
}

Drawing make_drawing(std::string name) {
  Drawing d;
  d.id = Id::random();
  d.name = std::move(name);
  return d;
}

}  // namespace vs::model
