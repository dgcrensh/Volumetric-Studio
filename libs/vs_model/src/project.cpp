// Project folders and zip files (project.hpp).

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <system_error>
#include <thread>

#include <fmt/format.h>

#include "manifest.hpp"
#include "zip.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"

namespace vs::model {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kManifest = "project.json";
constexpr std::string_view kCache = "cache";

// A cache file name: no folders, nothing that climbs out of cache/.
bool plain_name(std::string_view name) {
  return !name.empty() && name.find_first_of("/\\:") == std::string_view::npos && name != "." &&
         name.find("..") == std::string_view::npos;
}

std::string display(const fs::path& p) {
  const auto u = p.generic_u8string();
  return {u.begin(), u.end()};
}

SubjectRef file_subject(const fs::path& p) { return {core::SubjectKind::File, {}, display(p)}; }

Diagnostic io_error(const fs::path& p, std::string message) {
  return Diagnostic::error("model.project.io", fmt::format("{}: {}", display(p), message),
                           {file_subject(p)});
}

Result<Blob> read_file(const fs::path& p) {
  // Only files: a folder opens as a stream on some systems and "reads" as
  // an absurd size (H4).
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return io_error(p, "not a file");
  std::ifstream in(p, std::ios::binary);
  if (!in) return io_error(p, "can't open it for reading");
  Blob bytes;
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) return io_error(p, "can't read it");
  in.seekg(0, std::ios::beg);
  bytes.resize(static_cast<std::size_t>(size));
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    return io_error(p, "can't read it");
  }
  return bytes;
}

// A temporary name next to `p`, unique to this write (H4: two saves of one
// file used to share "<name>.tmp" and rename it from under each other).
fs::path temporary_for(const fs::path& p) {
  const std::string tag = core::Id::random().to_string().substr(0, 16);
  return p.parent_path() / (p.filename().u8string() + u8"." +
                            std::u8string(tag.begin(), tag.end()) + u8".tmp");
}

// Writes next to `p`, then moves it into place, so a failed save never
// leaves half a file.
Diagnostics write_file(const fs::path& p, std::span<const std::byte> bytes) {
  Diagnostics out;
  const fs::path tmp = temporary_for(p);
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      out.add(io_error(p, "can't open it for writing"));
      return out;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();  // flushes: a full disk shows here
    if (!f) {
      fs::remove(tmp, ec);
      out.add(io_error(p, "can't write it"));
      return out;
    }
  }
  // Windows refuses to replace a file while another rename is replacing
  // it (or a scanner holds it open) with "access denied"; that clears in
  // moments, so try again a few times before giving up.
  for (int attempt = 0; attempt < 50; ++attempt) {
    fs::rename(tmp, p, ec);
    if (!ec) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2 + attempt));
  }
  if (ec) {
    fs::remove(tmp, ec);
    out.add(io_error(p, "can't replace it"));
  }
  return out;
}

std::span<const std::byte> bytes_of(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

// Checks the blobs a manifest lists against what was read, and that every
// static volume's blob is there.
Result<Project> finish(std::pair<Project, std::vector<detail::BlobEntry>> read,
                       const std::function<std::optional<Blob>(const std::string&)>& fetch,
                       const fs::path& p, bool check_document) {
  Project& project = read.first;
  for (const auto& entry : read.second) {
    auto bytes = fetch(entry.path);
    if (!bytes) {
      return Diagnostic::error("model.project.blob_missing",
                               fmt::format("{}: the blob '{}' listed in the manifest is missing",
                                           display(p), entry.path),
                               {file_subject(p)});
    }
    if (bytes->size() != entry.size || core::blake3_of_bytes(*bytes).to_hex() != entry.blake3) {
      return Diagnostic::error("model.project.blob_corrupt",
                               fmt::format("{}: the blob '{}' doesn't match the manifest's size "
                                           "and hash; it was changed or damaged",
                                           display(p), entry.path),
                               {file_subject(p)});
    }
    project.blobs.emplace(entry.path, std::move(*bytes));
  }
  for (const auto& v : project.document.volumes) {
    const auto* s = std::get_if<StaticContents>(&v.contents);
    if (s && !s->blob.empty() && !project.blobs.contains(s->blob)) {
      return Diagnostic::error(
          "model.project.blob_missing",
          fmt::format("{}: the file '{}' of volume '{}' isn't in the project", display(p),
                      s->blob, v.name),
          {project.document.volume_subject(v.id), file_subject(p)});
    }
  }
  if (check_document) {
    for (const auto& d : check(project.document)) {
      if (d.is_error()) return d;
    }
  }
  return std::move(project);
}

}  // namespace

std::string Project::add_blob(Blob bytes, std::string_view extension) {
  const std::string hex = core::blake3_of_bytes(bytes).to_hex().substr(0, 32);
  std::string path = extension.empty() ? fmt::format("blobs/{}", hex)
                                       : fmt::format("blobs/{}.{}", hex, extension);
  blobs.insert_or_assign(path, std::move(bytes));
  return path;
}

