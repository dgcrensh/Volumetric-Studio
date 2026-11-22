#include "vs/model/configuration.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include <fmt/format.h>

namespace vs::model {

namespace {

std::string parameter_text(const feature::Param& p) {
  return p.expression.empty() ? number_text(p.value) : p.expression;
}

std::string volume_label(const Volume& v) {
  return v.name.empty() ? fmt::format("({})", v.id.to_string()) : v.name;
}

}  // namespace

std::string number_text(double value) {
  // {} is the shortest text that reads back as the same double.
  if (value == 0.0) return "0";  // -0 too
  return fmt::format("{}", value);
}

std::optional<double> plain_number(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.empty()) return std::nullopt;
  if (text.front() == '+') text.remove_prefix(1);
  double value = 0.0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<Drivable> drivable(const Document& document, const Id& target) {
  if (target.is_null()) return std::nullopt;
  if (const auto* var = document.layout.variables().find(target)) {
    return Drivable{DrivableKind::Variable, target, {}, var->name, var->expression};
  }
  for (const auto& v : document.volumes) {
    if (v.generated) continue;
    if (const auto* p = v.find_parameter(target)) {
      return Drivable{DrivableKind::Parameter, target, v.id,
                      fmt::format("{}.{}", volume_label(v), p->name), parameter_text(p->value)};
    }
    if (const auto* c = std::get_if<CompositeContents>(&v.contents)) {
      if (const auto* var = c->layout.variables().find(target)) {
        return Drivable{DrivableKind::CompositeVariable, target, v.id,
                        fmt::format("{}/{}", volume_label(v), var->name), var->expression};
      }
    }
  }
  return std::nullopt;
}

std::vector<Drivable> drivable_targets(const Document& document) {
  std::vector<Drivable> out;
  for (const auto& var : document.layout.variables().variables()) {
    out.push_back({DrivableKind::Variable, var.id, {}, var.name, var.expression});
  }
  for (const auto& v : document.volumes) {
    if (v.generated) continue;
    for (const auto& p : v.parameters) {
      out.push_back({DrivableKind::Parameter, p.id, v.id,
                     fmt::format("{}.{}", volume_label(v), p.name), parameter_text(p.value)});
    }
    if (const auto* c = std::get_if<CompositeContents>(&v.contents)) {
      for (const auto& var : c->layout.variables().variables()) {
        out.push_back({DrivableKind::CompositeVariable, var.id, v.id,
                       fmt::format("{}/{}", volume_label(v), var.name), var.expression});
      }
    }
  }
  return out;
}

Document apply_configuration(const Document& document, const Id& configuration) {
  Document out = document;
  out.configurations.clear();
  out.active_configuration = Id::null();
  const Configuration* config = document.find_configuration(configuration);
  if (config == nullptr) return out;
  for (const auto& value : config->values) {
    if (out.layout.variables().set_expression_unchecked(value.target, value.expression)) {
      continue;
    }
    for (auto& v : out.volumes) {
      bool done = false;
      for (auto& p : v.parameters) {
        if (!(p.id == value.target)) continue;
        if (const auto number = plain_number(value.expression)) {
          p.value = feature::Param::of(*number);
        } else {
          p.value = feature::Param::expr(value.expression);
        }
        done = true;
      }
      if (auto* c = std::get_if<CompositeContents>(&v.contents); c != nullptr && !done) {
        done = c->layout.variables().set_expression_unchecked(value.target, value.expression);
      }
      if (done) break;
    }
  }
  return out;
}

bool has_configurations(const Document& document) {
  return !document.configurations.empty() || !document.active_configuration.is_null();
}

Document configured(const Document& document) {
  if (!has_configurations(document)) return document;
  return apply_configuration(document, document.active_configuration);
}

std::size_t prune_configurations(Document& document) {
  std::size_t dropped = 0;
  for (auto& c : document.configurations) {
    const auto before = c.values.size();
    std::erase_if(c.values, [&](const ConfigurationValue& v) {
      return !drivable(document, v.target).has_value();
    });
    dropped += before - c.values.size();
  }
  return dropped;
}

}  // namespace vs::model
