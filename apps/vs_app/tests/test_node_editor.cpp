// The node editor (M15): graph editing rules, node previews, node
// diagnostics on the canvas, the template library, bake and unbake, and the
// two acceptance tests: the M12 templates built from scratch in the editor
// give the same hashes as their saved versions, and previewing any node of
// a 50-node graph shows within a second when its result is cached.

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QTableWidget>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "authoring.hpp"
#include "graph_edit.hpp"
#include "main_window.hpp"
#include "node_editor.hpp"
#include "node_preview.hpp"
#include "samples.hpp"
#include "viewport.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/templates.hpp"
#include "widgets.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;

namespace {

void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      180000));
  QApplication::processEvents();
}

std::filesystem::path bolt_bosses_path() {
  return samples::source_dir() / "libs" / "vs_proc" / "tests" / "data" / "bolt_bosses.vsproj";
}

Id volume_named(const model::Document& doc, const std::string& name) {
  const model::Volume* v = doc.find_volume(name);
  REQUIRE(v != nullptr);
  return v->id;
}

proc::Graph saved_template(const std::string& name) {
  std::ifstream f(samples::source_dir() / "libs" / "vs_proc" / "tests" / "data" / "templates" /
                  (name + ".vgraph.json"));
  std::stringstream ss;
  ss << f.rdbuf();
  auto g = proc::graph_from_json(ss.str());
  REQUIRE(g);
  return std::move(g).value();
}

// A graph with ids replaced by node names: what two graphs built apart
// share when they are the same graph.
std::set<std::string> canonical(const proc::Graph& g) {
  std::set<std::string> out;
  auto name = [&](const Id& id) {
    const proc::Node* n = g.find_node(id);
    return n != nullptr ? n->name : std::string("?");
  };
  for (const auto& p : g.parameters) out.insert("param " + p.name + "=" + std::to_string(p.value));
  for (const auto& n : g.nodes) {
    std::string s = "node " + n.name + ":" + n.type;
    for (const auto& [k, v] : n.settings) s += " " + k + "=" + app::sstr(app::setting_text(v));
    out.insert(s);
  }
  for (const auto& e : g.edges) {
    out.insert("edge " + name(e.from.node) + "." + e.from.port + "->" + name(e.to.node) + "." + e.to.port);
  }
  return out;
}

const eval::VolumeResult& result_of(app::MainWindow& w, const Id& volume) {
  const auto s = w.session().latest();
  REQUIRE(s);
  const eval::VolumeResult* r = s->evaluation.find(volume);
  REQUIRE(r != nullptr);
  return *r;
}

// Builds `saved` in the open graph editor the way a user would: exposed
// parameters in the parameters panel, nodes from the palette, names and
// settings typed into the inspector's node form, wires between ports.
void build_in_editor(app::MainWindow& w, const Id& volume, const proc::Graph& saved) {
  app::NodeEditor& ed = w.node_editor();
  REQUIRE(ed.volume() == volume);
  for (const auto& p : saved.parameters) REQUIRE_FALSE(ed.add_parameter(p.name, p.value).has_errors());

  std::map<Id, Id> ids;  // saved node -> built node
  for (const auto& n : saved.nodes) {
    // The starter graph has the boundary and the output already.
    Id id;
    for (const auto& have : ed.graph().nodes) {
      if (have.type == n.type && (n.type == "input.boundary" || n.type == "output")) id = have.id;
    }
    if (id.is_null()) {
      const auto palette = ed.palette_types();
      REQUIRE(std::find(palette.begin(), palette.end(), n.type) != palette.end());
      REQUIRE_FALSE(ed.add_node(n.type, std::nullopt, &id).has_errors());
    }
    ids[n.id] = id;
    w.select(app::Selection::of_node(id, volume));
    REQUIRE(w.node_form().node() == id);
    if (ed.graph().find_node(id)->name != n.name) {
      w.node_form().edit(".name", app::qstr(n.name));
      REQUIRE(samples::wait_until([&] { return ed.graph().find_node(id)->name == n.name; }, 10000));
    }
    for (const auto& [key, value] : n.settings) {
      const proc::Setting* have = ed.graph().find_node(id)->find_setting(key);
      if (have != nullptr && *have == value) continue;
      REQUIRE(w.node_form().field(key) != nullptr);
      w.node_form().edit(key, app::setting_text(value, ed.context()));
      REQUIRE(samples::wait_until(
          [&] {
            const proc::Setting* s = ed.graph().find_node(id)->find_setting(key);
            return s != nullptr && *s == value;
          },
          10000));
    }
  }
  for (const auto& e : saved.edges) {
    REQUIRE_FALSE(ed.connect_ports({ids.at(e.from.node), e.from.port}, {ids.at(e.to.node), e.to.port}).has_errors());
  }
}

}  // namespace

