#include "vs/sketch/serialize.hpp"

#include <array>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace vs::sketch {

using nlohmann::json;

namespace {

constexpr std::string_view kFormat = "vstudio.sketch";

json vec(const dvec2& v) { return json::array({v.x, v.y}); }
json vec(const dvec3& v) { return json::array({v.x, v.y, v.z}); }

// Reading. Each helper throws ReadError; sketch_from_json() turns it into a
// diagnostic, so nothing escapes the module.
struct ReadError {
  std::string message;
};

[[noreturn]] void fail(std::string message) { throw ReadError{std::move(message)}; }

const json& member(const json& j, std::string_view key, std::string_view where) {
  if (!j.is_object()) fail(fmt::format("{} must be an object", where));
  const auto it = j.find(key);
  if (it == j.end()) fail(fmt::format("{} has no \"{}\"", where, key));
  return *it;
}

double read_number(const json& j, std::string_view what) {
  if (!j.is_number()) fail(fmt::format("{} must be a number", what));
  return j.get<double>();
}

std::string read_string(const json& j, std::string_view what) {
  if (!j.is_string()) fail(fmt::format("{} must be a string", what));
  return j.get<std::string>();
}

Id read_id(const json& j, std::string_view what) {
  const auto id = Id::parse(read_string(j, what));
  if (!id) fail(fmt::format("{} is not an id (32 hex digits)", what));
  return *id;
}

// The id of an authored object (the sketch, an entity, a constraint): never
// null, which would match every unset reference.
Id read_own_id(const json& j, std::string_view what) {
  const Id id = read_id(j, what);
  if (id.is_null()) fail(fmt::format("{} is the null id; every object needs its own", what));
  return id;
}

template <std::size_t N>
std::array<double, N> read_numbers(const json& j, std::string_view what) {
  if (!j.is_array() || j.size() != N) fail(fmt::format("{} must be {} numbers", what, N));
  std::array<double, N> out{};
  for (std::size_t i = 0; i < N; ++i) out[i] = read_number(j[i], what);
  return out;
}

dvec2 read_vec2(const json& j, std::string_view what) {
  const auto a = read_numbers<2>(j, what);
  return {a[0], a[1]};
}

dvec3 read_vec3(const json& j, std::string_view what) {
  const auto a = read_numbers<3>(j, what);
  return {a[0], a[1], a[2]};
}

std::string optional_string(const json& j, std::string_view key, std::string_view what) {
  const auto it = j.find(key);
  return it == j.end() ? std::string{} : read_string(*it, what);
}

Entity read_entity(const json& j, std::size_t index) {
  const std::string where = fmt::format("entities[{}]", index);
  Entity e;
  e.id = read_own_id(member(j, "id", where), where + ".id");
  const std::string type = read_string(member(j, "type", where), where + ".type");
  if (type == "point") {
    e.geometry = Point{read_vec2(member(j, "position", where), where + ".position")};
  } else if (type == "line") {
    e.geometry = Line{read_id(member(j, "start", where), where + ".start"),
                      read_id(member(j, "end", where), where + ".end")};
  } else if (type == "circle") {
    e.geometry = Circle{read_id(member(j, "center", where), where + ".center"),
                        read_number(member(j, "radius", where), where + ".radius")};
  } else if (type == "arc") {
    e.geometry = Arc{read_id(member(j, "center", where), where + ".center"),
                     read_id(member(j, "start", where), where + ".start"),
                     read_id(member(j, "end", where), where + ".end")};
  } else {
    fail(fmt::format("{}.type \"{}\" is not a known entity type", where, type));
  }
  if (const auto it = j.find("construction"); it != j.end()) {
    if (!it->is_boolean()) fail(where + ".construction must be true or false");
    e.construction = it->get<bool>();
  }
  e.name = optional_string(j, "name", where + ".name");
  return e;
}

Constraint read_constraint(const json& j, std::size_t index) {
  const std::string where = fmt::format("constraints[{}]", index);
  Constraint c;
  c.id = read_own_id(member(j, "id", where), where + ".id");
  const std::string type = read_string(member(j, "type", where), where + ".type");
  const auto kind = parse_constraint_kind(type);
  if (!kind) fail(fmt::format("{}.type \"{}\" is not a known constraint type", where, type));
  c.kind = *kind;
  const json& refs = member(j, "refs", where);
  if (!refs.is_array()) fail(where + ".refs must be an array");
  for (std::size_t i = 0; i < refs.size(); ++i) {
    c.refs.push_back(read_id(refs[i], fmt::format("{}.refs[{}]", where, i)));
  }
  if (is_dimensional(c.kind)) {
    Dimension d;
    d.value = read_number(member(j, "value", where), where + ".value");
    d.expression = optional_string(j, "expression", where + ".expression");
    c.dimension = std::move(d);
  }
  c.name = optional_string(j, "name", where + ".name");
  return c;
}

}  // namespace

