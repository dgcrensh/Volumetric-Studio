#pragma once

// Paths and files for the commands (H4: one to_path() for every command).

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace vs::cli {

// A command-line argument as a path. Arguments are UTF-8 on every
// platform, so they go through u8string (on Windows a plain string would
// be read in the ANSI code page).
inline std::filesystem::path to_path(std::string_view arg) {
  return std::filesystem::path(std::u8string(arg.begin(), arg.end()));
}

// Writes `bytes` to `path`, replacing it. On failure (opening, writing or
// closing, such as a full disk) says so on `err` and returns false.
inline bool write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes,
                        std::ostream& err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) {
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
  }
  if (!f) {
    const auto u = path.generic_u8string();
    err << "vstudio-cli: can't write " << std::string(u.begin(), u.end()) << "\n";
    return false;
  }
  return true;
}

}  // namespace vs::cli
