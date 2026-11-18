#include <catch2/catch_test_macros.hpp>

#include <set>
#include <unordered_set>

#include "vs/core/id.hpp"

using namespace vs::core;

TEST_CASE("null ids", "[id]") {
  CHECK(Id().is_null());
  CHECK(Id::null() == Id());
  CHECK_FALSE(Id(0, 1).is_null());
  CHECK(Id().to_string() == "00000000000000000000000000000000");
}

TEST_CASE("text form round-trips", "[id]") {
  const Id id(0x0123456789abcdefull, 0xfedcba9876543210ull);
  CHECK(id.to_string() == "0123456789abcdeffedcba9876543210");
  CHECK(Id::parse(id.to_string()) == id);
  CHECK(Id::parse("0123456789ABCDEFFEDCBA9876543210") == id);

  CHECK_FALSE(Id::parse("").has_value());
  CHECK_FALSE(Id::parse("0123456789abcdef").has_value());
  CHECK_FALSE(Id::parse("0123456789abcdeffedcba987654321g").has_value());
  CHECK_FALSE(Id::parse("0123456789abcdeffedcba98765432100").has_value());
}

TEST_CASE("random ids are distinct and never null", "[id]") {
  std::unordered_set<Id> seen;
  for (int i = 0; i < 1000; ++i) {
    const Id id = Id::random();
    CHECK_FALSE(id.is_null());
    seen.insert(id);
  }
  CHECK(seen.size() == 1000);
}

TEST_CASE("derived ids are deterministic and distinct", "[id]") {
  const Id parent(1, 2);
  CHECK(Id::derive(parent, "a") == Id::derive(parent, "a"));
  CHECK(Id::derive(parent, "a") != Id::derive(parent, "b"));
  CHECK(Id::derive(parent, "a") != Id::derive(Id(1, 3), "a"));
  CHECK(Id::derive(parent, 0u) != Id::derive(parent, 1u));
  // Name and index derivations live in separate domains.
  CHECK(Id::derive(parent, 0u) != Id::derive(parent, std::string_view("0")));
}

TEST_CASE("ids order and hash", "[id]") {
  std::set<Id> ordered{Id(2, 0), Id(1, 5), Id(1, 2)};
  CHECK(*ordered.begin() == Id(1, 2));
  CHECK(*ordered.rbegin() == Id(2, 0));
  CHECK(std::hash<Id>{}(Id(1, 2)) == std::hash<Id>{}(Id(1, 2)));
  CHECK(hash_of(Id(1, 2)) == hash_of(std::uint64_t{1}, std::uint64_t{2}));
}
