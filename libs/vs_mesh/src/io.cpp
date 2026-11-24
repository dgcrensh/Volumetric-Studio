#include "vs/mesh/io.hpp"

#include "vs/core/xml.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include <fmt/format.h>
#include <miniz.h>

namespace vs::mesh {
namespace {

using core::Diagnostic;
using core::fvec3;
using core::Id;

Diagnostic io_error(std::string_view format, std::string message) {
  return Diagnostic::error("mesh.io." + std::string(format), std::move(message));
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view as_text(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// --- building meshes from files ------------------------------------------------

// -0 as +0: one position, one bit pattern (H4).
fvec3 canonical(fvec3 p) {
  for (float* c : {&p.x, &p.y, &p.z}) {
    if (*c == 0.0f) *c = 0.0f;
  }
  return p;
}

bool finite(const fvec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

// Merges corners with identical floats (STL), drops degenerate triangles.
// Every position is canonical: a -0 coordinate is stored as +0.
class Builder {
 public:
  std::uint32_t vertex(const fvec3& position) {
    const fvec3 p = canonical(position);
    const std::array<std::uint32_t, 3> key{std::bit_cast<std::uint32_t>(p.x),
                                           std::bit_cast<std::uint32_t>(p.y),
                                           std::bit_cast<std::uint32_t>(p.z)};
    auto [it, added] = index_.try_emplace(key, 0);
    if (added) it->second = mesh_.add_vertex(p);
    return it->second;
  }
  std::uint32_t raw_vertex(const fvec3& p) { return mesh_.add_vertex(canonical(p)); }
  // Room for about this many (H5): a closed mesh has half as many vertices
  // as triangles.
  void reserve(std::size_t vertices, std::size_t triangles) {
    mesh_.positions.reserve(vertices);
    mesh_.seam_vertices.reserve(vertices);
    mesh_.indices.reserve(3 * triangles);
    mesh_.triangle_volumes.reserve(triangles);
    index_.reserve(vertices);
  }
  void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    if (a == b || b == c || c == a) return;
    mesh_.add_triangle(a, b, c, Id::null());
  }
  Mesh take() { return std::move(mesh_); }
  [[nodiscard]] const Mesh& mesh() const { return mesh_; }

 private:
  struct KeyHash {
    std::size_t operator()(const std::array<std::uint32_t, 3>& k) const {
      return (static_cast<std::size_t>(k[0]) * 0x9e3779b97f4a7c15ull) ^
             (static_cast<std::size_t>(k[1]) * 0xc2b2ae3d27d4eb4full) ^ k[2];
    }
  };
  Mesh mesh_;
  std::unordered_map<std::array<std::uint32_t, 3>, std::uint32_t, KeyHash> index_;
};

// Whole-token number parsing, independent of the C locale.
std::optional<double> number(std::string_view s) {
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  double v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
  return v;
}

// Splits on spaces and tabs.
std::vector<std::string_view> words(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

template <typename F>
void for_each_line(std::string_view text, F&& f) {
  std::size_t line = 1;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    f(text.substr(0, nl), line++);
    if (nl == std::string_view::npos) break;
    text.remove_prefix(nl + 1);
  }
}

// --- little endian ---------------------------------------------------------------

void put_u32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
}
void put_f32(Bytes& out, float f) { put_u32(out, std::bit_cast<std::uint32_t>(f)); }
std::uint32_t get_u32(std::span<const std::byte> b, std::size_t at) {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(b[at + i]) << (8 * i);
  return v;
}
float get_f32(std::span<const std::byte> b, std::size_t at) {
  return std::bit_cast<float>(get_u32(b, at));
}

// --- zip ---------------------------------------------------------------------

struct ZipEnd {
  mz_zip_archive* zip;
  bool writer;
  ~ZipEnd() {
    if (writer) {
      mz_zip_writer_end(zip);
    } else {
      mz_zip_reader_end(zip);
    }
  }
};

std::string zip_reason(mz_zip_archive& zip) {
  const char* text = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
  return text ? text : "unknown error";
}

core::Result<Bytes> zip_write(const std::vector<std::pair<std::string, std::string>>& entries) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    return io_error("3mf", "can't start the package: " + zip_reason(zip));
  }
  const ZipEnd end{&zip, true};
  for (const auto& [name, text] : entries) {
    if (!mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(),
                               static_cast<mz_uint>(MZ_DEFAULT_LEVEL))) {
      return io_error("3mf", "can't add '" + name + "' to the package: " + zip_reason(zip));
    }
  }
  void* data = nullptr;
  std::size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&zip, &data, &size)) {
    return io_error("3mf", "can't finish the package: " + zip_reason(zip));
  }
  const std::unique_ptr<void, decltype(&mz_free)> owned(data, &mz_free);
  const auto* first = static_cast<const std::byte*>(data);
  return Bytes(first, first + size);
}

