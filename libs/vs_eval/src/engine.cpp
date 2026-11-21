#include "vs/eval/engine.hpp"
#include "vs/model/hierarchy.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "vs/eval/measure.hpp"
#include "vs/core/version.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/validate/containment.hpp"

namespace vs::eval {

using core::Diagnostic;
using core::Hasher;
using core::SubjectRef;

const VolumeResult* Evaluation::find(const Id& volume) const {
  for (const auto& v : volumes) {
    if (v.volume == volume) return &v;
  }
  return nullptr;
}

Hash256 Evaluation::output_hash() const {
  Hasher h;
  h.add(std::string_view("vstudio.eval.evaluation"));
  h.add(static_cast<std::uint64_t>(volumes.size()));
  for (const auto& v : volumes) {
    h.add(v.volume).add(v.status).add(v.stale_contents);
    h.add(v.contents != nullptr);
    if (v.contents) h.add(v.contents->output_hash);
  }
  h.add(combined.has_value());
  if (combined) {
    const mesh::Mesh& m = combined->weld.mesh;
    h.add(static_cast<std::uint64_t>(m.positions.size()));
    for (const auto& p : m.positions) h.add(p);
    h.add(static_cast<std::uint64_t>(m.indices.size()));
    for (const auto i : m.indices) h.add(i);
  }
  return h.finalize();
}

namespace {

// L2: per triangle of a volume's mesh, the containment slack its seams
// allow. Triangles touching a curved seam's vertex are the seam's chords of
// the shared surface, or meet them, so their centroids may stray outside by
// the seam's deflection plus the boundary tessellation's; nothing else gets
// any. Empty when no curved surface joins the volume.
std::vector<float> curved_seam_slack(const model::Document& document,
                                     const model::DocumentBuild& layout, const Id& volume,
                                     const mesh::Mesh& m, const core::ProductSettings& settings) {
  bool curved = false;
  for (std::size_t k = 0; k < document.connections.size(); ++k) {
    if (model::joins(document, document.connections[k], volume) && k < layout.connections.size() &&
        layout.connections[k].ok && layout.connections[k].cylinder) {
      curved = true;
    }
  }
  if (!curved) return {};
  std::unordered_set<Id> ids;
  for (const mesh::Seam& s : mesh::build_seams(document, layout, settings, volume).seams) {
    if (!s.curved()) continue;
    for (const mesh::SeamVertex& v : s.vertices) ids.insert(v.id);
  }
  std::vector<float> slack(m.triangle_count(), 0.0f);
  for (std::size_t t = 0; t < slack.size(); ++t) {
    for (const std::uint32_t v : m.triangle(t)) {
      if (!m.seam_vertices[v].is_null() && ids.contains(m.seam_vertices[v])) {
        slack[t] = static_cast<float>(2.0 * mesh::kSeamDeflection);
      }
    }
  }
  return slack;
}

// The document with each link's value written into its target: a layout
// variable takes it as its expression, a parameter as its value.
model::Document apply_links(const model::Document& document, const std::map<Id, double>& values) {
  model::Document out = document;
  for (const auto& [target, value] : values) {
    // The shortest text that reads back as the same double.
    if (out.layout.variables().find(target) != nullptr) {
      (void)out.layout.variables().set_expression(target, fmt::format("{}", value));
      continue;
    }
    for (auto& v : out.volumes) {
      for (auto& p : v.parameters) {
        if (p.id == target) p.value = feature::Param::of(value);
      }
      // A variable of a composite's layout (L3).
      if (auto* c = std::get_if<model::CompositeContents>(&v.contents);
          c != nullptr && c->layout.variables().find(target) != nullptr) {
        (void)c->layout.variables().set_expression(target, fmt::format("{}", value));
      }
    }
  }
  return out;
}

// A volume's contents measured for its composite parent's outputs (L3).
model::ContentsMeasure measure_contents(const model::Volume& v, const GeneratedContents& c,
                                        ThreadPool& pool) {
  if (!c.solid.is_empty()) return model::measure_solid(c.solid);
  model::ContentsMeasure m;
  if (!c.mesh || c.mesh->triangle_count() == 0) return m;
  model::Volume probe;
  probe.id = v.id;
  probe.outputs = {{Id::derive(v.id, "volume"), "volume", model::OutputQuantity::Volume, 1.0},
                   {Id::derive(v.id, "centre"), "centre", model::OutputQuantity::CentreOfGravity,
                    1.0},
                   {Id::derive(v.id, "envelope"), "envelope", model::OutputQuantity::Envelope,
                    1.0}};
  const auto measured = measure_mesh(probe, *c.mesh, pool);
  if (measured.size() != 3 || !measured[0].value || !measured[1].value || !measured[2].value) {
    return m;
  }
  m.volume = std::get<double>(*measured[0].value);
  m.moment = std::get<core::dvec3>(*measured[1].value) * m.volume;
  m.envelope = std::get<core::dbox3>(*measured[2].value);
  m.any = true;
  return m;
}

// The composite whose generator made `id` (a volume, or a surface joining
// generated volumes); null for anything authored (L4).
Id generated_owner(const model::Document& document, const Id& id) {
  if (const model::Volume* v = document.find_volume(id)) return v->generated ? v->parent : Id{};
  if (const model::ConnectionSurface* c = document.find_connection(id)) {
    for (const Id& vid : c->volumes) {
      const model::Volume* v = document.find_volume(vid);
      if (v != nullptr && v->generated) return v->parent;
    }
  }
  return {};
}

// One generation, before containment.
struct Generated {
  bool ran = false;
  std::shared_ptr<GeneratedContents> contents;
  std::optional<Hash256> second_hash;  // the determinism check's
};

}  // namespace

Engine::Engine(ProductSettings settings, EngineOptions options)
    : settings_(settings),
      options_(std::move(options)),
      pool_(options_.threads),
      cache_(options_.disk_cache, options_.memory_cache_bytes) {}

Engine::~Engine() = default;

std::map<Id, VolumeStatus> Engine::states() const {
  const std::lock_guard lock(state_mutex_);
  return states_;
}

void Engine::reset() {
  const std::lock_guard eval_lock(evaluate_mutex_);
  const std::lock_guard lock(state_mutex_);
  states_.clear();
  last_valid_.clear();
  layouts_.clear();
  cache_.clear_memory();
}

void Engine::set_generator(model::VolumeKind kind, std::shared_ptr<ContentsGenerator> generator) {
  const std::lock_guard eval_lock(evaluate_mutex_);
  if (generator) {
    generators_[kind] = std::move(generator);
  } else {
    generators_.erase(kind);
  }
}

std::shared_ptr<ContentsGenerator> Engine::generator(model::VolumeKind kind) const {
  const auto it = generators_.find(kind);
  return it == generators_.end() ? nullptr : it->second;
}

void Engine::set_layout_generator(std::shared_ptr<LayoutGenerator> generator) {
  const std::lock_guard eval_lock(evaluate_mutex_);
  layout_generator_ = std::move(generator);
  layouts_.clear();
}

std::shared_ptr<LayoutGenerator> Engine::layout_generator() const { return layout_generator_; }

// The document with every generating composite's children written in (L4),
// and what happened to the composites that got none.
struct Engine::Expansion {
  model::Document document;
  std::map<Id, Diagnostics> failed;  // generator or expansion failed: Failed
  std::set<Id> stale;                // no layout generator: Stale
};

Engine::Expansion Engine::expand_generated(const model::Document& document,
                                           const CancellationToken& cancel,
                                           EvaluationStats& stats) {
  Expansion out;
  out.document = document;
  const model::DocumentBuild layout = model::build_layout(document, settings_);
  ++stats.layout_builds;
  for (const std::size_t i : model::level_order(document)) {
    const model::Volume& v = document.volumes[i];
    const auto* comp = std::get_if<model::CompositeContents>(&v.contents);
    if (comp == nullptr || !comp->generates()) continue;
    const SubjectRef self = document.volume_subject(v.id);
    if (!layout_generator_) {
      out.stale.insert(v.id);
      out.failed[v.id].add(Diagnostic::warning(
          "eval.generated.no_generator",
          fmt::format("'{}': its children come from a procedure, and none is installed to run it",
                      self.label),
          {self}));
      continue;
    }
    // A boundary that failed fails the composite already (finish_composite).
    const std::optional<Hash256> key = cache_key(document, layout, i, settings_);
    if (!key) continue;
    std::shared_ptr<const LayoutOutcome> outcome;
    if (const auto it = layouts_.find(*key); it != layouts_.end()) {
      outcome = it->second;
    } else {
      outcome = std::make_shared<const LayoutOutcome>(
          layout_generator_->generate(document, layout, i, settings_, pool_, cancel));
      ++stats.layouts_generated;
      if (layouts_.size() >= 256) layouts_.clear();
      layouts_.emplace(*key, outcome);
    }
    if (!outcome->layout) {
      out.failed[v.id] = outcome->diagnostics;
      if (!out.failed[v.id].has_errors()) {
        out.failed[v.id].add(Diagnostic::error(
            "eval.generated.layout",
            fmt::format("'{}': its procedure made no children", self.label), {self}));
      }
      continue;
    }
    auto expanded = model::expand(out.document, v.id, *outcome->layout);
    Diagnostics problems;
    if (!expanded) {
      problems.add(expanded.error());
    } else {
      for (auto d : model::check(*expanded)) {
        if (!d.is_error()) continue;
        if (d.subjects.empty() || !(d.subjects.front().id == v.id)) {
          d.subjects.insert(d.subjects.begin(), self);
        }
        problems.add(std::move(d));
      }
    }
    if (problems.has_errors()) {
      out.failed[v.id] = std::move(problems);
      continue;
    }
    out.document = std::move(expanded).value();
  }
  return out;
}

Evaluation Engine::preview(const model::Project& project) {
  EvaluateOptions options;
  options.generate = false;
  return evaluate(project, options);
}

Evaluation Engine::evaluate(const model::Project& project, const EvaluateOptions& options) {
  const std::lock_guard eval_lock(evaluate_mutex_);

  // 1. check (below), and generated children (L4) written into the document.
  Diagnostics checked = model::check(project.document);
  EvaluationStats early_stats;
  Expansion expansion;
  const bool generates = std::any_of(
      project.document.volumes.begin(), project.document.volumes.end(),
      [](const model::Volume& v) {
        const auto* c = std::get_if<model::CompositeContents>(&v.contents);
        return c != nullptr && c->generates();
      });
  if (generates && !checked.has_errors()) {
    expansion = expand_generated(project.document, options.cancel, early_stats);
  }
  const model::Document& doc = generates && !checked.has_errors() ? expansion.document
                                                                  : project.document;
  const std::size_t n = doc.volumes.size();

  std::mutex progress_mutex;
  std::size_t scheduled = 0;
  std::size_t finished = 0;
  const auto notify = [&](ProgressEvent::Kind kind, const Id& volume, VolumeStatus status) {
    const std::lock_guard lock(progress_mutex);
    if (kind == ProgressEvent::Kind::VolumeFinished) ++finished;
    if (!options.progress) return;
    ProgressEvent e;
    e.kind = kind;
    e.volume = volume;
    e.status = status;
    e.done = finished;
    e.total = scheduled;
    options.progress(e);
  };
  const auto set_state = [&](const Id& volume, VolumeStatus status) {
    const std::lock_guard lock(state_mutex_);
    states_[volume] = status;
  };

  Evaluation ev;
  ev.stats = early_stats;
  ev.document = doc;
  ev.volumes.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    VolumeResult& r = ev.volumes[i];
    r.volume = doc.volumes[i].id;
    r.status = VolumeStatus::Stale;
    const auto it = last_valid_.find(r.volume);
    if (it != last_valid_.end()) {
      r.contents = it->second;
      r.stale_contents = true;
    }
  }

