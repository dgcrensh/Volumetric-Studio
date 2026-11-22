#include "vs/eval/cache.hpp"
#include "vs/model/hierarchy.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <iterator>
#include <sstream>
#include <variant>

#include <nlohmann/json.hpp>

#include "vs/core/version.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/serialize.hpp"
#include "vs/kernel/exchange.hpp"

namespace vs::eval {

using core::Hasher;
using model::VolumeStatus;

namespace {

void add_plane(Hasher& h, const kernel::Plane& p) { h.add(p.origin).add(p.normal); }

void add_named_faces(Hasher& h, const std::vector<feature::NamedFace>& faces) {
  h.add(static_cast<std::uint64_t>(faces.size()));
  for (const auto& f : faces) {
    h.add(f.name);
    h.add(f.cylinder.has_value());
    if (f.cylinder) {
      h.add(f.cylinder->axis.origin).add(f.cylinder->axis.direction);
      h.add(f.cylinder->radius).add(f.cylinder->inward);
    } else {
      add_plane(h, f.plane);
    }
  }
}

void add_optional(Hasher& h, const std::optional<double>& v) {
  h.add(v.has_value());
  if (v) h.add(*v);
}

std::optional<Hash256> cache_key_impl(const model::Document& document,
                                      const model::DocumentBuild& layout, std::size_t index,
                                      const core::ProductSettings& settings,
                                      const LayoutShapeHashes* hashes);

}  // namespace

LayoutShapeHashes::LayoutShapeHashes(std::shared_ptr<const model::DocumentBuild> layout)
    : layout_(std::move(layout)),
      boundaries_(layout_->volumes.size()),
      patches_(layout_->connections.size()) {}

Hash256 LayoutShapeHashes::boundary(std::size_t volume) const {
  auto& h = boundaries_.at(volume);
  if (!h) h = kernel::shape_hash(layout_->volumes[volume].boundary);
  return *h;
}

Hash256 LayoutShapeHashes::patch(std::size_t connection) const {
  auto& h = patches_.at(connection);
  if (!h) h = kernel::shape_hash(layout_->connections[connection].patch);
  return *h;
}

std::optional<Hash256> cache_key(const model::Document& document,
                                 const model::DocumentBuild& layout, std::size_t index,
                                 const core::ProductSettings& settings) {
  return cache_key_impl(document, layout, index, settings, nullptr);
}

std::optional<Hash256> cache_key(const model::Document& document,
                                 const model::DocumentBuild& layout, std::size_t index,
                                 const core::ProductSettings& settings,
                                 const LayoutShapeHashes& hashes) {
  return cache_key_impl(document, layout, index, settings, &hashes);
}

namespace {

std::optional<Hash256> cache_key_impl(const model::Document& document,
                                      const model::DocumentBuild& layout, std::size_t index,
                                      const core::ProductSettings& settings,
                                      const LayoutShapeHashes* hashes) {
  const model::Volume& v = document.volumes[index];
  const model::VolumeBuild& vb = layout.volumes[index];
  if (vb.boundary.is_empty() || vb.status == VolumeStatus::Failed) return std::nullopt;

  Hasher h;
  h.add(std::string_view("vstudio.eval.contents"));
  h.add(static_cast<std::uint64_t>(core::kEngineVersion));
  h.add(settings);
  h.add(v.kind());
  h.add(v.keep_out);

  // The content definition.
  if (const auto* sc = std::get_if<model::StaticContents>(&v.contents)) {
    h.add(sc->blob).add(sc->format);
  } else if (const auto* pc = std::get_if<model::ParametricContents>(&v.contents)) {
    h.add(feature::to_json(pc->tree));
    // L4: a template copy's offset, only when set so other keys don't change.
    if (!(v.contents_offset == core::dvec3{})) h.add(v.contents_offset);
  } else if (const auto* cc = std::get_if<model::CompositeContents>(&v.contents)) {
    // A composite's own part (L3); the engine adds its children's keys.
    h.add(feature::to_json(cc->layout));
    h.add(static_cast<std::uint64_t>(cc->delegations.size()));
    for (const auto& d : cc->delegations) h.add(d.surface).add(d.child);
    // L4: added only when set, so L3 composites keep their keys.
    if (cc->generates() || !cc->templates.empty()) {
      h.add(std::string_view("generator")).add(cc->generator);
      h.add(static_cast<std::uint64_t>(cc->templates.size()));
      for (const auto& t : cc->templates) {
        h.add(t.id).add(t.name).add(t.origin).add(static_cast<std::uint64_t>(t.contents.index()));
        if (const auto* tp = std::get_if<model::ParametricContents>(&t.contents)) {
          h.add(feature::to_json(tp->tree));
        } else if (const auto* tg = std::get_if<model::ProceduralContents>(&t.contents)) {
          h.add(tg->graph);
        } else {
          const auto& ts = std::get<model::StaticContents>(t.contents);
          h.add(ts.blob).add(ts.format);
        }
        for (const auto& p : t.parameters) {
          h.add(p.id).add(p.name).add(p.value.value).add(p.value.expression);
        }
        for (const auto& o : t.outputs) h.add(o.id).add(o.name).add(o.quantity).add(o.density);
      }
    }
  } else {
    h.add(std::get<model::ProceduralContents>(v.contents).graph);
  }

  // The boundary.
  h.add(hashes != nullptr ? hashes->boundary(index) : kernel::shape_hash(vb.boundary));
  add_named_faces(h, vb.boundary_faces);

  // The connection surfaces joining it.
  for (std::size_t k = 0; k < document.connections.size(); ++k) {
    const model::ConnectionSurface& c = document.connections[k];
    if (!model::joins(document, c, v.id)) continue;
    const model::ConnectionBuild& cb = layout.connections[k];
    h.add(c.id).add(c.type()).add(cb.ok);
    if (!cb.ok) continue;
    h.add(hashes != nullptr ? hashes->patch(k) : kernel::shape_hash(cb.patch));
    h.add(cb.frame.origin).add(cb.frame.x_axis).add(cb.frame.y_axis);
    add_optional(h, cb.gap);
    add_optional(h, cb.min_wall);
    h.add(static_cast<std::uint64_t>(cb.hole_centres.size()));
    for (const auto& p : cb.hole_centres) h.add(p);
    if (const auto* b = std::get_if<model::BoltedFlangeParams>(&c.params)) h.add(b->thread);
    h.add(c.seam.spacing);
  }

  // Parameters and seed: not for static files, which they can't change.
  if (v.kind() != model::VolumeKind::Static) {
    h.add(static_cast<std::uint64_t>(vb.parameters.size()));
    for (const auto& [name, value] : vb.parameters) h.add(name).add(value);
    h.add(static_cast<std::uint64_t>(v.seed));
  }

  // What it measures.
  h.add(static_cast<std::uint64_t>(v.outputs.size()));
  for (const auto& o : v.outputs) h.add(o.id).add(o.quantity).add(o.density);
  return h.finalize();
}

}  // namespace

Hash256 output_hash(VolumeStatus status, const kernel::Solid& solid, const mesh::Mesh* mesh,
                    const std::vector<model::OutputResult>& outputs, const ExtraContents* extra) {
  Hasher h;
  h.add(std::string_view("vstudio.eval.output"));
  h.add(status);
  h.add(kernel::shape_hash(solid));
  h.add(mesh != nullptr);
  if (mesh != nullptr) {
    h.add(static_cast<std::uint64_t>(mesh->positions.size()));
    for (const auto& p : mesh->positions) h.add(p);
    h.add(static_cast<std::uint64_t>(mesh->indices.size()));
    for (const auto i : mesh->indices) h.add(i);
  }
  h.add(static_cast<std::uint64_t>(outputs.size()));
  for (const auto& o : outputs) {
    h.add(o.output);
    h.add(o.value.has_value());
    if (!o.value) continue;
    h.add(o.value->index());
    std::visit([&](const auto& value) { h.add(value); }, *o.value);
  }
  // Only when present, so B-rep and mesh hashes are what M9 made them.
  if (extra != nullptr) h.add(std::string_view("extra")).add(extra->hash());
  return h.finalize();
}

// --- the cache ---------------------------------------------------------------------

namespace {

using nlohmann::json;

json vec_json(const core::dvec3& v) { return json::array({v.x, v.y, v.z}); }
core::dvec3 vec_from(const json& j) {
  return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

json entry_json(const GeneratedContents& c) {
  json outputs = json::array();
  for (const auto& o : c.outputs) {
    json jo{{"id", o.output.to_string()}};
    if (o.value) {
      if (const auto* d = std::get_if<double>(&*o.value)) {
        jo["scalar"] = *d;
      } else if (const auto* p = std::get_if<core::dvec3>(&*o.value)) {
        jo["vector"] = vec_json(*p);
      } else {
        const auto& b = std::get<core::dbox3>(*o.value);
        jo["box"] = json::array({vec_json(b.min), vec_json(b.max)});
      }
    }
    outputs.push_back(std::move(jo));
  }
  json diags = json::array();
  for (const auto& d : c.diagnostics) {
    json subjects = json::array();
    for (const auto& s : d.subjects) {
      subjects.push_back({{"kind", static_cast<int>(s.kind)},
                          {"id", s.id.to_string()},
                          {"label", s.label}});
    }
    diags.push_back({{"severity", static_cast<int>(d.severity)},
                     {"code", d.code},
                     {"message", d.message},
                     {"subjects", std::move(subjects)}});
  }
  return json{{"format", "vstudio.eval.cache"},
              {"engine", core::kEngineVersion},
              {"key", c.key.to_hex()},
              {"contents", c.mesh ? "mesh" : "brep"},
              {"output_hash", c.output_hash.to_hex()},
              {"outputs", std::move(outputs)},
              {"diagnostics", std::move(diags)}};
}

// --- meshes: "VSMESH01", then little-endian counts and arrays -----------------------

constexpr std::string_view kMeshMagic = "VSMESH01";

void put_u32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}
void put_u64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}
void put_id(std::string& out, const core::Id& id) {
  put_u64(out, id.hi);
  put_u64(out, id.lo);
}

std::string encode_mesh(const mesh::Mesh& m) {
  std::string out(kMeshMagic);
  out.reserve(out.size() + 16 + m.positions.size() * 28 + m.indices.size() * 4 +
              m.triangle_volumes.size() * 16);
  put_u64(out, m.positions.size());
  put_u64(out, m.triangle_count());
  for (const auto& p : m.positions) {
    for (const float f : {p.x, p.y, p.z}) put_u32(out, std::bit_cast<std::uint32_t>(f));
  }
  for (const auto& s : m.seam_vertices) put_id(out, s);
  for (const auto i : m.indices) put_u32(out, i);
  for (const auto& v : m.triangle_volumes) put_id(out, v);
  return out;
}

struct Reader {
  std::string_view data;
  std::size_t at = 0;
  bool ok = true;
  std::uint64_t get(int bytes) {
    if (at + static_cast<std::size_t>(bytes) > data.size()) {
      ok = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      v |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[at + static_cast<std::size_t>(i)]))
           << (8 * i);
    }
    at += static_cast<std::size_t>(bytes);
    return v;
  }
  core::Id id() {
    const std::uint64_t hi = get(8);
    const std::uint64_t lo = get(8);
    return {hi, lo};
  }
};

