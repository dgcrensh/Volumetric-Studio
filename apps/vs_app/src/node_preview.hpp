#pragma once

// A node's output drawn in place (M15): proc::preview() of an inspected
// value turned into scene items (role NodePreview) the viewport shows over
// the volume. Plain data, no Qt.
//
//   Field    its region as an iso-surface (where the field is negative,
//            meshed by proc::preview() on a coarse grid over the boundary's
//            box), or a slice: the samples on one z level of that grid,
//            coloured blue inside (negative), white at the surface and red
//            outside, with the zero contour drawn as lines
//   Mesh     the mesh
//   Frames   a small diamond at each frame's origin (Curves: at each point)
//   Scalar, Vector, List: nothing to draw; the text says what it is
//
// The preview is the value, not the contents: it isn't clipped to the
// boundary, and the viewport hides the volume's own contents while it is
// shown so the two don't hide each other.

#include <optional>
#include <string>

#include "scene.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/proc/inspect.hpp"

namespace vs::app {

enum class FieldView { Surface, Slice };

struct NodePreviewOptions {
  FieldView field = FieldView::Surface;
  double slice = 0.5;  // the slice's height through the grid, 0 (bottom) .. 1 (top)
};

// The colours previews are drawn in.
inline constexpr Rgb kPreviewColor{0.96f, 0.62f, 0.10f};
inline constexpr Rgb kPreviewInside{0.16f, 0.36f, 0.85f};
inline constexpr Rgb kPreviewOutside{0.85f, 0.22f, 0.18f};

// The scene for one previewed node of `volume`. `bounds` (the volume's
// boundary box) sizes the frame glyphs.
Scene preview_scene(const proc::Preview& preview, const Id& volume, const Id& node, const fbox3& bounds,
                    const NodePreviewOptions& options = {});

// What the node editor shows for an inspected node.
struct NodePreviewResult {
  Id volume;
  Id node;
  std::string port;      // the output previewed
  std::string text;      // describe() of the value, or why there is none
  proc::PortType type = proc::PortType::Scalar;
  Scene scene;
  core::Diagnostics diagnostics;  // the inspection's
  bool from_cache = false;        // the node itself came from the node cache
  std::size_t evaluated = 0;      // nodes the inspection ran
  double inspect_ms = 0.0;        // inspection (and inputs, when not cached)
  double preview_ms = 0.0;        // proc::preview() and the scene
  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

}  // namespace vs::app
