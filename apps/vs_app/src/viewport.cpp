#include "viewport.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>

#include <AIS_AnimationCamera.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Quantity_Color.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <Standard_Failure.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>

namespace vs::app {

// --- prepared scenes ------------------------------------------------------------------

struct PreparedItem {
  ItemRole role = ItemRole::Boundary;
  core::Id subject;
  core::Id volume;
  bool secondary = false;
  Rgb color;
  float transparency = 0.0f;
  bool first_chunk = true;  // the item's first (or only) part
  occ::handle<Graphic3d_ArrayOfTriangles> triangles;  // null when there are none
  occ::handle<Graphic3d_ArrayOfSegments> edges;       // null when there are none
};

struct PreparedScene {
  std::vector<PreparedItem> items;
};

namespace {

Quantity_Color qcolor(const Rgb& c, float scale = 1.0f) {
  return Quantity_Color(std::min(1.0f, c.r * scale), std::min(1.0f, c.g * scale),
                        std::min(1.0f, c.b * scale), Quantity_TOC_RGB);
}

// Big meshes go to the GPU a chunk per frame (see show_scene()), so no
// single frame uploads a million triangles.
constexpr std::size_t kChunkTriangles = 60000;

// Triangles [first, last) of `m`, with only the vertices they use.
occ::handle<Graphic3d_ArrayOfTriangles> triangle_chunk(const DisplayMesh& m, std::size_t first, std::size_t last) {
  std::vector<std::int32_t> remap(m.positions.size(), -1);
  std::vector<std::uint32_t> used;
  for (std::size_t i = 3 * first; i < 3 * last; ++i) {
    const auto v = m.indices[i];
    if (remap[v] < 0) {
      remap[v] = static_cast<std::int32_t>(used.size());
      used.push_back(v);
    }
  }
  occ::handle<Graphic3d_ArrayOfTriangles> out = new Graphic3d_ArrayOfTriangles(
      static_cast<int>(used.size()), static_cast<int>(3 * (last - first)), Graphic3d_ArrayFlags_VertexNormal);
  for (const auto v : used) {
    const auto& p = m.positions[v];
    const auto& n = m.normals[v];
    out->AddVertex(p.x, p.y, p.z, n.x, n.y, n.z);
  }
  for (std::size_t i = 3 * first; i < 3 * last; ++i) out->AddEdge(remap[m.indices[i]] + 1);
  return out;
}

// One item, or several chunks of a big one (the first carries the edges).
std::vector<PreparedItem> prepare_item(const SceneItem& item) {
  std::vector<PreparedItem> chunks;
  PreparedItem out;
  out.role = item.role;
  out.subject = item.subject;
  out.volume = item.volume;
  out.secondary = item.secondary;
  out.color = item.color;
  out.transparency = item.transparency;
  const DisplayMesh& m = item.mesh;
  for (std::size_t first = 0; first < m.triangle_count(); first += kChunkTriangles) {
    PreparedItem chunk = out;
    chunk.first_chunk = first == 0;
    chunk.triangles = triangle_chunk(m, first, std::min(m.triangle_count(), first + kChunkTriangles));
    chunks.push_back(std::move(chunk));
  }
  if (chunks.empty()) chunks.push_back(out);
  std::size_t segments = 0;
  for (const auto& line : m.edges) segments += line.size() > 1 ? line.size() - 1 : 0;
  if (segments > 0) {
    auto& edges = chunks.front().edges;
    edges = new Graphic3d_ArrayOfSegments(static_cast<int>(2 * segments));
    for (const auto& line : m.edges) {
      for (std::size_t i = 1; i < line.size(); ++i) {
        edges->AddVertex(line[i - 1].x, line[i - 1].y, line[i - 1].z);
        edges->AddVertex(line[i].x, line[i].y, line[i].z);
      }
    }
  }
  return chunks;
}

// One scene item: its triangles, shaded, and its edges as lines.
class MeshObject : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(MeshObject, AIS_InteractiveObject)