// --- rules (no window) -----------------------------------------------------------------------

TEST_CASE("Graph edits reject type mismatches and loops but allow unfinished graphs", "[app][graph]") {
  const app::GraphContext none;
  proc::Graph g = app::starter_graph();
  auto c = app::add_node(g, "field.shell", none);
  REQUIRE(c.ok());
  g = c.graph;
  const Id shell = c.node;
  // Unfinished: the shell's field input isn't connected yet.
  CHECK(app::node_marks(g, {}).at(shell).ports == std::vector<std::string>{"field"});
  c = app::add_node(g, "math.add", none);
  g = c.graph;
  const Id add = c.node;
  // A number into a field input: refused, the graph unchanged.
  auto bad = app::connect_ports(g, {add, "value"}, {shell, "field"});
  CHECK_FALSE(bad.ok());
  CHECK(bad.diagnostics.with_code("proc.graph.type_mismatch").size() > 0);
  CHECK(bad.graph == g);
  CHECK_FALSE(app::ports_compatible(g, {add, "value"}, {shell, "field"}));
  // Ports given input first work too.
  const Id boundary = g.nodes.front().id;
  c = app::connect_ports(g, {shell, "field"}, {boundary, "field"});
  REQUIRE(c.ok());
  g = c.graph;
  // A loop: two adds feeding each other.
  c = app::add_node(g, "math.add", none);
  g = c.graph;
  const Id add2 = c.node;
  g = app::connect_ports(g, {add, "value"}, {add2, "a"}).graph;
  auto loop = app::connect_ports(g, {add2, "value"}, {add, "a"});
  CHECK_FALSE(loop.ok());
  CHECK(loop.diagnostics.with_code("proc.graph.loop").size() > 0);
  // Connecting into a taken input replaces the edge.
  c = app::add_node(g, "field.sphere", none);
  g = c.graph;
  const Id ball = c.node;
  const Id output = g.nodes[1].id;
  c = app::connect_ports(g, {ball, "field"}, {output, "contents"});
  REQUIRE(c.ok());
  g = c.graph;
  CHECK(g.edge_into(output, "contents")->from.node == ball);
  std::size_t into = 0;
  for (const auto& e : g.edges) into += e.to == proc::PortRef{output, "contents"} ? 1 : 0;
  CHECK(into == 1);
  // Deleting a node takes its edges with it.
  c = app::remove_nodes(g, {ball});
  REQUIRE(c.ok());
  CHECK(c.graph.edge_into(output, "contents") == nullptr);
  CHECK(proc::check_graph(c.graph).with_code("proc.graph.missing_input").size() > 0);
}

TEST_CASE("Parameters: exposing an input, renaming, and new parameter and surface nodes", "[app][graph]") {
  proc::Graph g = app::starter_graph();
  auto c = app::add_node(g, "field.shell", {});
  g = c.graph;
  const Id shell = c.node;
  REQUIRE(app::set_setting(g, shell, "thickness", proc::Setting(1.5)).ok());
  g = app::set_setting(g, shell, "thickness", proc::Setting(1.5)).graph;
  c = app::expose_input(g, shell, "thickness");
  REQUIRE(c.ok());
  g = c.graph;
  REQUIRE(g.find_parameter("thickness") != nullptr);
  CHECK(g.find_parameter("thickness")->value == 1.5);
  CHECK(g.edge_into(shell, "thickness")->from.node == c.node);
  CHECK(g.find_node(shell)->find_setting("thickness") == nullptr);
  // Exposing it again: it is connected now.
  CHECK_FALSE(app::expose_input(g, shell, "thickness").ok());
  // Renaming a parameter renames what reads it.
  c = app::set_parameter(g, "thickness", "wall", 2.0);
  REQUIRE(c.ok());
  CHECK(c.graph.find_parameter("wall")->value == 2.0);
  const proc::Node* reader = c.graph.find_node(g.edge_into(shell, "thickness")->from.node);
  CHECK(std::get<std::string>(*reader->find_setting("name")) == "wall");
  // Removing a parameter something reads is refused.
  CHECK_FALSE(app::remove_parameter(c.graph, "wall").ok());

  // A parameter node in a graph without parameters brings one.
  auto p = app::add_node(app::starter_graph(), "input.parameter", {});
  REQUIRE(p.ok());
  CHECK(p.graph.parameters.size() == 1);
  // A surface node needs a surface joining the volume.
  auto s = app::add_node(app::starter_graph(), "input.surface", {});
  CHECK_FALSE(s.ok());
  app::GraphContext ctx;
  ctx.surfaces.push_back({Id::random(), "Flange", model::ConnectionType::BoltedFlange});
  s = app::add_node(app::starter_graph(), "input.surface", ctx);
  REQUIRE(s.ok());
  CHECK(std::get<std::string>(*s.graph.find_node(s.node)->find_setting("surface")) == ctx.surfaces[0].id.to_string());
  // Names stay unique.
  proc::Graph two = app::add_node(app::starter_graph(), "field.sphere", {}).graph;
  two = app::add_node(two, "field.sphere", {}).graph;
  CHECK(two.nodes[2].name == "Sphere");
  CHECK(two.nodes[3].name == "Sphere 2");
}