core::Result<std::map<std::string, std::string>> zip_read(std::span<const std::byte> archive) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
    return io_error("3mf", "not a zip package: " + zip_reason(zip));
  }
  const ZipEnd end{&zip, false};
  std::map<std::string, std::string> files;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) return io_error("3mf", "a damaged zip entry");
    if (stat.m_is_directory) continue;
    if (!stat.m_is_supported) {
      return io_error("3mf", std::string("'") + stat.m_filename + "' is compressed in a way that isn't supported");
    }
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
    if (!data) {
      return io_error("3mf", std::string("can't read '") + stat.m_filename + "': " + zip_reason(zip));
    }
    const std::unique_ptr<void, decltype(&mz_free)> owned(data, &mz_free);
    files.insert_or_assign(std::string(stat.m_filename),
                           std::string(static_cast<const char*>(data), size));
  }
  return files;
}

// The XML reader is vs_core's (moved there for SVG import, S1).
using core::xml::escape;
using core::xml::scan_xml;
using core::xml::Tag;

// A 3MF transform: p' = (x y z 1) * M, M 4 rows by 3 columns.
using Matrix = std::array<double, 12>;
constexpr Matrix kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

std::optional<Matrix> parse_matrix(std::string_view s) {
  const std::vector<std::string_view> w = words(s);
  if (w.size() != 12) return std::nullopt;
  Matrix m{};
  for (std::size_t i = 0; i < 12; ++i) {
    const auto v = number(w[i]);
    if (!v) return std::nullopt;
    m[i] = *v;
  }
  return m;
}

Matrix compose(const Matrix& a, const Matrix& b) {  // apply a, then b
  Matrix out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      double v = r == 3 ? b[static_cast<std::size_t>(9 + c)] : 0.0;
      for (int k = 0; k < 3; ++k) {
        v += a[static_cast<std::size_t>(r * 3 + k)] * b[static_cast<std::size_t>(k * 3 + c)];
      }
      out[static_cast<std::size_t>(r * 3 + c)] = v;
    }
  }
  return out;
}

core::dvec3 apply(const Matrix& m, const core::dvec3& p) {
  return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
          p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
}

double determinant(const Matrix& m) {
  return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
         m[2] * (m[3] * m[7] - m[4] * m[6]);
}

}  // namespace

// --- writing ----------------------------------------------------------------

core::Result<Bytes> write_stl(const Mesh& mesh) {
  if (mesh.empty()) return Diagnostic::error("mesh.io.empty", "There are no triangles to write");
  for (const Diagnostic& d : mesh.check()) {
    if (d.is_error()) return d;
  }
  Bytes out;
  out.reserve(84 + 50 * mesh.triangle_count());
  std::string header = "binary STL from Volumetric CAD Studio";
  header.resize(80, ' ');
  for (const char c : header) out.push_back(static_cast<std::byte>(c));
  put_u32(out, static_cast<std::uint32_t>(mesh.triangle_count()));
  for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
    const Triangle tri = mesh.triangle(t);
    const core::dvec3 a = core::to_fp64(mesh.positions[tri[0]]);
    const core::dvec3 b = core::to_fp64(mesh.positions[tri[1]]);
    const core::dvec3 c = core::to_fp64(mesh.positions[tri[2]]);
    const core::dvec3 n = core::normalize(core::cross(b - a, c - a));
    put_f32(out, static_cast<float>(n.x));
    put_f32(out, static_cast<float>(n.y));
    put_f32(out, static_cast<float>(n.z));
    for (const std::uint32_t v : tri) {
      put_f32(out, mesh.positions[v].x);
      put_f32(out, mesh.positions[v].y);
      put_f32(out, mesh.positions[v].z);
    }
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
  }
  return out;
}

