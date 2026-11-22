// L6 in the window (plan docs/plans/L6-drawings.md, "App"): the Drawings
// dock lists what Model > New Drawing made and every change is one undo
// step; the dimension dialog offers the view's anchors and commits one
// step; the drawing window draws the sheet from each evaluation; Export
// Drawing writes SVG, PDF and DXF.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QImage>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "drawing_dialogs.hpp"
#include "drawing_window.hpp"
#include "authoring.hpp"
#include "drawings.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "vs/draw/drawing.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;
using model::AnchorOn;
using model::AnchorPart;
using model::DimensionKind;

namespace {

std::filesystem::path bracket() {
  return samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj";
}

void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();
}

// Waits for the evaluation, then for the drawing window's newest sheet.
void settle_drawing(app::MainWindow& w) {
  settle(w);
  REQUIRE(samples::wait_until([&] { return !w.drawing_pending(); }, 120000));
  QApplication::processEvents();
}

int count_rows(const QTreeWidget& t) {
  int n = 0;
  for (QTreeWidgetItemIterator it(const_cast<QTreeWidget*>(&t)); *it != nullptr; ++it) ++n;
  return n;
}

std::size_t rows_of(const model::Document& d) { return app::drawing_rows(d).size(); }

std::string read(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const model::DrawingView* view_of(const model::Drawing& d, model::ViewDirection direction) {
  for (const auto& v : d.views) {
    if (v.direction == direction) return &v;
  }
  return nullptr;
}

std::optional<std::size_t> choice_index(const std::vector<app::AnchorChoice>& cs, AnchorOn on, const Id& target,
                                        AnchorPart part) {
  for (std::size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].anchor.on == on && cs[i].anchor.target == target && cs[i].anchor.part == part) return i;
  }
  return std::nullopt;
}

core::Diagnostics export_and_wait(app::MainWindow& w, const Id& drawing, const std::filesystem::path& path) {
  std::optional<core::Diagnostics> result;
  w.export_drawing_to(drawing, path, [&](const core::Diagnostics& d) { result = d; });
  REQUIRE(samples::wait_until([&] { return result.has_value(); }, 120000));
  return *result;
}

}  // namespace

TEST_CASE("The Drawings dock lists a new drawing, and deleting and undoing are one step each",
          "[app][gui][drawing]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket()));
  settle(w);
  QTreeWidget& tree = w.drawings_tree();
  CHECK(tree.topLevelItemCount() == 0);
  CHECK(w.shown_drawing().is_null());

  // Model > New Drawing: one step, listed, opened.
  const auto steps = w.history().undo_count();
  const Id drawing = w.new_drawing("Sheet");
  REQUIRE_FALSE(drawing.is_null());
  CHECK(w.history().undo_count() == steps + 1);
  const model::Drawing* d = w.history().project().document.find_drawing(drawing);
  REQUIRE(d != nullptr);
  REQUIRE(d->views.size() == 4);
  CHECK_FALSE(d->dimensions.empty());
  REQUIRE(tree.topLevelItemCount() == 1);
  CHECK(tree.topLevelItem(0)->text(0) == "Sheet");
  CHECK(tree.topLevelItem(0)->childCount() == 4);
  CHECK(count_rows(tree) == static_cast<int>(rows_of(w.history().project().document)));
  CHECK(w.shown_drawing() == drawing);
  CHECK(w.drawing_window().isVisible());
  // The dock is in the View menu's list of panels, as every dock is.
  CHECK(w.drawings_dock().toggleViewAction() != nullptr);

  // Deleting a view takes its dimensions; undo brings both back.
  const int all_rows = count_rows(tree);
  const model::DrawingView* front = view_of(*d, model::ViewDirection::Front);
  REQUIRE(front != nullptr);
  const Id front_id = front->id;
  REQUIRE_FALSE(w.delete_drawing_item(front_id).has_errors());
  CHECK(w.history().undo_count() == steps + 2);
  CHECK(w.history().project().document.find_drawing(drawing)->find_view(front_id) == nullptr);
  CHECK(count_rows(tree) < all_rows - 1);
  w.undo();
  CHECK(count_rows(tree) == all_rows);
  CHECK(w.history().project().document.find_drawing(drawing)->find_view(front_id) != nullptr);

  // Renaming the drawing and a view.
  REQUIRE_FALSE(w.rename_drawing_item(drawing, "Bracket").has_errors());
  CHECK(tree.topLevelItem(0)->text(0) == "Bracket");
  REQUIRE_FALSE(w.rename_drawing_item(front_id, "Elevation").has_errors());
  CHECK(tree.topLevelItem(0)->child(0)->text(0).startsWith("Elevation"));
  CHECK(w.rename_drawing_item(Id::random(), "x").has_errors());

  // Deleting the drawing closes its window; undo lists it again.
  REQUIRE_FALSE(w.delete_drawing_item(drawing).has_errors());
  CHECK(tree.topLevelItemCount() == 0);
  CHECK(w.shown_drawing().is_null());
  CHECK_FALSE(w.drawing_window().isVisible());
  w.undo();
  CHECK(tree.topLevelItemCount() == 1);
  CHECK(w.open_drawing(drawing));
  CHECK_FALSE(w.open_drawing(Id::random()));
  settle_drawing(w);
}