 public:
  explicit MeshObject(const PreparedItem& item) : item_(item) {
    Graphic3d_MaterialAspect material(Graphic3d_NameOfMaterial_Plastified);
    material.SetColor(qcolor(item.color));
    material.SetTransparency(item.transparency);
    fill_ = new Graphic3d_AspectFillArea3d();
    fill_->SetInteriorStyle(Aspect_IS_SOLID);
    fill_->SetInteriorColor(qcolor(item.color));
    fill_->SetFrontMaterial(material);
    fill_->SetBackMaterial(material);
    fill_->SetDistinguishOff();
    if (item.role == ItemRole::ConnectionSurface) {
      // The patch lies on boundary faces: draw it just in front of them.
      fill_->SetPolygonOffsets(Aspect_POM_Fill, -2.0f, -2.0f);
    }
    const float edge_scale = item.role == ItemRole::Sketch ? 1.0f : item.role == ItemRole::Contents ? 0.45f : 0.6f;
    const double width = item.role == ItemRole::Sketch ? 2.0 : item.role == ItemRole::Boundary ? 1.5 : 1.0;
    line_ = new Graphic3d_AspectLine3d(qcolor(item.color, edge_scale), Aspect_TOL_SOLID, width);
    // Only see-through items go to the transparent pass: an opaque object
    // given SetTransparency(0) would still skip writing depth there.
    if (item.transparency > 0.0f) SetTransparency(item.transparency);
  }

  [[nodiscard]] const PreparedItem& item() const { return item_; }

  bool AcceptDisplayMode(const int mode) const override { return mode == 0; }

  void Compute(const occ::handle<PrsMgr_PresentationManager>&, const occ::handle<Prs3d_Presentation>& prs,
               const int mode) override {
    if (mode != 0) return;
    if (!item_.triangles.IsNull()) {
      occ::handle<Graphic3d_Group> group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(fill_);
      group->AddPrimitiveArray(item_.triangles);
    }
    if (!item_.edges.IsNull()) {
      occ::handle<Graphic3d_Group> group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(line_);
      group->AddPrimitiveArray(item_.edges);
    }
  }

  void ComputeSelection(const occ::handle<SelectMgr_Selection>& selection, const int mode) override {
    if (mode != 0 || item_.triangles.IsNull()) return;
    occ::handle<SelectMgr_EntityOwner> owner = new SelectMgr_EntityOwner(this, priority());
    occ::handle<Select3D_SensitivePrimitiveArray> sensitive = new Select3D_SensitivePrimitiveArray(owner);
    sensitive->InitTriangulation(item_.triangles->Attributes(), item_.triangles->Indices(), TopLoc_Location());
    selection->Add(sensitive);
  }

 private:
  // A connection surface wins over the boundary face it lies on.
  int priority() const { return item_.role == ItemRole::ConnectionSurface ? 7 : 5; }

  PreparedItem item_;
  occ::handle<Graphic3d_AspectFillArea3d> fill_;
  occ::handle<Graphic3d_AspectLine3d> line_;
};

Aspect_VKeyMouse to_vkeys(Qt::MouseButtons buttons) {
  Aspect_VKeyMouse keys = Aspect_VKeyMouse_NONE;
  if (buttons & Qt::LeftButton) keys |= Aspect_VKeyMouse_LeftButton;
  if (buttons & Qt::MiddleButton) keys |= Aspect_VKeyMouse_MiddleButton;
  if (buttons & Qt::RightButton) keys |= Aspect_VKeyMouse_RightButton;
  return keys;
}

Aspect_VKeyFlags to_vflags(Qt::KeyboardModifiers modifiers) {
  Aspect_VKeyFlags flags = Aspect_VKeyFlags_NONE;
  if (modifiers & Qt::ShiftModifier) flags |= Aspect_VKeyFlags_SHIFT;
  if (modifiers & Qt::ControlModifier) flags |= Aspect_VKeyFlags_CTRL;
  if (modifiers & Qt::AltModifier) flags |= Aspect_VKeyFlags_ALT;
  return flags;
}

}  // namespace

// --- the widget ----------------------------------------------------------------------

