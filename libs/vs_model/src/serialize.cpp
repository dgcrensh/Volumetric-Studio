// JSON form of documents and project manifests (project.hpp).

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include "vs/core/version.hpp"
#include "vs/feature/serialize.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"

#include "manifest.hpp"

namespace vs::model {

namespace {

using nlohmann::json;
using ojson = nlohmann::ordered_json;  // writing keeps keys in the order given
using feature::PlaneRef;
using feature::PointRef;

// --- writing ------------------------------------------------------------------------

ojson param_json(const Param& p) {
  if (p.expression.empty()) return p.value;
  return ojson{{"value", p.value}, {"expression", p.expression}};
}

ojson entity_ref_json(const feature::SketchEntityRef& e) {
  return ojson{{"sketch_entity",
               ojson{{"sketch", e.sketch.to_string()}, {"entity", e.entity.to_string()}}}};
}

ojson plane_ref_json(const PlaneRef& ref) {
  if (const auto* p = std::get_if<feature::PrincipalPlane>(&ref)) {
    return ojson{{"principal", std::string(feature::to_string(*p))}};
  }
  if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) {
    return ojson{{"feature", f->feature.to_string()}};
  }
  const auto& face = std::get<feature::FaceRef>(ref);
  return ojson{{"face", ojson{{"feature", face.feature.to_string()}, {"name", face.name}}}};
}

ojson point_ref_json(const PointRef& ref) {
  if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) {
    return ojson{{"feature", f->feature.to_string()}};
  }
  return entity_ref_json(std::get<feature::SketchEntityRef>(ref));
}

ojson ids_json(const std::vector<Id>& ids) {
  ojson a = ojson::array();
  for (const Id& id : ids) a.push_back(id.to_string());
  return a;
}

ojson tree_json(const feature::FeatureTree& tree) { return ojson::parse(feature::to_json(tree)); }

ojson volume_json(const Volume& v) {
  ojson j;
  j["id"] = v.id.to_string();
  j["name"] = v.name;
  if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
    j["boundary"] = ojson{{"features", ids_json(fb->features)}};
  } else {
    const auto& b = std::get<BoxBoundary>(v.boundary);
    j["boundary"] = ojson{{"box", ojson{{"min", point_ref_json(b.min)}, {"max", point_ref_json(b.max)}}}};
  }
  if (v.keep_out) j["keep_out"] = true;
  ojson c;
  c["kind"] = std::string(to_string(v.kind()));
  if (const auto* s = std::get_if<StaticContents>(&v.contents)) {
    if (!s->blob.empty()) c["blob"] = s->blob;
    if (!s->format.empty()) c["format"] = s->format;
  } else if (const auto* p = std::get_if<ParametricContents>(&v.contents)) {
    c["tree"] = tree_json(p->tree);
  } else {
    const auto& g = std::get<ProceduralContents>(v.contents);
    if (!g.graph.empty()) c["graph"] = g.graph;
  }
  j["contents"] = std::move(c);
  if (!v.parameters.empty()) {
    ojson a = ojson::array();
    for (const auto& p : v.parameters) {
      a.push_back(ojson{{"id", p.id.to_string()}, {"name", p.name}, {"value", param_json(p.value)}});
    }
    j["parameters"] = std::move(a);
  }
  if (v.seed != 0) j["seed"] = v.seed;
  if (!v.outputs.empty()) {
    ojson a = ojson::array();
    for (const auto& o : v.outputs) {
      ojson oj{{"id", o.id.to_string()},
               {"name", o.name},
               {"quantity", std::string(to_string(o.quantity))}};
      if (o.quantity == OutputQuantity::Mass) oj["density"] = o.density;
      a.push_back(std::move(oj));
    }
    j["outputs"] = std::move(a);
  }
  return j;
}

