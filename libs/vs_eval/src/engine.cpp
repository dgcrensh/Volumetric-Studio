#include "vs/eval/engine.hpp"

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include "vs/eval/measure.hpp"
#include "vs/core/version.hpp"
#include "vs/mesh/io.hpp"
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

// The document with each link's value written into its target: a layout
// variable takes it as its expression, a parameter as its value.
model::Document apply_links(const model::Document& document, const std::map<Id, double>& values) {
  model::Document out = document;
  for (const auto& [target, value] : values) {
    if (out.layout.variables().find(target) != nullptr) {
      // The shortest text that reads back as the same double.
      (void)out.layout.variables().set_expression(target, fmt::format("{}", value));
      continue;
    }
    for (auto& v : out.volumes) {
      for (auto& p : v.parameters) {
        if (p.id == target) p.value = feature::Param::of(value);
      }
    }
  }
  return out;
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
      cache_(options_.disk_cache) {}

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

Evaluation Engine::preview(const model::Project& project) {
  EvaluateOptions options;
  options.generate = false;
  return evaluate(project, options);
}

Evaluation Engine::evaluate(const model::Project& project, const EvaluateOptions& options) {
  const std::lock_guard eval_lock(evaluate_mutex_);
  const model::Document& doc = project.document;
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

  // 1. check
  ev.diagnostics.append(model::check(doc));
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
  model::DocumentBuild layout;
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
      r.key = cache_key(ev.document, layout, i);
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
        } else if (auto m = mesh::read_mesh(it->second, sc->format); !m) {
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
      if (options_.check_determinism) generated[k].second_hash = generate(i)->output_hash;
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
        c.diagnostics.add(Diagnostic::error(
            "eval.nondeterministic",
            fmt::format("'{}': generating it twice from the same inputs gave different contents",
                        self.label),
            {self}));
      }
      if (c.ok()) {
        const kernel::Solid& boundary = layout.volumes[i].boundary;
        std::optional<validate::ContainmentResult> contained;
        if (!c.solid.is_empty()) {
          contained = validate::check_containment(boundary, c.solid, self, settings_);
        } else if (c.mesh) {
          contained = validate::check_containment(
              boundary, validate::MeshView{c.mesh->positions, c.mesh->indices}, self, settings_,
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