TEST_CASE("Ports are coloured by type, and settings read back as text", "[app][graph]") {
  std::set<std::tuple<float, float, float>> colours;
  for (const auto t : {proc::PortType::Scalar, proc::PortType::Vector, proc::PortType::Field, proc::PortType::Mesh,
                       proc::PortType::Frames, proc::PortType::Curves, proc::PortType::List}) {
    const app::Rgb c = app::port_color(t);
    colours.insert({c.r, c.g, c.b});
  }
  CHECK(colours.size() == 7);
  const app::GraphContext ctx{{{Id::random(), "S", model::ConnectionType::BoltedFlange}}};
  for (const proc::Setting& s : {proc::Setting(2.5), proc::Setting(core::dvec3{1, -2, 0.5}),
                                 proc::Setting(std::string("gyroid")), proc::Setting(ctx.surfaces[0].id.to_string())}) {
    const auto back = app::parse_setting(app::setting_text(s, ctx), s, ctx);
    REQUIRE(back);
    CHECK(*back == s);
  }
  CHECK(app::setting_text(proc::Setting(ctx.surfaces[0].id.to_string()), ctx) == QStringLiteral("S"));
  CHECK_FALSE(app::parse_setting(QStringLiteral("abc"), proc::Setting(1.0)));
  CHECK_FALSE(app::parse_setting(QStringLiteral("1, 2"), proc::Setting(core::dvec3{})));
}

TEST_CASE("Node marks name failed nodes and the inputs they name", "[app][graph]") {
  proc::Graph g = app::starter_graph();
  auto c = app::add_node(g, "math.divide", {});
  g = c.graph;
  const proc::Node& div = *g.find_node(c.node);
  core::Diagnostics d;
  d.add(core::Diagnostic::error(
      "proc.node.infeasible", "'Divide': these inputs can't all be met (b): division by zero",
      {proc::node_subject(div), {core::SubjectKind::Node, Id::derive(div.id, "b"), "Divide.b"}}));
  const auto marks = app::node_marks(g, d);
  REQUIRE(marks.contains(div.id));
  const auto& m = marks.at(div.id);
  CHECK(m.failed());
  CHECK(m.code == "proc.node.infeasible");
  CHECK(m.ports == std::vector<std::string>{"b"});
  REQUIRE(m.messages.size() == 1);
  CHECK(m.messages[0].find("division by zero") != std::string::npos);
  // Nodes nothing is said about have no mark; the starter graph is complete.
  CHECK_FALSE(marks.contains(g.nodes.front().id));
  // Layout: inputs left of what they feed.
  const auto pos = app::layered_layout(g);
  CHECK(pos.at(g.nodes[0].id).x < pos.at(g.nodes[1].id).x);
}