ojson connection_json(const ConnectionSurface& c) {
  ojson j;
  j["id"] = c.id.to_string();
  j["name"] = c.name;
  j["volumes"] = ids_json(c.volumes);
  if (const auto* sp = std::get_if<SketchPatch>(&c.geometry)) {
    j["geometry"] = ojson{{"sketch", sp->sketch.to_string()}};
  } else {
    const auto& dp = std::get<DatumPatch>(c.geometry);
    j["geometry"] = ojson{{"datum", ojson{{"plane", plane_ref_json(dp.plane)},
                                          {"u", param_json(dp.u)},
                                          {"v", param_json(dp.v)},
                                          {"width", param_json(dp.width)},
                                          {"height", param_json(dp.height)}}}};
  }
  j["type"] = std::string(to_string(c.type()));
  if (const auto* cl = std::get_if<ClearanceParams>(&c.params)) {
    j["gap"] = param_json(cl->gap);
  } else if (const auto* b = std::get_if<BoltedFlangeParams>(&c.params)) {
    ojson holes;
    if (const auto* bc = std::get_if<BoltCircle>(&b->holes.layout)) {
      holes["circle"] = ojson{{"count", param_json(bc->count)},
                              {"diameter", param_json(bc->diameter)},
                              {"start_angle", param_json(bc->start_angle)}};
    } else {
      const auto& g = std::get<BoltGrid>(b->holes.layout);
      holes["grid"] = ojson{{"columns", param_json(g.columns)},
                            {"rows", param_json(g.rows)},
                            {"pitch_x", param_json(g.pitch_x)},
                            {"pitch_y", param_json(g.pitch_y)}};
    }
    holes["u"] = param_json(b->holes.u);
    holes["v"] = param_json(b->holes.v);
    j["holes"] = std::move(holes);
    j["thread"] = b->thread;
    j["min_wall"] = param_json(b->min_wall);
  }
  j["seam"] = ojson{{"spacing", c.seam.spacing}};
  return j;
}

ojson document_json(const Document& d) {
  ojson j;
  j["id"] = d.id.to_string();
  j["name"] = d.name;
  j["layout"] = tree_json(d.layout);
  ojson volumes = ojson::array();
  for (const auto& v : d.volumes) volumes.push_back(volume_json(v));
  j["volumes"] = std::move(volumes);
  ojson connections = ojson::array();
  for (const auto& c : d.connections) connections.push_back(connection_json(c));
  j["connections"] = std::move(connections);
  ojson keep_outs = ojson::array();
  for (const auto& k : d.keep_outs) {
    keep_outs.push_back(ojson{{"id", k.id.to_string()},
                              {"name", k.name},
                              {"cutters", ids_json(k.cutters)},
                              {"volumes", ids_json(k.volumes)}});
  }
  j["keep_outs"] = std::move(keep_outs);
  ojson links = ojson::array();
  for (const auto& l : d.links) {
    links.push_back(ojson{{"id", l.id.to_string()},
                          {"source", ojson{{"volume", l.source.volume.to_string()},
                                           {"output", l.source.output.to_string()}}},
                          {"target", l.target.to_string()}});
  }
  j["links"] = std::move(links);
  return j;
}

// --- reading ------------------------------------------------------------------------

struct ReadError {
  std::string code;
  std::string message;
};

[[noreturn]] void fail(std::string_view where, std::string message,
                       std::string code = "model.json.bad_value") {
  throw ReadError{std::move(code), fmt::format("{}: {}", where, message)};
}

const json& member(const json& j, std::string_view key, std::string_view where) {
  if (!j.is_object()) fail(where, "expected an object");
  const auto it = j.find(key);
  if (it == j.end()) fail(where, fmt::format("missing \"{}\"", key), "model.json.missing_key");
  return *it;
}

const json* optional_member(const json& j, std::string_view key) {
  const auto it = j.find(key);
  return it == j.end() ? nullptr : &*it;
}

std::string read_string(const json& j, std::string_view where) {
  if (!j.is_string()) fail(where, "expected a string");
  return j.get<std::string>();
}

std::string read_optional_string(const json& j, std::string_view key, std::string_view where) {
  const json* m = optional_member(j, key);
  return m ? read_string(*m, fmt::format("{}.{}", where, key)) : std::string{};
}

Id read_id(const json& j, std::string_view where) {
  const auto id = Id::parse(read_string(j, where));
  if (!id) fail(where, "expected an id of 32 hex digits");
  return *id;
}

