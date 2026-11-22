#pragma once

// Views: projecting shapes and meshes into a view's plane (L6).
//
// A view looks along -toward. Its frame (view_frame()) has unit `right`,
// `up` and `toward` axes with right × up = toward; a point projects to
// (right · p, up · p), and a larger toward · p is nearer the viewer.
//
// A ViewProjector collects what a view shows and turns it into 2D lines in
// view millimetres (model mm, before the view's scale):
//
//   - outlines (boundaries, connection surfaces' patches) are drawn whole:
//     their edges and silhouettes, never hidden and never hiding anything;
//   - contents (B-rep solids, meshes) are drawn with hidden lines removed
//     among all the contents added: edges (a B-rep's edges, seams of closed
//     surfaces left out; a mesh's borders and creases) and silhouettes
//     (triangle edges between a face turned towards the viewer and one
//     turned away), each split into visible and hidden runs.
//
// Visibility is a ray test against every contents triangle, through a
// uniform grid over the view: a point of an edge is hidden when a triangle
// covering it is nearer the viewer by more than a small tolerance. Edges
// are sampled every `step`; hidden runs lying on a visible line are
// dropped (an edge seen edge-on behind its own face), then runs are
// simplified (Douglas-Peucker within `simplify`). Everything is FP64 in a
// fixed order; `parallel` only splits independent edges, folded in order,
// so the result is the same on any thread count.

#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/document.hpp"
#include "vs/validate/parallel.hpp"

namespace vs::draw {

using core::dvec2;
using core::dvec3;
using core::Id;

struct ViewFrame {
  dvec3 right{1, 0, 0};
  dvec3 up{0, 0, 1};
  dvec3 toward{0, -1, 0};
  [[nodiscard]] dvec2 project(const dvec3& p) const { return {dot(p, right), dot(p, up)}; }
  [[nodiscard]] double depth(const dvec3& p) const { return dot(p, toward); }
};

ViewFrame view_frame(model::ViewDirection direction);

struct ProjectionOptions {
  double deflection = 0.02;  // tessellation, mm
  double step = 0.1;         // between visibility samples, mm
  double simplify = 0.02;    // Douglas-Peucker tolerance, mm
  // Mesh edges between triangles meeting at more than this angle are
  // drawn as creases (radians).
  double crease_angle = 1.0471975511965976;  // 60°
  validate::ParallelFor parallel;  // empty: the calling thread
};

enum class LineKind { Outline, Visible, Hidden };

struct ViewLine {
  LineKind kind = LineKind::Visible;
  Id source;
  std::vector<dvec2> points;  // at least two
};

struct ProjectedView {
  std::vector<ViewLine> lines;  // outlines first, then contents, in the order added
  core::Diagnostics diagnostics;
};

class ViewProjector {
 public:
  ViewProjector(ViewFrame frame, ProjectionOptions options, core::ProductSettings settings);

  // Drawn whole; never hidden, never hiding.
  void add_outline(const Id& source, const kernel::Shape& shape);
  // Drawn with hidden lines removed, and hiding each other.
  void add_contents(const Id& source, const kernel::Shape& shape);
  void add_contents(const Id& source, const mesh::Mesh& mesh);

  // Projects everything added. `hidden`: also return hidden runs.
  [[nodiscard]] ProjectedView run(bool hidden) const;

 private:
  struct Item {
    Id source;
    bool contents = false;
    kernel::Shape shape;
    const mesh::Mesh* mesh = nullptr;
  };
  ViewFrame frame_;
  ProjectionOptions options_;
  core::ProductSettings settings_;
  std::vector<Item> items_;
};

// Every point of a shape's tessellation (its B-rep vertices and edge and
// face points), for extremes and bounds: what a drawing measures and
// places against. Empty when it can't be tessellated.
std::vector<dvec3> shape_points(const kernel::Shape& shape, double deflection,
                                const core::ProductSettings& settings);

}  // namespace vs::draw
