#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/module.hpp"

TEST_CASE("vs_kernel links", "[kernel]") {
  CHECK(vs::kernel::module_name() == "vs_kernel");
}
