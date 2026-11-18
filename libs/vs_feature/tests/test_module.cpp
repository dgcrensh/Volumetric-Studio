#include <catch2/catch_test_macros.hpp>

#include "vs/feature/module.hpp"

TEST_CASE("vs_feature skeleton links", "[feature]") {
  CHECK(vs::feature::module_name() == "vs_feature");
}
