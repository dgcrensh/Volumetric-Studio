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

#include <QWidget>

#include <memory>

#include "scene.hpp"
#include "vs/core/id.hpp"

class QImage;

namespace vs::app {

struct PreparedScene;

enum class StandardView { Iso, Top, Bottom, Front, Back, Left, Right };

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
  void fit_all();
  void set_view(StandardView view);

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
  friend struct Impl;
};

}  // namespace vs::app
