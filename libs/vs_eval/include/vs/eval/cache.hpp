#pragma once

// Cache keys and the contents cache (spec, "Cache key"; plan, M9).
//
// A volume's contents are a function of what reaches it through its
// boundary, its connection surfaces and its parameters, plus its seed, its
// content definition and the engine. The key hashes exactly those:
//
//   - the engine version (core::kEngineVersion), the ProductSettings (H3)
//     and the volume's kind;
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
// cache/) it holds contents that generated cleanly, as "<key>.brep"
// (kernel::write_brep) or, since M17, "<key>.mesh" (FP32 positions, seam
// vertex ids, indices and triangle volumes, bit for bit), plus
// "<key>.json" (status, outputs, diagnostics, output hash). Disk entries
// are never needed: a missing, unreadable or foreign-engine entry is a
// miss. The feature rebuild result of a parametric volume and a
// generator's extra contents are kept in memory only.
//
// The same files travel inside a project (M17, model::CacheFiles):
// encode_entry() writes one entry's files, insert_files() reads every entry
// in a set of files into memory.

#include <cstddef>
#include <filesystem>
#include <list>
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
#include "vs/model/project.hpp"

namespace vs::eval {

using core::Hash256;

// The key of `document.volumes[index]`, given a build_layout() of the same
// document with `settings`. None when the volume can't generate: its
// boundary or a parameter failed. The settings are part of the key (H3):
// the tolerance and working space change what generation accepts.
std::optional<Hash256> cache_key(const model::Document& document,
                                 const model::DocumentBuild& layout, std::size_t index,
                                 const core::ProductSettings& settings);

// kernel::shape_hash() of a layout build's boundaries and patches, each
// worked out the first time it is asked for and kept: what cache_key()
// spends most of its time on. The engine keeps one with each layout it
// keeps (decision 0029). Not thread-safe; the layout must not change.
class LayoutShapeHashes {
 public:
  explicit LayoutShapeHashes(std::shared_ptr<const model::DocumentBuild> layout);
  [[nodiscard]] Hash256 boundary(std::size_t volume) const;
  [[nodiscard]] Hash256 patch(std::size_t connection) const;

 private:
  std::shared_ptr<const model::DocumentBuild> layout_;
  mutable std::vector<std::optional<Hash256>> boundaries_;
  mutable std::vector<std::optional<Hash256>> patches_;
};

// cache_key() with the shape hashes of the same `layout` (or of an equal
// one) given: the same key.
std::optional<Hash256> cache_key(const model::Document& document,
                                 const model::DocumentBuild& layout, std::size_t index,
                                 const core::ProductSettings& settings,
                                 const LayoutShapeHashes& hashes);

// Contents made by a generator outside vs_eval (generator.hpp), such as a
// procedural graph's sampled field (vs_proc, M10). Opaque here: the engine
// only shares it and hashes it. Immutable once made.
struct ExtraContents {
  virtual ~ExtraContents() = default;
  // A digest of the contents, for output hashes and the determinism check.
  [[nodiscard]] virtual Hash256 hash() const = 0;
  // About how many bytes it holds, for the memory cache's budget (H5).
  [[nodiscard]] virtual std::size_t memory_bytes() const { return 0; }
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
  std::optional<mesh::Mesh> mesh;                         // static or generated mesh contents
  std::optional<feature::RebuildResult> rebuild;          // parametric, memory only
  std::shared_ptr<const ExtraContents> extra;             // a generator's, memory only
  std::vector<model::OutputResult> outputs;
  core::Diagnostics diagnostics;  // generation, then containment
  // BLAKE3 of the status, the contents (kernel::shape_hash, or the mesh's
  // positions and triangles, or the extra contents' hash) and the outputs' values: what the
  // determinism check compares.
  Hash256 output_hash;
  // Set by a generator's job that stopped early because the evaluation was
  // cancelled (M16): the engine drops it, neither shown nor cached, and the
  // volume keeps its last contents, Stale.
  bool cancelled = false;

  [[nodiscard]] bool ok() const { return status == model::VolumeStatus::Current; }
};

using ContentsPtr = std::shared_ptr<const GeneratedContents>;

// About how many bytes `contents` holds: its mesh's arrays, the extra
// contents' memory_bytes(), and a fixed estimate for a solid and for a
// parametric rebuild (OCCT doesn't say how big a shape is).
inline constexpr std::size_t kSolidEstimateBytes = 256 * 1024;
std::size_t contents_bytes(const GeneratedContents& contents);

// Generated contents by key, in memory and optionally on disk.
// Thread-safe.
//
// Memory is bounded (H5): entries are counted by contents_bytes(), and once
// they add up to more than the budget the least recently found or inserted
// ones are forgotten (the newest entry always stays, however big). A
// forgotten entry is still on disk when there is a directory; otherwise
// it regenerates when next needed. The evaluations holding it keep it.
class ContentsCache {
 public:
  inline static constexpr std::size_t kDefaultMemoryBudget = std::size_t{2} << 30;  // 2 GiB

  explicit ContentsCache(std::size_t memory_budget = kDefaultMemoryBudget)
      : memory_budget_(memory_budget) {}
  // Also reads and writes `directory` (created when needed).
  explicit ContentsCache(std::filesystem::path directory,
                         std::size_t memory_budget = kDefaultMemoryBudget);

  // From memory, else from disk (then kept in memory). `from_disk`, when
  // given, says which.
  [[nodiscard]] ContentsPtr find(const Hash256& key, bool* from_disk = nullptr);
  // Keeps `contents` in memory, and on disk when cacheable() (below).
  void insert(const ContentsPtr& contents);
  // Reads every entry in `files` (encode_entry()'s) into memory, skipping
  // damaged, foreign-engine and already held ones. Returns how many it
  // added.
  std::size_t insert_files(const model::CacheFiles& files);
  void clear_memory();
  // Entries in memory, and the bytes they count for.
  [[nodiscard]] std::size_t memory_size() const;
  [[nodiscard]] std::size_t memory_bytes() const;
  [[nodiscard]] std::size_t memory_budget() const { return memory_budget_; }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

 private:
  struct Entry {
    ContentsPtr contents;
    std::size_t bytes = 0;
    std::list<Hash256>::iterator use;  // in uses_
  };

  // Callers hold mutex_.
  ContentsPtr keep(const Hash256& key, ContentsPtr contents, bool replace);
  void touch(Entry& entry);

  mutable std::mutex mutex_;
  std::map<Hash256, Entry> memory_;
  std::list<Hash256> uses_;  // most recently used first
  std::size_t bytes_ = 0;
  std::size_t memory_budget_ = kDefaultMemoryBudget;
  std::filesystem::path directory_;
};

// Contents that can be stored as files: they generated cleanly (Current,
// not cancelled) into a solid or a mesh.
bool cacheable(const GeneratedContents& contents);
// Adds `contents`' files to `files` ("<key>.json" and "<key>.brep" or
// "<key>.mesh"); false (nothing added) when it isn't cacheable().
bool encode_entry(const GeneratedContents& contents, model::CacheFiles& files);
// The entry for `key` in `files`, or null when it is missing or unusable.
ContentsPtr decode_entry(const model::CacheFiles& files, const Hash256& key);

// The output hash of contents (see GeneratedContents::output_hash).
Hash256 output_hash(model::VolumeStatus status, const kernel::Solid& solid,
                    const mesh::Mesh* mesh, const std::vector<model::OutputResult>& outputs,
                    const ExtraContents* extra = nullptr);

}  // namespace vs::eval
