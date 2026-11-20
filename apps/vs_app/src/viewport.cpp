#include "viewport.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
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
#include <Select3D_SensitiveSegment.hxx>
#include <gp_Pnt.hxx>
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
  bool vertex_colors = false;  // per-vertex colours, drawn unlit
  occ::handle<Graphic3d_ArrayOfTriangles> triangles;  // null when there are none
  occ::handle<Graphic3d_ArrayOfSegments> edges;       // null when there are none
  // Pickable parts (M16), for items drawn in one part only: each face's
  // triangles (first, count) and each edge's polyline.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> face_ranges;
  std::vector<std::vector<fvec3>> edge_lines;
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
  const bool colored = m.colors.size() == m.positions.size();
  occ::handle<Graphic3d_ArrayOfTriangles> out = new Graphic3d_ArrayOfTriangles(
      static_cast<int>(used.size()), static_cast<int>(3 * (last - first)),
      colored ? Graphic3d_ArrayFlags_VertexNormal | Graphic3d_ArrayFlags_VertexColor : Graphic3d_ArrayFlags_VertexNormal);
  for (const auto v : used) {
    const auto& p = m.positions[v];
    const auto& n = m.normals[v];
    const int at = out->AddVertex(p.x, p.y, p.z, n.x, n.y, n.z);
    if (colored) {
      const Rgb& c = m.colors[v];
      out->SetVertexColor(at, c.r, c.g, c.b);
    }
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
  out.vertex_colors = !m.colors.empty() && m.colors.size() == m.positions.size();
  for (std::size_t first = 0; first < m.triangle_count(); first += kChunkTriangles) {
    PreparedItem chunk = out;
    chunk.first_chunk = first == 0;
    chunk.triangles = triangle_chunk(m, first, std::min(m.triangle_count(), first + kChunkTriangles));
    chunks.push_back(std::move(chunk));
  }
  if (chunks.empty()) chunks.push_back(out);
  if (chunks.size() == 1) {
    for (const auto& f : m.faces) chunks.front().face_ranges.emplace_back(f.first, f.count);
    if (m.edge_faces.size() == m.edges.size()) chunks.front().edge_lines = m.edges;
  }
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

// Selection modes of a MeshObject: the whole item, its faces, its edges.
constexpr int kModeWhole = 0;
constexpr int kModeFaces = 1;
constexpr int kModeEdges = 2;

// The owner of one face or edge of an item (pick mode, M16).
class PartOwner : public SelectMgr_EntityOwner {
  DEFINE_STANDARD_RTTI_INLINE(PartOwner, SelectMgr_EntityOwner)

 public:
  PartOwner(const occ::handle<SelectMgr_SelectableObject>& object, PickedPart::Kind kind, std::size_t index,
            int priority)
      : SelectMgr_EntityOwner(object, priority), kind_(kind), index_(index) {}
  [[nodiscard]] PickedPart::Kind kind() const { return kind_; }
  [[nodiscard]] std::size_t index() const { return index_; }

 private:
  PickedPart::Kind kind_;
  std::size_t index_;
};

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
    if (item.vertex_colors) fill_->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    if (item.role == ItemRole::NodePreview) {
      // Previews are drawn just in front of what they lie in or on.
      fill_->SetPolygonOffsets(Aspect_POM_Fill, -1.0f, -1.0f);
    }
    if (item.role == ItemRole::ConnectionSurface) {
      // The patch lies on boundary faces: draw it just in front of them.
      fill_->SetPolygonOffsets(Aspect_POM_Fill, -2.0f, -2.0f);
    }
    const float edge_scale = item.role == ItemRole::Sketch ? 1.0f : item.role == ItemRole::Contents ? 0.45f : 0.6f;
    const double width = item.role == ItemRole::Sketch || item.role == ItemRole::NodePreview ? 2.0
                         : item.role == ItemRole::Boundary                                   ? 1.5
                                                                                             : 1.0;
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
    if (item_.triangles.IsNull()) return;
    if (mode == kModeWhole) {
      occ::handle<SelectMgr_EntityOwner> owner = new SelectMgr_EntityOwner(this, priority());
      occ::handle<Select3D_SensitivePrimitiveArray> sensitive = new Select3D_SensitivePrimitiveArray(owner);
      sensitive->InitTriangulation(item_.triangles->Attributes(), item_.triangles->Indices(), TopLoc_Location());
      selection->Add(sensitive);
    } else if (mode == kModeFaces) {
      // Each face its own part. The triangles were added face by face, so
      // a face is a range of the index buffer.
      for (std::size_t f = 0; f < item_.face_ranges.size(); ++f) {
        const auto [first, count] = item_.face_ranges[f];
        if (count == 0) continue;
        occ::handle<SelectMgr_EntityOwner> owner = new PartOwner(this, PickedPart::Kind::Face, f, priority());
        occ::handle<Select3D_SensitivePrimitiveArray> sensitive = new Select3D_SensitivePrimitiveArray(owner);
        sensitive->InitTriangulation(item_.triangles->Attributes(), item_.triangles->Indices(), TopLoc_Location(),
                                     static_cast<int>(3 * first), static_cast<int>(3 * (first + count)) - 1);
        selection->Add(sensitive);
      }
    } else if (mode == kModeEdges) {
      for (std::size_t e = 0; e < item_.edge_lines.size(); ++e) {
        const auto& line = item_.edge_lines[e];
        occ::handle<SelectMgr_EntityOwner> owner = new PartOwner(this, PickedPart::Kind::Edge, e, priority() + 1);
        for (std::size_t i = 1; i < line.size(); ++i) {
          selection->Add(new Select3D_SensitiveSegment(owner, gp_Pnt(line[i - 1].x, line[i - 1].y, line[i - 1].z),
                                                       gp_Pnt(line[i].x, line[i].y, line[i].z)));
        }
      }
    }
  }

  [[nodiscard]] bool has_parts(PickMode mode) const {
    if (!item_.first_chunk) return false;
    if (mode == PickMode::Face) return !item_.face_ranges.empty();
    if (mode == PickMode::Edge) return !item_.edge_lines.empty() && item_.role != ItemRole::ConnectionSurface;
    return false;
  }

 private:
  // A connection surface wins over the boundary face it lies on.
  int priority() const { return item_.role == ItemRole::ConnectionSurface ? 7 : 5; }

  PreparedItem item_;
  occ::handle<Graphic3d_AspectFillArea3d> fill_;
  occ::handle<Graphic3d_AspectLine3d> line_;
};