std::vector<Id> read_ids(const json& j, std::string_view key, std::string_view where) {
  const json& a = member(j, key, where);
  if (!a.is_array()) fail(fmt::format("{}.{}", where, key), "expected an array");
  std::vector<Id> out;
  for (std::size_t i = 0; i < a.size(); ++i) {
    out.push_back(read_id(a[i], fmt::format("{}.{}[{}]", where, key, i)));
  }
  return out;
}

double read_number(const json& j, std::string_view where) {
  if (!j.is_number()) fail(where, "expected a number");
  return j.get<double>();
}

Param read_param(const json& j, std::string_view where) {
  if (j.is_number()) return Param::of(j.get<double>());
  if (j.is_string()) return Param::expr(j.get<std::string>());
  if (j.is_object()) {
    Param p;
    if (const json* v = optional_member(j, "value")) p.value = read_number(*v, where);
    if (const json* e = optional_member(j, "expression")) p.expression = read_string(*e, where);
    return p;
  }
  fail(where, "expected a number, an expression string or {\"value\", \"expression\"}");
}

Param read_param_or(const json& j, std::string_view key, std::string_view where, Param fallback) {
  const json* m = optional_member(j, key);
  return m ? read_param(*m, fmt::format("{}.{}", where, key)) : fallback;
}

feature::SketchEntityRef read_entity_ref(const json& j, std::string_view where) {
  return {read_id(member(j, "sketch", where), where), read_id(member(j, "entity", where), where)};
}

PlaneRef read_plane_ref(const json& j, std::string_view where) {
  if (!j.is_object()) fail(where, "expected a plane reference object");
  if (const json* p = optional_member(j, "principal")) {
    const auto s = read_string(*p, where);
    if (s == "xy") return feature::PrincipalPlane::XY;
    if (s == "yz") return feature::PrincipalPlane::YZ;
    if (s == "zx") return feature::PrincipalPlane::ZX;
    fail(where, fmt::format("unknown principal plane \"{}\" (xy, yz or zx)", s));
  }
  if (const json* f = optional_member(j, "feature")) return feature::FeatureRef{read_id(*f, where)};
  if (const json* f = optional_member(j, "face")) {
    return feature::FaceRef{read_id(member(*f, "feature", where), where),
                            read_string(member(*f, "name", where), where)};
  }
  fail(where, "expected \"principal\", \"feature\" or \"face\"");
}

PointRef read_point_ref(const json& j, std::string_view where) {
  if (!j.is_object()) fail(where, "expected a point reference object");
  if (const json* f = optional_member(j, "feature")) return feature::FeatureRef{read_id(*f, where)};
  if (const json* e = optional_member(j, "sketch_entity")) return read_entity_ref(*e, where);
  fail(where, "expected \"feature\" or \"sketch_entity\"");
}

feature::FeatureTree read_tree(const json& j, std::string_view where) {
  auto tree = feature::feature_tree_from_json(j.dump());
  if (!tree) {
    Diagnostic d = tree.error();
    throw ReadError{d.code, fmt::format("{}: {}", where, d.message)};
  }
  return std::move(tree).value();
}

OutputQuantity read_quantity(const json& j, std::string_view where) {
  const auto s = read_string(j, where);
  for (auto q : {OutputQuantity::Volume, OutputQuantity::Mass, OutputQuantity::SurfaceArea,
                 OutputQuantity::CentreOfGravity, OutputQuantity::Envelope}) {
    if (to_string(q) == s) return q;
  }
  fail(where, fmt::format("unknown quantity \"{}\"", s));
}

