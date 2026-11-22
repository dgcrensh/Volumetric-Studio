#include "vs/model/generated.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <utility>

#include <fmt/format.h>

#include "vs/feature/serialize.hpp"

namespace vs::model {

using core::Diagnostic;
using core::Hasher;

void hash_append(Hasher& h, const GeneratedLayout& layout) {
  h.add(std::string_view("vstudio.model.generated_layout"));
  h.add(static_cast<std::uint64_t>(layout.children.size()));
  for (const auto& c : layout.children) {
    h.add(c.key).add(c.name).add(c.template_name).add(c.box).add(c.seed);
    h.add(static_cast<std::uint64_t>(c.parameters.size()));
    for (const auto& [name, value] : c.parameters) h.add(name).add(value);
  }
  h.add(static_cast<std::uint64_t>(layout.surfaces.size()));
  for (const auto& s : layout.surfaces) {
    h.add(s.key).add(s.name).add(static_cast<std::uint64_t>(s.a)).add(static_cast<std::uint64_t>(s.b));
    h.add(s.type).add(s.gap).add(s.axis).add(s.offset);
    h.add(s.lo[0]).add(s.lo[1]).add(s.hi[0]).add(s.hi[1]);
  }
  h.add(static_cast<std::uint64_t>(layout.delegations.size()));
  for (const auto& d : layout.delegations) h.add(d.surface).add(static_cast<std::uint64_t>(d.child));
}

Id generated_child_id(const Id& composite, const Id& key) {
  return Id::derive(composite, "child:" + key.to_string());
}

Id generated_surface_id(const Id& composite, const Id& key) {
  return Id::derive(composite, "surface:" + key.to_string());
}

namespace {

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// Every id written in `text` (a run of exactly 32 hex digits standing on
// its own, as Id::to_string() writes them, also inside names such as a
// pattern copy's "<k>:<seed id>:<name>"), mapped by `map`.
std::string remap_ids(const std::string& text, const Id& from, const Id& to) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (is_hex(text[i]) && (i == 0 || !is_word(text[i - 1]))) {
      std::size_t j = i;
      while (j < text.size() && is_hex(text[j])) ++j;
      if (j - i == 32 && (j == text.size() || !is_word(text[j]))) {
        if (const auto id = Id::parse(std::string_view(text).substr(i, 32))) {
          out += (*id == from ? to : Id::derive(to, id->to_string())).to_string();
          i = j;
          continue;
        }
      }
      out.append(text, i, j - i);
      i = j;
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

Id derived(const Id& child, const Id& id) { return Id::derive(child, id.to_string()); }

bool finite(const core::dbox3& b) {
  return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) &&
         std::isfinite(b.max.x) && std::isfinite(b.max.y) && std::isfinite(b.max.z);
}

feature::PrincipalPlane plane_normal_to(int axis) {
  switch (axis) {
    case 0: return feature::PrincipalPlane::YZ;
    case 1: return feature::PrincipalPlane::ZX;
    default: return feature::PrincipalPlane::XY;
  }
}

}  // namespace

namespace {

// instantiate(), given the template tree's JSON when it is parametric
// (expand() writes it once per template, not once per child).
TemplateContents instantiate_from(const TemplateContents& contents, const std::string& json,
                                  const Id& template_id, const Id& child) {
  if (const auto* p = std::get_if<ParametricContents>(&contents)) {
    // Through the tree's JSON, which names every id it holds.
    auto tree = feature::feature_tree_from_json(remap_ids(json, template_id, child));
    if (tree) return ParametricContents{std::move(tree).value()};
    return *p;  // can't happen: the JSON is the tree's own, ids aside
  }
  // Static contents hold no ids; a graph is opaque, and its node ids need
  // not be unique across the document.
  return contents;
}

std::string tree_json(const TemplateContents& contents) {
  const auto* p = std::get_if<ParametricContents>(&contents);
  return p != nullptr ? feature::to_json(p->tree) : std::string{};
}

}  // namespace

TemplateContents instantiate(const TemplateContents& contents, const Id& template_id,
                             const Id& child) {
  return instantiate_from(contents, tree_json(contents), template_id, child);
}

Id generator_of(const Document& document, const Id& volume) {
  const Volume* v = document.find_volume(volume);
  if (v == nullptr || !v->generated) return {};
  return v->parent;
}

core::Result<Document> expand(const Document& document, const Id& composite,
                              const GeneratedLayout& layout) {
  const SubjectRef self = document.volume_subject(composite);
  Document out = document;
  Volume* parent = out.find_volume(composite);
  auto* contents = parent ? std::get_if<CompositeContents>(&parent->contents) : nullptr;
  if (contents == nullptr || !contents->generates()) {
    return Diagnostic::error("model.generated.not_generator",
                             fmt::format("'{}' isn't a composite with a generator", self.label),
                             {self});
  }
  feature::FeatureTree& tree = contents->layout;
  const auto param = [](double value) { return feature::Param::of(value); };

  std::vector<Id> child_ids;
  std::vector<Volume> made;
  // Each child's two points, appended to the layout in one go after the
  // loop (FeatureTree::append()), then its boundary refers to their ids.
  std::vector<feature::Feature> points;
  std::map<const ChildTemplate*, std::string> template_json;
  for (const GeneratedChild& c : layout.children) {
    const Id id = generated_child_id(composite, c.key);
    child_ids.push_back(id);
    if (c.box.is_empty() || !finite(c.box)) {
      return Diagnostic::error(
          "model.generated.child",
          fmt::format("'{}': its generator made '{}' with an empty box", self.label, c.name),
          {self});
    }
    for (const bool high : {false, true}) {
      const core::dvec3& at = high ? c.box.max : c.box.min;
      feature::Feature f;
      f.id = Id::derive(id, high ? "high" : "low");
      f.name = c.name + (high ? " high" : " low");
      f.definition = feature::DatumPointFeature{feature::PointAt{param(at.x), param(at.y), param(at.z)}};
      points.push_back(std::move(f));
    }

    Volume v;
    v.id = id;
    v.name = c.name;
    v.parent = composite;
    v.generated = true;
    v.seed = c.seed;
    if (c.template_name.empty()) {
      v.contents = StaticContents{};
      if (!c.parameters.empty()) {
        return Diagnostic::error(
            "model.generated.parameter",
            fmt::format("'{}': its generator sets parameters of '{}', which has no template",
                        self.label, c.name),
            {self});
      }
    } else {
      const ChildTemplate* t = contents->find_template(c.template_name);
      if (t == nullptr) {
        return Diagnostic::error(
            "model.generated.template",
            fmt::format("'{}': its generator fills '{}' from '{}', which isn't one of its "
                        "templates",
                        self.label, c.name, c.template_name),
            {self});
      }
      auto json = template_json.find(t);
      if (json == template_json.end()) json = template_json.emplace(t, tree_json(t->contents)).first;
      v.contents = as_contents(instantiate_from(t->contents, json->second, t->id, id));
      if (std::holds_alternative<ParametricContents>(t->contents)) {
        v.contents_offset = c.box.min - t->origin;
      }
      for (const auto& p : t->parameters) {
        VolumeParameter q = p;
        q.id = derived(id, p.id);
        v.parameters.push_back(std::move(q));
      }
      for (const auto& o : t->outputs) {
        PublishedOutput q = o;
        q.id = derived(id, o.id);
        v.outputs.push_back(std::move(q));
      }
      for (const auto& [name, value] : c.parameters) {
        auto it = std::find_if(v.parameters.begin(), v.parameters.end(),
                               [&](const VolumeParameter& p) { return p.name == name; });
        if (it == v.parameters.end()) {
          return Diagnostic::error(
              "model.generated.parameter",
              fmt::format("'{}': its generator sets '{}' of '{}', but template '{}' has no "
                          "parameter of that name",
                          self.label, name, c.name, c.template_name),
              {self});
        }
        it->value = param(value);
      }
    }
    made.push_back(std::move(v));
  }

  const std::vector<Id> point_ids = tree.append(std::move(points));
  for (std::size_t i = 0; i < made.size(); ++i) {
    made[i].boundary = BoxBoundary{feature::FeatureRef{point_ids[2 * i]},
                                   feature::FeatureRef{point_ids[2 * i + 1]}};
  }

  // Likewise each surface's plane.
  std::vector<feature::Feature> planes;
  std::vector<ConnectionSurface> surfaces;
  for (const GeneratedSurface& s : layout.surfaces) {
    if (s.a >= child_ids.size() || s.b >= child_ids.size() || s.a == s.b || s.axis < 0 ||
        s.axis > 2 || !(s.hi[0] > s.lo[0]) || !(s.hi[1] > s.lo[1]) || !std::isfinite(s.offset)) {
      return Diagnostic::error(
          "model.generated.surface",
          fmt::format("'{}': its generator made a surface '{}' that doesn't join two of its "
                      "children over a rectangle",
                      self.label, s.name),
          {self});
    }
    const Id id = generated_surface_id(composite, s.key);
    feature::Feature plane;
    plane.id = Id::derive(id, "plane");
    plane.name = s.name + " plane";
    plane.definition =
        feature::DatumPlaneFeature{feature::PlaneOffset{plane_normal_to(s.axis), param(s.offset)}};
    planes.push_back(std::move(plane));

    ConnectionSurface c;
    c.id = id;
    c.name = s.name;
    DatumPatch patch;
    patch.u = param((s.lo[0] + s.hi[0]) / 2);
    patch.v = param((s.lo[1] + s.hi[1]) / 2);
    patch.width = param(s.hi[0] - s.lo[0]);
    patch.height = param(s.hi[1] - s.lo[1]);
    c.geometry = patch;
    c.volumes = {child_ids[s.a], child_ids[s.b]};
    if (s.type == ConnectionType::Clearance) {
      c.params = ClearanceParams{param(s.gap)};
    } else {
      c.params = BondedParams{};
    }
    surfaces.push_back(std::move(c));
  }

  const std::vector<Id> plane_ids = tree.append(std::move(planes));
  for (std::size_t i = 0; i < surfaces.size(); ++i) {
    std::get<DatumPatch>(surfaces[i].geometry).plane = feature::FeatureRef{plane_ids[i]};
  }

  for (const GeneratedDelegation& d : layout.delegations) {
    if (d.child >= child_ids.size()) {
      return Diagnostic::error(
          "model.generated.surface",
          fmt::format("'{}': its generator passes a surface to a child it didn't make",
                      self.label),
          {self});
    }
    contents->delegations.push_back({d.surface, child_ids[d.child]});
  }

  for (auto& v : made) out.volumes.push_back(std::move(v));
  for (auto& c : surfaces) out.connections.push_back(std::move(c));
  return out;
}

}  // namespace vs::model
