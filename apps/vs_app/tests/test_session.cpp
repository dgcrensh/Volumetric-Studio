// The session: the engine on a background thread, results on the UI thread.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "samples.hpp"
#include "session.hpp"
#include "vs/proc/graph.hpp"
#include "wait.hpp"

using namespace vs;

TEST_CASE("Opening a project delivers a preview, then the evaluation, on the UI thread",
          "[app][session][acceptance]") {
  app::Session session;
  std::vector<app::SnapshotPtr> got;
  bool on_ui_thread = true;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) {
    on_ui_thread = on_ui_thread && QThread::currentThread() == QCoreApplication::instance()->thread();
    got.push_back(std::move(s));
  });
  const auto path = samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj";
  REQUIRE(session.open(path).empty());
  CHECK(session.path() == path);
  REQUIRE(samples::wait_until([&] { return got.size() == 2; }, 60000));
  CHECK(on_ui_thread);
  CHECK(got[0]->preview);
  CHECK_FALSE(got[1]->preview);
  CHECK(got[0]->generation == got[1]->generation);
  // The preview already has every boundary to draw.
  CHECK(got[0]->scene.count(app::ItemRole::Boundary) == got[1]->scene.count(app::ItemRole::Boundary));
  CHECK(got[1]->evaluation.ok());
  CHECK(got[1]->scene.count(app::ItemRole::Contents) > 0);
  CHECK(session.latest() == got[1]);
  REQUIRE(samples::wait_until([&] { return !session.busy(); }, 10000));
  for (const auto& [id, status] : session.states()) CHECK(status == model::VolumeStatus::Current);
}

TEST_CASE("A broken file still opens and reports why", "[app][session]") {
  app::Session session;
  app::SnapshotPtr last;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { last = std::move(s); });
  const auto path = samples::source_dir() / "libs" / "vs_validate" / "tests" / "data" / "loop.vsproj";
  REQUIRE(session.open(path).empty());
  REQUIRE(samples::wait_until([&] { return last && !last->preview; }, 60000));
  CHECK(last->evaluation.diagnostics.has_errors());

  const auto missing = session.open(samples::source_dir() / "no-such.vsproj");
  CHECK(missing.has_errors());
  CHECK(session.path() == path);  // the open project stays
}

TEST_CASE("Regenerating again supersedes the running evaluation", "[app][session]") {
  app::Session session;
  std::vector<app::SnapshotPtr> got;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { got.push_back(std::move(s)); });
  session.set_project(samples::long_procedural(0.5));
  session.regenerate();
  session.regenerate();
  REQUIRE(samples::wait_until([&] { return !got.empty() && !got.back()->preview && !session.busy(); }, 120000));
  // Only the newest request reaches the screen: generation 3 (set_project,
  // then two regenerates).
  for (const auto& s : got) CHECK(s->generation == 3);
  CHECK(got.back()->evaluation.ok());
}

TEST_CASE("Cancelling leaves a consistent model that the next evaluation finishes", "[app][session]") {
  app::Session session;
  app::SnapshotPtr last;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { last = std::move(s); });
  session.set_project(samples::load("bolt_bosses.vsproj"));
  session.cancel();
  REQUIRE(samples::wait_until([&] { return !session.busy() && last && !last->preview; }, 60000));
  CHECK(last->evaluation.cancelled);
  // Nothing generated, nothing broken: every volume is stale, none failed.
  for (const auto& r : last->evaluation.volumes) CHECK(r.status != model::VolumeStatus::Failed);
  session.regenerate();
  REQUIRE(samples::wait_until([&] { return !session.busy() && last && !last->preview && !last->evaluation.cancelled; },
                              60000));
  CHECK(last->evaluation.ok());
  for (const auto& r : last->evaluation.volumes) CHECK(r.status == model::VolumeStatus::Current);
}

TEST_CASE("Saving writes a project that loads back the same", "[app][session]") {
  app::Session session;
  const auto project = samples::load("bracket.vsproj");
  session.set_project(project);
  const auto path = std::filesystem::temp_directory_path() / "vs_app_session_save.vsproj";
  std::filesystem::remove_all(path);
  REQUIRE(session.save(path).empty());
  auto back = model::load_project(path);
  REQUIRE(back.ok());
  CHECK(model::content_hash(*back) == model::content_hash(project));
  std::filesystem::remove_all(path);
}

// --- H7: the session's races and wasted work ---------------------------------------------

namespace {

// Waits for the full evaluation of the newest request.
bool wait_full(app::Session& session, app::SnapshotPtr& last, int timeout_ms = 120000) {
  return samples::wait_until([&] { return !session.busy() && last && !last->preview && !last->drag; }, timeout_ms);
}

}  // namespace

