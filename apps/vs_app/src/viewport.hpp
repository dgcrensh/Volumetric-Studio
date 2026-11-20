#pragma once

// The 3D viewport: OCCT's AIS/V3d viewer in a Qt widget (plan: "OCCT's
// AIS/V3d viewer embedded in a Qt widget").
//
// It draws a Scene (scene.hpp): boundaries as transparent tints with their
// edges, contents shaded (grey when stale), connection surfaces coloured
// by type. Every item is a triangle mesh: B-reps arrive tessellated by
// vs_kernel on the session's worker thread, and prepare() turns the scene
// into OCCT primitive arrays there too, so the UI thread only swaps them
// in (decision 0013). OCCT's headers stay in viewport.cpp.
//
// Mouse: left drag orbits, middle or right drag pans, the wheel zooms
// about the cursor, a click selects (boundaries and connection surfaces
// are pickable; contents are picked through their boundary), and the view
// cube in the top right corner turns the view to a face, edge or corner.
//
// M16 adds two things:
//
//   - pick mode: a click picks a face or an edge of a B-rep item (each face
//     its own pickable part, picking.hpp) instead of selecting the item,
//     and only items on the given list can be picked (edit in context);
//   - dimension handles: small markers drawn on top of everything; a left
//     drag starting on one moves it along its direction instead of
//     orbiting, reported as the distance moved in mm.

#include <QWidget>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "picking.hpp"
#include "scene.hpp"
#include "vs/core/id.hpp"

class QImage;

namespace vs::app {

struct PreparedScene;

enum class StandardView { Iso, Top, Bottom, Front, Back, Left, Right };

enum class PickMode { None, Face, Edge };

// A dimension handle (dimension_drag.hpp) as the viewport draws it.
struct ViewHandle {
  core::dvec3 anchor{};
  core::dvec3 direction{};  // unit
};

class Viewport : public QWidget {
  Q_OBJECT

 public:
  explicit Viewport(QWidget* parent = nullptr);
  ~Viewport() override;

  // Thread-safe: GPU-ready arrays for `scene` (see above).
  static std::shared_ptr<const PreparedScene> prepare(const Scene& scene);

  // Replaces what is shown. `prepared` must come from prepare() of the
  // same scene (or be null: then it is prepared here). `fit` frames it.
  void show_scene(const Scene& scene, std::shared_ptr<const PreparedScene> prepared, bool fit);
  void clear();
  // Highlights the items of `subject` (a volume or a connection surface);
  // null clears the highlight. Doesn't emit picked().
  void set_highlight(const core::Id& subject);
  // Highlights several subjects at once (a feature and its volume, say).
  // Secondary items (sketches a feature uses) are shown only while their
  // subject is highlighted.
  void set_highlight(const std::vector<core::Id>& subjects);
  // Edit in context (M14): everything but olume's own items (its
  // boundary, contents and sketches) and the connection surfaces in
  // inputs is drawn dimmed and can't be picked. A null volume ends it.
  void set_context(const core::Id& volume, const std::vector<core::Id>& inputs);
  [[nodiscard]] const core::Id& context_volume() const;
  // A node preview (M15, node_preview.hpp) drawn over the scene, never
  // pickable; the contents of `volume` are hidden while it shows, so the
  // preview isn't buried in them. `prepared` as for show_scene().
  void set_overlay(const Scene& scene, std::shared_ptr<const PreparedScene> prepared, const core::Id& volume);
  void clear_overlay();
  // Overlay items shown, and the volume whose contents they replace.
  [[nodiscard]] std::size_t overlay_items() const;
  [[nodiscard]] const core::Id& overlay_volume() const;
  // Items shown dimmed; items of a role shown (parts of big meshes once).
  [[nodiscard]] std::size_t dimmed_items() const;
  [[nodiscard]] std::size_t shown_items(ItemRole role) const;
  void fit_all();
  void set_view(StandardView view);
  // Looks at the scene from the `eye` side (a direction from the scene
  // towards the viewer), framed.
  void look_along(const core::dvec3& eye);

  // --- picking faces and edges (M16)
  // In Face or Edge mode a click picks a part of one of `pickable`'s items
  // (subject and role) and emits partPicked() instead of picked().
  void set_pick_mode(PickMode mode, std::vector<std::pair<core::Id, ItemRole>> pickable = {});
  [[nodiscard]] PickMode pick_mode() const;
  // Picks at a point of the widget as a click there would (emitting what a
  // click emits); what was hit, if anything.
  std::optional<PickedPart> pick_at(QPointF position);
  // Where a model point appears in the widget (nullopt without a view).
  [[nodiscard]] std::optional<QPointF> project(const core::dvec3& point) const;

  // --- dimension handles (M16)
  void set_handles(std::vector<ViewHandle> handles);
  [[nodiscard]] std::size_t handle_count() const;
  // The handle drawn within a few pixels of `position`, if any.
  [[nodiscard]] std::optional<std::size_t> handle_at(QPointF position) const;
  // The distance (mm along the handle's direction) a move of the mouse
  // from `from` to `to` drags handle `index`.
  [[nodiscard]] double drag_distance(std::size_t index, QPointF from, QPointF to) const;

  // Renders the view off screen at the widget's size.
  [[nodiscard]] QImage render_image();
  // Whether OpenGL came up (false on a machine without a usable driver).
  [[nodiscard]] bool has_gpu() const;
  // Scene items shown (an item split into parts counts once).
  [[nodiscard]] std::size_t displayed_items() const;
  // True while parts of big meshes are still being added, one per frame.
  [[nodiscard]] bool loading() const;
  [[nodiscard]] double last_frame_ms() const { return last_frame_ms_; }

  QPaintEngine* paintEngine() const override { return nullptr; }

 signals:
  // The user clicked an item (its subject) or empty space (null).
  void picked(vs::core::Id subject);
  // Pick mode: the face or edge clicked.
  void partPicked(vs::app::PickedPart part);
  // A handle dragged `distance` mm along its direction since the press;
  // `finished` on release.
  void handleDragged(int index, double distance, bool finished);
  void frameRendered(double ms);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  double last_frame_ms_ = 0.0;
  // A handle drag in progress (index, where it started).
  std::optional<std::size_t> dragging_handle_;
  QPointF drag_from_;
  QPointF press_at_;
  friend struct Impl;
};

}  // namespace vs::app
