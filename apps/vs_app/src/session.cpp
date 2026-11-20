#include "session.hpp"

#include <QMetaObject>
#include <QPointer>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <utility>

#include "vs/core/hash.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/inputs.hpp"
#include "vs/proc/inspect.hpp"

namespace vs::app {

namespace detail {

// What a volume's graph inputs depend on: everything in the evaluated
// document but contents (the layout, boundaries, keep-outs, surfaces,
// parameters with link values applied, seeds), and the volume.
core::Hash256 inputs_key(const model::Document& document, const Id& volume) {
  model::Document copy = document;
  for (auto& v : copy.volumes) v.contents = model::StaticContents{};
  return core::hash_of(std::string_view(model::to_json(copy)), std::string_view(volume.to_string()));
}

struct PreviewState {
  std::mutex mutex;
  std::condition_variable wake;
  std::optional<std::pair<std::uint64_t, NodePreviewRequest>> pending;
  bool stopping = false;
  // Its own pool: the engine's is busy generating while previews run.
  eval::ThreadPool pool{std::max<std::size_t>(2, std::thread::hardware_concurrency() / 2)};
  std::atomic<std::size_t> evaluated{0};

  // Gathered inputs by inputs_key() (the preview thread only).
  std::vector<std::pair<core::Hash256, std::shared_ptr<const proc::GraphInputs>>> inputs;
  std::shared_ptr<const proc::GraphInputs> find_inputs(const core::Hash256& key) const {
    for (const auto& [k, v] : inputs) {
      if (k == key) return v;
    }
    return nullptr;
  }
  void keep_inputs(const core::Hash256& key, std::shared_ptr<const proc::GraphInputs> value) {
    if (inputs.size() >= 8) inputs.erase(inputs.begin());
    inputs.emplace_back(key, std::move(value));
  }
};

}  // namespace detail

namespace {

// The engine's pool leaves two hardware threads to the UI thread and the
// GPU driver: with every core busy generating, frames stall for 100 ms and
// more (measured on Dallas's desktop, M13).
eval::EngineOptions with_ui_headroom(eval::EngineOptions options) {
  if (options.threads == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    options.threads = hw > 3 ? hw - 2 : 1;
  }
  return options;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

Session::Session(eval::EngineOptions options, QObject* parent)
    : QObject(parent),
      engine_(settings_, with_ui_headroom(std::move(options))),
      project_(std::make_shared<model::Project>()) {
  generator_ = proc::install(engine_);
  preview_ = std::make_unique<detail::PreviewState>();
  worker_ = std::thread([this] { worker_loop(); });
  preview_worker_ = std::thread([this] { preview_loop(); });
}

Session::~Session() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    current_cancel_.cancel();
    jobs_.clear();
  }
  wake_.notify_all();
  {
    std::lock_guard lock(preview_->mutex);
    preview_->stopping = true;
    preview_->pending.reset();
  }
  preview_->wake.notify_all();
  worker_.join();
  preview_worker_.join();
}

// --- node previews (M15) -------------------------------------------------------------------

std::uint64_t Session::preview_node(NodePreviewRequest request) {
  const std::uint64_t id = ++preview_requested_;
  {
    std::lock_guard lock(preview_->mutex);
    preview_->pending = std::make_pair(id, std::move(request));
  }
  preview_->wake.notify_one();
  return id;
}

void Session::cancel_preview() {
  ++preview_requested_;
  std::lock_guard lock(preview_->mutex);
  preview_->pending.reset();
}

std::size_t Session::nodes_evaluated() const {
  return generator_->nodes_evaluated() + preview_->evaluated.load();
}

void Session::preview_loop() {
  for (;;) {
    std::pair<std::uint64_t, NodePreviewRequest> job;
    {
      std::unique_lock lock(preview_->mutex);
      preview_->wake.wait(lock, [this] { return preview_->stopping || preview_->pending.has_value(); });
      if (preview_->stopping) return;
      job = std::move(*preview_->pending);
      preview_->pending.reset();
    }
    preview_job(job.first, job.second);
  }
}

void Session::preview_job(std::uint64_t id, const NodePreviewRequest& request) {
  auto result = std::make_shared<NodePreviewResult>();
  result->volume = request.volume;
  result->node = request.node;
  const auto t0 = std::chrono::steady_clock::now();
  const Snapshot* s = request.snapshot.get();
  std::shared_ptr<const proc::GraphInputs> inputs;
  if (s != nullptr) {
    const auto& doc = s->evaluation.document;
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
      if (doc.volumes[i].id == request.volume) index = i;
    }
    if (index && *index < s->evaluation.build.volumes.size()) {
      const core::Hash256 key = detail::inputs_key(doc, request.volume);
      inputs = preview_->find_inputs(key);
      if (!inputs) {
        auto gathered = proc::gather_inputs(doc, s->evaluation.build, *index, settings_);
        if (gathered) {
          inputs = std::make_shared<const proc::GraphInputs>(std::move(gathered).value());
          preview_->keep_inputs(key, inputs);
        } else {
          result->diagnostics.add(gathered.error());
        }
      }
    }
  }
  if (!inputs) {
    if (result->diagnostics.empty()) {
      result->diagnostics.add(core::Diagnostic::error("app.preview.no_inputs",
                                                      "The volume's inputs aren't built yet, so there's nothing to "
                                                      "preview against"));
    }
    result->text = result->diagnostics.items().front().message;
  } else {
    proc::EvaluateGraphOptions options;
    options.cache = &generator_->cache();
    options.pool = &preview_->pool;
    const proc::NodeInspection inspection = proc::inspect_node(request.graph, *inputs, request.node, options);
    preview_->evaluated += inspection.evaluated;
    result->diagnostics = inspection.diagnostics;
    result->from_cache = inspection.from_cache;
    result->evaluated = inspection.evaluated;
    result->inspect_ms = ms_since(t0);
    const auto t1 = std::chrono::steady_clock::now();
    // The output to draw: the one asked for, else the first drawable one.
    const std::pair<std::string, proc::Value>* chosen = nullptr;
    for (const auto& out : inspection.outputs) {
      if (!request.port.empty() && out.first == request.port) chosen = &out;
    }
    if (chosen == nullptr) {
      for (const auto& out : inspection.outputs) {
        const auto t = proc::type_of(out.second);
        if (chosen == nullptr && t != proc::PortType::Scalar && t != proc::PortType::Vector &&
            t != proc::PortType::List) {
          chosen = &out;
        }
      }
      if (chosen == nullptr && !inspection.outputs.empty()) chosen = &inspection.outputs.front();
    }
    if (chosen != nullptr) {
      result->port = chosen->first;
      result->type = proc::type_of(chosen->second);
      const proc::Preview pv = proc::preview(chosen->second, *inputs, request.cell, &preview_->pool);
      result->text = pv.text;
      result->scene = preview_scene(pv, request.volume, request.node, core::to_fp32(inputs->bounds), request.options);
      // Every output's value, for the editor's status line.
      for (const auto& out : inspection.outputs) {
        if (&out == chosen) continue;
        result->text += fmt::format("\n{}: {}", out.first, proc::describe(out.second));
      }
    } else {
      for (const auto& d : inspection.diagnostics) {
        if (d.is_error()) {
          result->text = d.message;
          break;
        }
      }
    }
    result->preview_ms = ms_since(t1);
  }
  std::shared_ptr<const PreparedScene> prepared;
  if (preparer_) prepared = preparer_(result->scene);
  QMetaObject::invokeMethod(
      this,
      [this, id, result, prepared] {
        if (id != preview_requested_) return;  // a newer request is on its way
        emit nodePreviewed(id, result, prepared);
      },
      Qt::QueuedConnection);
}