  // Shows `c` if it is good, else the last valid contents, stale.
  const auto accept = [&](VolumeResult& r, const ContentsPtr& c) {
    r.diagnostics.append(c->diagnostics);
    if (c->ok()) {
      r.status = VolumeStatus::Current;
      r.contents = c;
      r.stale_contents = false;
      last_valid_[r.volume] = c;
    } else {
      r.status = VolumeStatus::Failed;
    }
  };

  // A composite (L3) never generates: once its children are done (they are
  // upstream of it), its status is the worst of theirs, its outputs add
  // their measures up and its key hashes theirs. `layout` is the current
  // wave's layout build.
  model::DocumentBuild layout;
  // Composites whose generated children fail the current layout's checks.
  std::set<Id> generated_layout_failed;
  std::vector<model::ContentsMeasure> measures(n);
  const bool composite_outputs = std::any_of(
      doc.volumes.begin(), doc.volumes.end(),
      [](const model::Volume& v) { return v.composite() && !v.outputs.empty(); });
  const auto measure_of = [&](std::size_t j) {
    const VolumeResult& r = ev.volumes[j];
    if (ev.document.volumes[j].composite()) return measures[j];
    if (!r.contents || r.status != VolumeStatus::Current) return model::ContentsMeasure{};
    return measure_contents(ev.document.volumes[j], *r.contents, pool_);
  };
  const auto finish_composite = [&](std::size_t i) {
    const model::Volume& v = ev.document.volumes[i];
    const model::VolumeBuild& vb = layout.volumes[i];
    VolumeResult& r = ev.volumes[i];
    auto c = std::make_shared<GeneratedContents>();
    c->status = vb.status == VolumeStatus::Failed || vb.boundary.is_empty()
                    ? VolumeStatus::Failed
                    : VolumeStatus::Current;
    c->rebuild = vb.contents_rebuild;
    Hasher key;
    key.add(std::string_view("vstudio.eval.composite"));
    const std::optional<Hash256> own = cache_key(ev.document, layout, i, settings_);
    key.add(own.has_value());
    if (own) key.add(*own);
    Hasher out;
    out.add(std::string_view("vstudio.eval.composite.output"));
    model::ContentsMeasure total;
    for (const std::size_t j : model::children(ev.document, v.id)) {
      const VolumeResult& child = ev.volumes[j];
      c->status = model::worse(c->status, child.status);
      key.add(child.volume).add(child.key.has_value());
      if (child.key) key.add(*child.key);
      out.add(child.volume).add(child.status).add(child.contents != nullptr);
      if (child.contents) out.add(child.contents->output_hash);
      if (composite_outputs) total.add(measure_of(j));
    }
    // L4: a generator that failed or isn't installed, or generated children
    // failing the layout checks.
    const SubjectRef self = ev.document.volume_subject(v.id);
    if (const auto it = expansion.failed.find(v.id); it != expansion.failed.end()) {
      c->diagnostics.append(it->second);
      c->status = model::worse(c->status, expansion.stale.contains(v.id) ? VolumeStatus::Stale
                                                                         : VolumeStatus::Failed);
    }
    if (generated_layout_failed.contains(v.id)) {
      c->status = VolumeStatus::Failed;
      c->diagnostics.add(Diagnostic::error(
          "eval.generated.layout",
          fmt::format("'{}': the children its procedure generated fail the layout checks (see "
                      "the layout's diagnostics); change the procedure or its parameters",
                      self.label),
          {self}));
    }
    r.diagnostics.append(c->diagnostics);
    measures[i] = total;
    c->outputs = model::composite_outputs(v, total);
    c->key = key.finalize();
    const Hash256 measured =
        output_hash(c->status, kernel::Solid{}, nullptr, c->outputs, nullptr);
    out.add(measured);
    c->output_hash = out.finalize();
    r.key = c->key;
    r.status = c->status;
    r.contents = std::move(c);
    r.stale_contents = false;
    set_state(r.volume, r.status);
  };

