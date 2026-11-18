#include <catch2/catch_test_macros.hpp>

#include "vs/mesh/module.hpp"

TEST_CASE("vs_mesh skeleton links", "[mesh]") {
  CHECK(vs::mesh::module_name() == "vs_mesh");
}
