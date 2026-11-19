// The stored-only zip reader and writer behind .vsproj zip files. Its header
// is private, so the test includes it from src/.

#include <catch2/catch_test_macros.hpp>

#include "../src/zip.hpp"

using namespace vs::model;
using namespace vs::model::detail;

namespace {
Blob bytes_of(std::string_view s) {
  Blob b;
  for (char c : s) b.push_back(static_cast<std::byte>(c));
  return b;
}
}  // namespace

TEST_CASE("CRC-32 matches the standard check value", "[model][zip]") {
  CHECK(crc32(bytes_of("123456789")) == 0xCBF43926u);
  CHECK(crc32({}) == 0u);
}

TEST_CASE("Stored zips round-trip, byte for byte the same each time", "[model][zip]") {
  const Blob a = bytes_of("{\"format\": \"vstudio.project\"}");
  const Blob b(1000, std::byte{0x5a});
  const Blob empty;
  const ZipEntries entries{{"project.json", a}, {"blobs/x.step", b}, {"blobs/empty", empty}};
  auto zip = zip_store(entries);
  REQUIRE(zip.ok());
  CHECK(looks_like_zip(*zip));
  auto again = zip_store(entries);
  REQUIRE(again.ok());
  CHECK(*again == *zip);  // fixed timestamps: deterministic

  auto files = zip_read(*zip);
  REQUIRE(files.ok());
  REQUIRE(files->size() == 3);
  CHECK(files->at("project.json") == a);
  CHECK(files->at("blobs/x.step") == b);
  CHECK(files->at("blobs/empty").empty());
}

TEST_CASE("Damaged and compressed zips are refused", "[model][zip]") {
  const Blob a = bytes_of("hello, zip");
  auto zip = zip_store({{"a.txt", a}});
  REQUIRE(zip.ok());

  SECTION("a flipped data byte fails the checksum") {
    Blob bad = *zip;
    bad[30 + 5 + 2] ^= std::byte{1};  // inside the data, after the local header and name
    CHECK(zip_read(bad).error().code == "model.project.zip");
  }
  SECTION("a deflated entry") {
    Blob bad = *zip;
    bad[8] = std::byte{8};  // local header method
    // The central directory's method, 46 + 5 bytes from the end record.
    const std::size_t central = 30 + 5 + a.size();
    bad[central + 10] = std::byte{8};
    CHECK(zip_read(bad).error().code == "model.project.zip_compressed");
  }
  SECTION("not a zip at all") {
    CHECK_FALSE(looks_like_zip(bytes_of("plain text, no zip here")));
    CHECK(zip_read(bytes_of("plain text, no zip here at all")).error().code ==
          "model.project.zip");
  }
}