TEST_CASE("Node previews: fields as surfaces or slices, frames as glyphs, meshes as they are", "[app][graph]") {
  // A sphere's samples on a small grid: negative inside.
  proc::Preview field;
  field.type = proc::PortType::Field;
  proc::SampledField s;
  s.grid.origin = {-5, -5, -5};
  s.grid.cell = 1.0;
  s.grid.nx = s.grid.ny = s.grid.nz = 11;
  for (std::uint32_t k = 0; k < 11; ++k) {
    for (std::uint32_t j = 0; j < 11; ++j) {
      for (std::uint32_t i = 0; i < 11; ++i) {
        const auto p = s.grid.point(i, j, k);
        s.values.push_back(static_cast<float>(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) - 3.0));
      }
    }
  }
  field.samples = s;
  mesh::Mesh m;
  m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  m.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  field.mesh = m;
  const Id v = Id::random(), n = Id::random();
  const core::fbox3 box = core::fbox3::from_min_max({-5, -5, -5}, {5, 5, 5});
  const app::Scene surface = app::preview_scene(field, v, n, box);
  REQUIRE(surface.items.size() == 1);
  CHECK(surface.items[0].role == app::ItemRole::NodePreview);
  CHECK(surface.items[0].mesh.triangle_count() == 4);
  CHECK(surface.items[0].mesh.colors.empty());

  app::NodePreviewOptions slice;
  slice.field = app::FieldView::Slice;
  const app::Scene cut = app::preview_scene(field, v, n, box, slice);
  REQUIRE(cut.items.size() == 1);
  const auto& sm = cut.items[0].mesh;
  CHECK(sm.positions.size() == 121);
  CHECK(sm.colors.size() == 121);
  CHECK(sm.triangle_count() == 200);
  CHECK_FALSE(sm.edges.empty());  // the zero contour
  // The middle of the slice is inside (bluish), the corner outside (reddish).
  const auto& mid = sm.colors[5 * 11 + 5];
  const auto& corner = sm.colors[0];
  CHECK(mid.b > mid.r);
  CHECK(corner.r > corner.b);

  proc::Preview frames;
  frames.type = proc::PortType::Frames;
  frames.points = {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}};
  const app::Scene glyphs = app::preview_scene(frames, v, n, box);
  REQUIRE(glyphs.items.size() == 1);
  CHECK(glyphs.items[0].mesh.triangle_count() == 3 * 8);

  proc::Preview number;
  number.type = proc::PortType::Scalar;
  CHECK(app::preview_scene(number, v, n, box).items.empty());
}

TEST_CASE("Graph commands keep a volume's graph inside its own inputs", "[app][graph]") {
  model::Project p = samples::load("bolt_bosses.vsproj");
  const Id bosses = volume_named(p.document, "Bosses");
  const Id q = volume_named(p.document, "Q");
  app::History h(p);
  auto g = app::graph_of(h.project().document, bosses);
  REQUIRE(g);
  // A surface that doesn't join the volume: a fresh one between nothing.
  proc::Graph reading = *g;
  for (auto& n : reading.nodes) {
    if (n.type == "input.surface") n.settings["surface"] = proc::Setting(Id::random().to_string());
  }
  const auto refused = h.apply(app::set_graph(bosses, reading, "Read elsewhere"));
  CHECK(refused.has_errors());
  CHECK(h.undo_count() == 0);
  // A parametric volume can't take a graph.
  CHECK(h.apply(app::make_procedural(q, app::starter_graph())).has_errors());
  // An unfinished graph is fine.
  proc::Graph unfinished = *g;
  unfinished.edges.clear();
  CHECK_FALSE(h.apply(app::set_graph(bosses, unfinished, "Disconnect all")).has_errors());
  CHECK(h.undo());
  CHECK(app::graph_of(h.project().document, bosses) == g);
}

// --- the window -------------------------------------------------------------------------------

