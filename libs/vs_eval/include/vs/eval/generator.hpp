#pragma once

// Content generators: how a library above vs_eval plugs a kind of contents
// into the engine (plan, M10: "procedural volumes join the M9 scheduler").
//
// vs_eval can't link vs_proc (the module rule), so the engine asks for a
// generator per volume kind instead: Engine::set_generator(kind, g). For a
// volume of that kind with contents (not a placeholder) whose key isn't
// cached, the engine calls
//
//   1. prepare(), on the calling thread, before the wave's generations
//      start. It may use the kernel (tessellate the boundary, say), which
//      is why it isn't on a worker. It returns a Job.
//   2. the Job, on any worker thread, once (twice with
//      `check_determinism`). It fills `out`: status (Current or Failed),
//      outputs, diagnostics and `extra` (cache.hpp), and must give the same
//      result every time it runs. The engine computes the output hash
//      afterwards. It must not use the kernel.
//
// A Job may itself use `pool` (thread_pool.hpp): a call from inside one of
// the pool's tasks runs inline, so results never depend on the thread
// count.
//
// A long Job may watch `cancel` (M16) and stop early, setting `cancelled`
// on `out`: the engine then treats the volume as one it never started
// (Stale, its last contents kept, nothing cached).
//
// Generated contents with a `mesh` (vs_proc's, from M11) are checked for
// containment like a static mesh, and go into the combined mesh as they
// are (mesh::combine()'s `generated`), so they must already conform to
// the seams of the surfaces joining the volume. Contents with only `extra`
// set aren't checked (the engine knows no geometry in them). Generated
// contents never go to the disk cache.

#include <cstddef>
#include <functional>

#include "vs/core/settings.hpp"
#include "vs/eval/cache.hpp"
#include "vs/eval/cancel.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::eval {

class ContentsGenerator {
 public:
  using Job = std::function<void(GeneratedContents& out)>;

  virtual ~ContentsGenerator() = default;

  // The job generating `document.volumes[index]`'s contents, given the
  // wave's layout build. On the calling thread. Never null.
  virtual Job prepare(const model::Document& document, const model::DocumentBuild& layout,
                      std::size_t index, const core::ProductSettings& settings, ThreadPool& pool,
                      const CancellationToken& cancel) = 0;
};

}  // namespace vs::eval
