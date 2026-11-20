#include "vs/model/check.hpp"

#include <algorithm>
#include <set>

#include <fmt/format.h>

#include "vs/feature/expression.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/references.hpp"

namespace vs::model {

using core::SubjectKind;

namespace {

bool is_solid_feature(const feature::Feature& f) {
  using feature::Operation;
  if (const auto* e = f.as<feature::ExtrudeFeature>()) return e->operation == Operation::Add;
  if (const auto* r = f.as<feature::RevolveFeature>()) return r->operation == Operation::Add;
  if (const auto* s = f.as<feature::SweepFeature>()) return s->operation == Operation::Add;
  if (const auto* l = f.as<feature::LoftFeature>()) return l->operation == Operation::Add;
  return false;
}

std::vector<std::string> expression_names(const std::string& text) {
  if (text.empty()) return {};
  auto e = feature::Expression::parse(text);
  if (!e) return {};
  return e->references();
}

class Checker {
 public:
  explicit Checker(const Document& doc) : doc_(doc) {}

  Diagnostics run() {
    structure();
    references();
    out_.append(DependencyGraph::build(doc_).check());
    return std::move(out_);
  }

 private:
  const Document& doc_;
  Diagnostics out_;

  void error(std::string code, const SubjectRef& by, std::string message,
             std::vector<SubjectRef> more = {}) {
    std::vector<SubjectRef> subjects{by};
    subjects.insert(subjects.end(), more.begin(), more.end());
    out_.add(Diagnostic::error(std::move(code), fmt::format("'{}': {}", by.label, message),
                               std::move(subjects)));
  }

  void take(Result<Target> r) {
    if (!r) out_.add(r.error());
  }
  void take(const Diagnostics& ds) { out_.append(ds); }

  // --- structure ------------------------------------------------------------

  void unique_ids() {
    std::set<Id> seen;
    auto note = [&](const Id& id, const SubjectRef& what) {
      if (id.is_null()) return;
      if (!seen.insert(id).second) {
        error("model.document.duplicate_id", what,
              fmt::format("its id {} is also used by something else in the document",
                          id.to_string()));
      }
    };
    auto note_tree = [&](const feature::FeatureTree& tree) {
      for (const auto& v : tree.variables().variables()) note(v.id, tree.variables().subject(v.id));
      for (const auto& f : tree.features()) {
        note(f.id, tree.subject(f.id));
        if (const auto* s = f.as<feature::SketchFeature>()) {
          if (!(s->sketch.id() == f.id)) note(s->sketch.id(), tree.subject(f.id));
          for (const auto& e : s->sketch.entities()) note(e.id, s->sketch.subject(e.id));
        }
      }
    };
    note_tree(doc_.layout);
    for (const auto& v : doc_.volumes) {
      note(v.id, doc_.volume_subject(v.id));
      for (const auto& p : v.parameters) {
        note(p.id, {SubjectKind::Parameter, p.id, fmt::format("{}.{}", v.name, p.name)});
      }
      for (const auto& o : v.outputs) note(o.id, doc_.output_subject({v.id, o.id}));
      if (const auto* t = v.tree()) note_tree(*t);
    }
    for (const auto& c : doc_.connections) note(c.id, doc_.connection_subject(c.id));
    for (const auto& k : doc_.keep_outs) note(k.id, doc_.keep_out_subject(k.id));
    for (const auto& l : doc_.links) note(l.id, doc_.link_subject(l.id));
  }