// A dimension handle (M16): a small diamond with an arrow the way the
// value grows, a fixed size on screen, drawn over everything. Never
// selectable: the viewport hit-tests handles itself, by their anchors.
class HandleObject : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(HandleObject, AIS_InteractiveObject)

 public:
  explicit HandleObject(const ViewHandle& h) : handle_(h) {
    SetTransformPersistence(
        new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, gp_Pnt(h.anchor.x, h.anchor.y, h.anchor.z)));
    SetZLayer(Graphic3d_ZLayerId_Topmost);
  }

  bool AcceptDisplayMode(const int mode) const override { return mode == 0; }

  void Compute(const occ::handle<PrsMgr_PresentationManager>&, const occ::handle<Prs3d_Presentation>& prs,
               const int mode) override {
    if (mode != 0) return;
    const Quantity_Color colour(0.95, 0.35, 0.10, Quantity_TOC_RGB);
    constexpr double r = 7.0;
    const gp_Pnt c(0, 0, 0);
    const gp_Pnt px(r, 0, 0), nx(-r, 0, 0), py(0, r, 0), ny(0, -r, 0), pz(0, 0, r), nz(0, 0, -r);
    occ::handle<Graphic3d_ArrayOfTriangles> tris = new Graphic3d_ArrayOfTriangles(24, 0, Graphic3d_ArrayFlags_None);
    const gp_Pnt* ring[4] = {&px, &py, &nx, &ny};
    for (int i = 0; i < 4; ++i) {
      const gp_Pnt& a = *ring[i];
      const gp_Pnt& b = *ring[(i + 1) % 4];
      tris->AddVertex(a);
      tris->AddVertex(b);
      tris->AddVertex(pz);
      tris->AddVertex(b);
      tris->AddVertex(a);
      tris->AddVertex(nz);
    }
    occ::handle<Graphic3d_AspectFillArea3d> fill = new Graphic3d_AspectFillArea3d();
    fill->SetInteriorStyle(Aspect_IS_SOLID);
    fill->SetInteriorColor(colour);
    fill->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    occ::handle<Graphic3d_Group> g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(fill);
    g->AddPrimitiveArray(tris);
    occ::handle<Graphic3d_ArrayOfSegments> arrow = new Graphic3d_ArrayOfSegments(2);
    arrow->AddVertex(c);
    arrow->AddVertex(gp_Pnt(handle_.direction.x * 30.0, handle_.direction.y * 30.0, handle_.direction.z * 30.0));
    occ::handle<Graphic3d_Group> l = prs->NewGroup();
    l->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(colour, Aspect_TOL_SOLID, 2.5));
    l->AddPrimitiveArray(arrow);
  }

  void ComputeSelection(const occ::handle<SelectMgr_Selection>&, const int) override {}

 private:
  ViewHandle handle_;
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
    // Pick mode: the viewport's own click handling picks (pick_at()).
    if (pick_mode != PickMode::None) return;
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

  // The face or edge an owner stands for (pick mode).
  static std::optional<PickedPart> part_of(const occ::handle<SelectMgr_EntityOwner>& o) {
    const auto part = occ::handle<PartOwner>::DownCast(o);
    if (part.IsNull()) return std::nullopt;
    const auto obj = occ::handle<MeshObject>::DownCast(part->Selectable());
    if (obj.IsNull()) return std::nullopt;
    return PickedPart{obj->item().subject, obj->item().role, part->kind(), part->index()};
  }

  [[nodiscard]] bool pickable_in_mode(const PreparedItem& item) const {
    return std::find(pickable.begin(), pickable.end(), std::make_pair(item.subject, item.role)) != pickable.end();
  }

  [[nodiscard]] bool highlighted(const core::Id& subject) const {
    return std::find(highlight.begin(), highlight.end(), subject) != highlight.end();
  }

  // Outside the volume being edited in context (see set_context()).
  [[nodiscard]] bool dimmed(const PreparedItem& item) const {
    if (context_volume.is_null() || item.volume == context_volume) return false;
    if (std::find(context_inside.begin(), context_inside.end(), item.volume) != context_inside.end()) return false;
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
    int selection_mode = item.role == ItemRole::Contents || dim ? -1 : kModeWhole;
    // Pick mode: faces or edges of the items on the list, nothing else.
    if (pick_mode != PickMode::None) {
      selection_mode = -1;
      if (!dim && pickable_in_mode(item) && obj->has_parts(pick_mode)) {
        selection_mode = pick_mode == PickMode::Face ? kModeFaces : kModeEdges;
      }
    }
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
      // A node preview shows in place of its volume's contents.
      if (item.role == ItemRole::Contents && overlay && !overlay_volume.is_null() && item.volume == overlay_volume) {
        continue;
      }
      if (item.first_chunk) {
        display(item);
      } else {
        pending.push_back(&item);
      }
    }
    if (!pending.empty()) chunk_timer->start();
  }

  // The node preview overlay: never pickable, never dimmed.
  void redisplay_overlay() {
    for (const auto& obj : overlay_objects) context->Remove(obj, false);
    overlay_objects.clear();
    if (!overlay) return;
    for (const auto& item : overlay->items) {
      occ::handle<MeshObject> obj = new MeshObject(item);
      context->Display(obj, 0, -1, false);
      overlay_objects.push_back(obj);
    }
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
  std::shared_ptr<const PreparedScene> overlay;
  core::Id overlay_volume;
  std::vector<occ::handle<MeshObject>> overlay_objects;
  std::vector<core::Id> highlight;
  core::Id context_volume;
  std::vector<core::Id> context_inputs;
  std::vector<core::Id> context_inside;
  PickMode pick_mode = PickMode::None;
  std::vector<std::pair<core::Id, ItemRole>> pickable;
  // Pick cycling: where the last pick was, and which of the parts under
  // it (1 = nearest).
  QPointF last_pick_at;
  bool last_pick_valid = false;
  int pick_rank = 1;
  std::vector<ViewHandle> handles;
  std::vector<occ::handle<HandleObject>> handle_objects;

  void redisplay_handles() {
    for (const auto& obj : handle_objects) context->Remove(obj, false);
    handle_objects.clear();
    for (const auto& h : handles) {
      occ::handle<HandleObject> obj = new HandleObject(h);
      context->Display(obj, 0, -1, false);
      handle_objects.push_back(obj);
    }
  }
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
  // In pick mode the selection shows the picked face or edge.
  if (impl_->pick_mode != PickMode::None) {
    update();
    return;
  }
  ctx->ClearSelected(false);
  for (const auto& obj : impl_->objects) {
    const auto& item = obj->item();
    if (item.role == ItemRole::Contents || item.role == ItemRole::Sketch || impl_->dimmed(item)) continue;
    if (impl_->highlighted(item.subject)) ctx->AddOrRemoveSelected(obj, false);
  }
  update();
}

