// The diagnostics registry: codes, severities and one place to act.

#include <set>

#include <catch2/catch_test_macros.hpp>

#include "vs/feature/tree.hpp"
#include "vs/validate/registry.hpp"

using namespace vs::validate;
using vs::core::Diagnostic;
using vs::core::SubjectKind;
using vs::core::SubjectRef;
using vs::model::Document;

TEST_CASE("Registered codes are unique, owned by vs_validate and described",
          "[validate][registry]") {
  std::set<std::string_view> seen;
  for (const auto& c : registered_codes()) {
    INFO(c.code);
    CHECK(c.code.starts_with("validate."));
    CHECK(seen.insert(c.code).second);
    CHECK_FALSE(c.summary.empty());
    CHECK(find_code(c.code) == &c);
  }
  CHECK(find_code("validate.no_such_code") == nullptr);
  CHECK(find_code("validate.overlap")->severity == vs::core::Severity::Error);
  CHECK(find_code("validate.contact")->severity == vs::core::Severity::Warning);
  CHECK(find_code("validate.containment")->act_on == ActOn::Contents);
  CHECK(find_code("validate.connection.edge_wall")->act_on == ActOn::ConnectionSurface);
}

TEST_CASE("Every diagnostic routes to exactly one place", "[validate][registry]") {
  Document doc = vs::model::make_document("Routing");
  // A volume with parametric contents: one feature and one variable inside.
  vs::model::Volume v;
  v.id = Id::random();
  v.name = "Part";
  vs::model::ParametricContents contents;
  const Id inner_var = contents.tree.variables().add("t", "2 mm").value();
  const Id inner_plane = contents.tree.add(vs::feature::DatumPlaneFeature{
      vs::feature::PlaneOffset{vs::feature::PrincipalPlane::XY, vs::feature::Param::of(1)}});
  v.contents = contents;
  doc.volumes.push_back(v);
  const Id layout_var = doc.layout.variables().add("w", "10 mm").value();
  const Id layout_plane = doc.layout.add(vs::feature::DatumPlaneFeature{
      vs::feature::PlaneOffset{vs::feature::PrincipalPlane::XY, vs::feature::Param::of(1)}});

  auto at = [&](std::string code, SubjectKind kind, const Id& id) {
    return route(Diagnostic::error(std::move(code), "m", {SubjectRef{kind, id, "x"}}), doc);
  };

  SECTION("registered codes use their entry") {
    const auto t = at("validate.containment", SubjectKind::Volume, v.id);
    CHECK(t.on == ActOn::Contents);
    CHECK(t.volume == v.id);
    CHECK(at("validate.overlap", SubjectKind::Boundary, v.id).on == ActOn::Layout);
    CHECK(at("validate.connection.gap", SubjectKind::ConnectionSurface, Id::random()).on ==
          ActOn::ConnectionSurface);
  }
  SECTION("features: in the layout, or in a volume's contents") {
    const auto layout = at("feature.extrude.failed", SubjectKind::Feature, layout_plane);
    CHECK(layout.on == ActOn::Feature);
    CHECK(layout.volume.is_null());
    const auto inner = at("feature.extrude.failed", SubjectKind::Feature, inner_plane);
    CHECK(inner.on == ActOn::Feature);
    CHECK(inner.volume == v.id);
    CHECK(at("sketch.solve.conflicting", SubjectKind::Constraint, Id::random()).on ==
          ActOn::Feature);
  }
  SECTION("variables: the layout's, or a volume's") {
    CHECK(at("feature.variable.loop", SubjectKind::Variable, layout_var).on == ActOn::Layout);
    const auto inner = at("feature.variable.loop", SubjectKind::Variable, inner_var);
    CHECK(inner.on == ActOn::Feature);
    CHECK(inner.volume == v.id);
  }
  SECTION("surfaces, nodes, contents and the rest") {
    CHECK(at("model.connection.bad_size", SubjectKind::ConnectionSurface, Id::random()).on ==
          ActOn::ConnectionSurface);
    CHECK(at("proc.node.failed", SubjectKind::Node, Id::random()).on == ActOn::Node);
    const auto contents_failed = at("model.contents.failed", SubjectKind::Volume, v.id);
    CHECK(contents_failed.on == ActOn::Contents);
    CHECK(contents_failed.volume == v.id);
    CHECK(at("model.volume.duplicate_name", SubjectKind::Volume, v.id).on == ActOn::Layout);
    CHECK(at("model.dependency.loop", SubjectKind::Link, Id::random()).on == ActOn::Layout);
    CHECK(at("model.reference.contents", SubjectKind::KeepOut, Id::random()).on ==
          ActOn::Layout);
  }
  SECTION("no subject: the layout as a whole") {
    const auto t = route(Diagnostic::error("core.settings.tolerance", "m"), doc);
    CHECK(t.on == ActOn::Layout);
    CHECK(t.subject.kind == SubjectKind::Layout);
  }
}