  void volume_structure(const Volume& v) {
    const SubjectRef self = doc_.volume_subject(v.id);
    for (const auto& w : doc_.volumes) {
      if (&w == &v) break;
      if (!v.name.empty() && w.name == v.name) {
        error("model.volume.duplicate_name", self,
              "another volume has the same name; volumes need distinct names");
        break;
      }
    }
    if (v.keep_out && !v.contents_empty()) {
      error("model.volume.keep_out_contents", self,
            "it is a keep-out volume, which reserves empty space; remove its contents or clear "
            "the keep-out flag");
    }
    std::set<std::string> names;
    for (const auto& p : v.parameters) {
      const SubjectRef ps{SubjectKind::Parameter, p.id, fmt::format("{}.{}", v.name, p.name)};
      if (!feature::is_valid_name(p.name)) {
        error("model.volume.bad_parameter", ps, "not a legal parameter name");
      } else if (!names.insert(p.name).second) {
        error("model.volume.bad_parameter", ps, "another parameter of the volume has this name");
      }
      for (const auto& name : expression_names(p.value.expression)) {
        take(check_variable_name(doc_, Scope::layout(), name, ps));
      }
    }
    std::set<std::string> outputs;
    for (const auto& o : v.outputs) {
      const SubjectRef os = doc_.output_subject({v.id, o.id});
      if (!outputs.insert(o.name).second) {
        error("model.volume.duplicate_output", os, "another output of the volume has this name");
      }
      if (o.quantity == OutputQuantity::Mass && !(o.density > 0.0)) {
        error("model.volume.bad_output", os, "a mass needs a positive density");
      }
    }
    if (const auto* s = std::get_if<StaticContents>(&v.contents)) {
      static const std::set<std::string> kFormats{"step", "iges", "stl", "3mf", "obj"};
      if (!s->blob.empty() && !kFormats.contains(s->format)) {
        error("model.volume.bad_format", self,
              fmt::format("static contents in an unknown format '{}' (step, iges, stl, 3mf or "
                          "obj)",
                          s->format));
      }
    }
    if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary); fb && fb->features.empty()) {
      error("model.boundary.empty", doc_.boundary_subject(v.id),
            "the boundary has no features; pick the layout features that make it");
    }
    if (const auto* t = v.tree()) take(t->validate());
  }

  void keep_out_structure(const KeepOut& k) {
    const SubjectRef self = doc_.keep_out_subject(k.id);
    if (k.cutters.empty()) error("model.keep_out.no_cutters", self, "it has nothing to cut with");
    if (k.volumes.empty()) error("model.keep_out.no_volumes", self, "it cuts no volume");
    for (const auto& id : k.volumes) {
      if (!doc_.find_volume(id) && classify(doc_, id).kind != TargetKind::InContents) {
        error("model.keep_out.not_volume", self, "one of the things it cuts isn't a volume");
      }
    }
  }

  void connection_structure(const ConnectionSurface& c) {
    const SubjectRef self = doc_.connection_subject(c.id);
    for (const auto& w : doc_.connections) {
      if (&w == &c) break;
      if (!c.name.empty() && w.name == c.name) {
        error("model.connection.duplicate_name", self,
              "another connection surface has the same name");
        break;
      }
    }
    const bool distinct = c.volumes.size() != 2 || !(c.volumes[0] == c.volumes[1]);
    if (c.volumes.empty() || c.volumes.size() > 2 || !distinct) {
      error("model.connection.volumes", self,
            "a connection surface joins two distinct volumes, or one volume and the outside");
    }
    for (const auto& id : c.volumes) {
      const Volume* v = doc_.find_volume(id);
      if (!v) {
        if (classify(doc_, id).kind != TargetKind::InContents) {
          error("model.connection.not_volume", self, "one of the things it joins isn't a volume");
        }
      } else if (v->keep_out) {
        error("model.connection.keep_out", self,
              fmt::format("it joins '{}', a keep-out volume, which has nothing to connect",
                          v->name),
              {doc_.volume_subject(id)});
      }
    }
    if (const auto* b = std::get_if<BoltedFlangeParams>(&c.params); b && !thread_size(b->thread)) {
      error("model.connection.unknown_thread", self,
            fmt::format("unknown thread size '{}' (M2 to M12, ISO metric coarse)", b->thread));
    }
    if (std::holds_alternative<BoltedFlangeParams>(c.params) &&
        std::holds_alternative<CylinderPatch>(c.geometry)) {
      error("model.connection.curved_flange", self,
            "a bolted flange must be planar; use a sketch profile or a datum rectangle, or make "
            "the surface bonded");
    }
  }

