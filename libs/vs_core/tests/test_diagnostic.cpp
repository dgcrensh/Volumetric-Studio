#include <catch2/catch_test_macros.hpp>

#include <string>

#include "vs/core/diagnostic.hpp"

using namespace vs::core;

TEST_CASE("diagnostics carry severity, code, message and subjects", "[diagnostic]") {
  const Id a(0, 1);
  const Id b(0, 2);
  const auto d = Diagnostic::error(
      "validate.overlap", "Sibling volumes overlap by 2 mm",
      {{SubjectKind::Volume, a, "Housing"}, {SubjectKind::Volume, b, "Lid"}});

  CHECK(d.is_error());
  CHECK(d.code == "validate.overlap");
  REQUIRE(d.target() != nullptr);
  CHECK(d.target()->id == a);  // the first subject is the "act here" target
  CHECK(d.to_string() ==
        "error [validate.overlap] Sibling volumes overlap by 2 mm (Volume 'Housing', Volume 'Lid')");

  const auto w = Diagnostic::warning("validate.contact", "Touching without a connection");
  CHECK_FALSE(w.is_error());
  CHECK(w.target() == nullptr);
  CHECK(w.to_string() == "warning [validate.contact] Touching without a connection");

  const auto unlabeled = Diagnostic::info("x.y", "z", {{SubjectKind::Node, Id(0, 255), ""}});
  CHECK(unlabeled.to_string() == "info [x.y] z (Node 000000000000000000000000000000ff)");
}

TEST_CASE("diagnostic collections", "[diagnostic]") {
  Diagnostics list;
  CHECK(list.empty());
  CHECK_FALSE(list.has_errors());

  list.add(Diagnostic::warning("a", "first"));
  CHECK_FALSE(list.has_errors());
  list.add(Diagnostic::error("b", "second"));
  list.add(Diagnostic::error("b", "third"));
  CHECK(list.has_errors());
  CHECK(list.size() == 3);
  CHECK(list.count(Severity::Error) == 2);
  CHECK(list.count(Severity::Warning) == 1);
  CHECK(list.with_code("b").size() == 2);
  CHECK(list[0].message == "first");

  Diagnostics more;
  more.add(Diagnostic::info("c", "fourth"));
  list.append(more);
  CHECK(list.size() == 4);
  CHECK(list.items().back().code == "c");
}

TEST_CASE("Result holds a value or an error", "[diagnostic]") {
  Result<int> good = 42;
  REQUIRE(good.ok());
  CHECK(*good == 42);
  CHECK(static_cast<bool>(good));

  Result<std::string> bad = Diagnostic::error("kernel.boolean", "Boolean failed");
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().code == "kernel.boolean");

  Result<std::string> text = std::string("hello");
  CHECK(text->size() == 5);
}