TEST_CASE("The node editor edits a volume's graph with undo, refusals and diagnostics on the canvas",
          "[app][gui][graph]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bolt_bosses_path()));
  settle(w);
  const Id bosses = volume_named(w.history().project().document, "Bosses");
  REQUIRE(w.edit_graph(bosses));
  app::NodeEditor& ed = w.node_editor();
  CHECK(ed.volume() == bosses);
  CHECK(ed.graph().nodes.size() == 15);
  CHECK(ed.marks().empty());
  CHECK(ed.parameters_table().rowCount() == 3);
  const auto before = model::content_hash(w.history().project());

  // The palette's search.
  ed.set_palette_filter(QStringLiteral("divide"));
  const auto shown = ed.palette_types();
  CHECK(std::find(shown.begin(), shown.end(), "math.divide") != shown.end());
  CHECK(std::find(shown.begin(), shown.end(), "field.sphere") == shown.end());
  ed.set_palette_filter({});
  CHECK(ed.palette_types().size() == proc::library().types().size());

  // A wire between different types is refused, with the reason.
  Id divide;
  REQUIRE_FALSE(ed.add_node("math.divide", QPointF(0, 900), &divide).has_errors());
  const Id boss = ed.graph().find_node(std::string_view("Boss"))->id;
  const auto refused = ed.connect_ports({divide, "value"}, {ed.graph().find_node(std::string_view("Skin"))->id, "field"});
  CHECK(refused.has_errors());
  CHECK(ed.message().contains(QStringLiteral("takes a field")));

  // b = 0 through the inspector, then the divide feeds the boss's height:
  // the generation stops there, and the canvas says why and where.
  w.select(app::Selection::of_node(divide, bosses));
  w.node_form().edit("b", QStringLiteral("0"));
  REQUIRE(samples::wait_until(
      [&] {
        const proc::Setting* s = ed.graph().find_node(divide)->find_setting("b");
        return s != nullptr && *s == proc::Setting(0.0);
      },
      10000));
  REQUIRE_FALSE(ed.connect_ports({divide, "value"}, {boss, "height"}).has_errors());
  settle(w);
  REQUIRE(ed.marks().contains(divide));
  const app::NodeMark& mark = ed.marks().at(divide);
  CHECK(mark.failed());
  CHECK(mark.code == "proc.node.infeasible");
  CHECK(mark.ports == std::vector<std::string>{"b"});
  CHECK(result_of(w, bosses).status == model::VolumeStatus::Failed);
  // The feature list says so too.
  bool listed = false;
  for (int i = 0; i < w.feature_tree().topLevelItemCount(); ++i) {
    const auto* item = w.feature_tree().topLevelItem(i);
    if (item->text(0) == QStringLiteral("Divide")) listed = item->text(2) == QStringLiteral("infeasible");
  }
  CHECK(listed);

  // Exposing an input from the inspector.
  w.select(app::Selection::of_node(boss, bosses));
  CHECK(w.node_form().field("radius") == nullptr);  // connected: nothing to type
  // Undo everything: the project comes back exactly.
  while (w.history().can_undo()) w.undo();
  CHECK(model::content_hash(w.history().project()) == before);
  CHECK(ed.graph().nodes.size() == 15);
  CHECK(ed.graph().find_node(divide) == nullptr);
  settle(w);
  CHECK(ed.marks().empty());
  CHECK(result_of(w, bosses).status == model::VolumeStatus::Current);
}

TEST_CASE("Selecting a node previews it in the viewport, as a surface or a slice", "[app][gui][graph]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bolt_bosses_path()));
  settle(w);
  const Id bosses = volume_named(w.history().project().document, "Bosses");
  REQUIRE(w.edit_graph(bosses));
  app::NodeEditor& ed = w.node_editor();
  auto preview = [&](const std::string& name, const std::string& port = {}) {
    const proc::Node* n = ed.graph().find_node(std::string_view(name));
    REQUIRE(n != nullptr);
    int shown = 0;
    auto conn = QObject::connect(&w, &app::MainWindow::nodePreviewShown, [&] { ++shown; });
    if (port.empty()) {
      w.select(app::Selection::of_node(n->id, bosses));
    } else {
      w.preview_node(n->id, port);
    }
    REQUIRE(samples::wait_until([&] { return shown > 0 && w.last_node_preview()->node == n->id; }, 30000));
    QObject::disconnect(conn);
    return w.last_node_preview();
  };

  const auto skin = preview("Skin");
  CHECK(skin->ok());
  CHECK(skin->type == proc::PortType::Field);
  CHECK(skin->from_cache);  // the volume generated through it
  REQUIRE(skin->scene.items.size() == 1);
  CHECK(skin->scene.items[0].mesh.triangle_count() > 100);
  CHECK(ed.previewed_node() == ed.graph().find_node(std::string_view("Skin"))->id);
  if (w.viewport().has_gpu()) {
    CHECK(w.viewport().overlay_items() == 1);
    CHECK(w.viewport().overlay_volume() == bosses);
    CHECK(w.viewport().shown_items(app::ItemRole::Contents) == 1);  // Q's; Bosses' is hidden
  }

  // The flange's bolt holes: frames, drawn as glyphs.
  const auto holes = preview("Flange", "holes");
  CHECK(holes->type == proc::PortType::Frames);
  CHECK(holes->port == "holes");
  REQUIRE(holes->scene.items.size() == 1);
  CHECK(holes->scene.items[0].mesh.triangle_count() % 8 == 0);
  CHECK(holes->scene.items[0].mesh.triangle_count() > 0);

  // A number: text only.
  const auto count = preview("Holes");
  CHECK(count->type == proc::PortType::Scalar);
  CHECK(count->scene.items.empty());
  CHECK_FALSE(count->text.empty());

  // A field as a slice.
  auto* combo = ed.findChild<QComboBox*>();
  REQUIRE(combo != nullptr);
  combo->setCurrentIndex(1);
  const auto sliced = preview("Skin");
  REQUIRE(sliced->scene.items.size() == 1);
  CHECK_FALSE(sliced->scene.items[0].mesh.colors.empty());

  // Selecting the volume ends the preview.
  w.select(app::Selection::of_volume(bosses));
  CHECK(w.viewport().overlay_items() == 0);
  CHECK(ed.previewed_node().is_null());
}

