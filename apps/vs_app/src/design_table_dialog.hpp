#pragma once

// Model > Design Table… (L5, decision 0037): every configuration at once,
// as a grid.
//
// Rows are configurations (the first column is the name), columns the
// numbers they drive (`width`, `Base.thickness`, `Housing/w`); a header's
// tooltip shows the authored expression, an empty cell keeps it. Add row,
// Copy row and Remove row edit the rows; Add column offers the drivable
// numbers that aren't columns yet (addable_columns()); Import CSV and
// Export CSV read and write the table (model::from_csv() / to_csv()).
//
// Like every authoring dialog it is modeless: the main window previews
// each change as set_design_table() and OK commits it as one undo step; a
// table the document refuses keeps the dialog open with the reason.

#include <filesystem>
#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/model/design_table.hpp"
#include "vs/model/document.hpp"
#include "widgets.hpp"

class QTableWidget;

namespace vs::app {

class DesignTableDialog : public EditDialog {
  Q_OBJECT

 public:
  explicit DesignTableDialog(const model::Document& document, QWidget* parent = nullptr);
  ~DesignTableDialog() override;

  // The grid as a table (rows in grid order), and back.
  [[nodiscard]] model::DesignTable table() const;
  void set_table(const model::DesignTable& table);

  [[nodiscard]] QTableWidget& grid() { return *grid_; }

  // The buttons' actions (tests and scripts drive these).
  void add_row(const std::string& name = {});
  // A copy of row `row` below it, named "<name> copy" (made unique).
  void copy_row(int row);
  void remove_row(int row);
  // Adds the drivable number `target` as a column; false when it isn't one
  // or is already a column.
  bool add_column(const Id& target);
  // The column labels Add column offers now.
  [[nodiscard]] std::vector<model::Drivable> addable() const;
  // Sets a cell (column 0 is the name, 1.. the driven numbers).
  void set_cell(int row, int column, const QString& text);

  // Replaces the grid with a CSV table; errors leave it as it was.
  core::Diagnostics import_csv_text(std::string_view text);
  core::Diagnostics import_csv(const std::filesystem::path& path);
  [[nodiscard]] std::string csv() const;
  bool export_csv(const std::filesystem::path& path) const;

 private:
  void rebuild_grid(const model::DesignTable& table);
  void add_column_menu();
  void import_dialog();
  void export_dialog();

  model::Document document_;  // the authored document the dialog opened on
  std::vector<model::DesignTableColumn> columns_;
  QTableWidget* grid_ = nullptr;
  bool filling_ = false;
};

}  // namespace vs::app
