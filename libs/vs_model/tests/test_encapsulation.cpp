// M5 acceptance: every attempt to reference a volume's contents from outside
// is rejected with a diagnostic, at authoring time (spec, design rule 1).

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/check.hpp"
#include "vs/model/references.hpp"

using namespace vs::model;
using bracket::codes;
using vs::feature::ExtrudeFeature;
using vs::feature::FaceRef;
using vs::feature::FeatureRef;
using vs::feature::SketchEntityRef;
using vs::feature::SketchFeature;

namespace {

// Applies `change`, expects it refused with `code`, naming `first` first
// (when null: a feature or variable, made inside `change`), and the document
// unchanged.
void refused(bracket::Bracket& b, const std::function<void(Document&)>& change,
             const std::string& code, const Id& first) {
  const Document before = b.project.document;
  const auto d = edit(b.project.document, change);
  INFO(codes(d));
  REQUIRE(d.has_errors());
  const auto found = d.with_code(code);
  REQUIRE(found.size() >= 1);
  REQUIRE(found[0]->target());
  if (first.is_null()) {
    const auto kind = found[0]->target()->kind;
    CHECK((kind == vs::core::SubjectKind::Feature || kind == vs::core::SubjectKind::Variable));
  } else {
    CHECK(found[0]->target()->id == first);
  }
  CHECK(b.project.document == before);
}

// An id of something inside the Base's contents: its sketch, a sketch
// entity, its extrusion.
Id inner_entity(const bracket::Bracket& b) {
  const auto* sk = b.project.document.find_volume(b.base)->tree()->sketch(b.inner_sketch);
  return sk->entities().front().id;
}

}  // namespace

TEST_CASE("The layout can't refer into a volume's contents", "[model][encapsulation]") {
  auto b = bracket::make_bracket();
  Id sketch_id;

  SECTION("a layout sketch placed on the contents' sketch") {
    refused(
        b,
        [&](Document& d) {
          vs::sketch::Sketch s(Id::random(), "Sneaky");
          sketch_id = d.layout.add(SketchFeature{s, FeatureRef{b.inner_sketch}});
        },
        "model.reference.contents", Id{});
  }
  SECTION("a layout extrude of the contents' sketch") {
    refused(
        b,
        [&](Document& d) {
          ExtrudeFeature e;
          e.sketch = b.inner_sketch;
          d.layout.add(e, "Sneaky extrude");
        },
        "model.reference.contents", Id{});
  }
  SECTION("a layout fillet on a face of the contents") {
    refused(
        b,
        [&](Document& d) {
          vs::feature::FilletFeature f;
          f.edges.push_back({FaceRef{b.inner_extrude, "end"}, std::nullopt});
          d.layout.add(f, "Sneaky fillet");
        },
        "model.reference.contents", Id{});
  }
  SECTION("a layout datum axis along a line of the contents") {
    refused(
        b,
        [&](Document& d) {
          vs::feature::DatumAxisFeature a;
          a.definition = vs::feature::AxisAlong{SketchEntityRef{b.inner_sketch, inner_entity(b)}};
          d.layout.add(a, "Sneaky axis");
        },
        "model.reference.contents", Id{});
  }
}

TEST_CASE("Nothing else outside a volume can refer into its contents", "[model][encapsulation]") {
  auto b = bracket::make_bracket();

  SECTION("a boundary made from the contents' extrusion") {
    refused(
        b,
        [&](Document& d) {
          d.find_volume(b.upright)->boundary = FeatureBoundary{{b.inner_extrude}};
        },
        "model.reference.contents", b.upright);
  }
  SECTION("a box boundary cornered on a point of the contents") {
    refused(
        b,
        [&](Document& d) {
          d.find_volume(b.upright)->boundary =
              BoxBoundary{SketchEntityRef{b.inner_sketch, inner_entity(b)},
                          SketchEntityRef{b.inner_sketch, inner_entity(b)}};
        },
        "model.reference.contents", b.upright);
  }
  SECTION("a keep-out cutting with the contents' extrusion") {
    const Id k = Id::random();
    refused(
        b,
        [&](Document& d) {
          d.keep_outs.push_back({k, "Sneaky keep-out", {b.inner_extrude}, {b.upright}});
        },
        "model.reference.contents", k);
  }
  SECTION("a connection surface patched from the contents' sketch") {
    refused(
        b,
        [&](Document& d) { d.find_connection(b.flange)->geometry = SketchPatch{b.inner_sketch}; },
        "model.reference.contents", b.flange);
  }
  SECTION("a connection surface on a plane of the contents") {
    refused(
        b,
        [&](Document& d) {
          DatumPatch dp;
          dp.plane = FaceRef{b.inner_extrude, "end"};
          d.find_connection(b.flange)->geometry = dp;
        },
        "model.reference.contents", b.flange);
  }
  SECTION("a link driving a variable inside the contents") {
    const Id inner_var =
        b.project.document.find_volume(b.base)->tree()->variables().add("x", "1").value();
    refused(
        b, [&](Document& d) { d.links[0].target = inner_var; }, "model.reference.contents",
        b.link);
  }
  SECTION("a layout variable using a name only the contents know") {
    REQUIRE(b.project.document.find_volume(b.base)->tree()->variables().add("secret", "1").ok());
    refused(
        b,
        [&](Document& d) {
          d.layout.variables().add_unchecked({Id::random(), "leak", "secret * 2"});
        },
        "model.reference.contents", Id{});
  }
  SECTION("a volume parameter using a name only another volume's contents know") {
    REQUIRE(b.project.document.find_volume(b.base)->tree()->variables().add("secret", "1").ok());
    refused(
        b,
        [&](Document& d) {
          d.find_volume(b.upright)->parameters[0].value = Param::expr("secret");
        },
        "model.reference.contents", b.upright_load);
  }
  SECTION("another volume's contents referring into these") {
    // Give the Upright a tree of its own that reaches into the Base's.
    refused(
        b,
        [&](Document& d) {
          Volume& up = *d.find_volume(b.upright);
          ParametricContents pc;
          ExtrudeFeature e;
          e.sketch = b.inner_sketch;
          pc.tree.add(e, "Sneaky");
          up.contents = std::move(pc);
        },
        "model.reference.contents", Id{});
  }
}

