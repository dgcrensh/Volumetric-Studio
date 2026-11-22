#include "design_table_dialog.hpp"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fstream>
#include <iterator>
#include <sstream>

#include "authoring.hpp"
#include "configurations.hpp"
#include "vs/model/configuration.hpp"

namespace vs::app {

namespace {

QString path_text(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
std::filesystem::path path_of(const QString& s) { return std::filesystem::path(s.toStdWString()); }

core::Diagnostics error(std::string code, std::string message) {
  core::Diagnostics out;
  out.add(core::Diagnostic::error(std::move(code), std::move(message)));
  return out;
}

QString header_tooltip(const model::DesignTableColumn& c) {
  return c.authored.empty() ? QObject::tr("%1\nAuthored: (none)").arg(qstr(c.label))
                            : QObject::tr("%1\nAuthored: %2").arg(qstr(c.label), qstr(c.authored));
}

}  // namespace

DesignTableDialog::DesignTableDialog(const model::Document& document, QWidget* parent)
    : EditDialog(tr("Design Table"), parent), document_(document) {
  setObjectName(QStringLiteral("design_table"));
  grid_ = new QTableWidget(this);
  grid_->setObjectName(QStringLiteral("grid"));
  grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  grid_->horizontalHeader()->setStretchLastSection(true);
  grid_->verticalHeader()->setVisible(false);
  grid_->setMinimumSize(560, 220);
  connect(grid_, &QTableWidget::itemChanged, this, [this] {
    if (!filling_) emit changed();
  });

  auto* buttons = new QHBoxLayout;
  const auto button = [&](const QString& label, const char* name, auto&& fn) {
    auto* b = new QPushButton(label, this);
    b->setObjectName(QString::fromLatin1(name));
    b->setAutoDefault(false);
    connect(b, &QPushButton::clicked, this, fn);
    buttons->addWidget(b);
    return b;
  };
  button(tr("Add row"), "add_row", [this] { add_row(); });
  button(tr("Copy row"), "copy_row", [this] {
    if (grid_->currentRow() >= 0) copy_row(grid_->currentRow());
  });
  button(tr("Remove row"), "remove_row", [this] {
    if (grid_->currentRow() >= 0) remove_row(grid_->currentRow());
  });
  button(tr("Add column…"), "add_column", [this] { add_column_menu(); });
  buttons->addStretch(1);
  button(tr("Import CSV…"), "import_csv", [this] { import_dialog(); });
  button(tr("Export CSV…"), "export_csv", [this] { export_dialog(); });

  auto* box = new QWidget(this);
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(grid_);
  layout->addLayout(buttons);
  form()->addRow(box);

  rebuild_grid(table_for_editing(document_));
  setMinimumWidth(640);
}

DesignTableDialog::~DesignTableDialog() = default;

void DesignTableDialog::rebuild_grid(const model::DesignTable& table) {
  filling_ = true;
  columns_ = table.columns;
  grid_->clear();
  grid_->setColumnCount(static_cast<int>(columns_.size()) + 1);
  grid_->setRowCount(0);
  auto* name_header = new QTableWidgetItem(tr("Configuration"));
  name_header->setToolTip(tr("The configuration's name"));
  grid_->setHorizontalHeaderItem(0, name_header);
  for (std::size_t k = 0; k < columns_.size(); ++k) {
    auto* h = new QTableWidgetItem(qstr(columns_[k].label));
    h->setToolTip(header_tooltip(columns_[k]));
    grid_->setHorizontalHeaderItem(static_cast<int>(k) + 1, h);
  }
  for (const auto& r : table.rows) {
    const int row = grid_->rowCount();
    grid_->insertRow(row);
    auto* name = new QTableWidgetItem(qstr(r.name));
    name->setData(Qt::UserRole, qstr(r.configuration.is_null() ? std::string() : r.configuration.to_string()));
    grid_->setItem(row, 0, name);
    for (std::size_t k = 0; k < columns_.size(); ++k) {
      const std::string cell = k < r.cells.size() ? r.cells[k] : std::string();
      grid_->setItem(row, static_cast<int>(k) + 1, new QTableWidgetItem(qstr(cell)));
    }
  }
  grid_->resizeColumnsToContents();
  filling_ = false;
}

model::DesignTable DesignTableDialog::table() const {
  model::DesignTable t;
  t.columns = columns_;
  for (int row = 0; row < grid_->rowCount(); ++row) {
    model::DesignTableRow r;
    if (const QTableWidgetItem* name = grid_->item(row, 0)) {
      r.name = sstr(name->text().trimmed());
      const std::string id = sstr(name->data(Qt::UserRole).toString());
      if (!id.empty()) {
        if (const auto parsed = Id::parse(id)) r.configuration = *parsed;
      }
    }
    for (std::size_t k = 0; k < columns_.size(); ++k) {
      const QTableWidgetItem* cell = grid_->item(row, static_cast<int>(k) + 1);
      r.cells.push_back(cell != nullptr ? sstr(cell->text().trimmed()) : std::string());
    }
    t.rows.push_back(std::move(r));
  }
  return t;
}

void DesignTableDialog::set_table(const model::DesignTable& table) {
  rebuild_grid(table);
  emit changed();
}

void DesignTableDialog::add_row(const std::string& name) {
  model::DesignTable t = table();
  std::vector<std::string> taken;
  for (const auto& r : t.rows) taken.push_back(r.name);
  model::DesignTableRow r;
  r.name = unique_name(name.empty() ? std::string("Configuration") : name, taken);
  r.cells.assign(columns_.size(), std::string());
  t.rows.push_back(std::move(r));
  set_table(t);
  grid_->setCurrentCell(grid_->rowCount() - 1, 0);
}

void DesignTableDialog::copy_row(int row) {
  model::DesignTable t = table();
  if (row < 0 || row >= static_cast<int>(t.rows.size())) return;
  std::vector<std::string> taken;
  for (const auto& r : t.rows) taken.push_back(r.name);
  model::DesignTableRow copy = t.rows[static_cast<std::size_t>(row)];
  copy.configuration = Id::null();
  copy.name = unique_name(copy.name + " copy", taken);
  t.rows.insert(t.rows.begin() + row + 1, std::move(copy));
  set_table(t);
  grid_->setCurrentCell(row + 1, 0);
}

void DesignTableDialog::remove_row(int row) {
  model::DesignTable t = table();
  if (row < 0 || row >= static_cast<int>(t.rows.size())) return;
  t.rows.erase(t.rows.begin() + row);
  set_table(t);
}

std::vector<model::Drivable> DesignTableDialog::addable() const {
  return addable_columns(document_, table());
}

bool DesignTableDialog::add_column(const Id& target) {
  for (const auto& d : addable()) {
    if (d.target != target) continue;
    // Keep drivable order: the column goes before the first later one.
    model::DesignTable t = table();
    const auto order = model::drivable_targets(document_);
    const auto rank = [&](const Id& id) {
      for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i].target == id) return i;
      }
      return order.size();
    };
    std::size_t at = 0;
    while (at < t.columns.size() && rank(t.columns[at].target) < rank(target)) ++at;
    t.columns.insert(t.columns.begin() + static_cast<std::ptrdiff_t>(at), {d.target, d.label, d.expression});
    for (auto& r : t.rows) r.cells.insert(r.cells.begin() + static_cast<std::ptrdiff_t>(at), std::string());
    set_table(t);
    return true;
  }
  return false;
}