TEST_CASE("The template library attaches a template; bake then unbake gives the project back", "[app][gui][graph]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bolt_bosses_path()));
  settle(w);
  const Id bosses = volume_named(w.history().project().document, "Bosses");
  REQUIRE_FALSE(w.apply(app::make_placeholder(bosses)).has_errors());
  settle(w);
  app::TemplateDialog* d = w.attach_template(bosses);
  REQUIRE(d != nullptr);
  d->choose("gyroid_infill");
  d->set_cell_size(1.0);
  CHECK(d->complete());
  w.flush_preview();
  CHECK(w.previewing());
  d->commit();
  CHECK(w.open_dialog() == nullptr);
  settle(w);
  auto g = app::graph_of(w.history().project().document, bosses);
  REQUIRE(g);
  proc::TemplateOptions o;
  o.cell_size = 1.0;
  CHECK(proc::to_json(*g) == proc::to_json(proc::make_template("gyroid_infill", o).value()));
  CHECK(w.graph_volume() == bosses);  // opened in the editor
  CHECK(result_of(w, bosses).status == model::VolumeStatus::Current);

  const auto before = model::content_hash(w.history().project());
  REQUIRE_FALSE(w.bake(bosses).has_errors());
  CHECK(proc::is_baked(*w.history().project().document.find_volume(bosses)));
  CHECK(w.graph_volume().is_null());       // nothing left to edit
  CHECK_FALSE(w.edit_graph(bosses));       // until unbaked
  settle(w);
  CHECK(result_of(w, bosses).status == model::VolumeStatus::Current);
  REQUIRE_FALSE(w.unbake(bosses).has_errors());
  CHECK(model::content_hash(w.history().project()) == before);
  settle(w);
  CHECK(result_of(w, bosses).status == model::VolumeStatus::Current);

  // bolt_bosses needs a bolted flange: the dialog offers the one joining.
  app::TemplateDialog* b = w.attach_template(bosses);
  REQUIRE(b != nullptr);
  b->choose("bolt_bosses");
  CHECK(b->complete());
  CHECK(proc::to_json(*b->graph()) == proc::to_json(saved_template("bolt_bosses")));
  b->reject();
}

TEST_CASE("The M12 templates built from scratch in the editor match their saved versions",
          "[app][gui][graph][acceptance]") {
  for (const std::string name : {"gyroid_infill", "graded_lattice", "bolt_bosses"}) {
    DYNAMIC_SECTION(name) {
      app::MainWindow w;
      w.show();
      REQUIRE(w.open_project(bolt_bosses_path()));
      settle(w);
      const Id bosses = volume_named(w.history().project().document, "Bosses");
      const proc::Graph saved = saved_template(name);

      // From scratch: an empty volume, filled by procedure, built node by node.
      REQUIRE_FALSE(w.apply(app::make_placeholder(bosses)).has_errors());
      REQUIRE(w.edit_graph(bosses));
      CHECK(w.node_editor().graph().nodes.size() == 2);  // the starter graph
      build_in_editor(w, bosses, saved);
      settle(w);
      const proc::Graph built = *app::graph_of(w.history().project().document, bosses);
      CHECK(canonical(built) == canonical(saved));
      CHECK(proc::check_graph(built).empty());
      const eval::VolumeResult built_result = result_of(w, bosses);
      REQUIRE(built_result.status == model::VolumeStatus::Current);
      REQUIRE(built_result.contents);
      const core::Hash256 built_hash = built_result.contents->output_hash;
      const auto built_outputs = built_result.contents->outputs;

      // The saved version, attached from the template library.
      app::TemplateDialog* d = w.attach_template(bosses);
      REQUIRE(d != nullptr);
      d->choose(name);
      d->set_cell_size(0.5);
      REQUIRE(d->graph());
      CHECK(proc::to_json(*d->graph()) == proc::to_json(saved));
      d->commit();
      settle(w);
      const eval::VolumeResult& saved_result = result_of(w, bosses);
      REQUIRE(saved_result.status == model::VolumeStatus::Current);
      CHECK(saved_result.contents->output_hash == built_hash);
      REQUIRE(saved_result.contents->outputs.size() == built_outputs.size());
      for (std::size_t i = 0; i < built_outputs.size(); ++i) {
        CHECK(saved_result.contents->outputs[i].value == built_outputs[i].value);
      }
    }
  }
}