TEST_CASE("A drag preview silences the regeneration it supersedes", "[app][session]") {
  app::Session session;
  bool started = false, dragging = false, drag_shown = false;
  int after_drag = 0;
  QObject::connect(&session, &app::Session::progressed, [&](int, int) {
    started = true;
    if (dragging) ++after_drag;
  });
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { drag_shown = drag_shown || s->drag; });
  session.set_project(samples::long_procedural(0.15));
  REQUIRE(samples::wait_until([&] { return started; }, 60000));
  // A drag cancels the long regeneration; nothing it reports afterwards
  // (the cancelled volume finishing, reports already queued) may reach
  // the progress bar, which the drag has taken down.
  dragging = true;
  session.drag_preview(session.project_ptr());
  REQUIRE(samples::wait_until([&] { return drag_shown && !session.busy(); }, 60000));
  samples::wait_until([] { return false; }, 200);
  CHECK(after_drag == 0);
}

TEST_CASE("Drag previews wait in one slot, newest first", "[app][session]") {
  app::Session session;
  bool started = false;
  std::vector<app::SnapshotPtr> drags;
  QObject::connect(&session, &app::Session::progressed, [&](int, int) { started = true; });
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) {
    if (s->drag) drags.push_back(std::move(s));
  });
  session.set_project(samples::long_procedural(0.15));
  REQUIRE(samples::wait_until([&] { return started; }, 60000));
  // A fast drag asks for a preview every mouse move, faster than the
  // worker can answer: they share one slot rather than queueing.
  std::vector<std::shared_ptr<const model::Project>> asked;
  for (int i = 0; i < 200; ++i) {
    auto p = std::make_shared<model::Project>(session.project());
    p->document.name = "drag " + std::to_string(i);
    asked.push_back(p);
    session.drag_preview(p);
  }
  CHECK(session.queued_jobs() <= 1);
  REQUIRE(samples::wait_until([&] { return !session.busy(); }, 60000));
  samples::wait_until([] { return false; }, 200);
  REQUIRE(!drags.empty());
  CHECK(drags.back()->project == asked.back());
  CHECK(drags.size() <= 2);
}

TEST_CASE("A new project starts with an empty node cache", "[app][session]") {
  app::Session session;
  app::SnapshotPtr last;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { last = std::move(s); });
  session.set_project(samples::long_procedural(0.5));
  REQUIRE(wait_full(session, last));
  CHECK(session.node_cache_entries() > 0);
  // The bracket has no procedural volume: nothing of the last project's
  // graphs is worth its memory any more.
  last.reset();
  session.set_project(samples::load("bracket.vsproj"));
  REQUIRE(wait_full(session, last));
  CHECK(session.node_cache_entries() == 0);
}

TEST_CASE("Cancelling a node preview stops its work", "[app][session]") {
  app::Session session;
  app::SnapshotPtr last;
  QObject::connect(&session, &app::Session::evaluated, [&](app::SnapshotPtr s) { last = std::move(s); });
  session.set_project(samples::long_procedural(0.5));
  REQUIRE(wait_full(session, last));
  const auto& volume = session.project().document.volumes.at(0);
  auto graph = proc::graph_from_json(std::get<model::ProceduralContents>(volume.contents).graph);
  REQUIRE(graph.ok());
  const proc::Node* field = nullptr;
  for (const auto& n : graph->nodes) {
    if (n.type == "field.union") field = &n;
  }
  REQUIRE(field != nullptr);
  std::vector<std::uint64_t> answered;
  QObject::connect(&session, &app::Session::nodePreviewed,
                   [&](quint64 id, std::shared_ptr<const app::NodePreviewResult>, auto) { answered.push_back(id); });
  app::NodePreviewRequest heavy;
  heavy.snapshot = last;
  heavy.volume = volume.id;
  heavy.graph = *graph;
  heavy.node = field->id;
  // A fine preview grid (near proc::kMaxGridSamples): seconds of
  // sampling and meshing.
  const auto size = last->scene.bounds.size();
  heavy.cell = static_cast<double>(std::max(size.x, std::max(size.y, size.z))) / 250.0;
  session.preview_node(heavy);
  samples::wait_until([] { return false; }, 300);
  QElapsedTimer clock;
  clock.start();
  session.cancel_preview();
  app::NodePreviewRequest light = heavy;
  light.cell = 0.0;  // the default coarse grid
  const auto id = session.preview_node(light);
  REQUIRE(samples::wait_until([&] { return !answered.empty(); }, 120000));
  const double ms = static_cast<double>(clock.elapsed());
  UNSCOPED_INFO("the coarse preview answered after " << ms << " ms");
  CHECK(answered.front() == id);
  // The fine one alone takes about 1.4 s here (decision 0036).
  CHECK(ms < 500.0);
}