  void link_structure(const Link& l) {
    const SubjectRef self = doc_.link_subject(l.id);
    const Volume* v = doc_.find_volume(l.source.volume);
    const PublishedOutput* o = v ? v->find_output(l.source.output) : nullptr;
    if (!o) {
      error("model.link.not_output", self, "its source isn't a published output of a volume");
    } else if (!is_scalar(o->quantity)) {
      error("model.link.not_scalar", self,
            fmt::format("the output '{}' is a {}, not a number; only volume, mass and surface "
                        "area can drive a variable or parameter",
                        o->name, to_string(o->quantity)));
    }
    const Target t = classify(doc_, l.target);
    if (t.kind != TargetKind::LayoutVariable && t.kind != TargetKind::Parameter &&
        t.kind != TargetKind::InContents) {
      error("model.link.bad_target", self,
            "its target isn't a layout variable or a volume's parameter");
    }
    for (const auto& other : doc_.links) {
      if (&other == &l) break;
      if (other.target == l.target) {
        error("model.link.duplicate_target", self,
              "another link already drives the same target", {doc_.link_subject(other.id)});
        break;
      }
    }
  }

  void structure() {
    unique_ids();
    take(doc_.layout.validate());
    for (const auto& v : doc_.volumes) volume_structure(v);
    for (const auto& k : doc_.keep_outs) keep_out_structure(k);
    for (const auto& c : doc_.connections) connection_structure(c);
    for (const auto& l : doc_.links) link_structure(l);
  }

  // --- references -------------------------------------------------------------

  void tree_references(const feature::FeatureTree& tree, const Scope& scope) {
    for (const auto& f : tree.features()) {
      const SubjectRef by = tree.subject(f.id);
      const auto refs = feature::references_of(f);
      for (const auto& id : refs.features) take(resolve(doc_, scope, id, &by));
      for (const auto& e : refs.sketch_entities) take(resolve(doc_, scope, e.entity, &by));
      for (const auto& name : refs.variables) take(check_variable_name(doc_, scope, name, by));
    }
    for (const auto& v : tree.variables().variables()) {
      const SubjectRef by = tree.variables().subject(v.id);
      for (const auto& name : expression_names(v.expression)) {
        take(check_variable_name(doc_, scope, name, by));
      }
    }
  }