namespace {

// A 50-node graph on the bolt_bosses box: parameters, maths, vectors,
// fields, frames, meshes and conversions, every node evaluated by the
// volume's generation.
proc::Graph fifty_nodes() {
  proc::Graph g;
  g.parameters = {{"wall", 1.2}, {"cell", 8.0}, {"thick", 1.6}};
  using S = proc::Setting;
  auto add = [&](const std::string& type, const std::string& name,
                 std::map<std::string, proc::Setting, std::less<>> settings = {}) {
    return g.add(type, name, std::move(settings));
  };
  auto link = [&](const Id& a, const std::string& pa, const Id& b, const std::string& pb) { g.link(a, pa, b, pb); };
  const Id wall = add("input.parameter", "wall", {{"name", S(std::string("wall"))}});
  const Id cell = add("input.parameter", "cell", {{"name", S(std::string("cell"))}});
  const Id thick = add("input.parameter", "thick", {{"name", S(std::string("thick"))}});
  const Id half = add("math.multiply", "Half cell", {{"b", S(0.5)}});
  link(cell, "value", half, "a");
  const Id slope = add("math.divide", "Slope", {{"b", S(10.0)}});
  link(thick, "value", slope, "a");
  const Id neg = add("math.negate", "Inward");
  link(slope, "value", neg, "a");
  const Id clamp = add("math.clamp", "Thickness", {{"min", S(0.4)}, {"max", S(2.0)}});
  link(thick, "value", clamp, "value");
  const Id centre = add("vector.compose", "Centre", {{"x", S(20.0)}, {"y", S(15.0)}, {"z", S(10.0)}});
  const Id spacing = add("vector.compose", "Spacing");
  link(half, "value", spacing, "x");
  link(half, "value", spacing, "y");
  link(half, "value", spacing, "z");
  const Id diagonal = add("vector.length", "Diagonal");
  link(spacing, "vector", diagonal, "vector");
  const Id range = add("list.range", "Steps", {{"count", S(5.0)}});
  link(half, "value", range, "step");
  const Id sum = add("list.sum", "Sum of steps");
  link(range, "list", sum, "list");
  const Id boundary = add("input.boundary", "Boundary");
  const Id skin = add("field.shell", "Skin");
  link(boundary, "field", skin, "field");
  link(wall, "value", skin, "thickness");
  const Id depth = add("query.distance_to_boundary", "Depth");
  const Id scaled = add("field.scale", "Depth scaled");
  link(depth, "distance", scaled, "field");
  link(neg, "value", scaled, "factor");
  const Id base = add("field.constant", "Thick");
  link(thick, "value", base, "value");
  const Id grade = add("field.add", "Grade");
  link(base, "field", grade, "a");
  link(scaled, "field", grade, "b");
  const Id graded = add("field.clamp", "Grade clamped", {{"min", S(0.4)}});
  link(grade, "field", graded, "field");
  link(clamp, "value", graded, "max");
  const Id gyroid = add("field.tpms", "Gyroid", {{"surface", S(std::string("gyroid"))}});
  link(cell, "value", gyroid, "cell_size");
  link(graded, "field", gyroid, "grade");
  const Id lattice = add("field.lattice", "Lattice", {{"cell", S(std::string("octet"))}});
  link(cell, "value", lattice, "cell_size");
  link(wall, "value", lattice, "thickness");
  const Id sphere = add("field.sphere", "Ball", {{"radius", S(8.0)}});
  link(centre, "vector", sphere, "centre");
  const Id box = add("field.box", "Block", {{"size", S(core::dvec3{14, 14, 8})}});
  link(centre, "vector", box, "centre");
  const Id cyl = add("field.cylinder", "Bore", {{"radius", S(3.0)}, {"height", S(30.0)}});
  link(centre, "vector", cyl, "centre");
  const Id blob = add("field.smooth_union", "Blob");
  link(sphere, "field", blob, "a");
  link(box, "field", blob, "b");
  const Id bored = add("field.difference", "Bored");
  link(blob, "field", bored, "a");
  link(cyl, "field", bored, "b");
  const Id core = add("field.offset", "Core", {{"distance", S(-0.5)}});
  link(bored, "field", core, "field");
  const Id infill = add("field.union", "Infill");
  link(gyroid, "field", infill, "a");
  link(lattice, "field", infill, "b");
  const Id cut = add("field.difference", "Infill less core");
  link(infill, "field", cut, "a");
  link(core, "field", cut, "b");
  const Id point = add("frames.point", "Middle");
  link(centre, "vector", point, "position");
  const Id grid = add("pattern.grid", "Grid", {{"spacing", S(core::dvec3{10, 10, 10})}});
  link(point, "frames", grid, "frames");
  const Id ring = add("pattern.circular", "Ring", {{"count", S(6.0)}});
  link(point, "frames", ring, "frames");
  const Id merged = add("frames.merge", "All frames");
  link(grid, "frames", merged, "a");
  link(ring, "frames", merged, "b");
  const Id count = add("frames.count", "Count");
  link(merged, "frames", count, "frames");
  const Id publish = add("output.publish", "Publish count", {{"name", S(std::string("pegs"))}});
  link(count, "count", publish, "value");
  const Id peg = add("mesh.cylinder", "Peg", {{"radius", S(1.5)}, {"height", S(6.0)}, {"segments", S(16.0)}});
  const Id pegs = add("mesh.instances", "Pegs");
  link(peg, "mesh", pegs, "template");
  link(grid, "frames", pegs, "frames");
  const Id slab = add("mesh.box", "Slab", {{"size", S(core::dvec3{12, 12, 6})}});
  link(centre, "vector", slab, "centre");
  const Id joined = add("mesh.union", "Joined");
  link(slab, "mesh", joined, "a");
  link(pegs, "mesh", joined, "b");
  const Id as_field = add("convert.mesh_to_field", "Joined field");
  link(joined, "mesh", as_field, "mesh");
  const Id measure = add("mesh.measure", "Measure");
  link(joined, "mesh", measure, "mesh");
  const Id publish2 = add("output.publish", "Publish volume", {{"name", S(std::string("peg_volume"))}});
  link(measure, "volume", publish2, "value");
  const Id ball = add("field.sphere", "Bead", {{"radius", S(1.5)}});
  const Id beads = add("field.instances", "Beads", {{"radius", S(2.0)}});
  link(ball, "field", beads, "template");
  link(ring, "frames", beads, "frames");
  const Id parts = add("field.union", "Parts");
  link(as_field, "field", parts, "a");
  link(beads, "field", parts, "b");
  const Id inside = add("field.intersection", "Infill inside");
  link(cut, "field", inside, "a");
  link(boundary, "field", inside, "b");
  const Id extras = add("field.intersection", "Parts inside");
  link(parts, "field", extras, "a");
  link(boundary, "field", extras, "b");
  const Id all1 = add("field.union", "Skin and infill");
  link(skin, "field", all1, "a");
  link(inside, "field", all1, "b");
  const Id all2 = add("field.smooth_union", "With parts", {{"radius", S(0.5)}});
  link(all1, "field", all2, "a");
  link(extras, "field", all2, "b");
  const Id output = add("output", "Contents", {{"cell_size", S(1.0)}});
  link(all2, "field", output, "contents");
  return g;
}

}  // namespace