struct Viewport::Impl : public AIS_ViewController {
  explicit Impl(Viewport* owner_widget) : owner(owner_widget) {
    auto& gestures = ChangeMouseGestureMap();
    gestures.Clear();
    gestures.Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_RotateOrbit);
    const auto left = static_cast<unsigned int>(Aspect_VKeyMouse_LeftButton);
    gestures.Bind(left | static_cast<unsigned int>(Aspect_VKeyFlags_SHIFT), AIS_MouseGesture_Pan);
    gestures.Bind(left | static_cast<unsigned int>(Aspect_VKeyFlags_CTRL), AIS_MouseGesture_Zoom);
    gestures.Bind(Aspect_VKeyMouse_MiddleButton, AIS_MouseGesture_Pan);
    gestures.Bind(Aspect_VKeyMouse_RightButton, AIS_MouseGesture_Pan);
  }

  // Creates the viewer on first use; false when OpenGL isn't available.
  bool ensure() {
    if (initialised) return gpu;
    initialised = true;
    try {
      occ::handle<Aspect_DisplayConnection> display = new Aspect_DisplayConnection();
      occ::handle<OpenGl_GraphicDriver> driver = new OpenGl_GraphicDriver(display, false);
      viewer = new V3d_Viewer(driver);
      viewer->SetDefaultLights();
      viewer->SetLightOn();
      view = viewer->CreateView();
      auto& params = view->ChangeRenderingParams();
      params.NbMsaaSamples = 4;
      params.TransparencyMethod = Graphic3d_RTM_BLEND_OIT;
      params.IsAntialiasingEnabled = true;
      occ::handle<WNT_Window> window = new WNT_Window(reinterpret_cast<Aspect_Handle>(owner->winId()));
      view->SetWindow(window);
      if (!window->IsMapped()) window->Map();
      view->SetBgGradientColors(Quantity_Color(0.86, 0.88, 0.91, Quantity_TOC_RGB),
                                Quantity_Color(0.62, 0.66, 0.72, Quantity_TOC_RGB),
                                Aspect_GradientFillMethod_Vertical, false);
      view->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
      view->MustBeResized();

      context = new AIS_InteractiveContext(viewer);
      auto selected = context->HighlightStyle(Prs3d_TypeOfHighlight_Selected);
      selected->SetColor(Quantity_Color(1.0, 0.85, 0.15, Quantity_TOC_RGB));
      selected->SetTransparency(0.55f);
      auto hover = context->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic);
      hover->SetColor(Quantity_Color(0.95, 0.95, 1.0, Quantity_TOC_RGB));
      hover->SetTransparency(0.7f);

      cube = new AIS_ViewCube();
      cube->SetSize(48);
      cube->SetBoxColor(Quantity_Color(0.88, 0.89, 0.92, Quantity_TOC_RGB));
      // OCCT is built without FreeType here, so the cube has no text:
      // the coloured axes say which way is which.
      for (const auto side : {V3d_Xpos, V3d_Ypos, V3d_Zpos, V3d_Xneg, V3d_Yneg, V3d_Zneg}) {
        cube->SetBoxSideLabel(side, "");
      }
      cube->SetAxesLabels("", "", "");
      cube->SetDrawAxes(true);
      cube->SetFixedAnimationLoop(false);
      cube->SetAutoStartAnimation(true);
      cube->SetViewAnimation(ViewAnimation());
      cube->SetTransformPersistence(new Graphic3d_TransformPers(
          Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, NCollection_Vec2<int>(80, 80)));
      context->Display(cube, 0, 0, false);
      gpu = true;
    } catch (const Standard_Failure&) {
      gpu = false;
      view.Nullify();
      context.Nullify();
    }
    return gpu;
  }

  void OnSelectionChanged(const occ::handle<AIS_InteractiveContext>& ctx,
                          const occ::handle<V3d_View>&) override {
    if (!occ::handle<AIS_ViewCubeOwner>::DownCast(ctx->DetectedOwner()).IsNull()) return;
    core::Id subject;
    ctx->InitSelected();
    if (ctx->MoreSelected()) {
      if (auto obj = occ::handle<MeshObject>::DownCast(ctx->SelectedInteractive()); !obj.IsNull()) {
        subject = obj->item().subject;
      }
    }
    emit owner->picked(subject);
  }

  void handleViewRedraw(const occ::handle<AIS_InteractiveContext>& ctx,
                        const occ::handle<V3d_View>& v) override {
    AIS_ViewController::handleViewRedraw(ctx, v);
    if (toAskNextFrame()) owner->update();  // animations (the view cube)
  }

  NCollection_Vec2<int> pixel(const QPointF& p) const {
    const double r = owner->devicePixelRatioF();
    return NCollection_Vec2<int>(static_cast<int>(p.x() * r), static_cast<int>(p.y() * r));
  }

  [[nodiscard]] bool highlighted(const core::Id& subject) const {
    return std::find(highlight.begin(), highlight.end(), subject) != highlight.end();
  }

  // Outside the volume being edited in context (see set_context()).
  [[nodiscard]] bool dimmed(const PreparedItem& item) const {
    if (context_volume.is_null() || item.volume == context_volume) return false;
    return !(item.role == ItemRole::ConnectionSurface &&
             std::find(context_inputs.begin(), context_inputs.end(), item.subject) != context_inputs.end());
  }

  void display(const PreparedItem& item) {
    const bool dim = dimmed(item);
    occ::handle<MeshObject> obj;
    if (dim) {
      PreparedItem grey = item;
      grey.color = Rgb{0.70f, 0.71f, 0.74f};
      grey.transparency = std::max(item.transparency, 0.9f);
      obj = new MeshObject(grey);
    } else {
      obj = new MeshObject(item);
    }
    // Contents are picked through their boundary (see the header); nothing
    // outside the volume edited in context can be picked.
    const int selection_mode = item.role == ItemRole::Contents || dim ? -1 : 0;
    context->Display(obj, 0, selection_mode, false);
    objects.push_back(obj);
  }

  // Removes everything shown and shows the prepared scene again (after the
  // context or the secondary items shown changed).
  void redisplay() {
    for (const auto& obj : objects) context->Remove(obj, false);
    objects.clear();
    pending.clear();
    if (!prepared) return;
    for (const auto& item : prepared->items) {
      if (item.secondary && !highlighted(item.subject)) continue;
      if (item.first_chunk) {
        display(item);
      } else {
        pending.push_back(&item);
      }
    }
    if (!pending.empty()) chunk_timer->start();
  }

  Viewport* owner;
  QTimer* chunk_timer = nullptr;
  std::deque<const PreparedItem*> pending;  // parts of big meshes still to show
  bool initialised = false;
  bool gpu = false;
  occ::handle<V3d_Viewer> viewer;
  occ::handle<V3d_View> view;
  occ::handle<AIS_InteractiveContext> context;
  occ::handle<AIS_ViewCube> cube;
  std::shared_ptr<const PreparedScene> prepared;
  std::vector<occ::handle<MeshObject>> objects;
  std::vector<core::Id> highlight;
  core::Id context_volume;
  std::vector<core::Id> context_inputs;
};