core::Diagnostics Session::open(const std::filesystem::path& path) {
  core::Diagnostics d;
  auto loaded = model::load_project(path, /*check_document=*/false);
  if (!loaded) {
    d.add(loaded.error());
    return d;
  }
  // Caches saved with the project (M17) seed the engine, so contents whose
  // keys still match aren't regenerated; stale ones are never looked up.
  set_project(std::move(loaded).value(), path, model::load_project_cache(path));
  return d;
}

void Session::set_project(model::Project project, std::filesystem::path path, model::CacheFiles cache) {
  cancel();
  project_ = std::make_shared<const model::Project>(std::move(project));
  has_project_ = true;
  path_ = std::move(path);
  latest_.reset();
  // A new project shares nothing with the last one worth keeping on screen.
  post([this, cache = std::make_shared<const model::CacheFiles>(std::move(cache))] {
    engine_.reset();
    cache_entries_loaded_ = engine_.cache().insert_files(*cache);
  });
  emit projectChanged();
  regenerate();
}

void Session::update_project(std::shared_ptr<const model::Project> project) {
  project_ = std::move(project);
  has_project_ = true;
  emit projectEdited();
  regenerate();
}

core::Diagnostics Session::save(const std::filesystem::path& path, const model::Project* project,
                                const model::CacheFiles* cache) {
  const auto layout =
      std::filesystem::is_directory(path) ? model::ProjectLayout::Folder : model::ProjectLayout::Zip;
  core::Diagnostics d =
      model::save_project(project != nullptr ? *project : *project_, path, layout, cache);
  if (!d.has_errors()) path_ = path;
  return d;
}

