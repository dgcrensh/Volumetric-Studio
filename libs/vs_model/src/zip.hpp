#pragma once

// Private: the zip reader and writer for .vsproj files, over the vendored
// miniz (third_party/miniz).
//
// Entries are written deflated (stored when deflate doesn't help) with a
// zero DOS timestamp, so the same project always makes the same bytes.
// Reading accepts stored and deflated entries, zip64 included, as written
// by Windows Explorer, 7-Zip, `zip` and the like, and checks each entry's
// CRC-32. Anything else is "model.project.zip". Decision 0006 explains why
// miniz is vendored instead of a vcpkg port.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vs/model/project.hpp"

namespace vs::model::detail {

using ZipEntries = std::vector<std::pair<std::string, std::span<const std::byte>>>;

// The archive's bytes, or "model.project.zip" when an entry can't be added.
Result<Blob> zip_store(const ZipEntries& entries);

// Every file in the archive by name (directories are skipped).
Result<std::map<std::string, Blob>> zip_read(std::span<const std::byte> archive);

// True when the bytes start like a zip archive.
bool looks_like_zip(std::span<const std::byte> bytes);

std::uint32_t crc32(std::span<const std::byte> bytes);

}  // namespace vs::model::detail