Volume read_volume(const json& j, const std::string& where) {
  Volume v;
  v.id = read_id(member(j, "id", where), where + ".id");
  v.name = read_optional_string(j, "name", where);
  const json& b = member(j, "boundary", where);
  if (b.contains("features")) {
    v.boundary = FeatureBoundary{read_ids(b, "features", where + ".boundary")};
  } else if (const json* box = optional_member(b, "box")) {
    v.boundary = BoxBoundary{read_point_ref(member(*box, "min", where), where + ".boundary.box.min"),
                             read_point_ref(member(*box, "max", where), where + ".boundary.box.max")};
  } else {
    fail(where + ".boundary", "expected \"features\" or \"box\"");
  }
  if (const json* k = optional_member(j, "keep_out")) {
    if (!k->is_boolean()) fail(where + ".keep_out", "expected true or false");
    v.keep_out = k->get<bool>();
  }
  const json& c = member(j, "contents", where);
  const std::string at = where + ".contents";
  const auto kind = read_string(member(c, "kind", at), at + ".kind");
  if (kind == "static") {
    v.contents = StaticContents{read_optional_string(c, "blob", at),
                                read_optional_string(c, "format", at)};
  } else if (kind == "parametric") {
    v.contents = ParametricContents{read_tree(member(c, "tree", at), at + ".tree")};
  } else if (kind == "procedural") {
    v.contents = ProceduralContents{read_optional_string(c, "graph", at)};
  } else {
    fail(at + ".kind", fmt::format("unknown kind \"{}\" (static, parametric or procedural)", kind));
  }
  if (const json* ps = optional_member(j, "parameters")) {
    if (!ps->is_array()) fail(where + ".parameters", "expected an array");
    for (std::size_t i = 0; i < ps->size(); ++i) {
      const std::string pw = fmt::format("{}.parameters[{}]", where, i);
      const json& p = (*ps)[i];
      v.parameters.push_back({read_id(member(p, "id", pw), pw + ".id"),
                              read_string(member(p, "name", pw), pw + ".name"),
                              read_param(member(p, "value", pw), pw + ".value")});
    }
  }
  if (const json* s = optional_member(j, "seed")) {
    if (!s->is_number_unsigned() && !(s->is_number_integer() && s->get<std::int64_t>() >= 0)) {
      fail(where + ".seed", "expected a whole number of 0 or more");
    }
    v.seed = s->get<std::uint64_t>();
  }
  if (const json* os = optional_member(j, "outputs")) {
    if (!os->is_array()) fail(where + ".outputs", "expected an array");
    for (std::size_t i = 0; i < os->size(); ++i) {
      const std::string ow = fmt::format("{}.outputs[{}]", where, i);
      const json& o = (*os)[i];
      PublishedOutput out;
      out.id = read_id(member(o, "id", ow), ow + ".id");
      out.name = read_string(member(o, "name", ow), ow + ".name");
      out.quantity = read_quantity(member(o, "quantity", ow), ow + ".quantity");
      if (const json* d = optional_member(o, "density")) out.density = read_number(*d, ow + ".density");
      v.outputs.push_back(std::move(out));
    }
  }
  return v;
}