void Viewport::set_context(const core::Id& volume, const std::vector<core::Id>& inputs,
                           const std::vector<core::Id>& inside) {
  if (impl_->context_volume == volume && impl_->context_inputs == inputs && impl_->context_inside == inside) return;
  impl_->context_inside = inside;
  impl_->context_volume = volume;
  impl_->context_inputs = inputs;
  if (!impl_->ensure()) return;
  impl_->redisplay();
  set_highlight(impl_->highlight);
}

const core::Id& Viewport::context_volume() const { return impl_->context_volume; }

void Viewport::set_overlay(const Scene& scene, std::shared_ptr<const PreparedScene> prepared, const core::Id& volume) {
  if (!prepared) prepared = prepare(scene);
  const bool hide_changed = impl_->overlay_volume != volume || !impl_->overlay;
  impl_->overlay = std::move(prepared);
  impl_->overlay_volume = volume;
  if (!impl_->ensure()) return;
  if (hide_changed) {
    impl_->redisplay();
    set_highlight(impl_->highlight);
  }
  impl_->redisplay_overlay();
  update();
}

void Viewport::clear_overlay() {
  if (!impl_->overlay) return;
  impl_->overlay.reset();
  impl_->overlay_volume = {};
  if (!impl_->ensure()) return;
  impl_->redisplay_overlay();
  impl_->redisplay();
  set_highlight(impl_->highlight);
  update();
}

