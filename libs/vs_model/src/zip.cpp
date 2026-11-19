#include "zip.hpp"

#include <array>
#include <cstdint>
#include <limits>

#include <fmt/format.h>

namespace vs::model::detail {

namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndOfCentral = 0x06054b50;
constexpr std::uint16_t kVersion = 20;     // 2.0: plain files
constexpr std::uint16_t kUtf8Names = 0x0800;  // general purpose bit 11
constexpr std::uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;  // 1980-01-01
constexpr std::uint16_t kDosTime = 0;

std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}

class Writer {
 public:
  Blob bytes;
  void u16(std::uint16_t v) {
    bytes.push_back(static_cast<std::byte>(v & 0xff));
    bytes.push_back(static_cast<std::byte>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v & 0xffff));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void raw(std::span<const std::byte> b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
  void text(const std::string& s) { raw(std::as_bytes(std::span(s.data(), s.size()))); }
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> b) : b_(b) {}
  [[nodiscard]] bool has(std::size_t at, std::size_t n) const {
    return at <= b_.size() && n <= b_.size() - at;
  }
  [[nodiscard]] std::uint16_t u16(std::size_t at) const {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b_[at]) |
                                      (std::to_integer<unsigned>(b_[at + 1]) << 8));
  }
  [[nodiscard]] std::uint32_t u32(std::size_t at) const {
    return static_cast<std::uint32_t>(u16(at)) | (static_cast<std::uint32_t>(u16(at + 2)) << 16);
  }
  [[nodiscard]] std::span<const std::byte> span(std::size_t at, std::size_t n) const {
    return b_.subspan(at, n);
  }
  [[nodiscard]] std::size_t size() const { return b_.size(); }

 private:
  std::span<const std::byte> b_;
};

Diagnostic zip_error(std::string message) {
  return Diagnostic::error("model.project.zip", std::move(message));
}

}  // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) {
  static const auto table = make_table();
  std::uint32_t c = 0xffffffffu;
  for (const std::byte b : bytes) {
    c = table[(c ^ std::to_integer<std::uint32_t>(b)) & 0xff] ^ (c >> 8);
  }
  return c ^ 0xffffffffu;
}

bool looks_like_zip(std::span<const std::byte> bytes) {
  if (bytes.size() < 4) return false;
  const Reader r(bytes);
  return r.u32(0) == kLocalHeader || r.u32(0) == kEndOfCentral;
}

Result<Blob> zip_store(const ZipEntries& entries) {
  constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
  if (entries.size() >= 0xffff) return zip_error("too many files for a zip archive");
  Writer w;
  struct Central {
    std::uint32_t crc, size, offset;
  };
  std::vector<Central> central;
  for (const auto& [name, data] : entries) {
    if (data.size() >= kMax || w.bytes.size() >= kMax || name.size() >= 0xffff) {
      return zip_error(fmt::format("'{}' is too large for a zip archive", name));
    }
    const Central c{crc32(data), static_cast<std::uint32_t>(data.size()),
                    static_cast<std::uint32_t>(w.bytes.size())};
    central.push_back(c);
    w.u32(kLocalHeader);
    w.u16(kVersion);
    w.u16(kUtf8Names);
    w.u16(0);  // stored
    w.u16(kDosTime);
    w.u16(kDosDate);
    w.u32(c.crc);
    w.u32(c.size);
    w.u32(c.size);
    w.u16(static_cast<std::uint16_t>(name.size()));
    w.u16(0);  // extra
    w.text(name);
    w.raw(data);
  }
  if (w.bytes.size() >= kMax) return zip_error("the project is too large for a zip archive");
  const auto dir_start = static_cast<std::uint32_t>(w.bytes.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& name = entries[i].first;
    const Central& c = central[i];
    w.u32(kCentralHeader);
    w.u16(kVersion);  // made by
    w.u16(kVersion);  // needed
    w.u16(kUtf8Names);
    w.u16(0);
    w.u16(kDosTime);
    w.u16(kDosDate);
    w.u32(c.crc);
    w.u32(c.size);
    w.u32(c.size);
    w.u16(static_cast<std::uint16_t>(name.size()));
    w.u16(0);  // extra
    w.u16(0);  // comment
    w.u16(0);  // disk
    w.u16(0);  // internal attributes
    w.u32(0);  // external attributes
    w.u32(c.offset);
    w.text(name);
  }
  if (w.bytes.size() >= kMax) return zip_error("the project is too large for a zip archive");
  const auto dir_size = static_cast<std::uint32_t>(w.bytes.size()) - dir_start;
  w.u32(kEndOfCentral);
  w.u16(0);
  w.u16(0);
  w.u16(static_cast<std::uint16_t>(entries.size()));
  w.u16(static_cast<std::uint16_t>(entries.size()));
  w.u32(dir_size);
  w.u32(dir_start);
  w.u16(0);  // comment
  return std::move(w.bytes);
}