void Session::regenerate() {
  if (!has_project_) return;
  const std::uint64_t generation = ++requested_;
  full_requested_ = generation;
  eval::CancellationToken cancel;
  {
    std::lock_guard lock(mutex_);
    current_cancel_.cancel();  // the running evaluation, if any
    current_cancel_ = cancel;
    newest_ = generation;
  }
  emit evaluationStarted(generation);
  post([this, generation, project = project_, cancel] { evaluate_job(generation, project, cancel); });
}

void Session::drag_preview(std::shared_ptr<const model::Project> project) {
  if (!has_project_ || !project) return;
  const std::uint64_t generation = ++requested_;
  eval::CancellationToken cancel;
  {
    std::lock_guard lock(mutex_);
    current_cancel_.cancel();
    current_cancel_ = cancel;
    newest_ = generation;
  }
  post([this, generation, project = std::move(project), cancel] { evaluate_job(generation, project, cancel, true); });
}

void Session::cancel() {
  std::lock_guard lock(mutex_);
  current_cancel_.cancel();
}

bool Session::busy() const {
  std::lock_guard lock(mutex_);
  return running_ != 0 || !jobs_.empty();
}

void Session::run_in_background(std::function<void()> work, std::function<void()> done) {
  post([this, work = std::move(work), done = std::move(done)] {
    work();
    QMetaObject::invokeMethod(this, [done] { done(); }, Qt::QueuedConnection);
  });
}

void Session::post(std::function<void()> job) {
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  wake_.notify_one();
}

void Session::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

void Session::evaluate_job(std::uint64_t generation, std::shared_ptr<const model::Project> project,
                           eval::CancellationToken cancel, bool drag) {
  {
    std::lock_guard lock(mutex_);
    if (generation != newest_ || stopping_) return;  // superseded before it started
    running_ = generation;
  }
  auto deliver = [this](std::shared_ptr<Snapshot> snapshot) {
    QMetaObject::invokeMethod(
        this,
        [this, snapshot] {
          // Only the newest request's results reach the screen. A drag
          // asks for a preview every frame, faster than they come: each
          // one still shows unless a regeneration (or a later drag
          // preview) was requested and shown since.
          if (snapshot->drag) {
            if (snapshot->generation < full_requested_) return;
            if (latest_ && latest_->generation > snapshot->generation) return;
          } else if (snapshot->generation != requested_) {
            return;
          }
          latest_ = snapshot;
          emit evaluated(snapshot);
        },
        Qt::QueuedConnection);
  };
  auto finish_snapshot = [&](std::shared_ptr<Snapshot> s, std::chrono::steady_clock::time_point t0) {
    s->evaluate_ms = ms_since(t0);
    const auto t1 = std::chrono::steady_clock::now();
    SceneOptions scene_options;
    scene_options.hide_stale_contents = s->drag;
    s->scene = build_scene(s->evaluation, settings_, scene_options);
    if (preparer_) s->prepared = preparer_(s->scene);
    s->scene_ms = ms_since(t1);
  };

  // 1. the preview tier
  {
    const auto t0 = std::chrono::steady_clock::now();
    auto s = std::make_shared<Snapshot>();
    s->generation = generation;
    s->preview = true;
    s->drag = drag;
    s->project = project;
    s->evaluation = engine_.preview(*project);
    finish_snapshot(s, t0);
    deliver(s);
  }
  if (drag) {
    // A drag shows the preview tier only; releasing the handle regenerates.
    std::lock_guard lock(mutex_);
    running_ = 0;
    return;
  }
  // 2. the full evaluation. Cancelled already, it returns at once with
  // every volume as it was (Stale), which is still worth showing when no
  // newer request replaced it.
  {
    const auto t0 = std::chrono::steady_clock::now();
    auto s = std::make_shared<Snapshot>();
    s->generation = generation;
    s->project = project;
    eval::EvaluateOptions options;
    options.cancel = cancel;
    options.progress = [this](const eval::ProgressEvent& e) {
      if (e.kind != eval::ProgressEvent::Kind::VolumeFinished &&
          e.kind != eval::ProgressEvent::Kind::VolumeStarted) {
        return;
      }
      const int done = static_cast<int>(e.done), total = static_cast<int>(e.total);
      QMetaObject::invokeMethod(this, [this, done, total] { emit progressed(done, total); },
                                Qt::QueuedConnection);
    };
    s->evaluation = engine_.evaluate(*project, options);
    // A cancelled evaluation still describes a consistent model (vs_eval),
    // but a newer request is coming; show it only if nothing replaced it.
    finish_snapshot(s, t0);
    deliver(s);
  }
  std::lock_guard lock(mutex_);
  running_ = 0;
}

}  // namespace vs::app
