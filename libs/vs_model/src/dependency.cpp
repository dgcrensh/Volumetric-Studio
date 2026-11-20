#include "vs/model/dependency.hpp"

#include <algorithm>
#include <functional>
#include <map>

#include <fmt/format.h>

#include "vs/model/hierarchy.hpp"

namespace vs::model {

using core::SubjectKind;

std::string_view to_string(NodeKind kind) {
  switch (kind) {
    case NodeKind::Variable: return "variable";
    case NodeKind::Feature: return "feature";
    case NodeKind::VolumeBoundary: return "boundary";
    case NodeKind::KeepOut: return "keep-out";
    case NodeKind::ConnectionSurface: return "connection surface";
    case NodeKind::Parameter: return "parameter";
    case NodeKind::VolumeContents: return "contents";
    case NodeKind::PublishedOutput: return "output";
    case NodeKind::Link: return "link";
  }
  return "node";
}

namespace {

constexpr std::string_view kArrow = " \xE2\x86\x92 ";  // →

class GraphBuilder {
 public:
  explicit GraphBuilder(const Document& doc) : doc_(doc) {}

  std::vector<DependencyNode> nodes;
  std::vector<std::vector<std::size_t>> inputs;

  void run() {
    add_nodes();
    add_edges();
  }

 private:
  const Document& doc_;
  std::map<std::pair<NodeKind, Id>, std::size_t> index_;

  std::size_t add(NodeKind kind, const Id& id, const Id& volume, SubjectRef subject,
                  std::string description) {
    const std::size_t i = nodes.size();
    nodes.push_back({kind, id, volume, std::move(subject), std::move(description)});
    inputs.emplace_back();
    index_.emplace(std::make_pair(kind, id), i);
    return i;
  }

  std::optional<std::size_t> find(NodeKind kind, const Id& id) const {
    const auto it = index_.find({kind, id});
    if (it == index_.end()) return std::nullopt;
    return it->second;
  }

  void edge(std::size_t from, std::optional<std::size_t> to) {
    if (!to) return;
    auto& in = inputs[from];
    if (std::find(in.begin(), in.end(), *to) == in.end()) in.push_back(*to);
  }

  std::string volume_name(const Id& vid) const { return doc_.volume_subject(vid).label; }

  void add_nodes() {
    const auto& vars = doc_.layout.variables();
    for (const auto& v : vars.variables()) {
      add(NodeKind::Variable, v.id, {}, vars.subject(v.id), fmt::format("variable '{}'", v.name));
    }
    for (const auto& f : doc_.layout.features()) {
      const auto s = doc_.layout.subject(f.id);
      add(NodeKind::Feature, f.id, {}, s, fmt::format("'{}'", s.label));
    }
    for (const auto& k : doc_.keep_outs) {
      const auto s = doc_.keep_out_subject(k.id);
      add(NodeKind::KeepOut, k.id, {}, s, fmt::format("keep-out '{}'", s.label));
    }
    for (const auto& c : doc_.connections) {
      const auto s = doc_.connection_subject(c.id);
      add(NodeKind::ConnectionSurface, c.id, {}, s,
          fmt::format("connection surface '{}'", s.label));
    }
    for (const auto& v : doc_.volumes) {
      const std::string name = volume_name(v.id);
      add(NodeKind::VolumeBoundary, v.id, v.id, doc_.boundary_subject(v.id),
          fmt::format("boundary of '{}'", name));
      for (const auto& p : v.parameters) {
        add(NodeKind::Parameter, p.id, v.id,
            {SubjectKind::Parameter, p.id, fmt::format("{}.{}", name, p.name)},
            fmt::format("parameter '{}' of '{}'", p.name, name));
      }
      add(NodeKind::VolumeContents, v.id, v.id, doc_.volume_subject(v.id),
          fmt::format("contents of '{}'", name));
      for (const auto& o : v.outputs) {
        add(NodeKind::PublishedOutput, o.id, v.id, doc_.output_subject({v.id, o.id}),
            fmt::format("output '{}' of '{}'", o.name, name));
      }
      // A composite's layout is a level of its own (L3): its variables and
      // features are nodes like the root layout's.
      if (const auto* comp = std::get_if<CompositeContents>(&v.contents)) {
        const auto& cvars = comp->layout.variables();
        for (const auto& var : cvars.variables()) {
          add(NodeKind::Variable, var.id, v.id, cvars.subject(var.id),
              fmt::format("variable '{}' of '{}'", var.name, name));
        }
        for (const auto& f : comp->layout.features()) {
          const auto s = comp->layout.subject(f.id);
          add(NodeKind::Feature, f.id, v.id, s, fmt::format("'{}' of '{}'", s.label, name));
        }
      }
    }
    for (const auto& l : doc_.links) {
      const auto s = doc_.link_subject(l.id);
      add(NodeKind::Link, l.id, {}, s, fmt::format("link '{}'", s.label));
    }
  }