void DesignTableDialog::set_cell(int row, int column, const QString& text) {
  if (row < 0 || row >= grid_->rowCount() || column < 0 || column >= grid_->columnCount()) return;
  if (QTableWidgetItem* item = grid_->item(row, column)) {
    item->setText(text);
  } else {
    grid_->setItem(row, column, new QTableWidgetItem(text));
  }
}

core::Diagnostics DesignTableDialog::import_csv_text(std::string_view text) {
  auto parsed = model::from_csv(document_, text);
  if (!parsed) {
    core::Diagnostics out;
    out.add(parsed.error());
    set_message(qstr(parsed.error().message));
    return out;
  }
  model::DesignTable t = std::move(*parsed);
  // Columns name targets by label: find them, for the headers' tooltips.
  const auto drivable = model::drivable_targets(document_);
  for (auto& c : t.columns) {
    for (const auto& d : drivable) {
      if (d.label == c.label) {
        if (c.target.is_null()) c.target = d.target;
        c.authored = d.expression;
      }
    }
  }
  // Rows keep their configuration by name (apply_design_table() does too).
  for (auto& r : t.rows) {
    if (!r.configuration.is_null()) continue;
    for (const auto& c : document_.configurations) {
      if (c.name == r.name) r.configuration = c.id;
    }
  }
  set_message({});
  set_table(t);
  return {};
}

core::Diagnostics DesignTableDialog::import_csv(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    auto d = error("app.design_table.read", "Can't read " + path.string() + ".");
    set_message(tr("Can't read %1.").arg(path_text(path)));
    return d;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return import_csv_text(text);
}

std::string DesignTableDialog::csv() const { return model::to_csv(table()); }

bool DesignTableDialog::export_csv(const std::filesystem::path& path) const {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << csv();
  return static_cast<bool>(out);
}

void DesignTableDialog::add_column_menu() {
  const auto choices = addable();
  QMenu menu(this);
  if (choices.empty()) {
    menu.addAction(tr("(every drivable number is a column)"))->setEnabled(false);
  }
  for (const auto& d : choices) {
    const Id target = d.target;
    QAction* a = menu.addAction(qstr(d.label), this, [this, target] { add_column(target); });
    a->setToolTip(tr("Authored: %1").arg(qstr(d.expression)));
  }
  auto* sender = findChild<QPushButton*>(QStringLiteral("add_column"));
  menu.exec(sender != nullptr ? sender->mapToGlobal(QPoint(0, sender->height())) : QCursor::pos());
}

void DesignTableDialog::import_dialog() {
  const QString file =
      QFileDialog::getOpenFileName(this, tr("Import design table"), {}, tr("CSV files (*.csv);;All files (*)"));
  if (file.isEmpty()) return;
  import_csv(path_of(file));
}

void DesignTableDialog::export_dialog() {
  const QString file =
      QFileDialog::getSaveFileName(this, tr("Export design table"), {}, tr("CSV files (*.csv);;All files (*)"));
  if (file.isEmpty()) return;
  if (!export_csv(path_of(file))) set_message(tr("Can't write %1.").arg(file));
}

}  // namespace vs::app