core::Result<Bytes> write_3mf(std::span<const ThreeMfObject> objects, std::string_view title) {
  std::size_t triangles = 0;
  for (const ThreeMfObject& o : objects) triangles += o.mesh ? o.mesh->triangle_count() : 0;
  if (triangles == 0) return Diagnostic::error("mesh.io.empty", "There are no triangles to write");
  for (const ThreeMfObject& o : objects) {
    if (o.mesh == nullptr) continue;
    for (const Diagnostic& d : o.mesh->check()) {
      if (d.is_error()) return d;
    }
  }

  std::string model;
  model.reserve(128 * triangles);
  model +=
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<model unit=\"millimeter\" xml:lang=\"en-US\" "
      "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
      "xmlns:vstudio=\"urn:volumetric-studio:3mf:2026\">\n";
  if (!title.empty()) model += " <metadata name=\"Title\">" + escape(title) + "</metadata>\n";
  model += " <metadata name=\"Application\">Volumetric CAD Studio</metadata>\n <resources>\n";
  int id = 0;
  for (const ThreeMfObject& o : objects) {
    if (o.mesh == nullptr || o.mesh->empty()) continue;
    ++id;
    fmt::format_to(std::back_inserter(model), "  <object id=\"{}\" type=\"model\" name=\"{}\">\n", id,
                   escape(o.name));
    if (!o.volume.is_null()) {
      model += "   <metadatagroup>\n    <metadata name=\"vstudio:volume\">" + o.volume.to_string() +
               "</metadata>\n   </metadatagroup>\n";
    }
    model += "   <mesh>\n    <vertices>\n";
    // Only the vertices its triangles use, in order of first use.
    std::vector<std::uint32_t> index(o.mesh->vertex_count(), 0xffffffffu);
    std::uint32_t next = 0;
    // Written in place (H5): no string per vertex or triangle.
    std::string tris;
    tris.reserve(64 * o.mesh->triangle_count());
    for (std::size_t t = 0; t < o.mesh->triangle_count(); ++t) {
      Triangle tri = o.mesh->triangle(t);
      for (std::uint32_t& v : tri) {
        if (index[v] == 0xffffffffu) {
          index[v] = next++;
          const fvec3& p = o.mesh->positions[v];
          fmt::format_to(std::back_inserter(model), "     <vertex x=\"{}\" y=\"{}\" z=\"{}\"/>\n", p.x, p.y, p.z);
        }
        v = index[v];
      }
      fmt::format_to(std::back_inserter(tris), "     <triangle v1=\"{}\" v2=\"{}\" v3=\"{}\"/>\n", tri[0], tri[1],
                     tri[2]);
    }
    model += "    </vertices>\n    <triangles>\n";
    model += tris;
    model += "    </triangles>\n   </mesh>\n  </object>\n";
  }
  model += " </resources>\n <build>\n";
  for (int i = 1; i <= id; ++i) model += fmt::format("  <item objectid=\"{}\"/>\n", i);
  model += " </build>\n</model>\n";

  const std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" "
      "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"model\" "
      "ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>\n";
  const std::string rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
      "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>\n";
  return zip_write({{"[Content_Types].xml", types}, {"_rels/.rels", rels}, {"3D/3dmodel.model", model}});
}

// --- reading ------------------------------------------------------------------

