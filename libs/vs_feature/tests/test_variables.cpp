#include <catch2/catch_test_macros.hpp>

#include "vs/feature/variables.hpp"

using namespace vs::feature;

TEST_CASE("variables evaluate in dependency order, whatever their order",
          "[feature][variables]") {
  VariableTable vars;
  // Authored top-down, so each refers only to earlier ones...
  const Id w = vars.add("width", "40 mm").value();
  const Id h = vars.add("half", "width / 2").value();
  const Id q = vars.add("quarter", "half / 2").value();
  auto values = vars.evaluate();
  CHECK(values.diagnostics.empty());
  CHECK(*values.get("quarter") == 10);
  CHECK(vars.dependency_order() == std::vector<Id>{w, h, q});

  // ...then re-pointed so the first depends on the last.
  const Id base = vars.add("base", "8").value();
  REQUIRE(vars.set_expression(w, "base * 5").empty());
  CHECK(vars.dependency_order() == std::vector<Id>{base, w, h, q});
  values = vars.evaluate();
  CHECK(*values.get("width") == 40);
  CHECK(*values.get("quarter") == 10);
  CHECK(values.get("nothing") == std::nullopt);
}

TEST_CASE("authoring errors leave the table unchanged", "[feature][variables]") {
  VariableTable vars;
  const Id a = vars.add("a", "1").value();

  CHECK(vars.add("2bad", "1").error().code == "feature.variable.bad_name");
  CHECK(vars.add("sin", "1").error().code == "feature.variable.bad_name");
  CHECK(vars.add("a", "2").error().code == "feature.variable.duplicate_name");
  CHECK(vars.add("b", "a +").error().code == "feature.expression.syntax");
  CHECK(vars.add("b", "c * 2").error().code == "feature.variable.unknown");
  CHECK(vars.add("b", "1", a).error().code == "feature.variable.duplicate_id");
  CHECK(vars.variables().size() == 1);

  CHECK(vars.set_expression(a, "a + 1")[0].code == "feature.variable.loop");
  CHECK(vars.set_expression(a, "zz")[0].code == "feature.variable.unknown");
  CHECK(vars.set_expression("nope", "1")[0].code == "feature.variable.missing");
  CHECK(vars.find(a)->expression == "1");
  CHECK(vars.check().empty());
}

TEST_CASE("a loop is reported from the edited variable round", "[feature][variables]") {
  VariableTable vars;
  const Id x = vars.add("x", "1").value();
  const Id y = vars.add("y", "x + 1").value();
  // The edit is to x, so the path starts there: x uses y, which uses x.
  const auto problems = vars.set_expression(x, "y * 2");
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].code == "feature.variable.loop");
  CHECK(problems[0].message.find("x \xE2\x86\x92 y \xE2\x86\x92 x") != std::string::npos);
  CHECK(problems[0].subjects[0].id == x);
  CHECK(problems[0].subjects[1].id == y);
}

TEST_CASE("removing a variable fails what used it at evaluation", "[feature][variables]") {
  VariableTable vars;
  const Id a = vars.add("a", "2").value();
  REQUIRE(vars.add("b", "a * 3").ok());
  REQUIRE(vars.add("c", "b + 1").ok());
  REQUIRE(vars.remove(a));
  CHECK_FALSE(vars.remove(a));
  const auto values = vars.evaluate();
  CHECK(values.values.empty());
  REQUIRE(values.diagnostics.size() == 2);
  CHECK(values.diagnostics[0].code == "feature.variable.unknown");
  CHECK(values.diagnostics[0].subjects[0].label == "b");
  CHECK(values.diagnostics[1].code == "feature.variable.upstream_failed");
  CHECK(values.diagnostics[1].subjects[0].label == "c");
}

TEST_CASE("check() finds loops and errors in an unchecked table", "[feature][variables]") {
  // As read from a file, where nothing was checked on the way in.
  VariableTable vars;
  vars.add_unchecked({Id::random(), "p", "q + 1"});
  vars.add_unchecked({Id::random(), "q", "r * 2"});
  vars.add_unchecked({Id::random(), "r", "p - 3"});
  vars.add_unchecked({Id::random(), "s", "4"});
  vars.add_unchecked({Id::random(), "s", "5"});
  vars.add_unchecked({Id::random(), "t", "(("});
  const auto problems = vars.check();
  CHECK(problems.with_code("feature.variable.loop").size() == 1);
  CHECK(problems.with_code("feature.variable.duplicate_name").size() == 1);
  CHECK(problems.with_code("feature.expression.syntax").size() == 1);
  const auto* loop = problems.with_code("feature.variable.loop")[0];
  CHECK(loop->message.find("p \xE2\x86\x92 q \xE2\x86\x92 r \xE2\x86\x92 p") != std::string::npos);

  // Evaluation leaves the loop without values and reports it.
  const auto values = vars.evaluate();
  CHECK(*values.get("s") == 4);
  CHECK_FALSE(values.get("p").has_value());
  CHECK(values.diagnostics.with_code("feature.variable.loop").size() == 1);
}
