#include "vs/model/document.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/format.h>

#include "lookup.hpp"

namespace vs::model {

using core::SubjectKind;

std::string_view to_string(VolumeKind kind) {
  switch (kind) {
    case VolumeKind::Static: return "static";
    case VolumeKind::Parametric: return "parametric";
    case VolumeKind::Procedural: return "procedural";
    case VolumeKind::Composite: return "composite";
  }
  return "static";
}

std::string_view to_string(OutputQuantity quantity) {
  switch (quantity) {
    case OutputQuantity::Volume: return "volume";
    case OutputQuantity::Mass: return "mass";
    case OutputQuantity::SurfaceArea: return "surface_area";
    case OutputQuantity::CentreOfGravity: return "centre_of_gravity";
    case OutputQuantity::Envelope: return "envelope";
    case OutputQuantity::Published: return "published";
  }
  return "volume";
}

bool is_scalar(OutputQuantity quantity) {
  return quantity == OutputQuantity::Volume || quantity == OutputQuantity::Mass ||
         quantity == OutputQuantity::SurfaceArea || quantity == OutputQuantity::Published;
}

std::string_view to_string(ConnectionType type) {
  switch (type) {
    case ConnectionType::Bonded: return "bonded";
    case ConnectionType::Clearance: return "clearance";
    case ConnectionType::BoltedFlange: return "bolted_flange";
  }
  return "bonded";
}

namespace {

// ISO 261 coarse threads, ISO 273 medium clearance holes.
constexpr std::array<ThreadSize, 9> kThreads{{
    {"M2", 2.0, 2.4},
    {"M2.5", 2.5, 2.9},
    {"M3", 3.0, 3.4},
    {"M4", 4.0, 4.5},
    {"M5", 5.0, 5.5},
    {"M6", 6.0, 6.6},
    {"M8", 8.0, 9.0},
    {"M10", 10.0, 11.0},
    {"M12", 12.0, 13.5},
}};

}  // namespace

std::optional<ThreadSize> thread_size(std::string_view name) {
  for (const auto& t : kThreads) {
    if (t.name == name) return t;
  }
  return std::nullopt;
}

std::vector<ThreadSize> thread_sizes() { return {kThreads.begin(), kThreads.end()}; }

// --- Volume -------------------------------------------------------------------------

bool Volume::contents_empty() const {
  if (const auto* s = std::get_if<StaticContents>(&contents)) return s->blob.empty();
  if (const auto* p = std::get_if<ParametricContents>(&contents)) {
    return p->tree.features().empty() && p->tree.variables().empty();
  }
  if (std::holds_alternative<CompositeContents>(contents)) return false;
  return std::get<ProceduralContents>(contents).graph.empty();
}

const feature::FeatureTree* Volume::tree() const {
  if (const auto* p = std::get_if<ParametricContents>(&contents)) return &p->tree;
  if (const auto* c = std::get_if<CompositeContents>(&contents)) return &c->layout;
  return nullptr;
}

feature::FeatureTree* Volume::tree() {
  return const_cast<feature::FeatureTree*>(std::as_const(*this).tree());
}

const Delegation* CompositeContents::delegation(const Id& surface) const {
  for (const auto& d : delegations) {
    if (d.surface == surface) return &d;
  }
  return nullptr;
}

Contents as_contents(const TemplateContents& contents) {
  return std::visit([](const auto& c) -> Contents { return c; }, contents);
}

std::optional<TemplateContents> as_template_contents(const Contents& contents) {
  if (const auto* s = std::get_if<StaticContents>(&contents)) return TemplateContents{*s};
  if (const auto* p = std::get_if<ParametricContents>(&contents)) return TemplateContents{*p};
  if (const auto* g = std::get_if<ProceduralContents>(&contents)) return TemplateContents{*g};
  return std::nullopt;
}

const ChildTemplate* CompositeContents::find_template(std::string_view name) const {
  for (const auto& t : templates) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

const VolumeParameter* Volume::find_parameter(const Id& pid) const {
  for (const auto& p : parameters) {
    if (p.id == pid) return &p;
  }
  return nullptr;
}

const VolumeParameter* Volume::find_parameter(std::string_view pname) const {
  for (const auto& p : parameters) {
    if (p.name == pname) return &p;
  }
  return nullptr;
}

const PublishedOutput* Volume::find_output(const Id& oid) const {
  for (const auto& o : outputs) {
    if (o.id == oid) return &o;
  }
  return nullptr;
}

const PublishedOutput* Volume::find_output(std::string_view oname) const {
  for (const auto& o : outputs) {
    if (o.name == oname) return &o;
  }
  return nullptr;
}

bool ConnectionSurface::joins(const Id& volume) const {
  return std::find(volumes.begin(), volumes.end(), volume) != volumes.end();
}

// --- Document -----------------------------------------------------------------------

namespace {

template <typename T>
const T* find_by_id(const std::vector<T>& items, const Id& id) {
  for (const auto& item : items) {
    if (item.id == id) return &item;
  }
  return nullptr;
}

bool tree_holds(const feature::FeatureTree& tree, const Id& id) {
  return feature::find_tree_id(tree, id).has_value();
}

}  // namespace

const Volume* Document::find_volume(const Id& vid) const { return find_by_id(volumes, vid); }
Volume* Document::find_volume(const Id& vid) {
  return const_cast<Volume*>(std::as_const(*this).find_volume(vid));
}
const Volume* Document::find_volume(std::string_view vname) const {
  for (const auto& v : volumes) {
    if (v.name == vname) return &v;
  }
  return nullptr;
}
const ConnectionSurface* Document::find_connection(const Id& sid) const {
  return find_by_id(connections, sid);
}
ConnectionSurface* Document::find_connection(const Id& sid) {
  return const_cast<ConnectionSurface*>(std::as_const(*this).find_connection(sid));
}
const KeepOut* Document::find_keep_out(const Id& kid) const { return find_by_id(keep_outs, kid); }
const Link* Document::find_link(const Id& lid) const { return find_by_id(links, lid); }

const Configuration* Document::find_configuration(const Id& cid) const {
  if (cid.is_null()) return nullptr;
  return find_by_id(configurations, cid);
}

const Configuration* Document::find_configuration(std::string_view cname) const {
  const auto it = std::find_if(configurations.begin(), configurations.end(),
                               [&](const Configuration& c) { return c.name == cname; });
  return it == configurations.end() ? nullptr : &*it;
}

const Drawing* Document::find_drawing(const Id& did) const {
  if (did.is_null()) return nullptr;
  return find_by_id(drawings, did);
}

Drawing* Document::find_drawing(const Id& did) {
  return const_cast<Drawing*>(std::as_const(*this).find_drawing(did));
}

const Drawing* Document::find_drawing(std::string_view dname) const {
  const auto it = std::find_if(drawings.begin(), drawings.end(),
                               [&](const Drawing& d) { return d.name == dname; });
  return it == drawings.end() ? nullptr : &*it;
}

const DrawingView* Drawing::find_view(const Id& vid) const {
  if (vid.is_null()) return nullptr;
  const auto it =
      std::find_if(views.begin(), views.end(), [&](const DrawingView& v) { return v.id == vid; });
  return it == views.end() ? nullptr : &*it;
}

const DrawingDimension* Drawing::find_dimension(const Id& did) const {
  if (did.is_null()) return nullptr;
  const auto it = std::find_if(dimensions.begin(), dimensions.end(),
                               [&](const DrawingDimension& d) { return d.id == did; });
  return it == dimensions.end() ? nullptr : &*it;
}

const ConfigurationValue* Configuration::find(const Id& target) const {
  const auto it = std::find_if(values.begin(), values.end(),
                               [&](const ConfigurationValue& v) { return v.target == target; });
  return it == values.end() ? nullptr : &*it;
}

const Volume* Document::contents_owner(const Id& item) const {
  if (item.is_null()) return nullptr;
  for (const auto& v : volumes) {
    if (const auto* t = v.tree(); t && tree_holds(*t, item)) return &v;
  }
  return nullptr;
}

SubjectRef Document::volume_subject(const Id& vid) const {
  return detail::volume_subject_of(find_volume(vid), vid);
}

SubjectRef Document::boundary_subject(const Id& vid) const {
  return detail::boundary_subject_of(find_volume(vid), vid);
}

SubjectRef Document::connection_subject(const Id& sid) const {
  return detail::connection_subject_of(find_connection(sid), sid);
}

SubjectRef Document::keep_out_subject(const Id& kid) const {
  return detail::keep_out_subject_of(find_keep_out(kid), kid);
}

SubjectRef Document::output_subject(const PublishedOutputRef& ref) const {
  return detail::output_subject_of(find_volume(ref.volume), ref);
}

SubjectRef Document::link_subject(const Id& lid) const {
  const Link* l = find_link(lid);
  if (!l) return {SubjectKind::Link, lid, "(missing link)"};
  std::string target = "(missing)";
  if (const auto* var = layout.variables().find(l->target)) {
    target = var->name;
  } else {
    for (const auto& v : volumes) {
      if (const auto* p = v.find_parameter(l->target)) {
        target = fmt::format("{}.{}", detail::name_or(v.name, "Volume", v.id), p->name);
      }
      // A variable of a composite's layout (L3).
      if (const auto* c = std::get_if<CompositeContents>(&v.contents)) {
        if (const auto* cvar = c->layout.variables().find(l->target)) {
          target = fmt::format("{}: {}", detail::name_or(v.name, "Volume", v.id), cvar->name);
        }
      }
    }
  }
  return {SubjectKind::Link, lid,
          fmt::format("{} \xE2\x86\x92 {}", output_subject(l->source).label, target)};
}

SubjectRef Document::configuration_subject(const Id& cid) const {
  const Configuration* c = find_configuration(cid);
  if (!c) return {SubjectKind::Configuration, cid, "(missing configuration)"};
  return {SubjectKind::Configuration, cid,
          fmt::format("configuration '{}'", c->name.empty() ? std::string("(unnamed)") : c->name)};
}

SubjectRef Document::drawing_subject(const Id& item) const {
  auto named = [](const std::string& text) {
    return text.empty() ? std::string("(unnamed)") : text;
  };
  for (const auto& d : drawings) {
    if (d.id == item) return {SubjectKind::Drawing, item, fmt::format("drawing '{}'", named(d.name))};
    if (const auto* v = d.find_view(item)) {
      return {SubjectKind::Drawing, item,
              fmt::format("view '{}' of drawing '{}'", named(v->name), named(d.name))};
    }
    if (d.find_dimension(item)) {
      const auto at = std::find_if(d.dimensions.begin(), d.dimensions.end(),
                                   [&](const DrawingDimension& x) { return x.id == item; });
      return {SubjectKind::Drawing, item,
              fmt::format("dimension {} of drawing '{}'", at - d.dimensions.begin() + 1,
                          named(d.name))};
    }
  }
  return {SubjectKind::Drawing, item, "(missing drawing)"};
}

std::string_view to_string(ViewDirection direction) {
  switch (direction) {
    case ViewDirection::Front: return "front";
    case ViewDirection::Back: return "back";
    case ViewDirection::Top: return "top";
    case ViewDirection::Bottom: return "bottom";
    case ViewDirection::Left: return "left";
    case ViewDirection::Right: return "right";
    case ViewDirection::Isometric: return "isometric";
  }
  return "front";
}

std::optional<ViewDirection> view_direction_from_string(std::string_view text) {
  for (auto d : {ViewDirection::Front, ViewDirection::Back, ViewDirection::Top,
                 ViewDirection::Bottom, ViewDirection::Left, ViewDirection::Right,
                 ViewDirection::Isometric}) {
    if (to_string(d) == text) return d;
  }
  if (text == "iso") return ViewDirection::Isometric;
  return std::nullopt;
}

std::string_view to_string(AnchorOn on) {
  switch (on) {
    case AnchorOn::Origin: return "origin";
    case AnchorOn::VolumeBoundary: return "boundary";
    case AnchorOn::Surface: return "surface";
    case AnchorOn::VolumeContents: return "contents";
  }
  return "origin";
}

std::string_view to_string(AnchorPart part) {
  switch (part) {
    case AnchorPart::Min: return "min";
    case AnchorPart::Max: return "max";
    case AnchorPart::Centre: return "centre";
    case AnchorPart::Face: return "face";
    case AnchorPart::Hole: return "hole";
  }
  return "centre";
}

std::string_view to_string(DimensionKind kind) {
  switch (kind) {
    case DimensionKind::Horizontal: return "horizontal";
    case DimensionKind::Vertical: return "vertical";
    case DimensionKind::Diameter: return "diameter";
  }
  return "horizontal";
}

Document make_document(std::string name) {
  Document d;
  d.id = Id::random();
  d.name = std::move(name);
  return d;
}

}  // namespace vs::model