TEST_CASE("Contents see only their own boundary and connection surfaces",
          "[model][encapsulation]") {
  auto b = bracket::make_bracket();
  auto add_inner = [&](Document& d, vs::feature::FeatureDefinition def) {
    d.find_volume(b.base)->tree()->add(std::move(def), "Inner");
  };

  SECTION("not the layout's geometry") {
    refused(
        b,
        [&](Document& d) {
          vs::sketch::Sketch s(Id::random(), "On the layout");
          add_inner(d, SketchFeature{s, FeatureRef{b.top_plane}});
        },
        "model.reference.parent", Id{});
  }
  SECTION("not the layout's variables") {
    refused(
        b,
        [&](Document& d) {
          auto* f = d.find_volume(b.base)->tree()->find(b.inner_extrude);
          std::get<ExtrudeFeature>(f->definition).depth = Param::expr("base_h");
        },
        "model.reference.parent", b.inner_extrude);
  }
  SECTION("not a sibling's boundary") {
    refused(
        b,
        [&](Document& d) {
          vs::sketch::Sketch s(Id::random(), "On the neighbour");
          add_inner(d, SketchFeature{s, FaceRef{b.upright, "x:end"}});
        },
        "model.reference.sibling", Id{});
  }
  SECTION("not a connection surface that doesn't join it") {
    const Id other = Id::random();
    refused(
        b,
        [&](Document& d) {
          ConnectionSurface c;
          c.id = other;
          c.name = "Upright top";
          c.geometry = DatumPatch{};
          c.volumes = {b.upright};
          d.connections.push_back(c);
          vs::sketch::Sketch s(Id::random(), "On the neighbour's surface");
          add_inner(d, SketchFeature{s, FeatureRef{other}});
        },
        "model.reference.not_input", Id{});
  }
  SECTION("its own boundary and connection surfaces are fine") {
    const auto d = edit(b.project.document, [&](Document& x) {
      vs::sketch::Sketch s1(Id::random(), "On the boundary");
      add_inner(x, SketchFeature{s1, FaceRef{b.base, b.base_block.to_string() + ":start"}});
      vs::sketch::Sketch s2(Id::random(), "On the flange");
      add_inner(x, SketchFeature{s2, FaceRef{b.flange, "patch"}});
    });
    INFO(codes(d));
    CHECK_FALSE(d.has_errors());
  }
}

TEST_CASE("resolve() classifies ids by scope", "[model][encapsulation]") {
  const auto b = bracket::make_bracket();
  const auto& doc = b.project.document;
  const auto layout = Scope::layout();
  const auto base = Scope::inside(b.base);
  const auto upright = Scope::inside(b.upright);

  CHECK(classify(doc, b.inner_extrude).kind == TargetKind::InContents);
  CHECK(classify(doc, b.inner_extrude).owner == b.base);
  CHECK(classify(doc, b.base_block).kind == TargetKind::LayoutFeature);
  CHECK(classify(doc, b.width).kind == TargetKind::LayoutVariable);
  CHECK(classify(doc, b.base_mass).kind == TargetKind::PublishedOutput);
  CHECK(classify(doc, Id::random()).kind == TargetKind::Unknown);

  // The three typed references cross the boundary; contents never do.
  CHECK(resolve(doc, layout, BoundaryRef{b.base}).ok());
  CHECK(resolve(doc, layout, ConnectionSurfaceRef{b.flange}).ok());
  CHECK(resolve(doc, layout, PublishedOutputRef{b.base, b.base_mass}).ok());
  CHECK(resolve(doc, layout, b.inner_extrude).error().code == "model.reference.contents");
  CHECK(resolve(doc, upright, b.inner_extrude).error().code == "model.reference.contents");
  CHECK(resolve(doc, base, b.inner_extrude).ok());

  // From inside: own boundary and surfaces, not the neighbour's.
  CHECK(resolve(doc, base, BoundaryRef{b.base}).ok());
  CHECK(resolve(doc, base, BoundaryRef{b.upright}).error().code == "model.reference.sibling");
  CHECK(resolve(doc, base, ConnectionSurfaceRef{b.flange}).ok());
  CHECK(resolve(doc, base, b.base_block).error().code == "model.reference.parent");
  CHECK(resolve(doc, base, b.upright_load).error().code == "model.reference.sibling");
  CHECK(resolve(doc, base, PublishedOutputRef{b.upright, b.upright_envelope}).error().code ==
        "model.reference.not_input");
  CHECK(resolve(doc, layout, PublishedOutputRef{b.base, b.upright_envelope}).error().code ==
        "model.reference.broken");
}