std::string to_json(const Sketch& sketch) {
  json out;
  out["format"] = kFormat;
  out["version"] = kSketchFormatVersion;
  out["id"] = sketch.id().to_string();
  if (!sketch.name().empty()) out["name"] = sketch.name();
  out["plane"] = {{"origin", vec(sketch.plane().origin)},
                  {"x_axis", vec(sketch.plane().x_axis)},
                  {"y_axis", vec(sketch.plane().y_axis)}};

  json entities = json::array();
  for (const auto& e : sketch.entities()) {
    json j;
    j["id"] = e.id.to_string();
    j["type"] = to_string(e.kind());
    std::visit(
        [&](const auto& g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, Point>) {
            j["position"] = vec(g.position);
          } else if constexpr (std::is_same_v<T, Line>) {
            j["start"] = g.start.to_string();
            j["end"] = g.end.to_string();
          } else if constexpr (std::is_same_v<T, Circle>) {
            j["center"] = g.center.to_string();
            j["radius"] = g.radius;
          } else {
            j["center"] = g.center.to_string();
            j["start"] = g.start.to_string();
            j["end"] = g.end.to_string();
          }
        },
        e.geometry);
    if (e.construction) j["construction"] = true;
    if (!e.name.empty()) j["name"] = e.name;
    entities.push_back(std::move(j));
  }
  out["entities"] = std::move(entities);

  json constraints = json::array();
  for (const auto& c : sketch.constraints()) {
    json j;
    j["id"] = c.id.to_string();
    j["type"] = to_string(c.kind);
    json refs = json::array();
    for (const auto& r : c.refs) refs.push_back(r.to_string());
    j["refs"] = std::move(refs);
    if (c.dimension) {
      j["value"] = c.dimension->value;
      if (!c.dimension->expression.empty()) j["expression"] = c.dimension->expression;
    }
    if (!c.name.empty()) j["name"] = c.name;
    constraints.push_back(std::move(j));
  }
  out["constraints"] = std::move(constraints);

  return out.dump(2) + "\n";
}

Result<Sketch> sketch_from_json(std::string_view text) {
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return Diagnostic::error("sketch.json.syntax", "The sketch file is not valid JSON");
  }
  try {
    if (!root.is_object()) fail("The sketch file must hold a JSON object");
    if (read_string(member(root, "format", "the sketch"), "format") != kFormat) {
      fail(fmt::format("\"format\" must be \"{}\"", kFormat));
    }
    const double version = read_number(member(root, "version", "the sketch"), "version");
    if (version != kSketchFormatVersion) {
      return Diagnostic::error(
          "sketch.json.version",
          fmt::format("Sketch format version {} is not supported (expected {})", version,
                      kSketchFormatVersion));
    }
    SketchPlane plane;
    const json& p = member(root, "plane", "the sketch");
    plane.origin = read_vec3(member(p, "origin", "plane"), "plane.origin");
    plane.x_axis = read_vec3(member(p, "x_axis", "plane"), "plane.x_axis");
    plane.y_axis = read_vec3(member(p, "y_axis", "plane"), "plane.y_axis");

    Sketch sketch(read_own_id(member(root, "id", "the sketch"), "id"),
                  optional_string(root, "name", "name"), plane);

    const json& entities = member(root, "entities", "the sketch");
    if (!entities.is_array()) fail("\"entities\" must be an array");
    for (std::size_t i = 0; i < entities.size(); ++i) {
      sketch.add_entity(read_entity(entities[i], i));
    }
    const json& constraints = member(root, "constraints", "the sketch");
    if (!constraints.is_array()) fail("\"constraints\" must be an array");
    for (std::size_t i = 0; i < constraints.size(); ++i) {
      sketch.add_constraint(read_constraint(constraints[i], i));
    }
    return sketch;
  } catch (const ReadError& e) {
    return Diagnostic::error("sketch.json.structure", e.message);
  }
}

}  // namespace vs::sketch
