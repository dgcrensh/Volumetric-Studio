#include <catch2/catch_test_macros.hpp>

#include "vs/validate/module.hpp"

TEST_CASE("vs_validate skeleton links", "[validate]") {
  CHECK(vs::validate::module_name() == "vs_validate");
}