ConnectionSurface read_connection(const json& j, const std::string& where) {
  ConnectionSurface c;
  c.id = read_id(member(j, "id", where), where + ".id");
  c.name = read_optional_string(j, "name", where);
  c.volumes = read_ids(j, "volumes", where);
  const json& g = member(j, "geometry", where);
  const std::string gw = where + ".geometry";
  if (const json* s = optional_member(g, "sketch")) {
    c.geometry = SketchPatch{read_id(*s, gw + ".sketch")};
  } else if (const json* d = optional_member(g, "datum")) {
    const std::string dw = gw + ".datum";
    DatumPatch dp;
    dp.plane = read_plane_ref(member(*d, "plane", dw), dw + ".plane");
    dp.u = read_param_or(*d, "u", dw, dp.u);
    dp.v = read_param_or(*d, "v", dw, dp.v);
    dp.width = read_param_or(*d, "width", dw, dp.width);
    dp.height = read_param_or(*d, "height", dw, dp.height);
    c.geometry = dp;
  } else {
    fail(gw, "expected \"sketch\" or \"datum\"");
  }
  const auto type = read_string(member(j, "type", where), where + ".type");
  if (type == "bonded") {
    c.params = BondedParams{};
  } else if (type == "clearance") {
    ClearanceParams p;
    p.gap = read_param_or(j, "gap", where, p.gap);
    c.params = p;
  } else if (type == "bolted_flange") {
    BoltedFlangeParams p;
    const json& h = member(j, "holes", where);
    const std::string hw = where + ".holes";
    if (const json* bc = optional_member(h, "circle")) {
      BoltCircle circle;
      circle.count = read_param_or(*bc, "count", hw + ".circle", circle.count);
      circle.diameter = read_param_or(*bc, "diameter", hw + ".circle", circle.diameter);
      circle.start_angle = read_param_or(*bc, "start_angle", hw + ".circle", circle.start_angle);
      p.holes.layout = circle;
    } else if (const json* bg = optional_member(h, "grid")) {
      BoltGrid grid;
      grid.columns = read_param_or(*bg, "columns", hw + ".grid", grid.columns);
      grid.rows = read_param_or(*bg, "rows", hw + ".grid", grid.rows);
      grid.pitch_x = read_param_or(*bg, "pitch_x", hw + ".grid", grid.pitch_x);
      grid.pitch_y = read_param_or(*bg, "pitch_y", hw + ".grid", grid.pitch_y);
      p.holes.layout = grid;
    } else {
      fail(hw, "expected \"circle\" or \"grid\"");
    }
    p.holes.u = read_param_or(h, "u", hw, p.holes.u);
    p.holes.v = read_param_or(h, "v", hw, p.holes.v);
    if (const json* t = optional_member(j, "thread")) p.thread = read_string(*t, where + ".thread");
    p.min_wall = read_param_or(j, "min_wall", where, p.min_wall);
    c.params = p;
  } else {
    fail(where + ".type",
         fmt::format("unknown type \"{}\" (bonded, clearance or bolted_flange)", type));
  }
  if (const json* s = optional_member(j, "seam")) {
    if (const json* sp = optional_member(*s, "spacing")) {
      c.seam.spacing = read_number(*sp, where + ".seam.spacing");
    }
  }
  return c;
}

Document read_document(const json& j) {
  const std::string where = "document";
  Document d;
  d.id = read_id(member(j, "id", where), "document.id");
  d.name = read_optional_string(j, "name", where);
  d.layout = read_tree(member(j, "layout", where), "document.layout");
  auto array = [&](std::string_view key) -> const json* {
    const json* a = optional_member(j, key);
    if (a && !a->is_array()) fail(fmt::format("document.{}", key), "expected an array");
    return a;
  };
  if (const json* vs = array("volumes")) {
    for (std::size_t i = 0; i < vs->size(); ++i) {
      d.volumes.push_back(read_volume((*vs)[i], fmt::format("document.volumes[{}]", i)));
    }
  }
  if (const json* cs = array("connections")) {
    for (std::size_t i = 0; i < cs->size(); ++i) {
      d.connections.push_back(read_connection((*cs)[i], fmt::format("document.connections[{}]", i)));
    }
  }
  if (const json* ks = array("keep_outs")) {
    for (std::size_t i = 0; i < ks->size(); ++i) {
      const std::string kw = fmt::format("document.keep_outs[{}]", i);
      const json& k = (*ks)[i];
      d.keep_outs.push_back({read_id(member(k, "id", kw), kw + ".id"),
                             read_optional_string(k, "name", kw), read_ids(k, "cutters", kw),
                             read_ids(k, "volumes", kw)});
    }
  }
  if (const json* ls = array("links")) {
    for (std::size_t i = 0; i < ls->size(); ++i) {
      const std::string lw = fmt::format("document.links[{}]", i);
      const json& l = (*ls)[i];
      const json& s = member(l, "source", lw);
      d.links.push_back({read_id(member(l, "id", lw), lw + ".id"),
                         {read_id(member(s, "volume", lw), lw + ".source.volume"),
                          read_id(member(s, "output", lw), lw + ".source.output")},
                         read_id(member(l, "target", lw), lw + ".target")});
    }
  }
  return d;
}

template <typename F>
auto guarded(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const ReadError& e) {
    return Diagnostic::error(e.code, e.message);
  } catch (const json::exception& e) {
    return Diagnostic::error("model.json.bad_value", e.what());
  }
}

Result<json> parse(std::string_view text) {
  try {
    return json::parse(text);
  } catch (const json::parse_error& e) {
    return Diagnostic::error("model.json.syntax", fmt::format("Not valid JSON: {}", e.what()));
  }
}

