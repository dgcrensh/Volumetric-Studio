#include "selftest.hpp"

#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/core.hpp"
#include "vs/eval/module.hpp"
#include "vs/feature/module.hpp"
#include "vs/kernel/module.hpp"
#include "vs/kernel/kernel.hpp"
#include "vs/mesh/module.hpp"
#include "vs/model/module.hpp"
#include "vs/proc/module.hpp"
#include "vs/sketch/module.hpp"
#include "vs/sketch/solver.hpp"
#include "vs/validate/module.hpp"

namespace vs::cli {

namespace {

using namespace vs::core;

struct Check {
  std::string_view name;
  std::function<bool()> run;
};

// Each check is cheap and self-contained. The full suites live in the
// Catch2 tests; selftest is a smoke test of an installed binary.
std::vector<Check> checks() {
  return {
      {"blake3 reference vector",
       [] {
         return blake3_of_bytes({}).to_hex() ==
                "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
       }},
      {"hash encoding golden value",
       [] {
         // Mirrors the "id" case in vs_core's test_hash_golden.cpp.
         return hash_of(Id(0x0123456789abcdefull, 0xfedcba9876543210ull)).to_hex() ==
                "1640eaddd0b1ec57f30285858f5552c02714eb98b210c74a9fe085b70000f86e";
       }},
      {"float canonicalisation", [] { return hash_of(0.0) == hash_of(-0.0); }},
      {"default product settings are valid",
       [] { return ProductSettings::defaults().validate().empty(); }},
      {"transform inverse round-trip",
       [] {
         const auto t = dtransform::translate({5, -3, 2}) * dtransform::rotate({1, 2, 3}, 0.7) *
                        dtransform::scale({2, 3, 4});
         const auto inv = inverse(t);
         const dvec3 p{0.25, -7.5, 100};
         return inv && approx_equal(inv->apply_point(t.apply_point(p)), p, 1e-9);
       }},
      {"id text round-trip",
       [] {
         const Id id = Id::random();
         return Id::parse(id.to_string()) == id;
       }},
      {"kernel: unit box volume",
       [] {
         const auto box = vs::kernel::make_box(dbox3::from_min_max({0, 0, 0}, {1, 2, 3}),
                                               ProductSettings::defaults());
         if (!box) return false;
         const auto mass = vs::kernel::mass_properties(*box);
         return mass && std::abs(mass->volume - 6.0) < 1e-9;
       }},
      {"sketch: dimensioned line solves",
       [] {
         vs::sketch::Sketch s;
         const auto a = s.add_point({0, 0});
         const auto b = s.add_point({4, 1});
         const auto line = s.add_line(a, b);
         s.fix(a);
         s.horizontal(line);
         s.length(line, 10);
         const auto report = vs::sketch::solve(s, ProductSettings::defaults());
         return report.status == vs::sketch::SolveStatus::Solved &&
                std::abs(s.point(b)->x - 10.0) < 1e-9;
       }},
      {"all modules linked",
       [] {
         return vs::kernel::module_name() == "vs_kernel" &&
                vs::sketch::module_name() == "vs_sketch" &&
                vs::feature::module_name() == "vs_feature" &&
                vs::model::module_name() == "vs_model" &&
                vs::validate::module_name() == "vs_validate" &&
                vs::mesh::module_name() == "vs_mesh" && vs::eval::module_name() == "vs_eval" &&
                vs::proc::module_name() == "vs_proc";
       }},
  };
}

}  // namespace

bool run_selftest(std::ostream& out) {
  std::size_t passed = 0;
  const auto all = checks();
  for (const auto& check : all) {
    bool ok = false;
    try {
      ok = check.run();
    } catch (const std::exception& e) {
      out << "  exception: " << e.what() << "\n";
    }
    out << (ok ? "  pass  " : "  FAIL  ") << check.name << "\n";
    if (ok) ++passed;
  }
  out << "selftest: " << passed << "/" << all.size() << " checks passed\n";
  return passed == all.size();
}

}  // namespace vs::cli
