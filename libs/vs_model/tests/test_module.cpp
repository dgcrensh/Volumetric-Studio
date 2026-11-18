#include <catch2/catch_test_macros.hpp>

#include "vs/model/module.hpp"

TEST_CASE("vs_model skeleton links", "[model]") {
  CHECK(vs::model::module_name() == "vs_model");
}