std::optional<mesh::Mesh> decode_mesh(std::string_view data) {
  if (!data.starts_with(kMeshMagic)) return std::nullopt;
  Reader r{data, kMeshMagic.size()};
  const std::uint64_t vertices = r.get(8);
  const std::uint64_t triangles = r.get(8);
  // The arrays must fill the rest exactly (this also bounds the counts).
  const std::uint64_t need = vertices * 28 + triangles * 28;
  if (!r.ok || vertices > data.size() || triangles > data.size() ||
      need != data.size() - r.at) {
    return std::nullopt;
  }
  mesh::Mesh m;
  m.positions.resize(vertices);
  for (auto& p : m.positions) {
    p.x = std::bit_cast<float>(static_cast<std::uint32_t>(r.get(4)));
    p.y = std::bit_cast<float>(static_cast<std::uint32_t>(r.get(4)));
    p.z = std::bit_cast<float>(static_cast<std::uint32_t>(r.get(4)));
  }
  m.seam_vertices.resize(vertices);
  for (auto& s : m.seam_vertices) s = r.id();
  m.indices.resize(3 * triangles);
  for (auto& i : m.indices) i = static_cast<std::uint32_t>(r.get(4));
  m.triangle_volumes.resize(triangles);
  for (auto& v : m.triangle_volumes) v = r.id();
  if (!r.ok || m.check().has_errors()) return std::nullopt;
  return m;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Writes next to `path` under a name unique to this write (H4: engines
// sharing a cache folder shared "<name>.tmp"), then moves it into place.
bool write_file(const std::filesystem::path& path, const std::string& text) {
  const auto tmp = path.string() + "." + core::Id::random().to_string().substr(0, 16) + ".tmp";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();  // flushes: a full disk shows here
    if (!out) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::filesystem::remove(tmp, ec);
  return !ec;
}

std::optional<Hash256> hash_from_hex(const std::string& hex) {
  if (hex.size() != 64) return std::nullopt;
  Hash256 out;
  for (std::size_t i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (std::size_t k = 0; k < 2; ++k) {
      const char c = hex[2 * i + k];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
      else return std::nullopt;
    }
    out.bytes[i] = static_cast<std::uint8_t>(value);
  }
  return out;
}

// An entry's files by name ("<key>.json" and "<key>.brep" or ".mesh").
using Fetch = std::function<std::optional<std::string>(const std::string& name)>;

ContentsPtr read_entry(const Fetch& fetch, const Hash256& key) {
  const std::string name = key.to_hex();
  const auto meta = fetch(name + ".json");
  if (!meta) return nullptr;
  try {
    const json j = json::parse(*meta);
    if (j.at("format") != "vstudio.eval.cache" ||
        j.at("engine").get<unsigned>() != core::kEngineVersion || j.at("key") != name) {
      return nullptr;
    }
    const auto hash = hash_from_hex(j.at("output_hash").get<std::string>());
    if (!hash) return nullptr;

    auto out = std::make_shared<GeneratedContents>();
    out->key = key;
    out->status = VolumeStatus::Current;
    out->output_hash = *hash;
    // Entries written before M17 have no "contents": they are B-rep.
    if (j.value("contents", std::string("brep")) == "mesh") {
      const auto bytes = fetch(name + ".mesh");
      if (!bytes) return nullptr;
      auto m = decode_mesh(*bytes);
      if (!m) return nullptr;
      out->mesh = std::move(*m);
    } else {
      const auto brep = fetch(name + ".brep");
      if (!brep) return nullptr;
      auto shape = kernel::read_brep(*brep);
      if (!shape) return nullptr;
      auto solid = kernel::Solid::from(*shape);
      if (!solid) return nullptr;
      out->solid = *solid;
    }
    for (const auto& jo : j.at("outputs")) {
      model::OutputResult o;
      const auto id = core::Id::parse(jo.at("id").get<std::string>());
      if (!id) return nullptr;
      o.output = *id;
      if (jo.contains("scalar")) o.value = jo["scalar"].get<double>();
      if (jo.contains("vector")) o.value = vec_from(jo["vector"]);
      if (jo.contains("box")) {
        o.value = core::dbox3::from_min_max(vec_from(jo["box"].at(0)), vec_from(jo["box"].at(1)));
      }
      out->outputs.push_back(std::move(o));
    }
    for (const auto& jd : j.at("diagnostics")) {
      core::Diagnostic d;
      // Enumerations in range, or the entry is damaged (H4).
      const auto severity = jd.at("severity").get<std::int64_t>();
      if (severity < 0 || severity > static_cast<std::int64_t>(core::Severity::Error)) return nullptr;
      d.severity = static_cast<core::Severity>(severity);
      d.code = jd.at("code").get<std::string>();
      d.message = jd.at("message").get<std::string>();
      for (const auto& js : jd.at("subjects")) {
        core::SubjectRef s;
        const auto kind = js.at("kind").get<std::int64_t>();
        if (kind < 0 || kind > static_cast<std::int64_t>(core::SubjectKind::Link)) return nullptr;
        s.kind = static_cast<core::SubjectKind>(kind);
        s.id = core::Id::parse(js.at("id").get<std::string>()).value_or(core::Id::null());
        s.label = js.at("label").get<std::string>();
        d.subjects.push_back(std::move(s));
      }
      out->diagnostics.add(std::move(d));
    }
    return out;
  } catch (const std::exception&) {
    return nullptr;  // a damaged entry is a miss
  }
}

model::Blob to_blob(const std::string& s) {
  model::Blob out(s.size());
  if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
  return out;
}

}  // namespace

bool cacheable(const GeneratedContents& contents) {
  return contents.ok() && !contents.cancelled && (contents.mesh || !contents.solid.is_empty());
}

bool encode_entry(const GeneratedContents& contents, model::CacheFiles& files) {
  if (!cacheable(contents)) return false;
  const std::string name = contents.key.to_hex();
  if (contents.mesh) {
    files.insert_or_assign(name + ".mesh", to_blob(encode_mesh(*contents.mesh)));
  } else {
    files.insert_or_assign(name + ".brep", to_blob(kernel::write_brep(contents.solid)));
  }
  files.insert_or_assign(name + ".json", to_blob(entry_json(contents).dump(1)));
  return true;
}

ContentsPtr decode_entry(const model::CacheFiles& files, const Hash256& key) {
  return read_entry(
      [&](const std::string& name) -> std::optional<std::string> {
        const auto it = files.find(name);
        if (it == files.end()) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(it->second.data()), it->second.size());
      },
      key);
}

