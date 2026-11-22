#include "vs/model/check.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "vs/feature/expression.hpp"
#include "vs/model/configuration.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/index.hpp"
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
  explicit Checker(const Document& doc) : doc_(doc), ix_(doc) {}

  Diagnostics run() {
    structure();
    references();
    const DependencyGraph graph = DependencyGraph::build(ix_);
    const Diagnostics loops = graph.check();
    out_.append(loops);
    generated_links(graph);
    configurations(loops.has_errors());
    drawings();
    return std::move(out_);
  }

 private:
  const Document& doc_;
  const DocumentIndex ix_;
  Diagnostics out_;
  // Names and link targets seen so far by the structure checks, which run
  // in document order: each is compared with those above it.
  std::unordered_set<std::string> volume_names_, connection_names_;
  std::unordered_map<Id, Id> link_targets_;  // target -> first link driving it
  // The level whose layout the references being checked are made from (L3):
  // the root layout, or a composite's.
  Scope at_ = Scope::layout();

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
      if (id.is_null()) {
        error("model.document.null_id", what,
              "it has the null id, which every unset reference matches; give it an id of its own");
        return;
      }
      if (!seen.insert(id).second) {
        error("model.document.duplicate_id", what,
              fmt::format("its id {} is also used by something else in the document",
                          id.to_string()));
      }
    };
    auto note_tree = [&](const feature::FeatureTree& tree) {
      for (const auto& t : feature::tree_ids(tree)) note(t.id, t.subject);
    };
    note_tree(doc_.layout);
    for (const auto& v : doc_.volumes) {
      note(v.id, ix_.volume_subject(v.id));
      for (const auto& p : v.parameters) {
        note(p.id, {SubjectKind::Parameter, p.id, fmt::format("{}.{}", v.name, p.name)});
      }
      for (const auto& o : v.outputs) note(o.id, ix_.output_subject({v.id, o.id}));
      if (const auto* t = v.tree()) note_tree(*t);
      if (const auto* comp = std::get_if<CompositeContents>(&v.contents)) {
        for (const auto& t : comp->templates) {
          const SubjectRef ts{SubjectKind::Volume, t.id, fmt::format("template '{}'", t.name)};
          note(t.id, ts);
          for (const auto& p : t.parameters) note(p.id, ts);
          for (const auto& o : t.outputs) note(o.id, ts);
          if (const auto* pc = std::get_if<ParametricContents>(&t.contents)) note_tree(pc->tree);
        }
      }
    }
    for (const auto& c : doc_.connections) note(c.id, ix_.connection_subject(c.id));
    for (const auto& k : doc_.keep_outs) note(k.id, ix_.keep_out_subject(k.id));
    for (const auto& l : doc_.links) note(l.id, doc_.link_subject(l.id));
    for (const auto& c : doc_.configurations) note(c.id, doc_.configuration_subject(c.id));
    for (const auto& d : doc_.drawings) {
      note(d.id, doc_.drawing_subject(d.id));
      for (const auto& v : d.views) note(v.id, doc_.drawing_subject(v.id));
      for (const auto& m : d.dimensions) note(m.id, doc_.drawing_subject(m.id));
    }
  }

  // Drawings (L6): what they name must exist and be of the right kind.
  // Face names aren't checked: a boundary's need a build, and contents'
  // are disposable (vs_draw reports them when it draws).
  void drawings() {
    std::unordered_set<std::string> names;
    for (const auto& d : doc_.drawings) {
      const SubjectRef self = doc_.drawing_subject(d.id);
      if (d.name.empty()) {
        error("model.drawing.name", self, "it has no name; name it");
      } else if (!names.insert(d.name).second) {
        error("model.drawing.name", self,
              "another drawing has the same name; drawings need distinct names");
      }
      auto usable = [](double x) { return std::isfinite(x) && x > 0.0; };
      if (!usable(d.width) || !usable(d.height) || d.width > 10000.0 || d.height > 10000.0) {
        error("model.drawing.sheet", self, "its sheet needs a width and height between 0 and 10 m");
      }
      if (d.decimals < 0 || d.decimals > 6) {
        error("model.drawing.sheet", self, "dimensions show 0 to 6 decimals");
      }
      for (const auto& v : d.views) {
        const SubjectRef vs = doc_.drawing_subject(v.id);
        if (!usable(v.scale) || !std::isfinite(v.x) || !std::isfinite(v.y)) {
          error("model.drawing.sheet", vs, "a view needs a positive scale and a finite position");
        }
        for (const auto& id : v.volumes) {
          const Volume* vol = ix_.volume(id);
          if (!vol || vol->generated) {
            error("model.drawing.volume", vs,
                  fmt::format("it shows volume {}, which isn't in the document", id.to_string()));
          }
        }
      }
      for (const auto& m : d.dimensions) {
        const SubjectRef ms = doc_.drawing_subject(m.id);
        if (!d.find_view(m.view)) {
          error("model.drawing.view", ms, "it names no view of its drawing");
        }
        if (!std::isfinite(m.offset)) error("model.drawing.sheet", ms, "its offset isn't finite");
        anchor(ms, m.from, m.kind == DimensionKind::Diameter, "from");
        if (m.kind != DimensionKind::Diameter) anchor(ms, m.to, false, "to");
      }
    }
  }

  void anchor(const SubjectRef& by, const Anchor& a, bool diameter, std::string_view end) {
    auto bad = [&](std::string why) {
      error("model.drawing.anchor", by, fmt::format("its '{}' anchor {}", end, why));
    };
    auto parts = [&](std::initializer_list<AnchorPart> allowed) {
      if (std::find(allowed.begin(), allowed.end(), a.part) == allowed.end()) {
        bad(fmt::format("can't be the {} of a {}", to_string(a.part), to_string(a.on)));
        return false;
      }
      return true;
    };
    switch (a.on) {
      case AnchorOn::Origin:
        if (diameter) bad("is the origin, which has no diameter");
        parts({AnchorPart::Centre});
        return;
      case AnchorOn::VolumeBoundary:
      case AnchorOn::VolumeContents: {
        const Volume* v = ix_.volume(a.target);
        if (!v || v->generated) {
          bad("names a volume that isn't in the document");
          return;
        }
        if (!parts({AnchorPart::Min, AnchorPart::Max, AnchorPart::Centre, AnchorPart::Face})) {
          return;
        }
        if (a.part == AnchorPart::Face && a.face.empty()) bad("names no face");
        if (diameter && a.part != AnchorPart::Face) bad("needs a cylindrical face for a diameter");
        return;
      }
      case AnchorOn::Surface: {
        const ConnectionSurface* s = ix_.connection(a.target);
        if (!s) {
          bad("names a connection surface that isn't in the document");
          return;
        }
        if (!parts({AnchorPart::Min, AnchorPart::Max, AnchorPart::Centre, AnchorPart::Hole})) {
          return;
        }
        if (a.part == AnchorPart::Hole && s->type() != ConnectionType::BoltedFlange) {
          bad("names a bolt hole of a surface that isn't a bolted flange");
        }
        if (diameter && a.part == AnchorPart::Centre &&
            !std::holds_alternative<CylinderPatch>(s->geometry)) {
          bad("needs a bolt hole or a cylindrical patch for a diameter");
        } else if (diameter && a.part != AnchorPart::Hole && a.part != AnchorPart::Centre) {
          bad("needs a bolt hole or a cylindrical patch for a diameter");
        }
        return;
      }
    }
  }

  // A composite's delegations (L3): each passes a surface joining the
  // composite down to one of its children, once.
  void composite_structure(const Volume& v) {
    const SubjectRef self = ix_.volume_subject(v.id);
    const auto& comp = std::get<CompositeContents>(v.contents);
    std::set<Id> seen;
    for (const auto& d : comp.delegations) {
      const ConnectionSurface* s = ix_.connection(d.surface);
      const Volume* child = ix_.volume(d.child);
      if (s == nullptr || !ix_.joins(*s, v.id)) {
        error("model.composite.delegation", self,
              "it passes down a connection surface that doesn't join it",
              s ? std::vector<SubjectRef>{ix_.connection_subject(s->id)}
                : std::vector<SubjectRef>{});
      } else if (!seen.insert(d.surface).second) {
        error("model.composite.delegation", self,
              fmt::format("it passes '{}' down twice; one child meets a surface", s->name),
              {ix_.connection_subject(s->id)});
      }
      if (child == nullptr || !(child->parent == v.id)) {
        error("model.composite.delegation", self,
              "it passes a connection surface to a volume that isn't one of its children");
      } else if (child->keep_out) {
        error("model.composite.delegation", self,
              fmt::format("it passes a connection surface to '{}', a keep-out volume", child->name),
              {ix_.volume_subject(child->id)});
      }
    }
    templates(v, comp);
    // Seam-bearing surfaces joining it that no child meets: the weld will
    // refuse them, but a composite is often blocked out before its children.
    // A generator (L4) passes them down when it runs.
    if (comp.generates()) return;
    for (const std::size_t k : ix_.joining(v.id)) {
      const ConnectionSurface& s = doc_.connections[k];
      if (comp.delegation(s.id) != nullptr ||
          std::holds_alternative<ClearanceParams>(s.params)) {
        continue;
      }
      out_.add(Diagnostic::warning(
          "model.composite.undelegated",
          fmt::format("'{}': no child meets the connection surface '{}' that joins it; choose "
                      "the child lying against it",
                      self.label, ix_.connection_subject(s.id).label),
          {self, ix_.connection_subject(s.id)}));
    }
  }

  // A composite's templates (L4): named, once each, with legal parameter
  // names; never a composite (the type can't hold one).
  void templates(const Volume& v, const CompositeContents& comp) {
    const SubjectRef self = ix_.volume_subject(v.id);
    std::set<std::string> names;
    for (const auto& t : comp.templates) {
      if (t.name.empty()) {
        error("model.composite.template", self, "one of its templates has no name");
      } else if (!names.insert(t.name).second) {
        error("model.composite.template", self,
              fmt::format("two of its templates are named '{}'", t.name));
      }
      std::set<std::string> params;
      for (const auto& p : t.parameters) {
        if (!feature::is_valid_name(p.name) || !params.insert(p.name).second) {
          error("model.composite.template", self,
                fmt::format("template '{}': '{}' isn't a legal, distinct parameter name", t.name,
                            p.name));
        }
      }
    }
  }

  // Links into a generating composite's inputs (L4): its generator runs
  // before links carry values, so it would never see them.
  void generated_links(const DependencyGraph& graph) {
    for (const auto& v : doc_.volumes) {
      const auto* comp = std::get_if<CompositeContents>(&v.contents);
      if (comp == nullptr || !comp->generates()) continue;
      const auto node = graph.find(NodeKind::VolumeContents, v.id);
      if (!node) continue;
      for (const auto& l : doc_.links) {
        if (ix_.is_within(l.source.volume, v.id)) continue;  // inside it: its children's
        const auto source = graph.find(NodeKind::VolumeContents, l.source.volume);
        if (!source || !graph.depends_on(*node, *source)) continue;
        error("model.composite.generated_link", doc_.link_subject(l.id),
              fmt::format("it feeds the inputs of '{}', whose children a procedure generates "
                          "before links carry values; drive it with a parameter instead",
                          v.name),
              {ix_.volume_subject(v.id)});
      }
    }
  }

  // Configurations (L5, configuration.hpp): each must build. `base_loops`:
  // the authored document already has a dependency loop (reported once).
  void configurations(bool base_loops) {
    if (!doc_.active_configuration.is_null() &&
        doc_.find_configuration(doc_.active_configuration) == nullptr) {
      error("model.configuration.active",
            {SubjectKind::Configuration, doc_.active_configuration, "active configuration"},
            "the active configuration no longer exists; choose another, or the authored values");
    }
    if (doc_.configurations.empty()) return;
    std::unordered_map<Id, Drivable> targets;
    for (auto& d : drivable_targets(doc_)) targets.emplace(d.target, std::move(d));
    std::unordered_map<Id, Id> linked;  // target -> link
    for (const auto& l : doc_.links) linked.emplace(l.target, l.id);
    std::set<std::string> names;
    // The variable tables' own findings as authored, so that only what a
    // configuration adds is reported.
    std::optional<std::set<std::string>> base_root;
    std::unordered_map<Id, std::set<std::string>> base_composite;
    const auto messages = [](const Diagnostics& ds) {
      std::set<std::string> out;
      for (const auto& d : ds) {
        if (d.is_error()) out.insert(d.message);
      }
      return out;
    };

    for (const auto& c : doc_.configurations) {
      const SubjectRef self = doc_.configuration_subject(c.id);
      if (c.name.empty()) {
        error("model.configuration.name", self, "it has no name; name it");
      } else if (!names.insert(c.name).second) {
        error("model.configuration.name", self,
              "another configuration has the same name; configurations need distinct names");
      }
      std::set<Id> seen;
      bool expressions = false;
      bool root_variables = false;
      std::set<Id> composites;  // whose layout variables it drives
      for (const auto& value : c.values) {
        const auto it = targets.find(value.target);
        if (it == targets.end()) {
          error("model.configuration.target", self,
                "one of its values drives something that isn't a layout variable or a volume's "
                "parameter (removed, or inside a volume's contents); remove the value");
          continue;
        }
        const Drivable& d = it->second;
        const SubjectRef target{d.kind == DrivableKind::Parameter ? SubjectKind::Parameter
                                                                  : SubjectKind::Variable,
                                d.target, d.label};
        if (!seen.insert(value.target).second) {
          error("model.configuration.target", self,
                fmt::format("it drives '{}' twice; keep one value", d.label), {target});
          continue;
        }
        if (linked.contains(value.target)) {
          out_.add(Diagnostic::warning(
              "model.configuration.linked",
              fmt::format("'{}': a link also drives '{}'; the link's value replaces this one "
                          "once its source has generated",
                          self.label, d.label),
              {self, target, doc_.link_subject(linked.at(value.target))}));
        }
        if (d.kind == DrivableKind::Variable) root_variables = true;
        if (d.kind == DrivableKind::CompositeVariable) composites.insert(d.owner);
        if (plain_number(value.expression)) continue;
        if (value.expression.find_first_not_of(" \t") == std::string::npos) {
          error("model.configuration.expression", self,
                fmt::format("'{}' has an empty value; remove it to keep the authored value",
                            d.label),
                {target});
          continue;
        }
        auto parsed = feature::Expression::parse(value.expression);
        if (!parsed) {
          error("model.configuration.expression", self,
                fmt::format("'{}' = '{}' doesn't parse: {}", d.label, value.expression,
                            parsed.error().message),
                {target});
          continue;
        }
        expressions = true;
        if (d.kind != DrivableKind::Parameter) continue;  // the tables' check, below
        const Volume* v = ix_.volume(d.owner);
        for (const auto& name : parsed->references()) {
          for (const auto& problem :
               check_variable_name(ix_, Scope{v != nullptr ? v->parent : Id{}}, name, self)) {
            if (!problem.is_error()) continue;
            error("model.configuration.expression", self,
                  fmt::format("'{}' = '{}': {}", d.label, value.expression, problem.message),
                  {target});
          }
        }
      }
      if (!root_variables && composites.empty() && !expressions) continue;

      const Document applied = apply_configuration(doc_, c.id);
      const auto report_tables = [&](const Diagnostics& now, const std::set<std::string>& before) {
        for (const auto& d : now) {
          if (!d.is_error() || before.contains(d.message)) continue;
          std::vector<SubjectRef> more(d.subjects.begin(), d.subjects.end());
          error("model.configuration.expression", self, fmt::format("{} ({})", d.message, d.code),
                std::move(more));
        }
      };
      if (root_variables) {
        if (!base_root) base_root = messages(doc_.layout.variables().check());
        report_tables(applied.layout.variables().check(), *base_root);
      }
      for (const Id& id : composites) {
        const auto* before = std::get_if<CompositeContents>(&ix_.volume(id)->contents);
        const Volume* after = applied.find_volume(id);
        if (before == nullptr || after == nullptr || after->tree() == nullptr) continue;
        if (!base_composite.contains(id)) {
          base_composite.emplace(id, messages(before->layout.variables().check()));
        }
        report_tables(after->tree()->variables().check(), base_composite.at(id));
      }
      if (expressions && !base_loops) {
        const Diagnostics loops = DependencyGraph::build(applied).check();
        for (const auto& d : loops) {
          if (!d.is_error()) continue;
          error("model.configuration.loop", self,
                fmt::format("its values close a dependency loop: {}", d.message), d.subjects);
          break;
        }
      }
    }
  }

  // Volumes a surface, keep-out or link names must be siblings (L3).
  void same_level(const std::vector<Id>& volumes, const SubjectRef& by) {
    std::optional<Id> level;
    for (const Id& id : volumes) {
      const Volume* v = ix_.volume(id);
      if (v == nullptr) continue;
      if (level && !(*level == v->parent)) {
        error("model.level.mixed", by,
              "the volumes it names have different parents; it may only name volumes of one "
              "level, and is owned by their parent");
        return;
      }
      level = v->parent;
    }
  }

  void volume_structure(const Volume& v) {
    const SubjectRef self = ix_.volume_subject(v.id);
    // Generated volumes (L4) are named by their generator and found by id.
    // (volume_structure() runs in document order: the names are those above.)
    if (!v.generated) {
      if (!v.name.empty() && volume_names_.contains(v.name)) {
        error("model.volume.duplicate_name", self,
              "another volume has the same name; volumes need distinct names");
      }
      volume_names_.insert(v.name);
    }
    if (v.parent == v.id && !v.id.is_null()) {
      error("model.volume.parent", self, "it is its own parent; it is inside itself");
    } else if (!v.parent.is_null()) {
      const Volume* p = ix_.volume(v.parent);
      const std::vector<Id> up = ix_.ancestors(v.id);
      if (p == nullptr || !p->composite()) {
        error("model.volume.parent", self,
              p == nullptr ? std::string("its parent volume no longer exists")
                           : fmt::format("its parent '{}' isn't a composite volume; only a "
                                         "composite has child volumes",
                                         p->name),
              p == nullptr ? std::vector<SubjectRef>{}
                           : std::vector<SubjectRef>{ix_.volume_subject(p->id)});
      } else if (!up.empty() && ix_.volume(up.back()) != nullptr &&
                 !ix_.volume(up.back())->parent.is_null()) {
        error("model.volume.parent", self, "its parents form a loop; it is inside itself");
      }
    }
    if (v.composite()) composite_structure(v);
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
        take(check_variable_name(ix_, Scope{v.parent}, name, ps));
      }
    }
    std::set<std::string> outputs;
    for (const auto& o : v.outputs) {
      const SubjectRef os = ix_.output_subject({v.id, o.id});
      if (!outputs.insert(o.name).second) {
        error("model.volume.duplicate_output", os, "another output of the volume has this name");
      }
      if (o.quantity == OutputQuantity::Mass && !(o.density > 0.0)) {
        error("model.volume.bad_output", os, "a mass needs a positive density");
      }
      if (v.composite() && (o.quantity == OutputQuantity::SurfaceArea ||
                            o.quantity == OutputQuantity::Published)) {
        error("model.volume.bad_output", os,
              fmt::format("a composite volume's outputs add up its children's measures; its {} "
                          "doesn't add up, so publish it from a child",
                          to_string(o.quantity)));
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
      error("model.boundary.empty", ix_.boundary_subject(v.id),
            "the boundary has no features; pick the layout features that make it");
    }
    if (const auto* t = v.tree()) take(t->validate());
  }

  void keep_out_structure(const KeepOut& k) {
    const SubjectRef self = ix_.keep_out_subject(k.id);
    if (k.cutters.empty()) error("model.keep_out.no_cutters", self, "it has nothing to cut with");
    if (k.volumes.empty()) error("model.keep_out.no_volumes", self, "it cuts no volume");
    for (const auto& id : k.volumes) {
      if (!ix_.volume(id) && ix_.classify(id).kind != TargetKind::InContents) {
        error("model.keep_out.not_volume", self, "one of the things it cuts isn't a volume");
      }
    }
  }

  void connection_structure(const ConnectionSurface& c) {
    const SubjectRef self = ix_.connection_subject(c.id);
    const auto generated = [&](const ConnectionSurface& s) {
      return std::any_of(s.volumes.begin(), s.volumes.end(), [&](const Id& id) {
        const Volume* v = ix_.volume(id);
        return v != nullptr && v->generated;
      });
    };
    // (In document order: the names are those of the surfaces above.)
    if (!generated(c)) {
      if (!c.name.empty() && connection_names_.contains(c.name)) {
        error("model.connection.duplicate_name", self,
              "another connection surface has the same name");
      }
      connection_names_.insert(c.name);
    }
    const bool distinct = c.volumes.size() != 2 || !(c.volumes[0] == c.volumes[1]);
    if (c.volumes.empty() || c.volumes.size() > 2 || !distinct) {
      error("model.connection.volumes", self,
            "a connection surface joins two distinct volumes, or one volume and the outside");
    }
    for (const auto& id : c.volumes) {
      const Volume* v = ix_.volume(id);
      if (!v) {
        if (ix_.classify(id).kind != TargetKind::InContents) {
          error("model.connection.not_volume", self, "one of the things it joins isn't a volume");
        }
      } else if (v->keep_out) {
        error("model.connection.keep_out", self,
              fmt::format("it joins '{}', a keep-out volume, which has nothing to connect",
                          v->name),
              {ix_.volume_subject(id)});
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
    const Volume* v = ix_.volume(l.source.volume);
    const PublishedOutput* o = v ? v->find_output(l.source.output) : nullptr;
    if (!o) {
      error("model.link.not_output", self, "its source isn't a published output of a volume");
    } else if (!is_scalar(o->quantity)) {
      error("model.link.not_scalar", self,
            fmt::format("the output '{}' is a {}, not a number; only volume, mass and surface "
                        "area can drive a variable or parameter",
                        o->name, to_string(o->quantity)));
    }
    const Target t = ix_.classify(l.target);
    if (t.kind != TargetKind::LayoutVariable && t.kind != TargetKind::Parameter &&
        t.kind != TargetKind::InContents) {
      error("model.link.bad_target", self,
            "its target isn't a layout variable or a volume's parameter");
    }
    // Inside a composite (L3) the target is a variable of the composite's
    // layout or a child's parameter; the composite's own parameters are
    // driven from outside it.
    const Id level = ix_.level_of(l);
    if (!level.is_null() && t.owner == level) {
      const Volume* c = ix_.volume(level);
      const feature::FeatureTree* tree = c != nullptr ? c->tree() : nullptr;
      if (t.kind == TargetKind::Parameter ||
          (t.kind == TargetKind::InContents &&
           (tree == nullptr || tree->variables().find(l.target) == nullptr))) {
        error("model.link.bad_target", self,
              "inside a composite, its target must be a variable of the composite's layout or a "
              "child's parameter");
      }
    }
    // (In document order: the first link above with the same target.)
    const auto [first, fresh] = link_targets_.emplace(l.target, l.id);
    if (!fresh) {
      error("model.link.duplicate_target", self, "another link already drives the same target",
            {doc_.link_subject(first->second)});
    }
  }

  void structure() {
    unique_ids();
    take(doc_.layout.validate());
    for (const auto& v : doc_.volumes) volume_structure(v);
    for (const auto& k : doc_.keep_outs) {
      keep_out_structure(k);
      same_level(k.volumes, ix_.keep_out_subject(k.id));
    }
    for (const auto& c : doc_.connections) {
      connection_structure(c);
      same_level(c.volumes, ix_.connection_subject(c.id));
    }
    for (const auto& l : doc_.links) link_structure(l);
  }

  // --- references -------------------------------------------------------------

  void tree_references(const feature::FeatureTree& tree, const Scope& scope) {
    const auto subjects = tree.subjects();
    for (std::size_t i = 0; i < subjects.size(); ++i) {
      const feature::Feature& f = tree.features()[i];
      const SubjectRef& by = subjects[i];
      const auto refs = feature::references_of(f);
      for (const auto& id : refs.features) take(resolve(ix_, scope, id, &by));
      for (const auto& e : refs.sketch_entities) take(resolve(ix_, scope, e.entity, &by));
      for (const auto& name : refs.variables) take(check_variable_name(ix_, scope, name, by));
    }
    for (const auto& v : tree.variables().variables()) {
      const SubjectRef by = tree.variables().subject(v.id);
      for (const auto& name : expression_names(v.expression)) {
        take(check_variable_name(ix_, scope, name, by));
      }
    }
  }

  void layout_id(const Id& id, const SubjectRef& by) {
    take(resolve(ix_, at_, id, &by));
  }
  void layout_param(const Param& p, const SubjectRef& by) {
    for (const auto& name : expression_names(p.expression)) {
      take(check_variable_name(ix_, at_, name, by));
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
    const SubjectRef by = ix_.boundary_subject(v.id);
    if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
      for (const auto& id : fb->features) {
        auto r = resolve(ix_, at_, id, &by);
        if (!r) {
          out_.add(r.error());
          continue;
        }
        const feature::FeatureTree* layout = ix_.level_layout(at_.volume);
        const feature::Feature* f = layout != nullptr ? layout->find(id) : nullptr;
        if (!f || !is_solid_feature(*f)) {
          error("model.boundary.not_solid_feature", by,
                "each boundary feature must be a layout extrude, revolve, sweep or loft that adds "
                "material",
                f ? std::vector<SubjectRef>{layout->subject(id)} : std::vector<SubjectRef>{});
        }
      }
    } else {
      const auto& box = std::get<BoxBoundary>(v.boundary);
      layout_point(box.min, by);
      layout_point(box.max, by);
    }
  }

  void keep_out_references(const KeepOut& k) {
    const SubjectRef by = ix_.keep_out_subject(k.id);
    for (const auto& id : k.cutters) {
      auto r = resolve(ix_, at_, id, &by);
      if (!r) {
        out_.add(r.error());
        continue;
      }
      if (r->kind == TargetKind::Volume) {
        const Volume* v = ix_.volume(id);
        if (!v->keep_out) {
          error("model.keep_out.bad_cutter", by,
                fmt::format("it cuts with '{}', which isn't a keep-out volume; mark it as one, "
                            "or cut with a layout feature",
                            v->name),
                {ix_.volume_subject(id)});
        }
        continue;
      }
      const feature::FeatureTree* layout = ix_.level_layout(at_.volume);
      const feature::Feature* f = layout != nullptr ? layout->find(id) : nullptr;
      if (!f || !is_solid_feature(*f)) {
        error("model.keep_out.bad_cutter", by,
              "each cutter must be a layout extrude, revolve, sweep or loft, or a keep-out "
              "volume");
      }
    }
    for (const auto& id : k.volumes) layout_id(id, by);
  }

  void connection_references(const ConnectionSurface& c) {
    const SubjectRef by = ix_.connection_subject(c.id);
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
      at_ = Scope{v.parent};
      boundary_references(v);
      if (const auto* t = v.tree()) tree_references(*t, Scope::inside(v.id));
    }
    for (const auto& k : doc_.keep_outs) {
      at_ = Scope{ix_.level_of(k)};
      keep_out_references(k);
    }
    for (const auto& c : doc_.connections) {
      at_ = Scope{ix_.level_of(c)};
      connection_references(c);
    }
    for (const auto& l : doc_.links) {
      at_ = Scope{ix_.level_of(l)};
      const SubjectRef by = doc_.link_subject(l.id);
      take(resolve(ix_, at_, l.source.volume, &by));
      take(resolve(ix_, at_, l.source.output, &by));
      take(resolve(ix_, at_, l.target, &by));
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