Result<std::map<std::string, Blob>> zip_read(std::span<const std::byte> archive) {
  const Reader r(archive);
  // The end-of-central-directory record: 22 bytes plus a comment of up to 64 KiB.
  if (r.size() < 22) return zip_error("not a zip archive (too short)");
  std::size_t eocd = r.size() - 22;
  const std::size_t lowest = r.size() > 22 + 0xffff ? r.size() - 22 - 0xffff : 0;
  while (r.u32(eocd) != kEndOfCentral) {
    if (eocd == lowest) return zip_error("not a zip archive (no central directory)");
    --eocd;
  }
  const std::size_t count = r.u16(eocd + 10);
  const std::size_t dir_size = r.u32(eocd + 12);
  const std::size_t dir_start = r.u32(eocd + 16);
  if (count == 0xffff || dir_start == 0xffffffffu) {
    return zip_error("zip64 archives aren't supported");
  }
  if (!r.has(dir_start, dir_size)) return zip_error("the zip's central directory is damaged");

  std::map<std::string, Blob> out;
  std::size_t at = dir_start;
  for (std::size_t i = 0; i < count; ++i) {
    if (!r.has(at, 46) || r.u32(at) != kCentralHeader) {
      return zip_error("the zip's central directory is damaged");
    }
    const std::uint16_t flags = r.u16(at + 8);
    const std::uint16_t method = r.u16(at + 10);
    const std::uint32_t crc = r.u32(at + 16);
    const std::size_t packed = r.u32(at + 20);
    const std::size_t size = r.u32(at + 24);
    const std::size_t name_len = r.u16(at + 28);
    const std::size_t extra_len = r.u16(at + 30);
    const std::size_t comment_len = r.u16(at + 32);
    const std::size_t local = r.u32(at + 42);
    if (!r.has(at + 46, name_len + extra_len + comment_len)) {
      return zip_error("the zip's central directory is damaged");
    }
    const auto name_bytes = r.span(at + 46, name_len);
    std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
    at += 46 + name_len + extra_len + comment_len;

    if (!name.empty() && name.back() == '/') continue;  // a directory
    if (flags & 0x1) return zip_error(fmt::format("'{}' in the zip is encrypted", name));
    if (method != 0) {
      return Diagnostic::error(
          "model.project.zip_compressed",
          fmt::format("'{}' in the zip is compressed; the studio reads only uncompressed "
                      "(\"stored\") zips. Save the project from the studio, or unzip it and open "
                      "the folder",
                      name));
    }
    if (packed != size) return zip_error(fmt::format("'{}' in the zip is damaged", name));
    if (!r.has(local, 30) || r.u32(local) != kLocalHeader) {
      return zip_error(fmt::format("'{}' in the zip is damaged", name));
    }
    const std::size_t data_at = local + 30 + r.u16(local + 26) + r.u16(local + 28);
    if (!r.has(data_at, size)) return zip_error(fmt::format("'{}' in the zip is cut short", name));
    const auto data = r.span(data_at, size);
    if (crc32(data) != crc) {
      return zip_error(fmt::format("'{}' in the zip fails its checksum", name));
    }
    out.insert_or_assign(std::move(name), Blob(data.begin(), data.end()));
  }
  return out;
}

}  // namespace vs::model::detail
