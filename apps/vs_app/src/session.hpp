#pragma once

// The open project and the engine that evaluates it, off the UI thread.
//
// A Session owns the project, a vs_eval Engine (with vs_proc's generator
// installed) and one worker thread. regenerate() queues an evaluation of
// the project as it stands and cancels the one running, if any; the
// worker runs only the newest request. Each evaluation is delivered twice
// on the UI thread, as a Snapshot through evaluated():
//
//   1. the preview tier (Engine::preview()): boundaries, connection
//      surfaces and cached contents, at once;
//   2. the full evaluation, when it finishes.
//
// A Snapshot is immutable and carries everything the panels and the
// viewport need: the evaluation, the scene (scene.hpp, tessellated on the
// worker) and, when a preparer is installed, the viewport's GPU-ready
// arrays (also made on the worker), so the UI thread only swaps them in.
//
// While an evaluation runs, states() reports each volume's live status
// (Regenerating included), and progressed() reports volumes finished.
// Other long work (export, validation) goes through run_in_background(),
// on the same worker, in order.

#include <QObject>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "node_preview.hpp"
#include "scene.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/proc/graph.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/project.hpp"

namespace vs::proc {
class ProceduralGenerator;
}

namespace vs::app {

// Viewport data made on the worker thread from a scene (viewport.cpp).
struct PreparedScene;

struct Snapshot;

namespace detail {
struct PreviewState;
}

// A node to preview (M15): `node` of `graph` (the volume's graph as it
// stands, which may be newer than the snapshot's: graph edits don't change
// the layout) in `volume`, against `snapshot`'s layout build.
struct NodePreviewRequest {
  std::shared_ptr<const Snapshot> snapshot;
  Id volume;
  proc::Graph graph;
  Id node;
  std::string port;  // empty: the first output there is something to draw for
  NodePreviewOptions options;
  double cell = 0.0;  // the preview grid (0: proc::kPreviewCells along the box)
};

struct Snapshot {
  std::uint64_t generation = 0;  // which regenerate() request it answers
  bool preview = false;          // the preview tier (see above)
  // A dimension drag's preview (M16, drag_preview()): the preview tier
  // only, with volumes being changed shown as their boundary.
  bool drag = false;
  std::shared_ptr<const model::Project> project;  // the project as evaluated
  eval::Evaluation evaluation;
  Scene scene;
  std::shared_ptr<const PreparedScene> prepared;  // null without a preparer
  double evaluate_ms = 0.0;
  double scene_ms = 0.0;
};
using SnapshotPtr = std::shared_ptr<const Snapshot>;

class Session : public QObject {
  Q_OBJECT

 public:
  explicit Session(eval::EngineOptions options = {}, QObject* parent = nullptr);
  ~Session() override;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Reads a folder or zip project without enforcing the authoring rules,
  // so a broken file still opens and shows what is wrong with it, then
  // regenerates. The caches saved with it (M17) seed the engine first, so
  // contents whose keys still match aren't regenerated. On failure the
  // current project stays.
  core::Diagnostics open(const std::filesystem::path& path);
  // Replaces the project (no file, or `path`) and regenerates, with
  // `cache` (eval::project_cache_files()) in the engine's cache. The last
  // project's caches (contents, nodes, preview inputs) are dropped.
  void set_project(model::Project project, std::filesystem::path path = {},
                   model::CacheFiles cache = {});
  // An edit (or a dialog's live preview) of the same project: replaces it
  // and regenerates, keeping the engine's cache and each volume's last
  // valid contents, the file path and the view.
  void update_project(std::shared_ptr<const model::Project> project);
  // Saves to `path`: a folder when `path` is an existing folder, else a
  // zip file. With `project`, saves that instead of the one shown (which
  // may be a dialog's preview). With `cache`, those files become the
  // project's caches (an empty set removes them; null leaves a folder's).
  core::Diagnostics save(const std::filesystem::path& path, const model::Project* project = nullptr,
                         const model::CacheFiles* cache = nullptr);
  // How many cache entries the last open() loaded into the engine (read on
  // the UI thread after the first snapshot arrives).
  [[nodiscard]] std::size_t cache_entries_loaded() const { return cache_entries_loaded_; }

  void regenerate();
  // A dimension drag (M16): shows `project` (the drag's edit applied, not
  // kept) as the preview tier only: no generation, contents of volumes the
  // edit changes left out (boundary only), untouched volumes' contents from
  // the cache. Cancels whatever is running; the requests wait in one slot,
  // so a fast drag skips the positions the worker couldn't keep up with
  // (H7). The project shown stays as it was; regenerate() or
  // update_project() ends the drag.
  //
  // Whatever a newer request replaced is dropped: its progress isn't
  // reported (progressed() is for the newest request only), and the worker
  // stops before building its scene.
  void drag_preview(std::shared_ptr<const model::Project> project);
  void cancel();
  [[nodiscard]] bool busy() const;

