#pragma once

// The node editor (M15): build and inspect a procedural volume's graph.
//
//   NodeEditor     the Graph panel: a searchable node palette, the canvas,
//                  the graph's exposed parameters and a line saying what
//                  the previewed node holds
//   NodeForm       the selected node's settings, shown in the inspector:
//                  its name, the values of its unconnected number and
//                  vector inputs, and its properties (choices, the
//                  surface it reads, the parameter it reads, ...)
//   TemplateDialog the template library: attach a starter template to a
//                  volume (previewed live like every authoring dialog)
//
// The canvas draws nodes with typed, colour-coded ports (graph_edit.hpp's
// port_color()) and edges in their type's colour. Dragging from a port
// draws a wire; while it is drawn only ports of the same type light up,
// and dropping it on one connects them (replacing what fed that input).
// Nodes are added from the palette (double-click, or drag onto the
// canvas) or the canvas's context menu, and deleted with Delete.
// Failed and infeasible nodes are outlined red, with the message under
// them and the inputs it names ringed; nodes with an input not connected
// yet are outlined amber.
//
// Every change is a graph_edit.hpp function turned into one undoable
// Command (set_graph()) and handed to the owner's apply function, so the
// editor never changes a project itself: it shows whatever the History
// holds, and undo puts the graph back exactly. Node positions aren't part
// of the graph (decision 0015): the editor keeps them per node id while the
// app runs and lays out nodes it hasn't seen in columns by depth.

#include <QDialog>
#include <QGraphicsView>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "graph_edit.hpp"
#include "history.hpp"
#include "node_preview.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/graph.hpp"
#include "widgets.hpp"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace vs::app {

class GraphScene;
class NodeItem;

class NodeEditor : public QWidget {
  Q_OBJECT

 public:
  using ApplyFn = std::function<core::Diagnostics(const Command&)>;

  explicit NodeEditor(QWidget* parent = nullptr);
  ~NodeEditor() override;

  // Where changes go (the main window's apply()).
  void set_apply(ApplyFn apply) { apply_ = std::move(apply); }

  // Shows `volume`'s graph as `project` holds it, marked with what
  // `evaluation` (may be null) said about its nodes. Keeps the selection
  // and the view.
  void show_graph(const model::Project& project, const eval::Evaluation* evaluation, const Id& volume);
  void clear();
  [[nodiscard]] const Id& volume() const { return volume_; }
  [[nodiscard]] const proc::Graph& graph() const { return graph_; }
  [[nodiscard]] const GraphContext& context() const { return context_; }

  // --- changes. Each applies one undoable change; a refused one leaves the
  // graph as it was, with the reason on the editor's message line.
  // Adds a node of `type` at `at` (scene coordinates; nullopt: below the
  // others); `added` gets its id.
  core::Diagnostics add_node(const std::string& type, std::optional<QPointF> at = std::nullopt, Id* added = nullptr);
  core::Diagnostics connect_ports(const proc::PortRef& a, const proc::PortRef& b);
  core::Diagnostics disconnect(const proc::Edge& edge);
  core::Diagnostics remove_nodes(const std::vector<Id>& nodes);
  core::Diagnostics rename_node(const Id& node, const std::string& name);
  core::Diagnostics set_setting(const Id& node, const std::string& key, std::optional<proc::Setting> value);
  // The same from a NodeForm's text, read as the setting's kind (empty
  // text puts an input back to its default).
  core::Diagnostics set_setting_text(const Id& node, const std::string& key, const QString& text);
  core::Diagnostics add_parameter(const std::string& name, double value);
  core::Diagnostics set_parameter(const std::string& name, const std::string& new_name, double value);
  core::Diagnostics remove_parameter(const std::string& name);
  core::Diagnostics expose_input(const Id& node, const std::string& port, Id* reader = nullptr);
  // Removes the selected nodes and edges (the Delete key).
  void delete_selection();

  // --- selection, layout, marks
  void select_node(const Id& node);
  [[nodiscard]] Id selected_node() const;
  [[nodiscard]] std::optional<QPointF> node_position(const Id& node) const;
  void move_node(const Id& node, QPointF at);
  void auto_layout();
  [[nodiscard]] const std::map<Id, NodeMark>& marks() const { return marks_; }
  [[nodiscard]] QString message() const;

  // --- palette
  void set_palette_filter(const QString& text);
  // The node types the palette lists now, in order.
  [[nodiscard]] std::vector<std::string> palette_types() const;
  [[nodiscard]] QLineEdit& palette_search() { return *search_; }

