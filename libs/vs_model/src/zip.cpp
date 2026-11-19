#include "zip.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>

#include <fmt/format.h>
#include <miniz.h>

namespace vs::model::detail {

namespace {

Diagnostic zip_error(std::string message) {
  return Diagnostic::error("model.project.zip", std::move(message));
}

std::string reason(mz_zip_archive& zip) {
  const char* text = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
  return text ? text : "unknown error";
}

// Ends a reader or writer on every path out.
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

}  // namespace

Result<Blob> zip_store(const ZipEntries& entries) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    return zip_error(fmt::format("can't start the zip archive: {}", reason(zip)));
  }
  const ZipEnd end{&zip, true};
  for (const auto& [name, bytes] : entries) {
    // Level 6, miniz's default; an entry deflate can't shrink is stored.
    if (!mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                               static_cast<mz_uint>(MZ_DEFAULT_LEVEL))) {
      return zip_error(fmt::format("can't add '{}' to the zip archive: {}", name, reason(zip)));
    }
  }
  void* data = nullptr;
  std::size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&zip, &data, &size)) {
    return zip_error(fmt::format("can't finish the zip archive: {}", reason(zip)));
  }
  const std::unique_ptr<void, decltype(&mz_free)> owned(data, &mz_free);
  const auto* first = static_cast<const std::byte*>(data);
  return Blob(first, first + size);
}

Result<std::map<std::string, Blob>> zip_read(std::span<const std::byte> archive) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
    return zip_error(fmt::format("not a readable zip archive: {}", reason(zip)));
  }
  const ZipEnd end{&zip, false};
  std::map<std::string, Blob> files;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      return zip_error(fmt::format("entry {} of the zip archive is damaged: {}", i, reason(zip)));
    }
    if (stat.m_is_directory) continue;
    if (!stat.m_is_supported) {
      return zip_error(fmt::format("'{}' in the zip archive uses an unsupported method "
                                   "or encryption", stat.m_filename));
    }
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
    if (!data) {
      return zip_error(
          fmt::format("can't read '{}' from the zip archive: {}", stat.m_filename, reason(zip)));
    }
    const std::unique_ptr<void, decltype(&mz_free)> owned(data, &mz_free);
    const auto* first = static_cast<const std::byte*>(data);
    files.insert_or_assign(std::string(stat.m_filename), Blob(first, first + size));
  }
  return files;
}

bool looks_like_zip(std::span<const std::byte> bytes) {
  // A local file header, or the end record of an empty archive.
  if (bytes.size() < 4) return false;
  const auto b = [&](std::size_t i) { return std::to_integer<unsigned>(bytes[i]); };
  return b(0) == 'P' && b(1) == 'K' &&
         ((b(2) == 3 && b(3) == 4) || (b(2) == 5 && b(3) == 6));
}

std::uint32_t crc32(std::span<const std::byte> bytes) {
  return static_cast<std::uint32_t>(
      mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

}  // namespace vs::model::detail