  // 1. check
  ev.diagnostics.append(checked);
  if (ev.diagnostics.has_errors()) {
    for (const auto& r : ev.volumes) set_state(r.volume, r.status);
    notify(ProgressEvent::Kind::Finished, {}, VolumeStatus::Stale);
    return ev;
  }
  ev.checked = true;

  // Which volumes each one waits for: those its contents depend on, through
  // links into its boundary or parameters.
  const model::DependencyGraph graph = model::DependencyGraph::build(doc);
  std::vector<std::vector<std::size_t>> upstream(n);
  {
    std::vector<std::optional<std::size_t>> node(n);
    for (std::size_t i = 0; i < n; ++i) {
      node[i] = graph.find(model::NodeKind::VolumeContents, doc.volumes[i].id);
    }
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        if (i != j && node[i] && node[j] && graph.depends_on(*node[i], *node[j])) {
          upstream[i].push_back(j);
        }
      }
    }
  }

  std::map<Id, double> link_values;
  bool values_changed = true;
  std::vector<bool> done(n, false);
  Diagnostics layout_diagnostics;  // of the last layout build and its checks

  for (;;) {
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
      if (done[i]) continue;
      if (std::all_of(upstream[i].begin(), upstream[i].end(),
                      [&](std::size_t j) { return done[j]; })) {
        ready.push_back(i);
      }
    }

    // 2. the layout, rebuilt when link values changed it
    if (values_changed) {
      ev.document = apply_links(doc, link_values);
      layout = model::build_layout(ev.document, settings_);
      ev.layout = validate::validate_layout(ev.document, layout, settings_);
      layout_diagnostics = layout.diagnostics;
      layout_diagnostics.append(ev.layout.diagnostics);
      generated_layout_failed.clear();
      for (const auto& d : layout_diagnostics) {
        if (!d.is_error()) continue;
        for (const auto& s : d.subjects) {
          const Id owner = generated_owner(ev.document, s.id);
          if (!owner.is_null()) generated_layout_failed.insert(owner);
        }
      }
      ++ev.stats.layout_builds;
      values_changed = false;
      notify(ProgressEvent::Kind::LayoutBuilt, {}, VolumeStatus::Stale);
    }
    if (ready.empty()) break;
    if (options.cancel.cancelled()) {
      ev.cancelled = true;
      break;
    }
    ++ev.stats.waves;

    // 3. keys, cache, and what must regenerate
    std::vector<std::size_t> to_run;
    for (const std::size_t i : ready) {
      const model::Volume& v = ev.document.volumes[i];
      VolumeResult& r = ev.volumes[i];
      const SubjectRef self = ev.document.volume_subject(v.id);
      if (v.composite()) {
        finish_composite(i);
        continue;
      }
      const auto blocker = std::find_if(upstream[i].begin(), upstream[i].end(), [&](std::size_t j) {
        return ev.volumes[j].status != VolumeStatus::Current;
      });
      if (blocker != upstream[i].end()) {
        const SubjectRef other = ev.document.volume_subject(ev.document.volumes[*blocker].id);
        r.diagnostics.add(Diagnostic::warning(
            "eval.blocked",
            fmt::format("'{}': not regenerated, because '{}', which it depends on, isn't current",
                        self.label, other.label),
            {self, other}));
        r.status = VolumeStatus::Stale;
        ++ev.stats.blocked;
        continue;
      }
      r.key = cache_key(ev.document, layout, i, settings_);
      if (!r.key) {
        r.status = VolumeStatus::Failed;  // its boundary or a parameter failed
        continue;
      }
      if (v.kind() == model::VolumeKind::Procedural && !v.contents_empty() &&
          generator(v.kind()) == nullptr) {
        r.status = VolumeStatus::Stale;  // no generator for node graphs installed
        continue;
      }
      bool disk = false;
      if (ContentsPtr hit = cache_.find(*r.key, &disk)) {
        r.from_cache = true;
        r.from_disk = disk;
        ++ev.stats.cache_hits;
        if (disk) ++ev.stats.disk_hits;
        accept(r, hit);
        continue;
      }
      if (!options.generate) {
        r.status = VolumeStatus::Stale;  // the preview tier
        continue;
      }
      to_run.push_back(i);
    }

    {
      const std::lock_guard lock(progress_mutex);
      scheduled += to_run.size();
    }
    for (const std::size_t i : to_run) set_state(ev.volumes[i].volume, VolumeStatus::Regenerating);

    const model::Document& wave_doc = ev.document;

    // Generators prepare their jobs here, on the calling thread.
    std::vector<ContentsGenerator::Job> jobs(n);
    for (const std::size_t i : to_run) {
      const model::Volume& v = wave_doc.volumes[i];
      if (v.contents_empty()) continue;
      if (const auto gen = generator(v.kind())) {
        jobs[i] = gen->prepare(wave_doc, layout, i, settings_, pool_, options.cancel);
      }
    }

    const auto generate = [&](std::size_t i) {
      const model::Volume& v = wave_doc.volumes[i];
      auto g = std::make_shared<GeneratedContents>();
      g->key = *ev.volumes[i].key;
      if (jobs[i]) {
        g->status = VolumeStatus::Current;
        jobs[i](*g);
        g->output_hash = output_hash(g->status, g->solid, g->mesh ? &*g->mesh : nullptr,
                                     g->outputs, g->extra.get());
        return g;
      }
      const auto* sc = std::get_if<model::StaticContents>(&v.contents);
      if (sc != nullptr && !v.keep_out && !sc->blob.empty() && mesh::is_mesh_format(sc->format)) {
        // vs_model leaves mesh files to vs_mesh: read them here.
        const SubjectRef self = wave_doc.volume_subject(v.id);
        const auto it = project.blobs.find(sc->blob);
        if (it == project.blobs.end()) {
          g->status = VolumeStatus::Failed;
          g->diagnostics.add(Diagnostic::error(
              "model.contents.missing_blob",
              fmt::format("'{}': its file '{}' isn't in the project", self.label, sc->blob),
              {self}));
        } else if (auto m = mesh::read_mesh(*it->second, sc->format); !m) {
          g->status = VolumeStatus::Failed;
          Diagnostic d = m.error();
          d.subjects.insert(d.subjects.begin(), self);
          g->diagnostics.add(std::move(d));
        } else {
          mesh::assign_volume(*m, v.id);
          g->outputs = measure_mesh(v, *m, pool_);
          g->mesh = std::move(m).value();
          g->status = VolumeStatus::Current;
        }
      } else {
        model::ContentsBuild cb =
            model::build_contents(wave_doc, layout, i, settings_, &project.blobs);
        g->status = cb.status == VolumeStatus::Current ? VolumeStatus::Current
                                                       : VolumeStatus::Failed;
        g->solid = std::move(cb.contents);
        g->rebuild = std::move(cb.contents_rebuild);
        g->outputs = std::move(cb.outputs);
        g->diagnostics = std::move(cb.diagnostics);
      }
      g->output_hash =
          output_hash(g->status, g->solid, g->mesh ? &*g->mesh : nullptr, g->outputs);
      return g;
    };

    std::vector<Generated> generated(to_run.size());
    pool_.run(to_run.size(), [&](std::size_t k) {
      if (options.cancel.cancelled()) return;
      const std::size_t i = to_run[k];
      notify(ProgressEvent::Kind::VolumeStarted, ev.volumes[i].volume, VolumeStatus::Regenerating);
      generated[k].contents = generate(i);
      if (options_.check_determinism && !generated[k].contents->cancelled &&
          !options.cancel.cancelled()) {
        // A run cut short by a cancellation compares nothing (plan H1).
        const auto second = generate(i);
        if (!second->cancelled) generated[k].second_hash = second->output_hash;
      }
      generated[k].ran = true;
    });

    // 4. containment (on this thread), cache, publish
    for (std::size_t k = 0; k < to_run.size(); ++k) {
      const std::size_t i = to_run[k];
      VolumeResult& r = ev.volumes[i];
      Generated& g = generated[k];
      if (!g.ran || g.contents->cancelled) {
        ev.cancelled = true;
        r.status = VolumeStatus::Stale;
        set_state(r.volume, r.status);
        continue;
      }
      GeneratedContents& c = *g.contents;
      const model::Volume& v = ev.document.volumes[i];
      const SubjectRef self = ev.document.volume_subject(v.id);
      if (g.second_hash && *g.second_hash != c.output_hash) {
        // Contents that differ run to run are never current: they mustn't
        // be shown, replace the last valid contents, unblock dependants or
        // reach the disk cache (plan H1).
        c.status = VolumeStatus::Failed;
        c.diagnostics.add(Diagnostic::error(
            "eval.nondeterministic",
            fmt::format("'{}': generating it twice from the same inputs gave different contents",
                        self.label),
            {self}));
        c.output_hash = output_hash(c.status, c.solid, c.mesh ? &*c.mesh : nullptr, c.outputs,
                                    c.extra.get());
      }
      if (c.ok()) {
        const kernel::Solid& boundary = layout.volumes[i].boundary;
        std::optional<validate::ContainmentResult> contained;
        if (!c.solid.is_empty()) {
          contained = validate::check_containment(boundary, c.solid, self, settings_);
        } else if (c.mesh) {
          const std::vector<float> slack =
              curved_seam_slack(ev.document, layout, v.id, *c.mesh, settings_);
          contained = validate::check_containment(
              boundary, validate::MeshView{c.mesh->positions, c.mesh->indices, slack}, self,
              settings_,
              [this](std::size_t count, const std::function<void(std::size_t)>& task) {
                pool_.run(count, task);
              });
        }
        if (contained && !contained->diagnostics.empty()) {
          c.diagnostics.append(contained->diagnostics);
          if (contained->diagnostics.has_errors()) c.status = VolumeStatus::Failed;
        }
        c.output_hash = output_hash(c.status, c.solid, c.mesh ? &*c.mesh : nullptr, c.outputs,
                                    c.extra.get());
      }
      ev.stats.regenerated += options_.check_determinism ? 2 : 1;
      r.regenerated = true;
      ContentsPtr shared = std::move(g.contents);
      cache_.insert(shared);
      accept(r, shared);
      set_state(r.volume, r.status);
      notify(ProgressEvent::Kind::VolumeFinished, r.volume, r.status);
    }

    for (const std::size_t i : ready) done[i] = true;

    // Publish: each link whose source just became current carries its value.
    for (const auto& l : ev.document.links) {
      const model::Volume* src = ev.document.find_volume(l.source.volume);
      if (src == nullptr) continue;
      const std::size_t s = static_cast<std::size_t>(src - ev.document.volumes.data());
      if (std::find(ready.begin(), ready.end(), s) == ready.end()) continue;
      const VolumeResult& r = ev.volumes[s];
      if (r.status != VolumeStatus::Current || !r.contents) continue;
      for (const auto& o : r.contents->outputs) {
        if (o.output != l.source.output || !o.value) continue;
        const auto* value = std::get_if<double>(&*o.value);
        if (value == nullptr) continue;
        const auto it = link_values.find(l.target);
        if (it == link_values.end() || it->second != *value) {
          link_values[l.target] = *value;
          values_changed = true;
        }
      }
    }
    if (ev.cancelled) break;
  }

  // The result as a build: the last layout, with what each volume shows.
  ev.build = std::move(layout);
  for (std::size_t i = 0; i < n && i < ev.build.volumes.size(); ++i) {
    const VolumeResult& r = ev.volumes[i];
    model::VolumeBuild& vb = ev.build.volumes[i];
    vb.status = r.status;
    if (r.contents) {
      vb.contents = r.contents->solid;
      vb.outputs = r.contents->outputs;
      vb.contents_rebuild = r.contents->rebuild;
    }
    vb.diagnostics.append(r.diagnostics);
  }
  ev.diagnostics.append(layout_diagnostics);
  for (const auto& r : ev.volumes) ev.diagnostics.append(r.diagnostics);
  if (ev.cancelled) {
    ev.diagnostics.add(Diagnostic::info("eval.cancelled",
                                        "The evaluation was cancelled; unfinished volumes are "
                                        "stale"));
  }

  // 5. weld
  if (options.weld && options.generate && !ev.cancelled) {
    // Meshes a generator made (procedural contents) go in as they are.
    mesh::GeneratedMeshes generated;
    for (std::size_t i = 0; i < n; ++i) {
      const VolumeResult& r = ev.volumes[i];
      if (r.contents && r.contents->mesh &&
          !std::holds_alternative<model::StaticContents>(ev.document.volumes[i].contents)) {
        generated.emplace(r.volume, &*r.contents->mesh);
      }
    }
    ev.combined = mesh::combine(ev.document, ev.build, &project.blobs, settings_, options.combine,
                                generated);
    ev.diagnostics.append(ev.combined->diagnostics);
  }

  for (const auto& r : ev.volumes) set_state(r.volume, r.status);
  notify(ProgressEvent::Kind::Finished, {}, VolumeStatus::Stale);
  return ev;
}

std::vector<Hash256> output_hashes_by_threads(const model::Project& project,
                                              const ProductSettings& settings,
                                              const std::vector<std::size_t>& thread_counts,
                                              bool weld) {
  std::vector<Hash256> out;
  for (const std::size_t threads : thread_counts) {
    EngineOptions eo;
    eo.threads = threads;
    Engine engine(settings, eo);
    EvaluateOptions options;
    options.weld = weld;
    out.push_back(engine.evaluate(project, options).output_hash());
  }
  return out;
}

}  // namespace vs::eval