TEST_CASE("The dimension dialog offers the view's anchors and commits one step",
          "[app][gui][drawing][dialogs]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket()));
  settle(w);
  const Id drawing = w.new_drawing("Sheet");
  REQUIRE_FALSE(drawing.is_null());
  settle(w);
  const model::Document& doc = w.history().project().document;
  const Id base = doc.find_volume("Base")->id;
  const Id upright = doc.find_volume("Upright")->id;
  const model::DrawingView* front = view_of(*doc.find_drawing(drawing), model::ViewDirection::Front);
  REQUIRE(front != nullptr);
  const Id front_id = front->id;
  const std::size_t dimensions = doc.find_drawing(drawing)->dimensions.size();
  const int rows = count_rows(w.drawings_tree());

  app::DimensionDialog* d = w.add_drawing_dimension(drawing, front_id);
  REQUIRE(d != nullptr);
  CHECK(w.open_dialog() == d);
  CHECK(d->view_box().currentText() == "Front");
  // The front view's horizontal anchors: the origin first, the bracket's
  // boundaries, the flange, and the contents' (disposable) ones after.
  const auto& choices = d->choices();
  REQUIRE_FALSE(choices.empty());
  CHECK(d->from_box().count() == static_cast<int>(choices.size()));
  CHECK(choices.front().anchor.on == AnchorOn::Origin);
  const auto from = choice_index(choices, AnchorOn::VolumeBoundary, base, AnchorPart::Min);
  const auto to = choice_index(choices, AnchorOn::VolumeBoundary, upright, AnchorPart::Max);
  REQUIRE(from);
  REQUIRE(to);
  CHECK(choice_index(choices, AnchorOn::VolumeContents, base, AnchorPart::Max).has_value());
  CHECK(d->to_box().isVisibleTo(d));

  // A diameter needs one anchor: the second box goes, and seen from the
  // front no bolt hole is a circle.
  d->kind_box().setCurrentIndex(2);
  CHECK_FALSE(d->to_box().isVisibleTo(d));
  for (const auto& c : d->choices()) CHECK(c.anchor.part != AnchorPart::Hole);
  d->kind_box().setCurrentIndex(0);
  CHECK(d->to_box().isVisibleTo(d));
  REQUIRE(d->choices().size() == choices.size());

  REQUIRE(d->choose(true, *from));
  REQUIRE(d->choose(false, *to));
  d->offset_box().setValue(-15.0);
  w.flush_preview();
  const auto steps = w.history().undo_count();
  d->commit();
  CHECK(w.open_dialog() == nullptr);
  CHECK(w.history().undo_count() == steps + 1);
  const model::Drawing* after = w.history().project().document.find_drawing(drawing);
  REQUIRE(after->dimensions.size() == dimensions + 1);
  const model::DrawingDimension& added = after->dimensions.back();
  CHECK(added.view == front_id);
  CHECK(added.kind == DimensionKind::Horizontal);
  CHECK(added.from.on == AnchorOn::VolumeBoundary);
  CHECK(added.from.target == base);
  CHECK(added.to.target == upright);
  CHECK(added.offset == -15.0);
  CHECK_FALSE(added.disposable());
  CHECK(count_rows(w.drawings_tree()) == rows + 1);

  // Editing it: the dialog opens on its anchors; a disposable one is
  // listed in the dock as such.
  app::DimensionDialog* e = w.edit_drawing_dimension(drawing, added.id);
  REQUIRE(e != nullptr);
  CHECK(e->value() == added);
  const auto contents_max = choice_index(e->choices(), AnchorOn::VolumeContents, base, AnchorPart::Max);
  REQUIRE(contents_max);
  REQUIRE(e->choose(false, *contents_max));
  w.flush_preview();
  e->commit();
  CHECK(w.open_dialog() == nullptr);
  CHECK(w.history().undo_count() == steps + 2);
  CHECK(w.history().project().document.find_drawing(drawing)->dimensions.back().disposable());
  bool listed_disposable = false;
  for (QTreeWidgetItemIterator it(&w.drawings_tree()); *it != nullptr; ++it) {
    if ((*it)->text(0).contains("(Base contents max)") && (*it)->font(0).italic()) listed_disposable = true;
  }
  CHECK(listed_disposable);

  // Undo, twice: the drawing as New Drawing made it.
  w.undo();
  w.undo();
  CHECK(w.history().project().document.find_drawing(drawing)->dimensions.size() == dimensions);
  CHECK(count_rows(w.drawings_tree()) == rows);

  // A view dialog: OK adds one view.
  app::ViewDialog* v = w.add_drawing_view(drawing);
  REQUIRE(v != nullptr);
  CHECK(v->all_volumes().isChecked());
  v->direction_box().setCurrentIndex(1);  // back
  w.flush_preview();
  v->commit();
  CHECK(w.open_dialog() == nullptr);
  CHECK(w.history().project().document.find_drawing(drawing)->views.size() == 5);
  CHECK(w.history().project().document.find_drawing(drawing)->views.back().direction == model::ViewDirection::Back);
  settle_drawing(w);
}

