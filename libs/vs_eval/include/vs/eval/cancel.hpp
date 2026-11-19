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
  };
  Kind kind = Kind::LayoutBuilt;
  core::Id volume;                                      // VolumeStarted, VolumeFinished
  model::VolumeStatus status = model::VolumeStatus::Stale;  // VolumeFinished
  std::size_t done = 0;   // volumes regenerated so far in this evaluation
  std::size_t total = 0;  // volumes scheduled for regeneration so far
};

// Called on whichever thread made progress, one call at a time.
using ProgressCallback = std::function<void(const ProgressEvent&)>;

}  // namespace vs::eval