// The first error of the document's authoring check, if any.
std::optional<Diagnostic> first_error(const Document& d) {
  for (const auto& diag : check(d)) {
    if (diag.is_error()) return diag;
  }
  return std::nullopt;
}

}  // namespace

std::string to_json(const Document& document) { return document_json(document).dump(2); }

Result<Document> document_from_json(std::string_view text) {
  auto j = parse(text);
  if (!j) return j.error();
  auto d = guarded([&]() -> Result<Document> { return read_document(*j); });
  if (!d) return d;
  if (auto e = first_error(*d)) return *e;
  return d;
}

std::string manifest_json(const Project& project) {
  ojson j;
  j["format"] = "vstudio.project";
  j["version"] = kProjectFormatVersion;
  j["engine"] = core::kEngineVersion;
  j["document"] = document_json(project.document);
  ojson blobs = ojson::array();
  for (const auto& [path, bytes] : project.blobs) {
    blobs.push_back(ojson{{"path", path},
                          {"size", bytes.size()},
                          {"blake3", core::blake3_of_bytes(bytes).to_hex()}});
  }
  j["blobs"] = std::move(blobs);
  return j.dump(2) + "\n";
}

namespace detail {

Result<std::pair<Project, std::vector<BlobEntry>>> read_manifest(std::string_view text) {
  auto j = parse(text);
  if (!j) return j.error();
  using Out = std::pair<Project, std::vector<BlobEntry>>;
  auto r = guarded([&]() -> Result<Out> {
    const json& m = *j;
    if (!m.is_object()) fail("manifest", "expected an object");
    if (read_string(member(m, "format", "manifest"), "format") != "vstudio.project") {
      fail("format", "not a vstudio.project manifest", "model.project.not_project");
    }
    const json& version = member(m, "version", "manifest");
    if (!version.is_number_integer() || version.get<int>() < 1) {
      fail("version", "expected a whole number of 1 or more");
    }
    if (version.get<int>() > kProjectFormatVersion) {
      fail("version",
           fmt::format("the project is format version {}, newer than this build reads (up to {}); "
                       "open it with a newer studio",
                       version.get<int>(), kProjectFormatVersion),
           "model.project.version");
    }
    Out out;
    out.first.document = read_document(member(m, "document", "manifest"));
    if (const json* bs = optional_member(m, "blobs")) {
      if (!bs->is_array()) fail("blobs", "expected an array");
      for (std::size_t i = 0; i < bs->size(); ++i) {
        const std::string bw = fmt::format("blobs[{}]", i);
        const json& b = (*bs)[i];
        BlobEntry e;
        e.path = read_string(member(b, "path", bw), bw + ".path");
        const json& size = member(b, "size", bw);
        if (!size.is_number_unsigned()) fail(bw + ".size", "expected a whole number");
        e.size = size.get<std::uint64_t>();
        e.blake3 = read_string(member(b, "blake3", bw), bw + ".blake3");
        if (!e.path.starts_with("blobs/") || e.path.find("..") != std::string::npos ||
            e.path.find('\\') != std::string::npos) {
          fail(bw + ".path", "a blob path must lie under blobs/");
        }
        out.second.push_back(std::move(e));
      }
    }
    return out;
  });
  return r;
}

}  // namespace detail

Result<Project> project_from_manifest(std::string_view text) {
  auto r = detail::read_manifest(text);
  if (!r) return r.error();
  if (auto e = first_error(r->first.document)) return *e;
  return std::move(r->first);
}

core::Hash256 content_hash(const Document& document) {
  const std::string text = to_json(document);
  return core::blake3_of_bytes(std::as_bytes(std::span(text.data(), text.size())));
}

core::Hash256 content_hash(const Project& project) {
  core::Hasher h;
  h.add(std::string_view("vstudio.project"));
  h.add(content_hash(project.document));
  h.add(project.blobs.size());
  for (const auto& [path, bytes] : project.blobs) {
    h.add(path);
    h.add(core::blake3_of_bytes(bytes));
  }
  return h.finalize();
}

}  // namespace vs::model
