#pragma once

// Cancellation and progress for long evaluations (plan, M9).
//
// A CancellationToken is a shared flag: copies share it, so the thread that
// started a job can cancel it while the job runs elsewhere. The engine
// checks it before each volume and between scheduling waves; work already
// under way finishes (the kernel can't be interrupted mid-operation), and
// nothing after it starts. Cancelling never leaves half a result behind:
// see engine.hpp for what a cancelled evaluation keeps.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

#include "vs/core/id.hpp"
#include "vs/model/build.hpp"

namespace vs::eval {

class CancellationToken {
 public:
  CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
  void cancel() const { flag_->store(true); }
  [[nodiscard]] bool cancelled() const { return flag_->load(); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

// One step of an evaluation, for progress bars and the volume tree.
struct ProgressEvent {
  enum class Kind {
    LayoutBuilt,     // the layout (and boundaries) of a wave is built
    VolumeStarted,   // `volume` is regenerating
    VolumeFinished,  // `volume` has its new status
    Finished,        // the evaluation is done (or cancelled)
    PhaseFinished,   // a step of the evaluation ended (`phase`), for profiling
  };
  Kind kind = Kind::LayoutBuilt;
  core::Id volume;                                      // VolumeStarted, VolumeFinished
  model::VolumeStatus status = model::VolumeStatus::Stale;  // VolumeFinished
  std::size_t done = 0;   // volumes regenerated so far in this evaluation
  std::size_t total = 0;  // volumes scheduled for regeneration so far
  // PhaseFinished: which step ended, a static name (null otherwise). Phases
  // run one after another on the calling thread, and some repeat (once per
  // wave), so the time between two such events is that phase's, for
  // vstudio-cli bench-rack. The names are:
  //   check                       model::check() of the authored document
  //   children: layout            L4: the layout for the generators (built or reused)
  //   children: keys              L4: a generating composite's cache key
  //   children: generate          L4: its generator run (or a cached outcome)
  //   children: expand and check  L4: model::expand(), then check() again
  //   dependencies                the dependency graph and who waits for whom
  //   layout and checks           model::build_layout() and
  //                               validate::validate_layout(), or the kept
  //                               layout reused (once per link change)
  //   keys and cache              each ready volume's key, cache lookups and
  //                               composites finished (once per wave)
  //   generate                    the wave's jobs prepared and run
  //   containment                 containment, caching, publishing
  //   result                      the evaluation's build and diagnostics
  //   weld                        mesh::combine()
  const char* phase = nullptr;
};

// Called on whichever thread made progress, one call at a time.
using ProgressCallback = std::function<void(const ProgressEvent&)>;

}  // namespace vs::eval
