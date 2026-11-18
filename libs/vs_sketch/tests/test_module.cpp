#include <catch2/catch_test_macros.hpp>

#include "vs/sketch/module.hpp"

TEST_CASE("vs_sketch skeleton links", "[sketch]") {
  CHECK(vs::sketch::module_name() == "vs_sketch");
}