core::Result<Mesh> read_stl(std::span<const std::byte> file) {
  Builder b;
  // Binary when the size matches the triangle count; ASCII otherwise.
  bool binary = false;
  if (file.size() >= 84) {
    const std::uint32_t n = get_u32(file, 80);
    binary = file.size() == 84 + 50 * static_cast<std::uint64_t>(n);
  }
  if (binary) {
    const std::uint32_t n = get_u32(file, 80);
    b.reserve(n / 2 + 3, n);
    for (std::uint32_t t = 0; t < n; ++t) {
      const std::size_t at = 84 + 50 * static_cast<std::size_t>(t) + 12;
      std::uint32_t v[3];
      for (std::size_t k = 0; k < 3; ++k) {
        const fvec3 p{get_f32(file, at + 12 * k), get_f32(file, at + 12 * k + 4),
                      get_f32(file, at + 12 * k + 8)};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
          return io_error("stl", fmt::format("triangle {} has a coordinate that isn't a number", t));
        }
        v[k] = b.vertex(p);
      }
      b.triangle(v[0], v[1], v[2]);
    }
  } else {
    const std::string_view text = as_text(file);
    if (text.substr(0, 5) != "solid" && text.find("facet") == std::string_view::npos) {
      return io_error("stl", "neither a binary STL (the size doesn't match its triangle count) nor an ASCII one");
    }
    std::vector<std::uint32_t> corners;
    std::optional<Diagnostic> problem;
    for_each_line(text, [&](std::string_view line, std::size_t number_) {
      if (problem) return;
      const auto w = words(line);
      if (w.empty()) return;
      if (w[0] == "vertex") {
        if (w.size() != 4) {
          problem = io_error("stl", fmt::format("line {}: a vertex needs three coordinates", number_));
          return;
        }
        const auto x = number(w[1]), y = number(w[2]), z = number(w[3]);
        if (!x || !y || !z) {
          problem = io_error("stl", fmt::format("line {}: a coordinate isn't a number", number_));
          return;
        }
        const fvec3 p{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
        if (!finite(p)) {
          problem = io_error("stl", fmt::format("line {}: a coordinate is too large for a float", number_));
          return;
        }
        corners.push_back(b.vertex(p));
      } else if (w[0] == "endloop") {
        if (corners.size() != 3) {
          problem = io_error("stl", fmt::format("line {}: a facet needs three vertices", number_));
          return;
        }
        b.triangle(corners[0], corners[1], corners[2]);
        corners.clear();
      }
    });
    if (problem) return *problem;
  }
  if (b.mesh().empty()) return Diagnostic::error("mesh.io.empty", "The STL file has no triangles");
  return b.take();
}