Diagnostics save_project(const Project& project, const fs::path& path, ProjectLayout layout,
                         const CacheFiles* cache) {
  Diagnostics out;
  const std::string manifest = manifest_json(project);
  if (layout == ProjectLayout::Zip) {
    detail::ZipEntries entries;
    entries.emplace_back(std::string(kManifest), bytes_of(manifest));
    for (const auto& [name, bytes] : project.blobs) entries.emplace_back(name, bytes);
    if (cache != nullptr) {
      for (const auto& [name, bytes] : *cache) {
        if (plain_name(name)) entries.emplace_back(fmt::format("{}/{}", kCache, name), bytes);
      }
    }
    auto zip = detail::zip_store(entries);
    if (!zip) {
      Diagnostic d = zip.error();
      d.message = fmt::format("{}: {}", display(path), d.message);
      d.subjects.push_back(file_subject(path));
      out.add(std::move(d));
      return out;
    }
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
      out.add(io_error(path, "a folder is in the way of the zip file"));
      return out;
    }
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    return write_file(path, *zip);
  }

  std::error_code ec;
  if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
    out.add(io_error(path, "a file is in the way of the project folder"));
    return out;
  }
  fs::create_directories(path / "blobs", ec);
  if (ec) {
    out.add(io_error(path, "can't create the project folder"));
    return out;
  }
  for (const auto& [name, bytes] : project.blobs) {
    out.append(write_file(path / fs::path(name), bytes));
    if (out.has_errors()) return out;
  }
  // Blobs the project no longer holds.
  for (const auto& entry : fs::directory_iterator(path / "blobs", ec)) {
    const std::string name = "blobs/" + display(entry.path().filename());
    if (entry.is_regular_file(ec) && !project.blobs.contains(name)) fs::remove(entry.path(), ec);
  }
  // The manifest last, so a reader never sees it before its blobs.
  out.append(write_file(path / kManifest, bytes_of(manifest)));
  if (cache != nullptr && !out.has_errors()) {
    // Caches are never needed, so failing to write one isn't an error.
    const fs::path dir = path / kCache;
    fs::create_directories(dir, ec);
    for (const auto& [name, bytes] : *cache) {
      if (plain_name(name)) (void)write_file(dir / fs::path(name), bytes);
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_regular_file(ec) && !cache->contains(display(entry.path().filename()))) {
        fs::remove(entry.path(), ec);
      }
    }
  }
  return out;
}

CacheFiles load_project_cache(const fs::path& path) {
  CacheFiles out;
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    for (const auto& entry : fs::directory_iterator(path / kCache, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const std::string name = display(entry.path().filename());
      if (name.ends_with(".tmp")) continue;  // a write that didn't finish
      auto bytes = read_file(entry.path());
      if (bytes) out.emplace(name, std::move(*bytes));
    }
    return out;
  }
  if (!fs::is_regular_file(path, ec)) return out;
  auto bytes = read_file(path);
  if (!bytes || !detail::looks_like_zip(*bytes)) return out;
  auto files = detail::zip_read(*bytes);
  if (!files) return out;
  const std::string prefix = fmt::format("{}/", kCache);
  for (auto& [name, data] : *files) {
    if (name.starts_with(prefix) && plain_name(std::string_view(name).substr(prefix.size()))) {
      out.emplace(name.substr(prefix.size()), std::move(data));
    }
  }
  return out;
}

Result<Project> load_project(const fs::path& path, bool check_document) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    const fs::path manifest_path = path / kManifest;
    if (!fs::exists(manifest_path, ec)) {
      return Diagnostic::error("model.project.not_project",
                               fmt::format("{}: no {} in the folder", display(path), kManifest),
                               {file_subject(path)});
    }
    auto text = read_file(manifest_path);
    if (!text) return text.error();
    auto read = detail::read_manifest(
        std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
    if (!read) return read.error();
    return finish(std::move(*read),
                  [&](const std::string& name) -> std::optional<Blob> {
                    auto bytes = read_file(path / fs::path(name));
                    if (!bytes) return std::nullopt;
                    return std::move(*bytes);
                  },
                  path, check_document);
  }

  if (!fs::exists(path, ec)) return io_error(path, "no such file or folder");
  auto bytes = read_file(path);
  if (!bytes) return bytes.error();
  if (!detail::looks_like_zip(*bytes)) {
    return Diagnostic::error("model.project.not_project",
                             fmt::format("{}: neither a project folder nor a zip file",
                                         display(path)),
                             {file_subject(path)});
  }
  auto files = detail::zip_read(*bytes);
  if (!files) {
    Diagnostic d = files.error();
    d.message = fmt::format("{}: {}", display(path), d.message);
    d.subjects.push_back(file_subject(path));
    return d;
  }
  const auto manifest = files->find(std::string(kManifest));
  if (manifest == files->end()) {
    return Diagnostic::error("model.project.not_project",
                             fmt::format("{}: no {} in the zip", display(path), kManifest),
                             {file_subject(path)});
  }
  auto read = detail::read_manifest(std::string_view(
      reinterpret_cast<const char*>(manifest->second.data()), manifest->second.size()));
  if (!read) return read.error();
  return finish(
      std::move(*read),
      [&](const std::string& name) -> std::optional<Blob> {
        const auto it = files->find(name);
        if (it == files->end()) return std::nullopt;
        return it->second;
      },
      path, check_document);
}

}  // namespace vs::model
