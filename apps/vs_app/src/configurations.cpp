#include "configurations.hpp"

#include <algorithm>
#include <set>

#include "authoring.hpp"

namespace vs::app {

namespace {

core::Diagnostics error(std::string code, std::string message) {
  core::Diagnostics out;
  out.add(core::Diagnostic::error(std::move(code), std::move(message)));
  return out;
}

model::Configuration* find(model::Document& d, const Id& id) {
  for (auto& c : d.configurations) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

core::Diagnostics missing() {
  return error("app.edit.no_configuration", "That configuration is no longer there.");
}

}  // namespace

Command add_configuration(std::string name, Id copy_of, bool activate, Id id) {
  if (id.is_null()) id = Id::random();
  std::string label = "Add configuration " + name;
  return {std::move(label),
          [name = std::move(name), copy_of, activate, id](model::Project& p) -> core::Diagnostics {
            model::Document& d = p.document;
            std::vector<std::string> taken;
            for (const auto& c : d.configurations) taken.push_back(c.name);
            model::Configuration c{id, unique_name(name.empty() ? std::string("Configuration") : name, taken), {}};
            if (!copy_of.is_null()) {
              const model::Configuration* from = d.find_configuration(copy_of);
              if (from == nullptr) return missing();
              c.values = from->values;
            }
            d.configurations.push_back(std::move(c));
            if (activate) d.active_configuration = id;
            return {};
          }};
}

Command rename_configuration(Id configuration, std::string name) {
  return {"Rename configuration", [configuration, name = std::move(name)](model::Project& p) -> core::Diagnostics {
            model::Configuration* c = find(p.document, configuration);
            if (c == nullptr) return missing();
            c->name = name;
            return {};
          }};
}

Command remove_configuration(Id configuration) {
  return {"Delete configuration", [configuration](model::Project& p) -> core::Diagnostics {
            model::Document& d = p.document;
            if (std::erase_if(d.configurations,
                              [&](const model::Configuration& c) { return c.id == configuration; }) == 0) {
              return missing();
            }
            if (d.active_configuration == configuration) d.active_configuration = Id::null();
            return {};
          }};
}

Command set_configuration_value(Id configuration, Id target, std::string expression) {
  return {"Edit configuration", [configuration, target, expression = std::move(expression)](
                                    model::Project& p) -> core::Diagnostics {
            model::Configuration* c = find(p.document, configuration);
            if (c == nullptr) return missing();
            const bool clear = expression.find_first_not_of(" \t") == std::string::npos;
            auto it = std::find_if(c->values.begin(), c->values.end(),
                                   [&](const model::ConfigurationValue& v) { return v.target == target; });
            if (clear) {
              if (it != c->values.end()) c->values.erase(it);
              return {};
            }
            if (!model::drivable(p.document, target)) {
              return error("app.edit.not_drivable",
                           "A configuration can drive only layout variables and volume parameters.");
            }
            if (it != c->values.end()) {
              it->expression = expression;
            } else {
              c->values.push_back({target, expression});
            }
            return {};
          }};
}

Command set_active_configuration(Id configuration) {
  return {"Show configuration", [configuration](model::Project& p) -> core::Diagnostics {
            if (!configuration.is_null() && p.document.find_configuration(configuration) == nullptr) {
              return missing();
            }
            p.document.active_configuration = configuration;
            return {};
          }};
}

Command set_design_table(model::DesignTable table) {
  return {"Edit design table", [table = std::move(table)](model::Project& p) -> core::Diagnostics {
            auto applied = model::apply_design_table(p.document, table);
            if (!applied) {
              core::Diagnostics out;
              out.add(applied.error());
              return out;
            }
            p.document = std::move(*applied);
            return {};
          }};
}

std::vector<ConfigurationChoice> configuration_choices(const model::Document& document) {
  std::vector<ConfigurationChoice> out;
  out.push_back({Id::null(), "Authored values", document.active_configuration.is_null()});
  for (const auto& c : document.configurations) {
    out.push_back({c.id, c.name, c.id == document.active_configuration});
  }
  return out;
}

std::optional<std::string> active_value(const model::Document& document, const Id& target) {
  const model::Configuration* c = document.find_configuration(document.active_configuration);
  if (c == nullptr) return std::nullopt;
  const model::ConfigurationValue* v = c->find(target);
  if (v == nullptr) return std::nullopt;
  return v->expression;
}

model::DesignTable table_for_editing(const model::Document& document, const std::vector<Id>& extra) {
  model::DesignTable table = model::design_table(document, true);
  std::set<Id> keep(extra.begin(), extra.end());
  for (const auto& c : document.configurations) {
    for (const auto& v : c.values) keep.insert(v.target);
  }
  std::vector<std::size_t> columns;
  for (std::size_t k = 0; k < table.columns.size(); ++k) {
    if (keep.contains(table.columns[k].target)) columns.push_back(k);
  }
  model::DesignTable out;
  for (const std::size_t k : columns) out.columns.push_back(table.columns[k]);
  for (const auto& r : table.rows) {
    model::DesignTableRow row{r.configuration, r.name, {}};
    for (const std::size_t k : columns) row.cells.push_back(r.cells[k]);
    out.rows.push_back(std::move(row));
  }
  return out;
}

std::vector<model::Drivable> addable_columns(const model::Document& document,
                                             const model::DesignTable& table) {
  std::set<Id> present;
  for (const auto& c : table.columns) present.insert(c.target);
  std::vector<model::Drivable> out;
  for (auto& d : model::drivable_targets(document)) {
    if (!present.contains(d.target)) out.push_back(std::move(d));
  }
  return out;
}

}  // namespace vs::app