std::size_t Viewport::overlay_items() const { return impl_->overlay_objects.size(); }

const core::Id& Viewport::overlay_volume() const { return impl_->overlay_volume; }

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
  impl_->last_pick_valid = false;
  fit_all();
}

void Viewport::look_along(const core::dvec3& eye) {
  if (!impl_->ensure()) return;
  impl_->view->SetProj(eye.x, eye.y, eye.z);
  impl_->last_pick_valid = false;
  fit_all();
}

// --- picking (M16) -------------------------------------------------------------------------

void Viewport::set_pick_mode(PickMode mode, std::vector<std::pair<core::Id, ItemRole>> pickable) {
  // The same mode again (a dialog's next pick) keeps what is shown. Where
  // the last pick was is kept across modes, so clicking again at one place
  // still picks what lies behind; moving the camera or a new scene forgets it.
  if (mode == impl_->pick_mode && pickable == impl_->pickable) return;
  impl_->pick_mode = mode;
  impl_->pickable = std::move(pickable);
  if (!impl_->ensure()) return;
  impl_->context->ClearSelected(false);
  impl_->context->SetPixelTolerance(mode == PickMode::Edge ? 6 : 2);
  impl_->redisplay();
  set_highlight(impl_->highlight);
  setCursor(mode == PickMode::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
  update();
}

PickMode Viewport::pick_mode() const { return impl_->pick_mode; }

std::optional<PickedPart> Viewport::pick_at(QPointF position) {
  if (!impl_->ensure()) return std::nullopt;
  auto& ctx = impl_->context;
  const auto p = impl_->pixel(position);
  impl_->view->Invalidate();
  ctx->MoveTo(p.x(), p.y(), impl_->view, false);
  if (!ctx->HasDetected()) {
    if (impl_->pick_mode == PickMode::None) {
      ctx->ClearSelected(false);
      emit picked({});
    }
    update();
    return std::nullopt;
  }
  std::optional<PickedPart> part;
  if (impl_->pick_mode != PickMode::None) {
    // Clicking again at the same place picks what lies behind the last
    // pick there (faces hidden by others, a boundary face under a patch).
    const auto& selector = ctx->MainSelector();
    const int n = selector->NbPicked();
    const bool again = impl_->last_pick_valid && std::abs(position.x() - impl_->last_pick_at.x()) <= 2.0 &&
                       std::abs(position.y() - impl_->last_pick_at.y()) <= 2.0;
    impl_->pick_rank = again ? impl_->pick_rank % std::max(n, 1) + 1 : 1;
    impl_->last_pick_at = position;
    impl_->last_pick_valid = true;
    occ::handle<SelectMgr_EntityOwner> owner = n > 0 ? selector->Picked(impl_->pick_rank) : ctx->DetectedOwner();
    ctx->ClearSelected(false);
    if (!owner.IsNull()) ctx->AddOrRemoveSelected(owner, false);
    part = Impl::part_of(owner);
    if (part) emit partPicked(*part);
    update();
    return part;
  }
  ctx->SelectDetected(AIS_SelectionScheme_Replace);
  part = Impl::part_of(ctx->DetectedOwner());
  if (impl_->pick_mode != PickMode::None) {
    if (part) emit partPicked(*part);
  } else if (auto obj = occ::handle<MeshObject>::DownCast(ctx->DetectedInteractive()); !obj.IsNull()) {
    emit picked(obj->item().subject);
  }
  update();
  return part;
}

std::optional<QPointF> Viewport::project(const core::dvec3& point) const {
  if (impl_->view.IsNull()) return std::nullopt;
  int x = 0, y = 0;
  impl_->view->Convert(point.x, point.y, point.z, x, y);
  const double r = devicePixelRatioF();
  return QPointF(x / r, y / r);
}

// --- dimension handles (M16) -----------------------------------------------------------------

void Viewport::set_handles(std::vector<ViewHandle> handles) {
  impl_->handles = std::move(handles);
  dragging_handle_.reset();
  if (!impl_->ensure()) return;
  impl_->redisplay_handles();
  update();
}

std::size_t Viewport::handle_count() const { return impl_->handles.size(); }

std::optional<std::size_t> Viewport::handle_at(QPointF position) const {
  constexpr double kRadius = 9.0;
  std::optional<std::size_t> best;
  double best_d = kRadius;
  for (std::size_t i = 0; i < impl_->handles.size(); ++i) {
    const auto at = project(impl_->handles[i].anchor);
    if (!at) continue;
    const double d = std::hypot(at->x() - position.x(), at->y() - position.y());
    if (d <= best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

double Viewport::drag_distance(std::size_t index, QPointF from, QPointF to) const {
  if (index >= impl_->handles.size()) return 0.0;
  const ViewHandle& h = impl_->handles[index];
  // Pixels the handle moves per mm along its direction, on screen.
  const auto a = project(h.anchor);
  const auto b = project(h.anchor + h.direction);
  if (!a || !b) return 0.0;
  const QPointF per_mm = *b - *a;
  const double len2 = per_mm.x() * per_mm.x() + per_mm.y() * per_mm.y();
  if (len2 < 1e-6) return 0.0;  // the direction points at the viewer
  const QPointF moved = to - from;
  return (moved.x() * per_mm.x() + moved.y() * per_mm.y()) / len2;
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
  press_at_ = event->position();
  // A left press on a dimension handle drags it instead of orbiting.
  if (event->button() == Qt::LeftButton) {
    if (const auto h = handle_at(event->position())) {
      dragging_handle_ = *h;
      drag_from_ = event->position();
      emit handleDragged(static_cast<int>(*h), 0.0, false);
      return;
    }
  }
  if (impl_->UpdateMouseButtons(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
  if (impl_->view.IsNull()) return;
  if (dragging_handle_ && event->button() == Qt::LeftButton) {
    const std::size_t h = *dragging_handle_;
    dragging_handle_.reset();
    emit handleDragged(static_cast<int>(h), drag_distance(h, drag_from_, event->position()), true);
    return;
  }
  // Pick mode: a click (a press and release in place) picks a part.
  if (impl_->pick_mode != PickMode::None && event->button() == Qt::LeftButton &&
      std::hypot(event->position().x() - press_at_.x(), event->position().y() - press_at_.y()) < 4.0) {
    impl_->UpdateMouseButtons(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                              to_vflags(event->modifiers()), false);
    pick_at(event->position());
    return;
  }
  if (impl_->UpdateMouseButtons(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
  if (impl_->view.IsNull()) return;
  if (dragging_handle_) {
    emit handleDragged(static_cast<int>(*dragging_handle_), drag_distance(*dragging_handle_, drag_from_, event->position()),
                       false);
    return;
  }
  if (event->buttons() != Qt::NoButton) impl_->last_pick_valid = false;  // orbiting or panning
  if (impl_->UpdateMousePosition(impl_->pixel(event->position()), to_vkeys(event->buttons()),
                                 to_vflags(event->modifiers()), false)) {
    update();
  }
}

void Viewport::wheelEvent(QWheelEvent* event) {
  if (impl_->view.IsNull()) return;
  impl_->last_pick_valid = false;
  if (impl_->UpdateZoom(Aspect_ScrollDelta(impl_->pixel(event->position()),
                                           static_cast<double>(event->angleDelta().y()) / 8.0))) {
    update();
  }
}

}  // namespace vs::app