std::size_t contents_bytes(const GeneratedContents& contents) {
  std::size_t bytes = sizeof(GeneratedContents);
  if (contents.mesh) bytes += contents.mesh->memory_bytes();
  if (!contents.solid.is_empty()) bytes += kSolidEstimateBytes;
  if (contents.rebuild) bytes += kSolidEstimateBytes;
  if (contents.extra) bytes += contents.extra->memory_bytes();
  return bytes;
}

ContentsCache::ContentsCache(std::filesystem::path directory, std::size_t memory_budget)
    : memory_budget_(memory_budget), directory_(std::move(directory)) {}

void ContentsCache::touch(Entry& entry) { uses_.splice(uses_.begin(), uses_, entry.use); }

ContentsPtr ContentsCache::keep(const Hash256& key, ContentsPtr contents, bool replace) {
  auto it = memory_.find(key);
  if (it != memory_.end()) {
    touch(it->second);
    if (!replace) return it->second.contents;
    bytes_ -= it->second.bytes;
    it->second.bytes = contents_bytes(*contents);
    it->second.contents = std::move(contents);
    bytes_ += it->second.bytes;
  } else {
    uses_.push_front(key);
    const std::size_t bytes = contents_bytes(*contents);
    it = memory_.emplace(key, Entry{std::move(contents), bytes, uses_.begin()}).first;
    bytes_ += bytes;
  }
  // Forget the least recently used, never the entry just kept.
  while (bytes_ > memory_budget_ && uses_.size() > 1) {
    const auto old = memory_.find(uses_.back());
    bytes_ -= old->second.bytes;
    memory_.erase(old);
    uses_.pop_back();
  }
  return it->second.contents;
}

