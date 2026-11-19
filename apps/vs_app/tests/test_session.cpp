// The session: the engine on a background thread, results on the UI thread.

#include <QCoreApplication>
#include <QThread>

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "samples.hpp"
#include "session.hpp"

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