  [[nodiscard]] const model::Project& project() const { return *project_; }
  [[nodiscard]] std::shared_ptr<const model::Project> project_ptr() const { return project_; }
  [[nodiscard]] bool has_project() const { return has_project_; }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] SnapshotPtr latest() const { return latest_; }
  [[nodiscard]] std::map<core::Id, model::VolumeStatus> states() const { return engine_.states(); }
  [[nodiscard]] const core::ProductSettings& settings() const { return settings_; }
  [[nodiscard]] eval::Engine& engine() { return engine_; }

  using Preparer = std::function<std::shared_ptr<const PreparedScene>(const Scene&)>;
  // Called on the worker threads for every snapshot's scene and node
  // preview. Set it before the first regenerate() (a later one is safe,
  // and applies from the next snapshot).
  void set_preparer(Preparer preparer);

  // Runs `work` on the worker thread after what is queued, then `done` on
  // the UI thread (unless the session is gone by then).
  void run_in_background(std::function<void()> work, std::function<void()> done);

  // Node previews (M15) run on a thread of their own, so a preview never
  // waits behind a long evaluation. The node is inspected
  // (proc::inspect_node()) with the procedural generator's node cache, so
  // a node the volume just generated through costs nothing to inspect,
  // and the volume's inputs (its boundary and patches, tessellated) are
  // kept per layout, so only the first preview after a layout change pays
  // for them. Only the newest request is answered, through nodePreviewed()
  // on the UI thread; the result's scene is already prepared for the
  // viewport. A new request stops the one running (H7). Returns the
  // request's number.
  std::uint64_t preview_node(NodePreviewRequest request);
  // Drops any pending preview and stops the one running, within a
  // fraction of a second; neither answer is delivered.
  void cancel_preview();
  // Node evaluations the procedural generator and the previews ran.
  [[nodiscard]] std::size_t nodes_evaluated() const;
  // Entries in the procedural generator's node cache (tests).
  [[nodiscard]] std::size_t node_cache_entries() const;
  // Jobs waiting for the worker, not counting the one running (tests).
  [[nodiscard]] std::size_t queued_jobs() const;

 signals:
  void projectChanged();
  // update_project() replaced the project.
  void projectEdited();
  void evaluationStarted(quint64 generation);
  void progressed(int done, int total);
  void evaluated(vs::app::SnapshotPtr snapshot);
  // The answer to the newest preview_node() request.
  void nodePreviewed(quint64 request, std::shared_ptr<const vs::app::NodePreviewResult> result,
                     std::shared_ptr<const vs::app::PreparedScene> prepared);

 private:
  void post(std::function<void()> job);
  void worker_loop();
  void evaluate_job(std::uint64_t generation, std::shared_ptr<const model::Project> project,
                    eval::CancellationToken cancel, bool drag = false);
  // Whether a newer request (or the session closing) replaced `generation`.
  [[nodiscard]] bool superseded(std::uint64_t generation) const;
  [[nodiscard]] Preparer preparer() const;

  core::ProductSettings settings_ = core::ProductSettings::defaults();
  eval::Engine engine_;
  std::shared_ptr<const model::Project> project_;
  bool has_project_ = false;
  std::filesystem::path path_;
  SnapshotPtr latest_;
  mutable std::mutex preparer_mutex_;
  Preparer preparer_;  // under preparer_mutex_
  std::atomic<std::size_t> cache_entries_loaded_{0};

  // UI thread only.
  std::uint64_t requested_ = 0;
  std::uint64_t full_requested_ = 0;  // the newest regenerate() (not a drag)
  // Shared with the worker.
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::uint64_t newest_ = 0;    // the newest generation requested
  std::uint64_t running_ = 0;   // the generation being evaluated (0: none)
  eval::CancellationToken current_cancel_;
  // The newest drag preview waiting for the worker (drag_preview()): one
  // slot, so a fast drag never queues a job per mouse move.
  std::shared_ptr<const model::Project> drag_project_;
  std::uint64_t drag_generation_ = 0;
  eval::CancellationToken drag_cancel_;
  bool drag_queued_ = false;
  std::thread worker_;

  // Node previews (preview_node()).
  void preview_loop();
  void preview_job(std::uint64_t id, const NodePreviewRequest& request, const eval::CancellationToken& cancel);
  std::shared_ptr<proc::ProceduralGenerator> generator_;
  std::unique_ptr<detail::PreviewState> preview_;
  std::uint64_t preview_requested_ = 0;  // UI thread
  std::thread preview_worker_;
};

}  // namespace vs::app
