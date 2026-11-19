#pragma once

// Private: a minimal zip reader and writer for .vsproj files.
//
// Entries are written "stored" (no compression), with a fixed timestamp
// (1980-01-01 00:00), so the same project always makes the same bytes.
// Reading accepts stored entries and checks each one's CRC-32; deflated
// entries (what most zip tools make) are refused with
// "model.project.zip_compressed", and so are zip64 archives. Decision 0005
// explains why there is no compression library.

#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vs/model/project.hpp"

namespace vs::model::detail {

using ZipEntries = std::vector<std::pair<std::string, std::span<const std::byte>>>;

// The archive's bytes, or "model.project.zip" when an entry or the whole
// archive is too large for a plain (non-zip64) zip.
Result<Blob> zip_store(const ZipEntries& entries);

// Every file in the archive by name (directories are skipped).
Result<std::map<std::string, Blob>> zip_read(std::span<const std::byte> archive);

// True when the bytes start like a zip archive.
bool looks_like_zip(std::span<const std::byte> bytes);

std::uint32_t crc32(std::span<const std::byte> bytes);

}  // namespace vs::model::detail
