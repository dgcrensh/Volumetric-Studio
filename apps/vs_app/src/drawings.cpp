#include "drawings.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "authoring.hpp"
#include "vs/draw/drawing.hpp"
#include "vs/draw/view.hpp"

namespace vs::app {

namespace {

using model::Anchor;
using model::AnchorOn;
using model::AnchorPart;
using model::DimensionKind;

core::Diagnostics error(std::string code, std::string message) {
  core::Diagnostics out;
  out.add(core::Diagnostic::error(std::move(code), std::move(message)));
  return out;
}

core::Diagnostics no_drawing() { return error("app.edit.no_drawing", "That drawing is no longer there."); }
core::Diagnostics no_view() { return error("app.edit.no_view", "That view is no longer there."); }
core::Diagnostics no_dimension() {
  return error("app.edit.no_dimension", "That dimension is no longer there.");
}

model::DrawingView* find_view(model::Drawing& d, const Id& id) {
  for (auto& v : d.views) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

model::DrawingDimension* find_dimension(model::Drawing& d, const Id& id) {
  for (auto& m : d.dimensions) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

bool parallel(const core::dvec3& a, const core::dvec3& b) {
  const double l = core::length(a) * core::length(b);
  return l > 0 && std::abs(dot(a, b)) >= (1 - 1e-9) * l;
}

std::string part_text(const Anchor& a) {
  switch (a.part) {
    case AnchorPart::Min:
      return "min";
    case AnchorPart::Max:
      return "max";
    case AnchorPart::Centre:
      return "centre";
    case AnchorPart::Face:
      return "face " + a.face;
    case AnchorPart::Hole:
      return "hole " + std::to_string(a.hole + 1);
  }
  return {};
}

std::string volume_name(const model::Document& doc, const Id& id) {
  const model::Volume* v = doc.find_volume(id);
  return v ? v->name : std::string("(missing volume)");
}

// A contents face "<feature id>:<name>" as "<feature label> <name>".
std::string contents_face_text(const model::Document& doc, const Anchor& a) {
  const model::Volume* v = doc.find_volume(a.target);
  const auto colon = a.face.find(':');
  if (v != nullptr && v->tree() != nullptr && colon != std::string::npos) {
    const auto feature = Id::parse(a.face.substr(0, colon));
    if (feature && v->tree()->find(*feature) != nullptr) {
      return "face " + v->tree()->label(*feature) + " " + a.face.substr(colon + 1);
    }
  }
  return part_text(a);
}

}  // namespace

// --- commands ---------------------------------------------------------------------------------

Command add_drawing(model::Drawing drawing) {
  if (drawing.id.is_null()) drawing.id = Id::random();
  std::string label = "Add drawing " + drawing.name;
  return {std::move(label), [drawing = std::move(drawing)](model::Project& p) -> core::Diagnostics {
            std::vector<std::string> taken;
            for (const auto& d : p.document.drawings) taken.push_back(d.name);
            model::Drawing d = drawing;
            d.name = unique_name(d.name.empty() ? std::string("Drawing") : d.name, taken);
            p.document.drawings.push_back(std::move(d));
            return {};
          }};
}

Command rename_drawing(Id drawing, std::string name) {
  return {"Rename drawing", [drawing, name = std::move(name)](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            d->name = name;
            return {};
          }};
}

Command remove_drawing(Id drawing) {
  return {"Delete drawing", [drawing](model::Project& p) -> core::Diagnostics {
            if (std::erase_if(p.document.drawings, [&](const model::Drawing& d) { return d.id == drawing; }) ==
                0) {
              return no_drawing();
            }
            return {};
          }};
}

Command set_sheet(Id drawing, double width, double height, int decimals) {
  return {"Edit sheet", [=](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            d->width = width;
            d->height = height;
            d->decimals = decimals;
            return {};
          }};
}

Command add_view(Id drawing, model::DrawingView view) {
  if (view.id.is_null()) view.id = Id::random();
  return {"Add view", [drawing, view = std::move(view)](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            std::vector<std::string> taken;
            for (const auto& v : d->views) taken.push_back(v.name);
            model::DrawingView v = view;
            v.name = unique_name(v.name.empty() ? std::string(model::to_string(v.direction)) : v.name, taken);
            d->views.push_back(std::move(v));
            return {};
          }};
}

Command update_view(Id drawing, model::DrawingView view) {
  return {"Edit view", [drawing, view = std::move(view)](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            model::DrawingView* v = find_view(*d, view.id);
            if (v == nullptr) return no_view();
            *v = view;
            return {};
          }};
}

Command remove_view(Id drawing, Id view) {
  return {"Delete view", [drawing, view](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            if (std::erase_if(d->views, [&](const model::DrawingView& v) { return v.id == view; }) == 0) {
              return no_view();
            }
            std::erase_if(d->dimensions, [&](const model::DrawingDimension& m) { return m.view == view; });
            return {};
          }};
}

Command add_dimension(Id drawing, model::DrawingDimension dimension) {
  if (dimension.id.is_null()) dimension.id = Id::random();
  return {"Add dimension", [drawing, dimension = std::move(dimension)](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            d->dimensions.push_back(dimension);
            return {};
          }};
}

Command update_dimension(Id drawing, model::DrawingDimension dimension) {
  return {"Edit dimension", [drawing, dimension = std::move(dimension)](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            model::DrawingDimension* m = find_dimension(*d, dimension.id);
            if (m == nullptr) return no_dimension();
            *m = dimension;
            return {};
          }};
}

Command remove_dimension(Id drawing, Id dimension) {
  return {"Delete dimension", [drawing, dimension](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            if (std::erase_if(d->dimensions,
                              [&](const model::DrawingDimension& m) { return m.id == dimension; }) == 0) {
              return no_dimension();
            }
            return {};
          }};
}

Command move_dimension(Id drawing, Id dimension, double offset) {
  return {"Move dimension", [=](model::Project& p) -> core::Diagnostics {
            model::Drawing* d = p.document.find_drawing(drawing);
            if (d == nullptr) return no_drawing();
            model::DrawingDimension* m = find_dimension(*d, dimension);
            if (m == nullptr) return no_dimension();
            m->offset = offset;
            return {};
          }};
}

// --- rows -------------------------------------------------------------------------------------

std::string anchor_label(const model::Document& document, const Anchor& anchor) {
  switch (anchor.on) {
    case AnchorOn::Origin:
      return "Origin";
    case AnchorOn::VolumeBoundary:
      return volume_name(document, anchor.target) + " boundary " + part_text(anchor);
    case AnchorOn::Surface: {
      const model::ConnectionSurface* c = document.find_connection(anchor.target);
      return (c ? c->name : std::string("(missing surface)")) + " " + part_text(anchor);
    }
    case AnchorOn::VolumeContents: {
      const std::string part =
          anchor.part == AnchorPart::Face ? contents_face_text(document, anchor) : part_text(anchor);
      return "(" + volume_name(document, anchor.target) + " contents " + part + ")";
    }
  }
  return {};
}

std::vector<DrawingRow> drawing_rows(const model::Document& document) {
  std::vector<DrawingRow> rows;
  for (const auto& d : document.drawings) {
    rows.push_back({DrawingRowKind::Drawing, d.id, d.id, 0, d.name, false});
    for (const auto& v : d.views) {
      std::string label = v.name + " (" + std::string(model::to_string(v.direction)) + ", " +
                          draw::scale_text(v.scale) + ")";
      rows.push_back({DrawingRowKind::View, v.id, d.id, 1, std::move(label), false});
      for (const auto& m : d.dimensions) {
        if (m.view != v.id) continue;
        std::string text;
        switch (m.kind) {
          case DimensionKind::Horizontal:
          case DimensionKind::Vertical:
            text = std::string(m.kind == DimensionKind::Horizontal ? "Horizontal" : "Vertical") + ": " +
                   anchor_label(document, m.from) + " to " + anchor_label(document, m.to);
            break;
          case DimensionKind::Diameter:
            text = "Diameter: " + anchor_label(document, m.from);
            break;
        }
        rows.push_back({DrawingRowKind::Dimension, m.id, d.id, 2, std::move(text), m.disposable()});
      }
    }
  }
  return rows;
}

// --- anchors ----------------------------------------------------------------------------------

std::vector<AnchorChoice> anchor_choices(const eval::Evaluation& evaluation, const model::DrawingView& view,
                                         DimensionKind kind) {
  const model::Document& doc = evaluation.document;
  const draw::ViewFrame frame = draw::view_frame(view.direction);
  const bool diameter = kind == DimensionKind::Diameter;
  const core::dvec3 along = kind == DimensionKind::Vertical ? frame.up : frame.right;

  // The volumes the view shows, parents first, as vs_draw shows them.
  std::map<Id, std::vector<Id>> children;
  for (const auto& v : doc.volumes) {
    if (!v.parent.is_null()) children[v.parent].push_back(v.id);
  }
  std::vector<Id> shown;
  std::set<Id> seen;
  auto collect = [&](auto& self, const Id& id) -> void {
    if (!seen.insert(id).second) return;
    shown.push_back(id);
    const auto it = children.find(id);
    if (it == children.end()) return;
    for (const auto& c : it->second) self(self, c);
  };
  if (view.volumes.empty()) {
    for (const auto& v : doc.volumes) collect(collect, v.id);
  } else {
    for (const auto& id : view.volumes) {
      if (doc.find_volume(id) != nullptr) collect(collect, id);
    }
  }

  std::vector<AnchorChoice> stable;
  std::vector<AnchorChoice> disposable;
  auto add = [&](Anchor a) {
    AnchorChoice c{a, anchor_label(doc, a), a.disposable()};
    (c.disposable ? disposable : stable).push_back(std::move(c));
  };
  auto add_extremes = [&](AnchorOn on, const Id& target) {
    for (auto part : {AnchorPart::Min, AnchorPart::Max, AnchorPart::Centre}) add({on, target, part, {}, 0});
  };
  // A face the dimension can measure: a plane facing along it, or (for a
  // linear one, by its axis, or a diameter) a cylinder facing the viewer.
  auto fits = [&](const feature::NamedFace& f) {
    if (f.cylinder) return parallel(f.cylinder->axis.direction, frame.toward);
    return !diameter && parallel(f.plane.normal, along);
  };

  if (!diameter) add({AnchorOn::Origin, {}, AnchorPart::Centre, {}, 0});
  for (const auto& id : shown) {
    const model::VolumeBuild* vb = evaluation.build.find_volume(id);
    if (vb == nullptr || vb->boundary.is_empty()) continue;
    if (!diameter) add_extremes(AnchorOn::VolumeBoundary, id);
    for (const auto& f : vb->boundary_faces) {
      if (fits(f)) add({AnchorOn::VolumeBoundary, id, AnchorPart::Face, f.name, 0});
    }
  }
  const std::set<Id> shown_set(shown.begin(), shown.end());
  for (const auto& c : doc.connections) {
    if (std::none_of(c.volumes.begin(), c.volumes.end(), [&](const Id& v) { return shown_set.contains(v); })) {
      continue;
    }
    const model::ConnectionBuild* cb = evaluation.build.find_connection(c.id);
    if (cb == nullptr || !cb->ok || cb->patch.is_empty()) continue;
    if (!diameter) {
      add_extremes(AnchorOn::Surface, c.id);
    } else if (cb->cylinder && parallel(cb->frame.normal(), frame.toward)) {
      add({AnchorOn::Surface, c.id, AnchorPart::Centre, {}, 0});
    }
    const bool holes_face_viewer = !cb->cylinder && parallel(cb->frame.normal(), frame.toward);
    if (diameter && !holes_face_viewer) continue;
    for (std::uint32_t k = 0; k < cb->hole_centres.size(); ++k) {
      add({AnchorOn::Surface, c.id, AnchorPart::Hole, {}, k});
    }
  }
  for (const auto& r : evaluation.volumes) {
    if (!shown_set.contains(r.volume) || !r.contents) continue;
    const auto& contents = *r.contents;
    if (contents.solid.is_empty() && (!contents.mesh || contents.mesh->empty())) continue;
    if (!diameter) add_extremes(AnchorOn::VolumeContents, r.volume);
    if (!contents.rebuild || contents.solid.is_empty()) continue;
    for (const auto& fr : contents.rebuild->features) {
      for (const auto& f : fr.faces) {
        if (fits(f)) {
          add({AnchorOn::VolumeContents, r.volume, AnchorPart::Face, fr.feature.to_string() + ":" + f.name, 0});
        }
      }
    }
  }
  // A composite's contents are its leaves', generated children included.
  if (!diameter) {
    for (const auto& id : shown) {
      const model::Volume* v = doc.find_volume(id);
      if (v != nullptr && v->kind() == model::VolumeKind::Composite) add_extremes(AnchorOn::VolumeContents, id);
    }
  }
  stable.insert(stable.end(), std::make_move_iterator(disposable.begin()),
                std::make_move_iterator(disposable.end()));
  return stable;
}

}  // namespace vs::app