  // Edges to the layout variables an expression names.
  void uses_expression(std::size_t from, const std::string& text, const Id& level = {}) {
    if (text.empty()) return;
    auto e = feature::Expression::parse(text);
    if (!e) return;
    for (const auto& name : e->references()) uses_variable(from, name, level);
  }
  // A name at `level`: a variable of its layout, or inside a composite one
  // of the composite's parameters.
  void uses_variable(std::size_t from, std::string_view name, const Id& level = {}) {
    if (const feature::FeatureTree* t = level_layout(doc_, level)) {
      if (const auto* v = t->variables().find(name)) {
        edge(from, find(NodeKind::Variable, v->id));
        return;
      }
    }
    if (const Volume* c = level.is_null() ? nullptr : doc_.find_volume(level)) {
      if (const auto* p = c->find_parameter(name)) edge(from, find(NodeKind::Parameter, p->id));
    }
  }
  void uses_param(std::size_t from, const Param& p, const Id& level = {}) {
    uses_expression(from, p.expression, level);
  }

  // A layout feature, sketch entity or other layout id: its feature node.
  void uses_layout(std::size_t from, const Id& id) {
    if (auto f = find(NodeKind::Feature, id)) {
      edge(from, f);
      return;
    }
    const auto in_tree = [&](const feature::FeatureTree& tree) {
      for (const auto& f : tree.features()) {
        if (const auto* s = f.as<feature::SketchFeature>()) {
          if (s->sketch.find_entity(id)) {
            edge(from, find(NodeKind::Feature, f.id));
            return true;
          }
        }
      }
      return false;
    };
    if (in_tree(doc_.layout)) return;
    for (const auto& v : doc_.volumes) {  // composites' layouts (L3)
      if (const auto* c = std::get_if<CompositeContents>(&v.contents); c && in_tree(c->layout)) {
        return;
      }
    }
  }
  void uses_plane(std::size_t from, const feature::PlaneRef& ref) {
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) uses_layout(from, f->feature);
    if (const auto* f = std::get_if<feature::FaceRef>(&ref)) uses_layout(from, f->feature);
  }
  void uses_point(std::size_t from, const feature::PointRef& ref) {
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) uses_layout(from, f->feature);
    if (const auto* e = std::get_if<feature::SketchEntityRef>(&ref)) uses_layout(from, e->sketch);
  }

  static bool changes_body(const feature::Feature& f) {
    switch (f.kind()) {
      case feature::FeatureKind::Sketch:
      case feature::FeatureKind::DatumPlane:
      case feature::FeatureKind::DatumAxis:
      case feature::FeatureKind::DatumPoint:
        return false;
      default:
        return true;
    }
  }

  // A layout's own edges: its variables' expressions, and its features'
  // references. `level` is the root (null) or the composite owning it,
  // whose boundary, joining surfaces and parameters its features may use.
  void tree_edges(const feature::FeatureTree& tree, const Id& level) {
    for (const auto& v : tree.variables().variables()) {
      uses_expression(*find(NodeKind::Variable, v.id), v.expression, level);
    }
    std::optional<std::size_t> last_body;
    for (const auto& f : tree.features()) {
      const std::size_t n = *find(NodeKind::Feature, f.id);
      const auto refs = feature::references_of(f);
      for (const auto& id : refs.features) {
        if (auto t = find(NodeKind::Feature, id)) {
          edge(n, t);
        } else if (!level.is_null() && id == level) {
          edge(n, find(NodeKind::VolumeBoundary, id));
        } else {
          edge(n, find(NodeKind::ConnectionSurface, id));
        }
      }
      for (const auto& name : refs.variables) uses_variable(n, name, level);
      if (const auto* ex = f.as<feature::ExtrudeFeature>();
          ex && ex->extent == feature::Extent::ThroughAll) {
        edge(n, last_body);
      }
      if (changes_body(f) && !f.suppressed) last_body = n;
    }
  }

