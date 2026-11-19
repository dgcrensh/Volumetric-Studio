#pragma once

// Project files (.vsproj): a document and its binary blobs.
//
// A project is a folder or a zip file with the same entries:
//
//   project.json           the manifest (below)
//   blobs/<hash>.<ext>     imported files, baked meshes: anything binary the
//                          document refers to, named by the first 32 hex
//                          digits of the content's BLAKE3 hash
//   cache/...              optional caches (M9). Never needed: loading
//                          ignores them and saving doesn't write them yet
//
// The manifest:
//
//   {
//     "format": "vstudio.project", "version": 1, "engine": <kEngineVersion>,
//     "document": {
//       "id": "<32 hex>", "name": "Bracket",
//       "layout": { ...a vstudio.features document (vs_feature serialize.hpp)... },
//       "volumes": [{
//         "id": "...", "name": "Base",
//         "boundary": {"features": ["<id>", ...]} | {"box": {"min": <point ref>, "max": <point ref>}},
//         "keep_out": false,
//         "contents": {"kind": "static", "blob": "blobs/...", "format": "step"} |
//                     {"kind": "parametric", "tree": { ...vstudio.features... }} |
//                     {"kind": "procedural", "graph": "<JSON text, kept as it is>"},
//         "parameters": [{"id": "...", "name": "wall", "value": <param>}],
//         "seed": 0,
//         "outputs": [{"id": "...", "name": "mass", "quantity": "mass", "density": 1.24}]
//       }],
//       "connections": [{
//         "id": "...", "name": "Flange", "volumes": ["<id>", "<id>"],
//         "geometry": {"sketch": "<id>"} |
//                     {"datum": {"plane": <plane ref>, "u": <param>, "v": <param>,
//                                "width": <param>, "height": <param>}},
//         "type": "bonded" | "clearance" | "bolted_flange",
//         "gap": <param>,                                  (clearance)
//         "holes": {"circle": {"count", "diameter", "start_angle"}} |
//                  {"grid": {"columns", "rows", "pitch_x", "pitch_y"}},
//                  with "u" and "v",                       (bolted flange)
//         "thread": "M4", "min_wall": <param>,             (bolted flange)
//         "seam": {"spacing": 0}
//       }],
//       "keep_outs": [{"id": "...", "name": "...", "cutters": [...], "volumes": [...]}],
//       "links": [{"id": "...", "source": {"volume": "<id>", "output": "<id>"},
//                  "target": "<id>"}]
//     },
//     "blobs": [{"path": "blobs/<hash>.step", "size": 1234, "blake3": "<64 hex>"}]
//   }
//
// <param>, <plane ref> and <point ref> are as in vs_feature's format. Keys
// at their defaults may be left out when reading.
//
// Versioning. "version" is the format version, 1 from the start. A reader
// accepts every version up to its own and rejects newer ones
// ("model.project.version"); when the format changes, the version goes up
// and the reader keeps reading the old versions. "engine" records the
// engine that wrote the file, for M9's caches.
//
// Reading checks structure and types ("model.json.*"), then every blob's
// size and hash ("model.project.blob_missing", "model.project.blob_corrupt"),
// then the document as if authored (check.hpp): a file that breaks
// encapsulation or holds a dependency loop is rejected with that error.
// Other failures: "model.project.io" (can't read or write),
// "model.project.not_project" (no project.json), "model.project.zip".

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/hash.hpp"
#include "vs/model/document.hpp"

namespace vs::model {

inline constexpr int kProjectFormatVersion = 1;

using Blob = std::vector<std::byte>;
// Blobs by path within the project ("blobs/<hash>.<ext>").
using BlobStore = std::map<std::string, Blob, std::less<>>;

struct Project {
  Document document;
  BlobStore blobs;

  // Stores `bytes` and returns its path, "blobs/<32 hex>.<extension>". The
  // same bytes always get the same path, so storing twice keeps one copy.
  std::string add_blob(Blob bytes, std::string_view extension);

  friend bool operator==(const Project&, const Project&) = default;
};

// The document as JSON (the manifest's "document"), and back. Reading checks
// the document (check.hpp) and rejects it on the first error.
std::string to_json(const Document& document);
Result<Document> document_from_json(std::string_view text);

// The whole manifest, and back (blobs are listed, not included; reading
// returns the project with an empty blob store).
std::string manifest_json(const Project& project);
Result<Project> project_from_manifest(std::string_view text);

// Content hashes: the BLAKE3 hash of the document's JSON (whose writer is
// deterministic: fixed key order, shortest round-trip numbers), and of a
// project, which adds each blob's path and bytes in path order. Equal
// documents hash alike; saving and loading keeps the hash.
core::Hash256 content_hash(const Document& document);
core::Hash256 content_hash(const Project& project);

enum class ProjectLayout { Folder, Zip };

// Writes the project to `path` as a folder or a zip file. A folder is
// written in place: project.json and the blobs are replaced, blobs the
// project no longer holds are removed, and anything else (cache/) is left
// alone. A zip is written to a temporary file and moved over `path`.
Diagnostics save_project(const Project& project, const std::filesystem::path& path,
                         ProjectLayout layout);

// Reads a folder or zip project, whichever `path` is.
Result<Project> load_project(const std::filesystem::path& path);

}  // namespace vs::model