ContentsPtr ContentsCache::find(const Hash256& key, bool* from_disk) {
  if (from_disk != nullptr) *from_disk = false;
  {
    const std::lock_guard lock(mutex_);
    const auto it = memory_.find(key);
    if (it != memory_.end()) {
      touch(it->second);
      return it->second.contents;
    }
  }
  if (directory_.empty()) return nullptr;
  ContentsPtr entry = read_entry([&](const std::string& name) { return read_file(directory_ / name); },
                                 key);
  if (!entry) return nullptr;
  if (from_disk != nullptr) *from_disk = true;
  const std::lock_guard lock(mutex_);
  return keep(key, std::move(entry), false);
}

void ContentsCache::insert(const ContentsPtr& contents) {
  if (!contents) return;
  {
    const std::lock_guard lock(mutex_);
    keep(contents->key, contents, true);
  }
  if (directory_.empty()) return;
  model::CacheFiles files;
  if (!encode_entry(*contents, files)) return;
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) return;  // the disk cache is never needed
  // The contents first: an entry is only found once its metadata exists.
  const std::string meta = contents->key.to_hex() + ".json";
  for (const auto& [name, bytes] : files) {
    if (name == meta) continue;
    if (!write_file(directory_ / name,
                    std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
      return;
    }
  }
  const auto& m = files.at(meta);
  write_file(directory_ / meta, std::string(reinterpret_cast<const char*>(m.data()), m.size()));
}