  void add_edges() {
    tree_edges(doc_.layout, {});
    for (const auto& v : doc_.volumes) {
      if (const auto* comp = std::get_if<CompositeContents>(&v.contents)) {
        tree_edges(comp->layout, v.id);
      }
    }

    for (const auto& v : doc_.volumes) {
      const std::size_t b = *find(NodeKind::VolumeBoundary, v.id);
      if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
        for (const auto& id : fb->features) uses_layout(b, id);
      } else {
        const auto& box = std::get<BoxBoundary>(v.boundary);
        uses_point(b, box.min);
        uses_point(b, box.max);
      }
      for (const auto& k : doc_.keep_outs) {
        if (std::find(k.volumes.begin(), k.volumes.end(), v.id) != k.volumes.end()) {
          edge(b, find(NodeKind::KeepOut, k.id));
        }
      }
      for (const auto& p : v.parameters) {
        uses_param(*find(NodeKind::Parameter, p.id), p.value, v.parent);
      }

      const std::size_t c = *find(NodeKind::VolumeContents, v.id);
      edge(c, b);
      for (const auto& s : doc_.connections) {
        if (joins(doc_, s, v.id)) edge(c, find(NodeKind::ConnectionSurface, s.id));
      }
      for (const auto& p : v.parameters) edge(c, find(NodeKind::Parameter, p.id));
      // A composite's contents are its children (L3).
      if (v.composite()) {
        for (const std::size_t i : children(doc_, v.id)) {
          edge(c, find(NodeKind::VolumeContents, doc_.volumes[i].id));
        }
      }
      for (const auto& o : v.outputs) edge(*find(NodeKind::PublishedOutput, o.id), c);
    }

    for (const auto& k : doc_.keep_outs) {
      const std::size_t n = *find(NodeKind::KeepOut, k.id);
      for (const auto& id : k.cutters) {
        if (doc_.find_volume(id)) {
          edge(n, find(NodeKind::VolumeBoundary, id));
        } else {
          uses_layout(n, id);
        }
      }
    }

    for (const auto& s : doc_.connections) {
      const std::size_t n = *find(NodeKind::ConnectionSurface, s.id);
      const Id level = level_of(doc_, s);
      if (const auto* sp = std::get_if<SketchPatch>(&s.geometry)) {
        uses_layout(n, sp->sketch);
      } else if (const auto* cp = std::get_if<CylinderPatch>(&s.geometry)) {
        if (const auto* f = std::get_if<feature::FeatureRef>(&cp->axis)) uses_layout(n, f->feature);
        if (const auto* e = std::get_if<feature::SketchEntityRef>(&cp->axis)) {
          uses_layout(n, e->sketch);
        }
        for (const Param* p : {&cp->radius, &cp->start_angle, &cp->sweep, &cp->start, &cp->length}) {
          uses_param(n, *p, level);
        }
      } else {
        const auto& dp = std::get<DatumPatch>(s.geometry);
        uses_plane(n, dp.plane);
        uses_param(n, dp.u, level);
        uses_param(n, dp.v, level);
        uses_param(n, dp.width, level);
        uses_param(n, dp.height, level);
      }
      if (const auto* c = std::get_if<ClearanceParams>(&s.params)) uses_param(n, c->gap, level);
      if (const auto* b = std::get_if<BoltedFlangeParams>(&s.params)) {
        uses_param(n, b->min_wall, level);
        uses_param(n, b->holes.u, level);
        uses_param(n, b->holes.v, level);
        if (const auto* bc = std::get_if<BoltCircle>(&b->holes.layout)) {
          uses_param(n, bc->count, level);
          uses_param(n, bc->diameter, level);
          uses_param(n, bc->start_angle, level);
        } else {
          const auto& g = std::get<BoltGrid>(b->holes.layout);
          uses_param(n, g.columns, level);
          uses_param(n, g.rows, level);
          uses_param(n, g.pitch_x, level);
          uses_param(n, g.pitch_y, level);
        }
      }
    }

    for (const auto& l : doc_.links) {
      const std::size_t n = *find(NodeKind::Link, l.id);
      edge(n, find(NodeKind::PublishedOutput, l.source.output));
      // The target uses the link.
      if (auto t = find(NodeKind::Variable, l.target)) edge(*t, n);
      if (auto t = find(NodeKind::Parameter, l.target)) edge(*t, n);
    }
  }
};

}  // namespace

DependencyGraph DependencyGraph::build(const Document& document) {
  GraphBuilder b(document);
  b.run();
  DependencyGraph g;
  g.nodes_ = std::move(b.nodes);
  g.inputs_ = std::move(b.inputs);
  return g;
}