TEST_CASE("The drawing window draws the sheet after each evaluation", "[app][gui][drawing]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket()));
  settle(w);
  const Id drawing = w.new_drawing("Sheet");
  REQUIRE_FALSE(drawing.is_null());
  settle_drawing(w);
  const auto result = w.drawing_window().result();
  REQUIRE(result != nullptr);
  INFO(samples::codes(result->diagnostics));
  CHECK(result->ok());
  CHECK(w.drawing_view().has_sheet());
  const draw::Sheet& sheet = w.drawing_view().sheet();
  CHECK(sheet.width == 420.0);
  const auto pens = [&](draw::Pen pen) {
    return std::count_if(sheet.paths.begin(), sheet.paths.end(), [&](const draw::SheetPath& p) { return p.pen == pen; });
  };
  CHECK(pens(draw::Pen::Visible) > 0);
  CHECK(pens(draw::Pen::Boundary) > 0);
  CHECK(pens(draw::Pen::Dimension) > 0);
  CHECK_FALSE(sheet.texts.empty());

  // Off screen: white paper with black lines and the boundaries' blue.
  const QImage image = w.drawing_view().render_image(QSize(1200, 860));
  std::size_t white = 0, dark = 0, blue = 0;
  for (int y = 0; y < image.height(); y += 1) {
    for (int x = 0; x < image.width(); x += 1) {
      const QColor c = image.pixelColor(x, y);
      if (c.red() > 250 && c.green() > 250 && c.blue() > 250) ++white;
      if (c.red() < 60 && c.green() < 60 && c.blue() < 60) ++dark;
      if (c.blue() > c.red() + 40 && c.blue() > 120) ++blue;
    }
  }
  INFO("white " << white << ", dark " << dark << ", blue " << blue);
  CHECK(white > 400000);
  CHECK(dark > 2000);
  CHECK(blue > 200);

  // A model edit redraws it: the drawing follows the evaluation.
  const auto shown = w.drawings_shown();
  const auto* width = w.history().project().document.layout.variables().find("width");
  REQUIRE(width != nullptr);
  REQUIRE_FALSE(w.apply(app::set_variable(Id{}, width->id, "width", "80")).has_errors());
  settle_drawing(w);
  CHECK(w.drawings_shown() > shown);
  CHECK(w.drawing_window().result() != result);
}

