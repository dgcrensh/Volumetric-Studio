#include "vs/model/design_table.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

#include <fmt/format.h>

namespace vs::model {

namespace {

std::string trim_copy(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return std::string(text);
}

Diagnostic table_error(std::string code, std::string message) {
  return Diagnostic::error(std::move(code), std::move(message));
}

// RFC 4180 records. Empty lines (a trailing newline, say) are skipped.
Result<std::vector<std::vector<std::string>>> parse_csv(std::string_view text) {
  std::vector<std::vector<std::string>> records;
  std::vector<std::string> record;
  std::string field;
  bool quoted = false;      // inside a quoted field
  bool was_quoted = false;  // the current field was quoted
  bool any = false;         // the current record has any character
  std::size_t line = 1;
  const auto end_field = [&] {
    record.push_back(std::move(field));
    field.clear();
    was_quoted = false;
  };
  const auto end_record = [&] {
    if (any || !record.empty()) {
      end_field();
      records.push_back(std::move(record));
    }
    record.clear();
    field.clear();
    any = false;
    was_quoted = false;
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          quoted = false;
        }
      } else {
        if (c == '\n') ++line;
        field.push_back(c);
      }
      continue;
    }
    switch (c) {
      case '"':
        if (!field.empty() || was_quoted) {
          return table_error("model.design_table.syntax",
                             fmt::format("line {}: a quote inside an unquoted field", line));
        }
        quoted = true;
        was_quoted = true;
        any = true;
        break;
      case ',':
        end_field();
        any = true;
        break;
      case '\r':
        if (i + 1 < text.size() && text[i + 1] == '\n') break;  // CRLF
        end_record();
        ++line;
        break;
      case '\n':
        end_record();
        ++line;
        break;
      default:
        if (was_quoted) {
          return table_error("model.design_table.syntax",
                             fmt::format("line {}: text after a quoted field", line));
        }
        field.push_back(c);
        any = true;
        break;
    }
  }
  if (quoted) {
    return table_error("model.design_table.syntax", "a quoted field isn't closed");
  }
  end_record();
  return records;
}

std::string csv_field(const std::string& text) {
  if (text.find_first_of(",\"\r\n") == std::string::npos && text == trim_copy(text)) return text;
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

}  // namespace

DesignTable design_table(const Document& document, bool all) {
  DesignTable out;
  std::set<Id> driven;
  for (const auto& c : document.configurations) {
    for (const auto& v : c.values) driven.insert(v.target);
  }
  for (const auto& d : drivable_targets(document)) {
    if (all || driven.contains(d.target)) out.columns.push_back({d.target, d.label, d.expression});
  }
  for (const auto& c : document.configurations) {
    DesignTableRow row{c.id, c.name, std::vector<std::string>(out.columns.size())};
    for (std::size_t k = 0; k < out.columns.size(); ++k) {
      if (const auto* v = c.find(out.columns[k].target)) row.cells[k] = v->expression;
    }
    out.rows.push_back(std::move(row));
  }
  return out;
}

std::string to_csv(const DesignTable& table) {
  std::string out = "configuration";
  for (const auto& c : table.columns) out += "," + csv_field(c.label);
  out += "\n";
  for (const auto& r : table.rows) {
    out += csv_field(r.name);
    for (std::size_t k = 0; k < table.columns.size(); ++k) {
      out += ",";
      if (k < r.cells.size()) out += csv_field(r.cells[k]);
    }
    out += "\n";
  }
  return out;
}

Result<DesignTable> from_csv(const Document& document, std::string_view text) {
  auto records = parse_csv(text);
  if (!records) return records.error();
  if (records->empty()) {
    return table_error("model.design_table.syntax",
                       "the table is empty; its first line names the columns");
  }
  std::unordered_map<std::string, Id> by_label;
  for (const auto& d : drivable_targets(document)) by_label.emplace(d.label, d.target);
  DesignTable table;
  const auto& header = records->front();
  for (std::size_t k = 1; k < header.size(); ++k) {
    const std::string label = trim_copy(header[k]);
    const auto it = by_label.find(label);
    if (it == by_label.end()) {
      return table_error(
          "model.design_table.column",
          fmt::format("column {} '{}' names no layout variable or volume parameter (write "
                      "'width', 'Volume.parameter' or 'Composite/variable')",
                      k + 1, label));
    }
    const auto d = drivable(document, it->second);
    table.columns.push_back({it->second, label, d ? d->expression : std::string{}});
  }
  for (std::size_t r = 1; r < records->size(); ++r) {
    const auto& record = (*records)[r];
    if (record.size() > header.size()) {
      return table_error("model.design_table.syntax",
                         fmt::format("row {} has {} cells but the header names {} columns", r + 1,
                                     record.size(), header.size()));
    }
    DesignTableRow row;
    row.name = trim_copy(record.front());
    if (const auto* c = document.find_configuration(row.name)) row.configuration = c->id;
    row.cells.resize(table.columns.size());
    for (std::size_t k = 1; k < record.size(); ++k) row.cells[k - 1] = trim_copy(record[k]);
    table.rows.push_back(std::move(row));
  }
  return table;
}

Result<Document> apply_design_table(const Document& document, const DesignTable& table) {
  // Columns: each a drivable target, once.
  std::vector<Id> targets;
  std::set<Id> seen;
  std::unordered_map<std::string, Id> by_label;
  for (const auto& d : drivable_targets(document)) by_label.emplace(d.label, d.target);
  for (std::size_t k = 0; k < table.columns.size(); ++k) {
    const auto& c = table.columns[k];
    Id target = c.target;
    if (target.is_null()) {
      if (const auto it = by_label.find(c.label); it != by_label.end()) target = it->second;
    }
    if (!drivable(document, target)) {
      return table_error("model.design_table.column",
                         fmt::format("column '{}' names no layout variable or volume parameter",
                                     c.label));
    }
    if (!seen.insert(target).second) {
      return table_error("model.design_table.column",
                         fmt::format("column '{}' drives the same number as another column",
                                     c.label));
    }
    targets.push_back(target);
  }
  // Rows: named, once each.
  std::set<std::string> names;
  for (const auto& r : table.rows) {
    if (r.name.empty()) {
      return table_error("model.design_table.row",
                         "a row has no name; the first cell names its configuration");
    }
    if (!names.insert(r.name).second) {
      return table_error("model.design_table.row",
                         fmt::format("two rows are named '{}'", r.name));
    }
  }
  Document out = document;
  out.configurations.clear();
  for (const auto& r : table.rows) {
    Configuration c;
    const Configuration* existing = !r.configuration.is_null()
                                        ? document.find_configuration(r.configuration)
                                        : document.find_configuration(r.name);
    c.id = existing != nullptr ? existing->id : Id::random();
    c.name = r.name;
    for (std::size_t k = 0; k < targets.size(); ++k) {
      const std::string cell = k < r.cells.size() ? trim_copy(r.cells[k]) : std::string{};
      if (!cell.empty()) c.values.push_back({targets[k], cell});
    }
    out.configurations.push_back(std::move(c));
  }
  if (!out.active_configuration.is_null() &&
      out.find_configuration(out.active_configuration) == nullptr) {
    out.active_configuration = Id::null();
  }
  return out;
}

}  // namespace vs::model
