#include "vs/feature/serialize.hpp"

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include "vs/sketch/serialize.hpp"

namespace vs::feature {

namespace {

using nlohmann::json;
using ojson = nlohmann::ordered_json;  // writing keeps keys in the order given

// --- writing ------------------------------------------------------------------

ojson param_json(const Param& p) {
  if (p.expression.empty()) return p.value;
  return ojson{{"value", p.value}, {"expression", p.expression}};
}

ojson plane_ref_json(const PlaneRef& ref) {
  if (const auto* p = std::get_if<PrincipalPlane>(&ref)) {
    return ojson{{"principal", std::string(to_string(*p))}};
  }
  if (const auto* f = std::get_if<FeatureRef>(&ref)) {
    return ojson{{"feature", f->feature.to_string()}};
  }
  const auto& face = std::get<FaceRef>(ref);
  return ojson{{"face", ojson{{"feature", face.feature.to_string()}, {"name", face.name}}}};
}

ojson entity_ref_json(const SketchEntityRef& e) {
  return ojson{{"sketch_entity",
               ojson{{"sketch", e.sketch.to_string()}, {"entity", e.entity.to_string()}}}};
}

ojson axis_ref_json(const AxisRef& ref) {
  if (const auto* p = std::get_if<PrincipalAxis>(&ref)) {
    return ojson{{"principal", std::string(to_string(*p))}};
  }
  if (const auto* f = std::get_if<FeatureRef>(&ref)) {
    return ojson{{"feature", f->feature.to_string()}};
  }
  return entity_ref_json(std::get<SketchEntityRef>(ref));
}

ojson point_ref_json(const PointRef& ref) {
  if (const auto* f = std::get_if<FeatureRef>(&ref)) {
    return ojson{{"feature", f->feature.to_string()}};
  }
  return entity_ref_json(std::get<SketchEntityRef>(ref));
}

void write_definition(ojson& j, const SketchFeature& s) {
  j["type"] = "sketch";
  j["plane"] = plane_ref_json(s.plane);
  j["sketch"] = ojson::parse(sketch::to_json(s.sketch));
}

void write_definition(ojson& j, const DatumPlaneFeature& d) {
  j["type"] = "datum_plane";
  if (const auto* o = std::get_if<PlaneOffset>(&d.definition)) {
    j["offset"] = ojson{{"base", plane_ref_json(o->base)}, {"distance", param_json(o->distance)}};
  } else if (const auto* t = std::get_if<PlaneThroughPoints>(&d.definition)) {
    j["through_points"] =
        ojson{{"a", point_ref_json(t->a)}, {"b", point_ref_json(t->b)}, {"c", point_ref_json(t->c)}};
  } else {
    const auto& a = std::get<PlaneAxisAngle>(d.definition);
    j["axis_angle"] = ojson{{"axis", axis_ref_json(a.axis)},
                           {"base", plane_ref_json(a.base)},
                           {"angle", param_json(a.angle)}};
  }
}

void write_definition(ojson& j, const DatumAxisFeature& d) {
  j["type"] = "datum_axis";
  if (const auto* t = std::get_if<AxisThroughPoints>(&d.definition)) {
    j["through_points"] = ojson{{"a", point_ref_json(t->a)}, {"b", point_ref_json(t->b)}};
  } else if (const auto* p = std::get_if<AxisPlaneIntersection>(&d.definition)) {
    j["plane_intersection"] = ojson{{"a", plane_ref_json(p->a)}, {"b", plane_ref_json(p->b)}};
  } else {
    j["along"] = axis_ref_json(std::get<AxisAlong>(d.definition).axis);
  }
}

void write_definition(ojson& j, const DatumPointFeature& d) {
  j["type"] = "datum_point";
  if (const auto* a = std::get_if<PointAt>(&d.definition)) {
    j["at"] = ojson{{"x", param_json(a->x)}, {"y", param_json(a->y)}, {"z", param_json(a->z)}};
  } else if (const auto* r = std::get_if<PointAtRef>(&d.definition)) {
    j["at_ref"] = point_ref_json(r->point);
  } else {
    const auto& ap = std::get<PointAxisPlane>(d.definition);
    j["axis_plane"] = ojson{{"axis", axis_ref_json(ap.axis)}, {"plane", plane_ref_json(ap.plane)}};
  }
}

void write_definition(ojson& j, const ExtrudeFeature& e) {
  const ExtrudeFeature defaults;
  j["type"] = "extrude";
  j["sketch"] = e.sketch.to_string();
  j["operation"] = std::string(to_string(e.operation));
  j["extent"] = std::string(to_string(e.extent));
  if (e.extent == Extent::Blind || e.extent == Extent::Symmetric) j["depth"] = param_json(e.depth);
  else if (e.depth != defaults.depth) j["depth"] = param_json(e.depth);
  if (e.extent == Extent::UpToPlane || e.up_to != defaults.up_to) {
    j["up_to"] = plane_ref_json(e.up_to);
  }
  if (e.draft != defaults.draft) j["draft"] = param_json(e.draft);
  if (e.reverse) j["reverse"] = true;
}

void write_definition(ojson& j, const RevolveFeature& r) {
  j["type"] = "revolve";
  j["sketch"] = r.sketch.to_string();
  j["operation"] = std::string(to_string(r.operation));
  j["axis"] = axis_ref_json(r.axis);
  j["angle"] = param_json(r.angle);
  if (r.reverse) j["reverse"] = true;
}

// --- reading ------------------------------------------------------------------

struct ReadError {
  std::string code;
  std::string message;
};

[[noreturn]] void fail(std::string_view where, std::string message,
                       std::string code = "feature.json.bad_value") {
  throw ReadError{std::move(code), fmt::format("{}: {}", where, message)};
}

const json& member(const json& j, std::string_view key, std::string_view where) {
  if (!j.is_object()) fail(where, "expected an object");
  const auto it = j.find(key);
  if (it == j.end()) {
    fail(where, fmt::format("missing \"{}\"", key), "feature.json.missing_key");
  }
  return *it;
}

std::string read_string(const json& j, std::string_view where) {
  if (!j.is_string()) fail(where, "expected a string");
  return j.get<std::string>();
}

Id read_id(const json& j, std::string_view where) {
  const auto id = Id::parse(read_string(j, where));
  if (!id) fail(where, "expected an id of 32 hex digits");
  return *id;
}

bool read_bool(const json& j, std::string_view key, std::string_view where) {
  const auto it = j.find(key);
  if (it == j.end()) return false;
  if (!it->is_boolean()) fail(fmt::format("{}.{}", where, key), "expected true or false");
  return it->get<bool>();
}

Param read_param(const json& j, std::string_view where) {
  if (j.is_number()) return Param::of(j.get<double>());
  if (j.is_string()) return Param::expr(j.get<std::string>());
  if (j.is_object()) {
    Param p;
    const auto v = j.find("value");
    if (v != j.end()) {
      if (!v->is_number()) fail(where, "\"value\" must be a number");
      p.value = v->get<double>();
    }
    const auto e = j.find("expression");
    if (e != j.end()) p.expression = read_string(*e, where);
    return p;
  }
  fail(where, "expected a number, an expression string or {\"value\", \"expression\"}");
}

PrincipalPlane read_principal_plane(const json& j, std::string_view where) {
  const auto s = read_string(j, where);
  if (s == "xy") return PrincipalPlane::XY;
  if (s == "yz") return PrincipalPlane::YZ;
  if (s == "zx") return PrincipalPlane::ZX;
  fail(where, fmt::format("unknown principal plane \"{}\" (xy, yz or zx)", s));
}

PrincipalAxis read_principal_axis(const json& j, std::string_view where) {
  const auto s = read_string(j, where);
  if (s == "x") return PrincipalAxis::X;
  if (s == "y") return PrincipalAxis::Y;
  if (s == "z") return PrincipalAxis::Z;
  fail(where, fmt::format("unknown principal axis \"{}\" (x, y or z)", s));
}

SketchEntityRef read_entity_ref(const json& j, std::string_view where) {
  return {read_id(member(j, "sketch", where), where), read_id(member(j, "entity", where), where)};
}

PlaneRef read_plane_ref(const json& j, std::string_view where) {
  if (!j.is_object()) fail(where, "expected a plane reference object");
  if (j.contains("principal")) return read_principal_plane(j["principal"], where);
  if (j.contains("feature")) return FeatureRef{read_id(j["feature"], where)};
  if (j.contains("face")) {
    const json& f = j["face"];
    return FaceRef{read_id(member(f, "feature", where), where),
                   read_string(member(f, "name", where), where)};
  }
  fail(where, "expected \"principal\", \"feature\" or \"face\"");
}

AxisRef read_axis_ref(const json& j, std::string_view where) {
  if (!j.is_object()) fail(where, "expected an axis reference object");
  if (j.contains("principal")) return read_principal_axis(j["principal"], where);
  if (j.contains("feature")) return FeatureRef{read_id(j["feature"], where)};
  if (j.contains("sketch_entity")) return read_entity_ref(j["sketch_entity"], where);
  fail(where, "expected \"principal\", \"feature\" or \"sketch_entity\"");
}

PointRef read_point_ref(const json& j, std::string_view where) {
  if (!j.is_object()) fail(where, "expected a point reference object");
  if (j.contains("feature")) return FeatureRef{read_id(j["feature"], where)};
  if (j.contains("sketch_entity")) return read_entity_ref(j["sketch_entity"], where);
  fail(where, "expected \"feature\" or \"sketch_entity\"");
}

FeatureDefinition read_definition(const json& j, const std::string& where) {
  const std::string type = read_string(member(j, "type", where), where + ".type");
  if (type == "sketch") {
    SketchFeature s;
    if (j.contains("plane")) s.plane = read_plane_ref(j["plane"], where + ".plane");
    auto sk = sketch::sketch_from_json(member(j, "sketch", where).dump());
    if (!sk) fail(where + ".sketch", sk.error().message, sk.error().code);
    s.sketch = std::move(*sk);
    return s;
  }
  if (type == "datum_plane") {
    DatumPlaneFeature d;
    if (j.contains("offset")) {
      const json& o = j["offset"];
      d.definition = PlaneOffset{read_plane_ref(member(o, "base", where), where + ".offset.base"),
                                 read_param(member(o, "distance", where), where + ".offset.distance")};
    } else if (j.contains("through_points")) {
      const json& t = j["through_points"];
      d.definition = PlaneThroughPoints{read_point_ref(member(t, "a", where), where + ".a"),
                                        read_point_ref(member(t, "b", where), where + ".b"),
                                        read_point_ref(member(t, "c", where), where + ".c")};
    } else if (j.contains("axis_angle")) {
      const json& a = j["axis_angle"];
      d.definition = PlaneAxisAngle{read_axis_ref(member(a, "axis", where), where + ".axis"),
                                    read_plane_ref(member(a, "base", where), where + ".base"),
                                    read_param(member(a, "angle", where), where + ".angle")};
    } else {
      fail(where, "a datum plane needs \"offset\", \"through_points\" or \"axis_angle\"");
    }
    return d;
  }
  if (type == "datum_axis") {
    DatumAxisFeature d;
    if (j.contains("through_points")) {
      const json& t = j["through_points"];
      d.definition = AxisThroughPoints{read_point_ref(member(t, "a", where), where + ".a"),
                                       read_point_ref(member(t, "b", where), where + ".b")};
    } else if (j.contains("plane_intersection")) {
      const json& p = j["plane_intersection"];
      d.definition = AxisPlaneIntersection{read_plane_ref(member(p, "a", where), where + ".a"),
                                           read_plane_ref(member(p, "b", where), where + ".b")};
    } else if (j.contains("along")) {
      d.definition = AxisAlong{read_axis_ref(j["along"], where + ".along")};
    } else {
      fail(where, "a datum axis needs \"through_points\", \"plane_intersection\" or \"along\"");
    }
    return d;
  }
  if (type == "datum_point") {
    DatumPointFeature d;
    if (j.contains("at")) {
      const json& a = j["at"];
      d.definition = PointAt{read_param(member(a, "x", where), where + ".x"),
                             read_param(member(a, "y", where), where + ".y"),
                             read_param(member(a, "z", where), where + ".z")};
    } else if (j.contains("at_ref")) {
      d.definition = PointAtRef{read_point_ref(j["at_ref"], where + ".at_ref")};
    } else if (j.contains("axis_plane")) {
      const json& a = j["axis_plane"];
      d.definition = PointAxisPlane{read_axis_ref(member(a, "axis", where), where + ".axis"),
                                    read_plane_ref(member(a, "plane", where), where + ".plane")};
    } else {
      fail(where, "a datum point needs \"at\", \"at_ref\" or \"axis_plane\"");
    }
    return d;
  }
  if (type == "extrude" || type == "revolve") {
    const Id sketch = read_id(member(j, "sketch", where), where + ".sketch");
    Operation op = Operation::Add;
    if (j.contains("operation")) {
      const auto s = read_string(j["operation"], where + ".operation");
      if (s == "add") op = Operation::Add;
      else if (s == "cut") op = Operation::Cut;
      else fail(where + ".operation", fmt::format("unknown operation \"{}\" (add or cut)", s));
    }
    const bool reverse = read_bool(j, "reverse", where);
    if (type == "extrude") {
      ExtrudeFeature e;
      e.sketch = sketch;
      e.operation = op;
      e.reverse = reverse;
      if (j.contains("extent")) {
        const auto s = read_string(j["extent"], where + ".extent");
        if (s == "blind") e.extent = Extent::Blind;
        else if (s == "through_all") e.extent = Extent::ThroughAll;
        else if (s == "up_to_plane") e.extent = Extent::UpToPlane;
        else if (s == "symmetric") e.extent = Extent::Symmetric;
        else fail(where + ".extent", fmt::format("unknown extent \"{}\"", s));
      }
      if (j.contains("depth")) e.depth = read_param(j["depth"], where + ".depth");
      if (j.contains("up_to")) e.up_to = read_plane_ref(j["up_to"], where + ".up_to");
      else if (e.extent == Extent::UpToPlane) member(j, "up_to", where);
      if (j.contains("draft")) e.draft = read_param(j["draft"], where + ".draft");
      return e;
    }
    RevolveFeature r;
    r.sketch = sketch;
    r.operation = op;
    r.reverse = reverse;
    r.axis = read_axis_ref(member(j, "axis", where), where + ".axis");
    if (j.contains("angle")) r.angle = read_param(j["angle"], where + ".angle");
    return r;
  }
  fail(where + ".type", fmt::format("unknown feature type \"{}\"", type),
       "feature.json.unknown_type");
}

}  // namespace

std::string to_json(const FeatureTree& tree) {
  ojson doc;
  doc["format"] = "vstudio.features";
  doc["version"] = kFeatureFormatVersion;
  ojson vars = ojson::array();
  for (const auto& v : tree.variables().variables()) {
    vars.push_back(ojson{{"id", v.id.to_string()}, {"name", v.name}, {"expression", v.expression}});
  }
  doc["variables"] = std::move(vars);
  ojson features = ojson::array();
  for (const auto& f : tree.features()) {
    ojson j;
    j["id"] = f.id.to_string();
    if (!f.name.empty()) j["name"] = f.name;
    if (f.suppressed) j["suppressed"] = true;
    std::visit([&j](const auto& def) { write_definition(j, def); }, f.definition);
    features.push_back(std::move(j));
  }
  doc["features"] = std::move(features);
  return doc.dump(2) + "\n";
}

Result<FeatureTree> feature_tree_from_json(std::string_view text) {
  json doc;
  try {
    doc = json::parse(text);
  } catch (const json::parse_error& e) {
    return Diagnostic::error("feature.json.syntax",
                             fmt::format("Not valid JSON: {}", e.what()));
  }
  try {
    if (!doc.is_object()) fail("document", "expected an object");
    if (read_string(member(doc, "format", "document"), "format") != "vstudio.features") {
      fail("format", "not a vstudio.features document", "feature.json.wrong_format");
    }
    const json& version = member(doc, "version", "document");
    if (!version.is_number_integer() || version.get<int>() > kFeatureFormatVersion ||
        version.get<int>() < 1) {
      fail("version", fmt::format("unsupported version (this build reads 1 to {})",
                                  kFeatureFormatVersion),
           "feature.json.version");
    }
    FeatureTree tree;
    if (doc.contains("variables")) {
      const json& vars = doc["variables"];
      if (!vars.is_array()) fail("variables", "expected an array");
      for (std::size_t i = 0; i < vars.size(); ++i) {
        const std::string where = fmt::format("variables[{}]", i);
        Variable v;
        v.id = read_id(member(vars[i], "id", where), where + ".id");
        v.name = read_string(member(vars[i], "name", where), where + ".name");
        const json& e = member(vars[i], "expression", where);
        v.expression = e.is_number() ? fmt::format("{}", e.get<double>())
                                     : read_string(e, where + ".expression");
        tree.variables().add_unchecked(std::move(v));
      }
    }
    if (doc.contains("features")) {
      const json& features = doc["features"];
      if (!features.is_array()) fail("features", "expected an array");
      for (std::size_t i = 0; i < features.size(); ++i) {
        const std::string where = fmt::format("features[{}]", i);
        const json& j = features[i];
        Feature f;
        f.id = read_id(member(j, "id", where), where + ".id");
        if (j.contains("name")) f.name = read_string(j["name"], where + ".name");
        f.suppressed = read_bool(j, "suppressed", where);
        f.definition = read_definition(j, where);
        if (tree.find(f.id)) {
          fail(where + ".id", "another feature already has this id", "feature.tree.duplicate_id");
        }
        tree.insert(tree.features().size(), std::move(f));
      }
    }
    // Authoring errors in the variables reject the file; a reference to a
    // variable that doesn't exist only fails the rebuild.
    for (const auto& d : tree.variables().check()) {
      if (d.is_error() && d.code != "feature.variable.unknown") return d;
    }
    return tree;
  } catch (const ReadError& e) {
    return Diagnostic::error(e.code, e.message);
  } catch (const json::exception& e) {
    return Diagnostic::error("feature.json.bad_value", e.what());
  }
}

}  // namespace vs::feature
