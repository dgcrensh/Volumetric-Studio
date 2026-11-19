#pragma once

// Private: reading a manifest without checking its document, so that
// project.cpp can check the blobs first (a missing blob is a clearer error
// than what the document check would say).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vs/model/project.hpp"

namespace vs::model::detail {

// One entry of the manifest's blob list.
struct BlobEntry {
  std::string path;
  std::uint64_t size = 0;
  std::string blake3;  // 64 hex digits
};

Result<std::pair<Project, std::vector<BlobEntry>>> read_manifest(std::string_view text);

}  // namespace vs::model::detail