  void layout_id(const Id& id, const SubjectRef& by) {
    take(resolve(doc_, Scope::layout(), id, &by));
  }
  void layout_param(const Param& p, const SubjectRef& by) {
    for (const auto& name : expression_names(p.expression)) {
      take(check_variable_name(doc_, Scope::layout(), name, by));
    }
  }
  void layout_point(const feature::PointRef& ref, const SubjectRef& by) {
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) layout_id(f->feature, by);
    if (const auto* e = std::get_if<feature::SketchEntityRef>(&ref)) {
      layout_id(e->sketch, by);
      layout_id(e->entity, by);
    }
  }
  void layout_plane(const feature::PlaneRef& ref, const SubjectRef& by) {
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) layout_id(f->feature, by);
    if (const auto* f = std::get_if<feature::FaceRef>(&ref)) layout_id(f->feature, by);
  }

  void boundary_references(const Volume& v) {
    const SubjectRef by = doc_.boundary_subject(v.id);
    if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
      for (const auto& id : fb->features) {
        auto r = resolve(doc_, Scope::layout(), id, &by);
        if (!r) {
          out_.add(r.error());
          continue;
        }
        const feature::Feature* f = doc_.layout.find(id);
        if (!f || !is_solid_feature(*f)) {
          error("model.boundary.not_solid_feature", by,
                "each boundary feature must be a layout extrude, revolve, sweep or loft that adds "
                "material",
                f ? std::vector<SubjectRef>{doc_.layout.subject(id)} : std::vector<SubjectRef>{});
        }
      }
    } else {
      const auto& box = std::get<BoxBoundary>(v.boundary);
      layout_point(box.min, by);
      layout_point(box.max, by);
    }
  }

  void keep_out_references(const KeepOut& k) {
    const SubjectRef by = doc_.keep_out_subject(k.id);
    for (const auto& id : k.cutters) {
      auto r = resolve(doc_, Scope::layout(), id, &by);
      if (!r) {
        out_.add(r.error());
        continue;
      }
      if (r->kind == TargetKind::Volume) {
        const Volume* v = doc_.find_volume(id);
        if (!v->keep_out) {
          error("model.keep_out.bad_cutter", by,
                fmt::format("it cuts with '{}', which isn't a keep-out volume; mark it as one, "
                            "or cut with a layout feature",
                            v->name),
                {doc_.volume_subject(id)});
        }
        continue;
      }
      const feature::Feature* f = doc_.layout.find(id);
      if (!f || !is_solid_feature(*f)) {
        error("model.keep_out.bad_cutter", by,
              "each cutter must be a layout extrude, revolve, sweep or loft, or a keep-out "
              "volume");
      }
    }
    for (const auto& id : k.volumes) layout_id(id, by);
  }

  void connection_references(const ConnectionSurface& c) {
    const SubjectRef by = doc_.connection_subject(c.id);
    if (const auto* sp = std::get_if<SketchPatch>(&c.geometry)) {
      layout_id(sp->sketch, by);
    } else if (const auto* cp = std::get_if<CylinderPatch>(&c.geometry)) {
      if (const auto* f = std::get_if<feature::FeatureRef>(&cp->axis)) layout_id(f->feature, by);
      if (const auto* e = std::get_if<feature::SketchEntityRef>(&cp->axis)) {
        layout_id(e->sketch, by);
        layout_id(e->entity, by);
      }
      for (const Param* p : {&cp->radius, &cp->start_angle, &cp->sweep, &cp->start, &cp->length}) {
        layout_param(*p, by);
      }
    } else {
      const auto& dp = std::get<DatumPatch>(c.geometry);
      layout_plane(dp.plane, by);
      layout_param(dp.u, by);
      layout_param(dp.v, by);
      layout_param(dp.width, by);
      layout_param(dp.height, by);
    }
    if (const auto* cl = std::get_if<ClearanceParams>(&c.params)) layout_param(cl->gap, by);
    if (const auto* b = std::get_if<BoltedFlangeParams>(&c.params)) {
      layout_param(b->min_wall, by);
      layout_param(b->holes.u, by);
      layout_param(b->holes.v, by);
      if (const auto* bc = std::get_if<BoltCircle>(&b->holes.layout)) {
        layout_param(bc->count, by);
        layout_param(bc->diameter, by);
        layout_param(bc->start_angle, by);
      } else {
        const auto& g = std::get<BoltGrid>(b->holes.layout);
        layout_param(g.columns, by);
        layout_param(g.rows, by);
        layout_param(g.pitch_x, by);
        layout_param(g.pitch_y, by);
      }
    }
    for (const auto& id : c.volumes) layout_id(id, by);
  }

  void references() {
    tree_references(doc_.layout, Scope::layout());
    for (const auto& v : doc_.volumes) {
      boundary_references(v);
      if (const auto* t = v.tree()) tree_references(*t, Scope::inside(v.id));
    }
    for (const auto& k : doc_.keep_outs) keep_out_references(k);
    for (const auto& c : doc_.connections) connection_references(c);
    for (const auto& l : doc_.links) {
      const SubjectRef by = doc_.link_subject(l.id);
      take(resolve(doc_, Scope::layout(), l.source.volume, &by));
      take(resolve(doc_, Scope::layout(), l.source.output, &by));
      take(resolve(doc_, Scope::layout(), l.target, &by));
    }
  }
};

}  // namespace

Diagnostics check(const Document& document) { return Checker(document).run(); }

Diagnostics edit(Document& document, const std::function<void(Document&)>& change) {
  Document copy = document;
  change(copy);
  Diagnostics findings = check(copy);
  if (!findings.has_errors()) document = std::move(copy);
  return findings;
}

}  // namespace vs::model