core::Result<Mesh> read_obj(std::span<const std::byte> file) {
  Builder b;
  std::vector<std::uint32_t> vertices;
  std::optional<Diagnostic> problem;
  for_each_line(as_text(file), [&](std::string_view line, std::size_t number_) {
    if (problem) return;
    const auto w = words(line.substr(0, line.find('#')));  // a comment may end any line
    if (w.empty()) return;
    if (w[0] == "v") {
      if (w.size() < 4) {
        problem = io_error("obj", fmt::format("line {}: a vertex needs three coordinates", number_));
        return;
      }
      const auto x = number(w[1]), y = number(w[2]), z = number(w[3]);
      if (!x || !y || !z) {
        problem = io_error("obj", fmt::format("line {}: a coordinate isn't a number", number_));
        return;
      }
      const fvec3 p{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
      if (!finite(p)) {
        problem = io_error("obj", fmt::format("line {}: a coordinate is too large for a float", number_));
        return;
      }
      vertices.push_back(b.raw_vertex(p));
    } else if (w[0] == "f") {
      std::vector<std::uint32_t> face;
      for (std::size_t i = 1; i < w.size(); ++i) {
        const std::string_view ref = w[i].substr(0, w[i].find('/'));
        long long k = 0;
        const auto [end, ec] = std::from_chars(ref.data(), ref.data() + ref.size(), k);
        const long long n = static_cast<long long>(vertices.size());
        if (ec != std::errc() || end != ref.data() + ref.size() || k == 0 || k > n || k < -n) {
          problem = io_error("obj", fmt::format("line {}: a face refers to a vertex that isn't there", number_));
          return;
        }
        face.push_back(vertices[static_cast<std::size_t>(k > 0 ? k - 1 : n + k)]);
      }
      if (face.size() < 3) {
        problem = io_error("obj", fmt::format("line {}: a face needs three vertices", number_));
        return;
      }
      for (std::size_t i = 1; i + 1 < face.size(); ++i) b.triangle(face[0], face[i], face[i + 1]);
    }
  });
  if (problem) return *problem;
  if (b.mesh().empty()) return Diagnostic::error("mesh.io.empty", "The OBJ file has no faces");
  return b.take();
}

core::Result<Mesh> read_3mf(std::span<const std::byte> file) {
  auto files = zip_read(file);
  if (!files) return files.error();
  // The model part: the package relationship's target, else the usual path.
  std::string part = "3D/3dmodel.model";
  if (const auto rels = files->find("_rels/.rels"); rels != files->end()) {
    scan_xml(rels->second, [&](const Tag& tag) {
      if (tag.name != "Relationship") return true;
      const auto type = tag.attribute("Type");
      const auto target = tag.attribute("Target");
      if (type && target && type->ends_with("/3dmodel")) {
        part = std::string(*target);
        if (!part.empty() && part.front() == '/') part.erase(0, 1);
        return false;
      }
      return true;
    });
  }
  const auto model = files->find(part);
  if (model == files->end()) return io_error("3mf", "the package has no model part ('" + part + "')");

  struct Object {
    std::vector<core::dvec3> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::pair<std::string, Matrix>> components;
  };
  std::map<std::string, Object> objects;
  std::vector<std::pair<std::string, Matrix>> items;
  double scale = 1.0;
  std::string current;
  Object* object = nullptr;  // the object being read, between its tags
  std::optional<Diagnostic> problem;
  const bool scanned = scan_xml(model->second, [&](const Tag& tag) {
    const auto fail = [&](std::string why) {
      problem = io_error("3mf", std::move(why));
      return false;
    };
    if (tag.name == "model" && !tag.closing) {
      const std::string unit(tag.attribute("unit").value_or("millimeter"));
      static const std::map<std::string, double, std::less<>> units{
          {"micron", 0.001}, {"millimeter", 1.0}, {"centimeter", 10.0},
          {"inch", 25.4},    {"foot", 304.8},     {"meter", 1000.0}};
      const auto u = units.find(unit);
      if (u == units.end()) return fail("unknown unit '" + unit + "'");
      scale = u->second;
    } else if (tag.name == "object") {
      if (tag.closing) {
        object = nullptr;
      } else {
        const auto id = tag.attribute("id");
        if (!id) return fail("an object has no id");
        current = std::string(*id);
        Object* o = &objects[current];
        object = tag.self_closing ? nullptr : o;
      }
    } else if (tag.closing) {
      // End tags of vertices, triangles, components and items carry nothing.
    } else if (tag.name == "vertex" && object != nullptr) {
      const auto x = number(tag.attribute("x").value_or("")), y = number(tag.attribute("y").value_or("")),
                 z = number(tag.attribute("z").value_or(""));
      if (!x || !y || !z) return fail("a vertex of object " + current + " has a bad coordinate");
      object->vertices.push_back(core::dvec3{*x, *y, *z} * scale);
    } else if (tag.name == "triangle" && object != nullptr) {
      std::array<std::uint32_t, 3> t{};
      const char* keys[] = {"v1", "v2", "v3"};
      for (std::size_t k = 0; k < 3; ++k) {
        const auto v = number(tag.attribute(keys[k]).value_or(""));
        if (!v || *v < 0 || *v != std::floor(*v) || *v >= 4294967295.0) {
          return fail("a triangle of object " + current + " has a bad vertex index");
        }
        t[k] = static_cast<std::uint32_t>(*v);
      }
      object->triangles.push_back(t);
    } else if ((tag.name == "component" && object != nullptr) || tag.name == "item") {
      const auto id = tag.attribute("objectid");
      if (!id) return fail("a component or build item has no objectid");
      Matrix m = kIdentity;
      if (const auto tr = tag.attribute("transform")) {
        const auto parsed = parse_matrix(*tr);
        if (!parsed) return fail("a transform isn't twelve numbers");
        m = *parsed;
        m[9] *= scale;
        m[10] *= scale;
        m[11] *= scale;
      }
      if (tag.name == "item") {
        items.emplace_back(std::string(*id), m);
      } else {
        object->components.emplace_back(std::string(*id), m);
      }
    }
    return true;
  });
  if (problem) return *problem;
  if (!scanned) return io_error("3mf", "the model part isn't well-formed XML");
  if (items.empty()) {
    for (const auto& [id, o] : objects) {
      if (!o.triangles.empty()) items.emplace_back(id, kIdentity);
    }
  }

  Builder b;
  std::optional<Diagnostic> bad;
  const auto place = [&](auto&& self, const std::string& id, const Matrix& m, int depth) -> void {
    if (bad) return;
    const auto it = objects.find(id);
    if (it == objects.end()) {
      bad = io_error("3mf", "a build item or component refers to object " + id + ", which isn't there");
      return;
    }
    if (depth > 32) {
      bad = io_error("3mf", "components nest too deep (or refer to themselves)");
      return;
    }
    const Object& o = it->second;
    const bool mirror = determinant(m) < 0;
    std::vector<std::uint32_t> map;
    map.reserve(o.vertices.size());
    for (const core::dvec3& p : o.vertices) {
      const fvec3 q = core::to_fp32(apply(m, p));
      if (!finite(q)) {
        bad = io_error("3mf", "a vertex of object " + id + " lies too far away for a float");
        return;
      }
      map.push_back(b.raw_vertex(q));
    }
    for (const auto& t : o.triangles) {
      if (t[0] >= map.size() || t[1] >= map.size() || t[2] >= map.size()) {
        bad = io_error("3mf", "a triangle of object " + id + " refers to a vertex that isn't there");
        return;
      }
      if (mirror) {
        b.triangle(map[t[0]], map[t[2]], map[t[1]]);
      } else {
        b.triangle(map[t[0]], map[t[1]], map[t[2]]);
      }
    }
    for (const auto& [child, cm] : o.components) self(self, child, compose(cm, m), depth + 1);
  };
  for (const auto& [id, m] : items) place(place, id, m, 0);
  if (bad) return *bad;
  if (b.mesh().empty()) return Diagnostic::error("mesh.io.empty", "The 3MF file has no triangles");
  return b.take();
}

bool is_mesh_format(std::string_view format) {
  const std::string f = lower(format);
  return f == "stl" || f == "obj" || f == "3mf";
}

core::Result<Mesh> read_mesh(std::span<const std::byte> file, std::string_view format) {
  const std::string f = lower(format);
  if (f == "stl") return read_stl(file);
  if (f == "obj") return read_obj(file);
  if (f == "3mf") return read_3mf(file);
  return Diagnostic::error("mesh.io.format", "Unknown mesh format '" + std::string(format) + "'");
}

void assign_volume(Mesh& mesh, const core::Id& volume) {
  std::fill(mesh.triangle_volumes.begin(), mesh.triangle_volumes.end(), volume);
}

Mesh extract_volume(const Mesh& mesh, const core::Id& volume) {
  Mesh out;
  std::vector<std::uint32_t> index(mesh.vertex_count(), 0xffffffffu);
  for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
    if (mesh.triangle_volumes[t] != volume) continue;
    Triangle tri = mesh.triangle(t);
    for (std::uint32_t& v : tri) {
      if (index[v] == 0xffffffffu) index[v] = out.add_vertex(mesh.positions[v], mesh.seam_vertices[v]);
      v = index[v];
    }
    out.add_triangle(tri, volume);
  }
  return out;
}

}  // namespace vs::mesh
