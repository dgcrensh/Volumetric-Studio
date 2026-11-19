#include <catch2/catch_test_macros.hpp>

#include "vs/proc/module.hpp"

TEST_CASE("vs_proc links", "[proc]") {
  CHECK(vs::proc::module_name() == "vs_proc");
}
