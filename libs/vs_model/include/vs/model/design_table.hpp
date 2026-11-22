#pragma once

// Design tables (L5, decision 0037): every configuration at once, as a
// table a spreadsheet can author.
//
// One row per configuration, one column per driven number (configuration.hpp
// for what may be driven, and the labels that name columns). A cell holds
// the value's expression; an empty cell keeps the authored value.
//
// CSV (RFC 4180: fields with commas, quotes or line breaks are quoted,
// quotes doubled; LF or CRLF):
//
//   configuration,width,Base.thickness,Housing/w
//   Light,60,4,
//   Heavy,80,"max(6, width / 10)",50
//
// apply_design_table() makes the table the document's configurations:
//
//   - a row whose name matches a configuration keeps its id; other rows
//     are new configurations; configurations with no row are removed (and
//     the active one cleared when it goes);
//   - a row's values are exactly its non-empty cells, in column order.
//
// Its errors leave the document as it was:
//   model.design_table.syntax   the text isn't CSV, or has no header, or a
//                               row has more cells than the header
//   model.design_table.column   a column names nothing drivable, or names
//                               the same number as another column
//   model.design_table.row      a row has no name, or another row's name
// The result must still pass check(): go through edit() (check.hpp).

#include <string>
#include <string_view>
#include <vector>

#include "vs/model/configuration.hpp"
#include "vs/model/document.hpp"

namespace vs::model {

struct DesignTableColumn {
  Id target;
  std::string label;     // configuration.hpp's labels
  std::string authored;  // the authored expression, for display
  friend bool operator==(const DesignTableColumn&, const DesignTableColumn&) = default;
};

struct DesignTableRow {
  Id configuration;  // null for a row that isn't a configuration yet
  std::string name;
  std::vector<std::string> cells;  // one per column; empty: not driven
  friend bool operator==(const DesignTableRow&, const DesignTableRow&) = default;
};

struct DesignTable {
  std::vector<DesignTableColumn> columns;
  std::vector<DesignTableRow> rows;
  friend bool operator==(const DesignTable&, const DesignTable&) = default;
};

// The document's configurations as a table. Columns: the numbers some
// configuration drives, or with `all` every drivable number, in
// drivable_targets() order (a driven target that is no longer drivable
// is left out: check() reports it).
DesignTable design_table(const Document& document, bool all = false);

// The table as CSV, and back. from_csv() names columns by label only
// (targets are found by apply_design_table()).
std::string to_csv(const DesignTable& table);
Result<DesignTable> from_csv(const Document& document, std::string_view text);

// The table as the document's configurations (see above). Columns are
// matched by target when set, else by label.
Result<Document> apply_design_table(const Document& document, const DesignTable& table);

}  // namespace vs::model