std::size_t ContentsCache::insert_files(const model::CacheFiles& files) {
  std::size_t added = 0;
  for (const auto& [name, bytes] : files) {
    if (!name.ends_with(".json")) continue;
    const auto key = hash_from_hex(name.substr(0, name.size() - 5));
    if (!key) continue;
    {
      const std::lock_guard lock(mutex_);
      if (memory_.contains(*key)) continue;
    }
    ContentsPtr entry = decode_entry(files, *key);
    if (!entry) continue;
    const std::lock_guard lock(mutex_);
    if (memory_.contains(*key)) continue;
    keep(*key, std::move(entry), false);
    ++added;
  }
  return added;
}

void ContentsCache::clear_memory() {
  const std::lock_guard lock(mutex_);
  memory_.clear();
  uses_.clear();
  bytes_ = 0;
}

std::size_t ContentsCache::memory_size() const {
  const std::lock_guard lock(mutex_);
  return memory_.size();
}

std::size_t ContentsCache::memory_bytes() const {
  const std::lock_guard lock(mutex_);
  return bytes_;
}

}  // namespace vs::eval

namespace vs::eval {

model::CacheFiles project_cache_files(const Evaluation& evaluation) {
  model::CacheFiles files;
  for (const auto& v : evaluation.volumes) {
    if (!v.contents || v.stale_contents || v.status != VolumeStatus::Current) continue;
    if (v.contents->rebuild) continue;  // parametric: rebuilt on load
    encode_entry(*v.contents, files);
  }
  return files;
}

}  // namespace vs::eval