std::optional<std::size_t> DependencyGraph::find(NodeKind kind, const Id& id) const {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].kind == kind && nodes_[i].id == id) return i;
  }
  return std::nullopt;
}

std::vector<std::size_t> DependencyGraph::dependants(std::size_t node) const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const auto& in = inputs_[i];
    if (std::find(in.begin(), in.end(), node) != in.end()) out.push_back(i);
  }
  return out;
}

bool DependencyGraph::depends_on(std::size_t node, std::size_t other) const {
  std::vector<bool> seen(nodes_.size(), false);
  std::vector<std::size_t> stack(inputs_[node].begin(), inputs_[node].end());
  while (!stack.empty()) {
    const std::size_t n = stack.back();
    stack.pop_back();
    if (n == other) return true;
    if (seen[n]) continue;
    seen[n] = true;
    stack.insert(stack.end(), inputs_[n].begin(), inputs_[n].end());
  }
  return false;
}

std::vector<std::vector<std::size_t>> DependencyGraph::loops() const {
  const std::size_t n = nodes_.size();
  enum Color : unsigned char { White, Grey, Black };
  std::vector<Color> color(n, White);
  std::vector<std::size_t> stack;
  std::vector<std::vector<std::size_t>> found;

  std::function<void(std::size_t)> visit = [&](std::size_t v) {
    color[v] = Grey;
    stack.push_back(v);
    for (const std::size_t w : inputs_[v]) {
      if (color[w] == Grey) {
        const auto at = std::find(stack.begin(), stack.end(), w);
        found.emplace_back(at, stack.end());
      } else if (color[w] == White) {
        visit(w);
      }
    }
    stack.pop_back();
    color[v] = Black;
  };
  for (std::size_t v = 0; v < n; ++v) {
    if (color[v] == White) visit(v);
  }

  // Loops of layout variables alone are the variable table's to report
  // (feature.variable.loop); leave them out here.
  found.erase(std::remove_if(found.begin(), found.end(),
                             [&](const std::vector<std::size_t>& loop) {
                               return std::all_of(loop.begin(), loop.end(), [&](std::size_t i) {
                                 return nodes_[i].kind == NodeKind::Variable;
                               });
                             }),
              found.end());

  // Start each loop at its link, if any, else at its first node in document order.
  for (auto& loop : found) {
    auto start = std::find_if(loop.begin(), loop.end(),
                              [&](std::size_t i) { return nodes_[i].kind == NodeKind::Link; });
    if (start == loop.end()) start = std::min_element(loop.begin(), loop.end());
    std::rotate(loop.begin(), start, loop.end());
  }
  return found;
}

std::string DependencyGraph::path(const std::vector<std::size_t>& loop) const {
  std::string out;
  for (const std::size_t i : loop) {
    out += nodes_[i].description;
    out += kArrow;
  }
  if (!loop.empty()) out += nodes_[loop.front()].description;
  return out;
}

Diagnostics DependencyGraph::check() const {
  Diagnostics out;
  for (const auto& loop : loops()) {
    std::vector<SubjectRef> subjects;
    for (const std::size_t i : loop) subjects.push_back(nodes_[i].subject);
    out.add(Diagnostic::error(
        "model.dependency.loop",
        fmt::format("Dependency loop: {}. Data must flow one way; a published output can feed "
                    "only things that aren't upstream of its volume",
                    path(loop)),
        std::move(subjects)));
  }
  return out;
}

std::vector<std::size_t> DependencyGraph::order() const {
  const std::size_t n = nodes_.size();
  // Kahn's algorithm over "uses" edges, smallest index first.
  std::vector<std::size_t> remaining(n);
  for (std::size_t i = 0; i < n; ++i) remaining[i] = inputs_[i].size();
  std::vector<std::vector<std::size_t>> users(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::size_t w : inputs_[i]) users[w].push_back(i);
  }
  std::vector<std::size_t> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (remaining[i] == 0) ready.push_back(i);
  }
  std::vector<std::size_t> out;
  while (!ready.empty()) {
    const auto it = std::min_element(ready.begin(), ready.end());
    const std::size_t v = *it;
    ready.erase(it);
    out.push_back(v);
    for (const std::size_t u : users[v]) {
      if (--remaining[u] == 0) ready.push_back(u);
    }
  }
  return out;
}

}  // namespace vs::model