Viewport::Viewport(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>(this)) {
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_NoSystemBackground);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);
  impl_->chunk_timer = new QTimer(this);
  impl_->chunk_timer->setInterval(16);
  connect(impl_->chunk_timer, &QTimer::timeout, this, [this] {
    if (!impl_->pending.empty()) {
      impl_->display(*impl_->pending.front());
      impl_->pending.pop_front();
      update();
    }
    if (impl_->pending.empty()) impl_->chunk_timer->stop();
  });
}

bool Viewport::loading() const { return !impl_->pending.empty(); }

Viewport::~Viewport() {
  if (!impl_->context.IsNull()) impl_->context->RemoveAll(false);
}

std::shared_ptr<const PreparedScene> Viewport::prepare(const Scene& scene) {
  auto out = std::make_shared<PreparedScene>();
  out->items.reserve(scene.items.size());
  for (const auto& item : scene.items) {
    for (auto& chunk : prepare_item(item)) out->items.push_back(std::move(chunk));
  }
  return out;
}

void Viewport::show_scene(const Scene& scene, std::shared_ptr<const PreparedScene> prepared, bool fit) {
  if (!prepared) prepared = prepare(scene);
  if (!impl_->ensure()) {
    impl_->prepared = std::move(prepared);
    return;
  }
  impl_->prepared = std::move(prepared);
  // Every item's first part now; the rest of big meshes one part per frame.
  impl_->redisplay();
  set_highlight(impl_->highlight);
  if (fit) fit_all();
  update();
}

void Viewport::clear() {
  show_scene({}, std::make_shared<PreparedScene>(), false);
}

void Viewport::set_highlight(const core::Id& subject) {
  set_highlight(subject.is_null() ? std::vector<core::Id>{} : std::vector<core::Id>{subject});
}

void Viewport::set_highlight(const std::vector<core::Id>& subjects) {
  // Secondary items (used sketches) come and go with the highlight.
  bool secondary_changed = false;
  if (impl_->prepared) {
    for (const auto& item : impl_->prepared->items) {
      if (!item.secondary) continue;
      const bool was = impl_->highlighted(item.subject);
      const bool now = std::find(subjects.begin(), subjects.end(), item.subject) != subjects.end();
      secondary_changed = secondary_changed || was != now;
    }
  }
  impl_->highlight = subjects;
  if (!impl_->ensure()) return;
  if (secondary_changed) impl_->redisplay();
  auto& ctx = impl_->context;
  ctx->ClearSelected(false);
  for (const auto& obj : impl_->objects) {
    const auto& item = obj->item();
    if (item.role == ItemRole::Contents || item.role == ItemRole::Sketch || impl_->dimmed(item)) continue;
    if (impl_->highlighted(item.subject)) ctx->AddOrRemoveSelected(obj, false);
  }
  update();
}

