#pragma once

// Solving sketches.
//
// `SketchSolver` is the studio's solver interface. Today it is implemented
// over PlaneGCS (FreeCAD's sketch solver, vendored in third_party/planegcs);
// no PlaneGCS type appears here, so another solver can replace it inside
// this module alone.
//
// A solve moves the sketch's points (and circle radii) so every constraint
// holds, and reports what it found:
//
//   Solved            every constraint holds and nothing can move.
//   UnderConstrained  every constraint holds; `dof` degrees of freedom
//                     remain, and `free_entities` names what can still move.
//   OverConstrained   some constraints conflict or are redundant, named in
//                     `conflicting` / `redundant`. With conflicts the sketch
//                     is left unchanged; redundant constraints alone are
//                     reported but the sketch is still solved.
//   Failed            the solver did not converge; the sketch is unchanged.
//
// The same findings come back as diagnostics ("sketch.solve.*"), whose first
// subject is the constraint or entity to act on and whose last is the sketch.
//
// Determinism: a solve is a pure function of the sketch up to the solver's
// tolerance, not bit for bit. PlaneGCS orders some internal work by pointer
// value, so two solves of the same sketch can differ in the last bits of a
// coordinate. Cache keys should hash a sketch's inputs, not solved positions.

#include <memory>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::sketch {

using core::ProductSettings;

enum class SolveStatus { Solved, UnderConstrained, OverConstrained, Failed };

std::string_view to_string(SolveStatus status);

struct SolveReport {
  SolveStatus status = SolveStatus::Failed;
  // Remaining degrees of freedom. Negative is never reported; an
  // over-constrained sketch reports what is left after its redundant
  // constraints are set aside.
  int dof = 0;
  // Points, circles and arcs with at least one undetermined parameter, in
  // sketch order. (A line is free when one of its end points is.)
  std::vector<Id> free_entities;
  // Constraints taking part in a conflict, in sketch order.
  std::vector<Id> conflicting;
  // Constraints that only repeat what others already fix, in sketch order.
  std::vector<Id> redundant;
  // The findings above as diagnostics, plus any structural errors that kept
  // the sketch from being solved at all.
  Diagnostics diagnostics;
  // True when the sketch's points were updated by this solve.
  bool applied = false;

  [[nodiscard]] bool ok() const {
    return status == SolveStatus::Solved || status == SolveStatus::UnderConstrained;
  }
};

class SketchSolver {
 public:
  virtual ~SketchSolver() = default;

  // Solves `sketch` in place (see the statuses above).
  virtual SolveReport solve(Sketch& sketch, const ProductSettings& settings) = 0;

  // Drag solving: moves `point` towards `target` while every constraint keeps
  // holding, letting the rest of the sketch follow. When the point can't
  // reach the target (it is constrained), it moves as close as the
  // constraints allow. Fails like solve() when the sketch itself does.
  virtual SolveReport drag(Sketch& sketch, const Id& point, dvec2 target,
                           const ProductSettings& settings) = 0;
};

// The PlaneGCS-backed solver.
std::unique_ptr<SketchSolver> make_solver();

// Convenience: solve with a fresh default solver.
SolveReport solve(Sketch& sketch, const ProductSettings& settings);
SolveReport drag(Sketch& sketch, const Id& point, dvec2 target, const ProductSettings& settings);

}  // namespace vs::sketch
