#include "vs/model/document.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/format.h>

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
  if (tree.find(id) || tree.variables().find(id)) return true;
  for (const auto& f : tree.features()) {
    if (const auto* s = f.as<feature::SketchFeature>()) {
      if (s->sketch.id() == id || s->sketch.find_entity(id) || s->sketch.find_constraint(id)) {
        return true;
      }
    }
  }
  return false;
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

const Volume* Document::contents_owner(const Id& item) const {
  if (item.is_null()) return nullptr;
  for (const auto& v : volumes) {
    if (const auto* t = v.tree(); t && tree_holds(*t, item)) return &v;
  }
  return nullptr;
}

namespace {
std::string name_or(const std::string& name, std::string_view fallback, const Id& id) {
  if (!name.empty()) return name;
  return fmt::format("{} {}", fallback, id.to_string().substr(0, 8));
}
}  // namespace

SubjectRef Document::volume_subject(const Id& vid) const {
  const Volume* v = find_volume(vid);
  return {SubjectKind::Volume, vid, v ? name_or(v->name, "Volume", vid) : "(missing volume)"};
}

SubjectRef Document::boundary_subject(const Id& vid) const {
  const Volume* v = find_volume(vid);
  return {SubjectKind::Boundary, vid,
          fmt::format("boundary of '{}'", v ? name_or(v->name, "Volume", vid) : "(missing)")};
}

SubjectRef Document::connection_subject(const Id& sid) const {
  const ConnectionSurface* s = find_connection(sid);
  return {SubjectKind::ConnectionSurface, sid,
          s ? name_or(s->name, "Connection", sid) : "(missing connection surface)"};
}

SubjectRef Document::keep_out_subject(const Id& kid) const {
  const KeepOut* k = find_keep_out(kid);
  return {SubjectKind::KeepOut, kid, k ? name_or(k->name, "Keep-out", kid) : "(missing keep-out)"};
}

SubjectRef Document::output_subject(const PublishedOutputRef& ref) const {
  const Volume* v = find_volume(ref.volume);
  const PublishedOutput* o = v ? v->find_output(ref.output) : nullptr;
  if (!o) return {SubjectKind::PublishedOutput, ref.output, "(missing output)"};
  return {SubjectKind::PublishedOutput, ref.output,
          fmt::format("{}.{}", name_or(v->name, "Volume", v->id), o->name)};
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
        target = fmt::format("{}.{}", name_or(v.name, "Volume", v.id), p->name);
      }
      // A variable of a composite's layout (L3).
      if (const auto* c = std::get_if<CompositeContents>(&v.contents)) {
        if (const auto* cvar = c->layout.variables().find(l->target)) {
          target = fmt::format("{}: {}", name_or(v.name, "Volume", v.id), cvar->name);
        }
      }
    }
  }
  return {SubjectKind::Link, lid,
          fmt::format("{} \xE2\x86\x92 {}", output_subject(l->source).label, target)};
}

Document make_document(std::string name) {
  Document d;
  d.id = Id::random();
  d.name = std::move(name);
  return d;
}

}  // namespace vs::model
