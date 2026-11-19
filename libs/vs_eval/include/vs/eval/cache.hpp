#pragma once

// Cache keys and the contents cache (spec, "Cache key"; plan, M9).
//
// A volume's contents are a function of what reaches it through its
// boundary, its connection surfaces and its parameters, plus its seed, its
// content definition and the engine. The key hashes exactly those:
//
//   - the engine version (core::kEngineVersion) and the volume's kind;
//   - the content definition: the static file's blob path (which is its
//     content hash) and format, the parametric feature tree (its JSON), or
//     the procedural graph text;
//   - the boundary after keep-outs (kernel::shape_hash) and its named faces;
//   - each built connection surface joining the volume, in document order:
//     id, type, patch shape and frame, evaluated gap, minimum wall, thread,
//     bolt hole centres and seam spacing;
//   - the evaluated parameters, by name, and the seed;
//   - the published outputs it measures (id, quantity, density).
//
// Static contents never regenerate from parameters or a seed (a file is a
// file), so their keys leave those two out: a parameter on a static volume
// can change without re-reading the file. The boundary stays in, because
// containment is checked against it.
//
// Published outputs are versioned by the key of the contents that produced
// them: a dependant sees a change exactly when that key changes.
//
// The cache holds generated contents by key. In memory it holds everything
// it is given; on disk (optional, a directory such as a folder project's
// cache/) it holds B-rep contents that generated cleanly, as
// "<key>.brep" (kernel::write_brep) plus "<key>.json" (status, outputs,
// diagnostics, output hash). Disk entries are never needed: a missing,
// unreadable or foreign-engine entry is a miss. The feature rebuild result
// of a parametric volume is kept in memory only.

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::eval {

using core::Hash256;

// The key of `document.volumes[index]`, given a build_layout() of the same
// document. None when the volume can't generate: its boundary or a
// parameter failed.
std::optional<Hash256> cache_key(const model::Document& document,
                                 const model::DocumentBuild& layout, std::size_t index);

// Contents made by a generator outside vs_eval (generator.hpp), such as a
// procedural graph's sampled field (vs_proc, M10). Opaque here: the engine
// only shares it and hashes it. Immutable once made.
struct ExtraContents {
  virtual ~ExtraContents() = default;
  // A digest of the contents, for output hashes and the determinism check.
  [[nodiscard]] virtual Hash256 hash() const = 0;
};

// What one generation of a volume produced. Immutable once made, and
// shared between the cache, the engine's state and evaluations.
struct GeneratedContents {
  Hash256 key;
  // Current, or Failed (the contents failed to generate or weren't
  // contained). Failed contents are cached, so the same failing inputs
  // don't regenerate, but are never shown.
  model::VolumeStatus status = model::VolumeStatus::Current;
  kernel::Solid solid;                                    // B-rep contents
  std::optional<mesh::Mesh> mesh;                         // static mesh contents
  std::optional<feature::RebuildResult> rebuild;          // parametric, memory only
  std::shared_ptr<const ExtraContents> extra;             // a generator's, memory only
  std::vector<model::OutputResult> outputs;
  core::Diagnostics diagnostics;  // generation, then containment
  // BLAKE3 of the status, the contents (kernel::shape_hash, or the mesh's
  // positions and triangles, or the extra contents' hash) and the outputs' values: what the
  // determinism check compares.
  Hash256 output_hash;

  [[nodiscard]] bool ok() const { return status == model::VolumeStatus::Current; }
};

using ContentsPtr = std::shared_ptr<const GeneratedContents>;

// Thread-safe.
class ContentsCache {
 public:
  ContentsCache() = default;
  // Also reads and writes `directory` (created when needed).
  explicit ContentsCache(std::filesystem::path directory);

  // From memory, else from disk (then kept in memory). `from_disk`, when
  // given, says which.
  [[nodiscard]] ContentsPtr find(const Hash256& key, bool* from_disk = nullptr);
  // Keeps `contents` in memory, and on disk when it is clean B-rep contents.
  void insert(const ContentsPtr& contents);
  void clear_memory();
  [[nodiscard]] std::size_t memory_size() const;
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

 private:
  mutable std::mutex mutex_;
  std::map<Hash256, ContentsPtr> memory_;
  std::filesystem::path directory_;
};

// The output hash of contents (see GeneratedContents::output_hash).
Hash256 output_hash(model::VolumeStatus status, const kernel::Solid& solid,
                    const mesh::Mesh* mesh, const std::vector<model::OutputResult>& outputs,
                    const ExtraContents* extra = nullptr);

}  // namespace vs::eval