  // --- preview
  // Shows a node preview's text and diagnostics (the viewport draws its
  // scene); a result for another graph or a node no longer there is ignored.
  void show_preview(const NodePreviewResult& result);
  void clear_preview();
  [[nodiscard]] const Id& previewed_node() const { return previewed_; }
  [[nodiscard]] QString preview_text() const;
  [[nodiscard]] NodePreviewOptions preview_options() const;

  [[nodiscard]] QGraphicsView& canvas() { return *view_; }
  [[nodiscard]] QTableWidget& parameters_table() { return *parameters_; }

 signals:
  // The user picked a node on the canvas (null: none).
  void nodeSelected(vs::core::Id volume, vs::core::Id node);
  // Preview this node's output `port` (empty: the first drawable one).
  void previewRequested(vs::core::Id node, QString port);
  // The field view (surface or slice, slice height) changed.
  void previewOptionsChanged();
  void templatesRequested();
  void bakeRequested();
  void closeRequested();

 private:
  friend class GraphScene;
  friend class NodeItem;
  core::Diagnostics commit(const GraphChange& change);
  void rebuild_scene();
  void rebuild_parameters();
  void rebuild_palette();
  void show_message(const QString& text, bool error);
  void node_context_menu(const Id& node, const QPoint& screen);
  void canvas_context_menu(const QPointF& at, const QPoint& screen);
  QPointF free_position() const;

  ApplyFn apply_;
  Id volume_;
  proc::Graph graph_;
  GraphContext context_;
  core::Diagnostics diagnostics_;          // check and evaluation
  core::Diagnostics preview_diagnostics_;  // the previewed node's inspection
  std::map<Id, NodeMark> marks_;
  std::map<Id, QPointF> positions_;        // kept across rebuilds, undo included
  Id previewed_;
  QString preview_text_;
  bool rebuilding_ = false;

  GraphScene* scene_ = nullptr;
  QGraphicsView* view_ = nullptr;
  QLineEdit* search_ = nullptr;
  QTreeWidget* palette_ = nullptr;
  QTableWidget* parameters_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* message_ = nullptr;
  QLabel* preview_label_ = nullptr;
  QComboBox* field_view_ = nullptr;
  class QSlider* slice_ = nullptr;
};

// The selected node's settings (the inspector shows it for a node).
class NodeForm : public QWidget {
  Q_OBJECT

 public:
  explicit NodeForm(QWidget* parent = nullptr);
  // Shows `node` of `graph`; a null node clears the form.
  void show_node(const proc::Graph& graph, const Id& node, const GraphContext& context);
  [[nodiscard]] const Id& node() const { return node_; }
  // The editor for a setting (an input's or a property's), by key: a
  // QLineEdit for numbers, names and vectors ("x, y, z"), a QComboBox for
  // choices, surfaces and parameters; ".name" is the node's name. Null
  // when the node has none.
  [[nodiscard]] QWidget* field(const std::string& key) const;
  // Sets a field's text (or picks a combo entry by text) and commits it,
  // as typing and pressing Enter does.
  void edit(const std::string& key, const QString& text);

 signals:
  void settingEdited(vs::core::Id node, QString key, QString text);
  void renamed(vs::core::Id node, QString name);
  void exposeRequested(vs::core::Id node, QString port);

 private:
  // Emits settingEdited() after the current event, so the form may be
  // rebuilt by the change it asks for.
  void later(const QString& key, const QString& text);
  Id node_;
  proc::Graph shown_;  // the graph the form was built from
  std::size_t surfaces_ = 0;
  QFormLayout* form_ = nullptr;
  QLabel* header_ = nullptr;
  std::map<std::string, QWidget*> fields_;
};

// The text a setting is shown as in a NodeForm, and back: numbers, "x, y,
// z" vectors and strings (surfaces by name). nullopt when it doesn't parse.
QString setting_text(const proc::Setting& value, const GraphContext& context = {});
std::optional<proc::Setting> parse_setting(const QString& text, const proc::Setting& like,
                                           const GraphContext& context = {});

// The template library (proc::templates()) as an authoring dialog.
class TemplateDialog : public EditDialog {
  Q_OBJECT

 public:
  TemplateDialog(const model::Document& document, const Id& volume, QWidget* parent = nullptr);
  // The chosen template's graph, ready for the volume (nullopt: incomplete).
  [[nodiscard]] std::optional<proc::Graph> graph() const;
  [[nodiscard]] std::string template_name() const;
  void choose(const std::string& name);
  void set_cell_size(double mm);
  [[nodiscard]] bool complete() const override;

 private:
  void update_doc();
  QListWidget* list_ = nullptr;
  QLabel* doc_ = nullptr;
  QLineEdit* cell_ = nullptr;
  QComboBox* surface_ = nullptr;
  GraphContext context_;
};

}  // namespace vs::app