TEST_CASE("Previewing any node of a 50-node graph shows within a second when its result is cached",
          "[app][gui][graph][acceptance]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bolt_bosses_path()));
  settle(w);
  const Id bosses = volume_named(w.history().project().document, "Bosses");
  const proc::Graph g = fifty_nodes();
  REQUIRE(g.nodes.size() >= 50);
  REQUIRE(proc::check_graph(g).empty());
  REQUIRE_FALSE(w.apply(app::set_graph(bosses, g, "Fifty nodes")).has_errors());
  settle(w);  // generates the volume: every node's outputs are cached
  REQUIRE(result_of(w, bosses).status == model::VolumeStatus::Current);
  REQUIRE(w.edit_graph(bosses));

  double worst = 0.0;
  std::string worst_node;
  for (const auto& n : g.nodes) {
    int shown = 0;
    auto conn = QObject::connect(&w, &app::MainWindow::nodePreviewShown, [&] { ++shown; });
    QElapsedTimer t;
    t.start();
    w.select(app::Selection::of_node(n.id, bosses));
    REQUIRE(samples::wait_until([&] { return shown > 0 && w.last_node_preview()->node == n.id; }, 30000));
    const double ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
    QObject::disconnect(conn);
    const auto r = w.last_node_preview();
    INFO(n.name << " (" << n.type << "): " << ms << " ms, " << r->text);
    CHECK(r->ok());
    const proc::NodeType* type = proc::library().find(n.type);
    // Nodes reading the volume's inputs are never cached (cheap); every
    // other node comes from the cache the generation filled.
    if (type->cached && !type->outputs.empty() && n.type.rfind("scatter.", 0) != 0) CHECK(r->from_cache);
    CHECK(ms < 1000.0);
    if (ms > worst) {
      worst = ms;
      worst_node = n.name;
    }
  }
  WARN("slowest node preview: " << worst_node << ", " << worst << " ms of " << g.nodes.size() << " nodes");
}
