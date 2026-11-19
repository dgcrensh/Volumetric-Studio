#include "session.hpp"

#include <QMetaObject>
#include <QPointer>

#include <chrono>

#include "vs/proc/generator.hpp"

namespace vs::app {

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
  proc::install(engine_);
  worker_ = std::thread([this] { worker_loop(); });
}

Session::~Session() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    current_cancel_.cancel();
    jobs_.clear();
  }
  wake_.notify_all();
  worker_.join();
}

core::Diagnostics Session::open(const std::filesystem::path& path) {
  core::Diagnostics d;
  auto loaded = model::load_project(path, /*check_document=*/false);
  if (!loaded) {
    d.add(loaded.error());
    return d;
  }
  set_project(std::move(loaded).value(), path);
  return d;
}

void Session::set_project(model::Project project, std::filesystem::path path) {
  cancel();
  project_ = std::make_shared<const model::Project>(std::move(project));
  has_project_ = true;
  path_ = std::move(path);
  latest_.reset();
  // A new project shares nothing with the last one worth keeping on screen.
  post([this] { engine_.reset(); });
  emit projectChanged();
  regenerate();
}

core::Diagnostics Session::save(const std::filesystem::path& path) {
  const auto layout =
      std::filesystem::is_directory(path) ? model::ProjectLayout::Folder : model::ProjectLayout::Zip;
  core::Diagnostics d = model::save_project(*project_, path, layout);
  if (!d.has_errors()) path_ = path;
  return d;
}

void Session::regenerate() {
  if (!has_project_) return;
  const std::uint64_t generation = ++requested_;
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
                           eval::CancellationToken cancel) {
  {
    std::lock_guard lock(mutex_);
    if (generation != newest_ || stopping_) return;  // superseded before it started
    running_ = generation;
  }
  auto deliver = [this](std::shared_ptr<Snapshot> snapshot) {
    QMetaObject::invokeMethod(
        this,
        [this, snapshot] {
          // Only the newest request's results reach the screen.
          if (snapshot->generation != requested_) return;
          latest_ = snapshot;
          emit evaluated(snapshot);
        },
        Qt::QueuedConnection);
  };
  auto finish_snapshot = [&](std::shared_ptr<Snapshot> s, std::chrono::steady_clock::time_point t0) {
    s->evaluate_ms = ms_since(t0);
    const auto t1 = std::chrono::steady_clock::now();
    s->scene = build_scene(s->evaluation, settings_);
    if (preparer_) s->prepared = preparer_(s->scene);
    s->scene_ms = ms_since(t1);
  };

  // 1. the preview tier
  {
    const auto t0 = std::chrono::steady_clock::now();
    auto s = std::make_shared<Snapshot>();
    s->generation = generation;
    s->preview = true;
    s->project = project;
    s->evaluation = engine_.preview(*project);
    finish_snapshot(s, t0);
    deliver(s);
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
