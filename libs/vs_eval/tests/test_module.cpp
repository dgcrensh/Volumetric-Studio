#include <catch2/catch_test_macros.hpp>

#include "vs/eval/module.hpp"

TEST_CASE("vs_eval skeleton links", "[eval]") {
  CHECK(vs::eval::module_name() == "vs_eval");
}