void Viewport::set_context(const core::Id& volume, const std::vector<core::Id>& inputs) {
  if (impl_->context_volume == volume && impl_->context_inputs == inputs) return;
  impl_->context_volume = volume;
  impl_->context_inputs = inputs;
  if (!impl_->ensure()) return;
  impl_->redisplay();
  set_highlight(impl_->highlight);
}

const core::Id& Viewport::context_volume() const { return impl_->context_volume; }

std::size_t Viewport::dimmed_items() const {
  std::size_t n = 0;
  for (const auto& obj : impl_->objects) n += obj->item().first_chunk && impl_->dimmed(obj->item()) ? 1 : 0;
  return n;
}

std::size_t Viewport::shown_items(ItemRole role) const {
  std::size_t n = 0;
  for (const auto& obj : impl_->objects) n += obj->item().first_chunk && obj->item().role == role ? 1 : 0;
  return n;
}

void Viewport::fit_all() {
  if (!impl_->ensure()) return;
  impl_->view->FitAll(0.08, false);
  impl_->view->ZFitAll();
  update();
}

void Viewport::set_view(StandardView v) {
  if (!impl_->ensure()) return;
  V3d_TypeOfOrientation o = V3d_TypeOfOrientation_Zup_AxoRight;
  switch (v) {
    case StandardView::Iso: o = V3d_TypeOfOrientation_Zup_AxoRight; break;
    case StandardView::Top: o = V3d_Zpos; break;
    case StandardView::Bottom: o = V3d_Zneg; break;
    case StandardView::Front: o = V3d_Yneg; break;
    case StandardView::Back: o = V3d_Ypos; break;
    case StandardView::Left: o = V3d_Xneg; break;
    case StandardView::Right: o = V3d_Xpos; break;
  }
  impl_->view->SetProj(o);
  fit_all();
}

QImage Viewport::render_image() {
  if (!impl_->ensure()) return {};
  int w = 0, h = 0;
  impl_->view->Window()->Size(w, h);
  if (w <= 0 || h <= 0) return {};
  Image_PixMap pixmap;
  V3d_ImageDumpOptions options;
  options.Width = w;
  options.Height = h;
  options.BufferType = Graphic3d_BT_RGBA;
  if (!pixmap.InitZero(Image_Format_RGBA, static_cast<size_t>(w), static_cast<size_t>(h)) ||
      !impl_->view->ToPixMap(pixmap, options)) {
    return {};
  }
  QImage image(w, h, QImage::Format_RGBA8888);
  for (int row = 0; row < h; ++row) {
    std::memcpy(image.scanLine(row), pixmap.Row(static_cast<size_t>(row)), static_cast<size_t>(w) * 4);
  }
  return image;
}

bool Viewport::has_gpu() const { return impl_->ensure(); }

std::size_t Viewport::displayed_items() const {
  std::size_t n = 0;
  for (const auto& obj : impl_->objects) n += obj->item().first_chunk ? 1 : 0;
  return n;
}

void Viewport::paintEvent(QPaintEvent*) {
  if (!impl_->ensure()) return;
  QElapsedTimer timer;
  timer.start();
  // Objects shown or highlighted since the last frame don't invalidate the
  // view by themselves; a full redraw each paint keeps the screen honest
  // (and the frame time measured here is the real one).
  impl_->view->Invalidate();
  impl_->view->InvalidateImmediate();
  impl_->FlushViewEvents(impl_->context, impl_->view, true);
  last_frame_ms_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
  emit frameRendered(last_frame_ms_);
}

void Viewport::resizeEvent(QResizeEvent*) {
  if (!impl_->view.IsNull()) {
    impl_->view->MustBeResized();
    update();
  }
}

void Viewport::mousePressEvent(QMouseEvent* event) {
  if (impl_->view.IsNull()) return;
  if (impl_->UpdateMouseButtons(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  if (impl_->view.IsNull()) return;
  if (impl_->UpdateMouseButtons(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (impl_->view.IsNull()) return;
  if (impl_->UpdateMousePosition(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                 to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::wheelEvent(QWheelEvent* event) {
  if (impl_->view.IsNull()) return;
  if (impl_->UpdateZoom(Aspect_ScrollDelta(impl_->pixel(event->position()),
                                           static_cast<double>(event->angleDelta().y()) / 8.0))) {
    update();
  }
}

}  // namespace vs::app