TEST_CASE("A drawing with disposable dimensions draws them in their own pen", "[app][gui][drawing]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "apps" / "vstudio-cli" / "tests" / "data" / "drawn_ball.vsproj"));
  settle(w);
  const auto& drawings = w.history().project().document.drawings;
  REQUIRE(drawings.size() == 1);
  REQUIRE(w.open_drawing(drawings[0].id));
  settle_drawing(w);
  const auto result = w.drawing_window().result();
  REQUIRE(result != nullptr);
  INFO(samples::codes(result->diagnostics));
  CHECK(result->ok());
  REQUIRE(result->dimensions.size() == 2);
  CHECK_FALSE(result->dimensions[0].disposable);
  CHECK(result->dimensions[1].disposable);
  CHECK(result->dimensions[1].drawn);
  const auto& paths = result->sheet.paths;
  CHECK(std::any_of(paths.begin(), paths.end(), [](const draw::SheetPath& p) { return p.pen == draw::Pen::Disposable; }));
  CHECK(std::any_of(paths.begin(), paths.end(), [](const draw::SheetPath& p) { return p.pen == draw::Pen::Visible; }));
  // The dock marks the disposable one.
  int italic = 0;
  for (QTreeWidgetItemIterator it(&w.drawings_tree()); *it != nullptr; ++it) {
    if ((*it)->font(0).italic()) ++italic;
  }
  CHECK(italic == 1);
}

TEST_CASE("Export Drawing writes SVG, PDF and DXF", "[app][gui][drawing][ship]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket()));
  settle(w);
  const Id drawing = w.new_drawing("Sheet");
  REQUIRE_FALSE(drawing.is_null());
  settle_drawing(w);

  const auto dir = std::filesystem::temp_directory_path() / "vs_app_drawing_export";
  std::filesystem::create_directories(dir);
  for (const char* ext : {".svg", ".pdf", ".dxf"}) {
    const auto path = dir / (std::string("bracket") + ext);
    std::filesystem::remove(path);
    const auto d = export_and_wait(w, drawing, path);
    INFO(ext << ": " << samples::codes(d));
    CHECK_FALSE(d.has_errors());
    REQUIRE(std::filesystem::exists(path));
    const std::string bytes = read(path);
    if (std::string(ext) == ".svg") {
      CHECK((bytes.starts_with("<?xml") || bytes.starts_with("<svg")));
      CHECK(bytes.find("</svg>") != std::string::npos);
    } else if (std::string(ext) == ".pdf") {
      CHECK(bytes.starts_with("%PDF-"));
    } else {
      // A DXF opens with group code 0 and SECTION.
      std::string first, second;
      std::size_t at = 0;
      const auto line = [&] {
        const std::size_t end = bytes.find('\n', at);
        std::string s = bytes.substr(at, end - at);
        at = end == std::string::npos ? bytes.size() : end + 1;
        s.erase(0, s.find_first_not_of(" \t"));
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
      };
      first = line();
      second = line();
      CHECK(first == "0");
      CHECK(second == "SECTION");
    }
  }
  // Another extension, or no such drawing: refused, nothing written.
  CHECK(samples::codes(export_and_wait(w, drawing, dir / "bracket.txt")).find("draw.file.format") != std::string::npos);
  CHECK(samples::codes(export_and_wait(w, Id::random(), dir / "none.svg")).find("app.drawing.missing") !=
        std::string::npos);
  CHECK_FALSE(std::filesystem::exists(dir / "none.svg"));
}
