// The layout checks' box path (decision 0032) against OCCT's: random
// boxes placed to overlap, touch face to face, along an edge or at a
// corner, or miss, by amounts on both sides of the tolerance. Both paths
// must report the same codes, subjects and face names, with numbers within
// the tolerance; an edge or corner touch may be placed anywhere along where
// the boxes meet.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "curved.hpp"
#include "fixtures.hpp"
#include "nested.hpp"
#include "rack.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/generated.hpp"
#include "vs/validate/layout.hpp"

using namespace vs::model;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::Id;

namespace {

// A small, portable generator: the same numbers on every standard library.
struct Random {
  std::uint64_t state;
  std::uint64_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  template <typename T>
  const T& pick(const std::vector<T>& from) {
    return from[next() % from.size()];
  }
};

double at(const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
void set(dvec3& v, int axis, double value) { (axis == 0 ? v.x : axis == 1 ? v.y : v.z) = value; }

// The Rack made 100 x 100 x 10 mm, with `boxes` as its generated children.
Document rack_of(const std::vector<dbox3>& boxes,
                 const std::vector<GeneratedSurface>& surfaces = {}) {
  rack::Rack r = rack::make_rack();
  auto& layout = r.project.document.layout;
  for (const auto& f : std::vector<vs::feature::Feature>(layout.features())) {
    if (f.name != "Base high" && f.name != "Rack high") continue;
    auto* p = layout.find(f.id)->as<vs::feature::DatumPointFeature>();
    auto& point = std::get<vs::feature::PointAt>(p->definition);
    point.x = vs::feature::Param::of(100.0);
    point.y = vs::feature::Param::of(100.0);
  }
  GeneratedLayout g;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    GeneratedChild c;
    c.key = Id::derive(Id{}, "box " + std::to_string(i));
    c.name = "Box " + std::to_string(i);
    c.template_name = "Cell";
    c.box = boxes[i];
    g.children.push_back(std::move(c));
  }
  g.surfaces = surfaces;
  g.delegations = {{r.seat, 0}};
  auto expanded = expand(r.project.document, r.rack, g);
  REQUIRE(expanded);
  return std::move(*expanded);
}

// A surface between boxes 0 and 1 on a side of box 0 (nudged off it, or
// into it), spanning that side, part of it or more.
GeneratedSurface surface_on(const std::vector<dbox3>& boxes, Random& rnd) {
  static const std::vector<double> nudges{0.0, 0.0, 4e-7, -4e-7, 1e-5, -1e-5, 0.1, -0.3};
  static const std::vector<double> margins{0.5, 0.5, 0.0, -0.5};
  const dbox3& a = boxes[0];
  GeneratedSurface s;
  s.key = Id::derive(Id{}, "surface");
  s.name = "Join";
  s.a = 0;
  s.b = 1;
  if (rnd.next() % 4 == 0) {
    s.type = ConnectionType::Clearance;
    s.gap = 0.2;
  }
  s.axis = static_cast<int>(rnd.next() % 3);
  const bool high = rnd.next() % 2 == 0;
  s.offset = (high ? at(a.max, s.axis) : at(a.min, s.axis)) + rnd.pick(nudges);
  // x plane: y then z; y plane: z then x; z plane: x then y.
  const int u = (s.axis + 1) % 3;
  const int v = (s.axis + 2) % 3;
  const double margin = rnd.pick(margins);
  s.lo = {at(a.min, u) + margin, at(a.min, v) + margin};
  s.hi = {at(a.max, u) - margin, at(a.max, v) - margin};
  return s;
}

// A box next to `a` along each axis: overlapping, flush (by an amount
// around the tolerance), or apart.
dbox3 neighbour(const dbox3& a, Random& rnd) {
  static const std::vector<double> sizes{1.0, 2.5, 4.0, 7.0};
  static const std::vector<double> nudges{0.0, 4e-7, -4e-7, 1e-5, -1e-5, 1e-2, -1e-2};
  dvec3 lo, hi;
  for (int axis = 0; axis < 3; ++axis) {
    const double size = axis == 2 ? rnd.pick(sizes) / 2.0 : rnd.pick(sizes);
    const double nudge = rnd.pick(nudges);
    double start = 0;
    switch (rnd.next() % 4) {
      case 0:  // flush past a's high side
        start = at(a.max, axis) + nudge;
        break;
      case 1:  // flush before a's low side
        start = at(a.min, axis) - size - nudge;
        break;
      case 2:  // overlapping
        start = at(a.min, axis) + (at(a.max, axis) - at(a.min, axis)) / 2.0 - size / 2.0 + nudge;
        break;
      default:  // sharing a's low side
        start = at(a.min, axis) + nudge;
        break;
    }
    set(lo, axis, start);
    set(hi, axis, start + size);
  }
  return dbox3::from_min_max(lo, hi);
}

std::string fmt_box(const dbox3& b) {
  char text[256];
  std::snprintf(text, sizeof text, "(%.9f %.9f %.9f) (%.9f %.9f %.9f)", b.min.x, b.min.y, b.min.z,
                b.max.x, b.max.y, b.max.z);
  return text;
}

// The surface of a box plus its edges' length: what a volume (or a flat
// region's area) can move by, per unit of tolerance, when OCCT snaps its
// sides.
double surface(const dbox3& b) {
  const dvec3 s = b.size();
  return 2.0 * (s.x * s.y + s.y * s.z + s.z * s.x) + 2.0 * (s.x + s.y + s.z);
}

bool near(double a, double b, double slack) { return std::abs(a - b) <= slack; }

bool near(const dbox3& a, const dbox3& b, double slack) {
  for (int axis = 0; axis < 3; ++axis) {
    if (!near(at(a.min, axis), at(b.min, axis), slack) ||
        !near(at(a.max, axis), at(b.max, axis), slack)) {
      return false;
    }
  }
  return true;
}

dbox3 box_of(const Document& doc, const DocumentBuild& build, const Id& volume) {
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    if (doc.volumes[i].id == volume) return vs::kernel::bounding_box(build.volumes[i].boundary);
  }
  return dbox3::empty();
}

// A message with its numbers left out, and "reaches ... inside" read as
// "is ... from": a patch wholly inside a box and parallel to its nearest
// side is as deep everywhere as at its nearest point, a tie OCCT's
// rounding breaks either way.
std::string words(const std::string& message) {
  std::string out;
  for (std::size_t i = 0; i < message.size(); ++i) {
    const char c = message[i];
    const bool digit = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e';
    const bool in_word = i > 0 && message[i - 1] != ' ' && !(message[i - 1] >= '0' && message[i - 1] <= '9');
    if (digit && !(c == 'e' && in_word) && !(c == '.' && i + 1 == message.size())) {
      if (out.empty() || out.back() != '#') out += '#';
      continue;
    }
    out += c;
  }
  const std::string reaches = " reaches # mm inside ";
  const auto at_reaches = out.find(reaches);
  if (at_reaches != std::string::npos) out.replace(at_reaches, reaches.size(), " is # mm from ");
  return out;
}

// Whether two sides, or a patch and a side, are nearly but not quite the
// tolerance apart: OCCT's fuzzy booleans and distances decide such pairs
// either way (a 1.2e-6 mm overlap read as a touch, sides 1.2e-6 mm apart
// snapped together), so the comparison leaves them out.
bool in_fuzzy_band(const std::vector<dbox3>& boxes, const std::vector<GeneratedSurface>& surfaces,
                   double tol) {
  const auto band = [tol](double d) {
    const double a = std::abs(d);
    return a > 0.5 * tol && a < 3.0 * tol;
  };
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    for (std::size_t j = i + 1; j < boxes.size(); ++j) {
      for (int axis = 0; axis < 3; ++axis) {
        for (const double a : {at(boxes[i].min, axis), at(boxes[i].max, axis)}) {
          for (const double b : {at(boxes[j].min, axis), at(boxes[j].max, axis)}) {
            if (band(a - b)) return true;
          }
        }
      }
    }
  }
  for (const auto& s : surfaces) {
    for (const std::size_t k : {s.a, s.b}) {
      if (band(s.offset - at(boxes[k].min, s.axis)) || band(s.offset - at(boxes[k].max, s.axis))) {
        return true;
      }
    }
  }
  return false;
}

std::string codes(const vs::validate::LayoutReport& r) {
  std::string out;
  for (const auto& d : r.diagnostics) {
    out += d.code;
    for (const auto& s : d.subjects) out += " " + s.id.to_string() + " " + s.label;
    out += "\n";
  }
  return out;
}

}  // namespace

TEST_CASE("The box path reports what OCCT does on random boxes", "[validate][box]") {
  const auto settings = vs::core::ProductSettings::defaults();
  const double tol = settings.linear_tolerance_mm;
  Random rnd{0x0032};
  int overlaps = 0, faces = 0, edges = 0, off_boundary = 0, skipped = 0;
  const int trials = 300;
  for (int trial = 0; trial < trials; ++trial) {
    INFO("trial " << trial);
    const double width = rnd.pick(std::vector<double>{3.0, 5.0, 8.0});
    const dbox3 first = dbox3::from_min_max({40, 40, 3}, {40 + width, 47, 6.5});
    std::vector<dbox3> boxes{first};
    const std::size_t count = 2 + rnd.next() % 3;
    while (boxes.size() < count) boxes.push_back(neighbour(rnd.pick(boxes), rnd));
    std::vector<GeneratedSurface> surfaces;
    if (rnd.next() % 2 == 0) surfaces.push_back(surface_on(boxes, rnd));
    if (in_fuzzy_band(boxes, surfaces, tol)) {
      ++skipped;
      continue;
    }
    const Document doc = rack_of(boxes, surfaces);
    const DocumentBuild build = build_layout(doc, settings);

    const auto fast = vs::validate::validate_layout(doc, build, settings);
    const auto occt = vs::validate::validate_layout(doc, build, settings, {}, {false});
    CHECK(fast.pairs_tested == occt.pairs_tested);
    for (const auto& b : boxes) {
      UNSCOPED_INFO(fmt_box(b));
    }
    REQUIRE(codes(fast) == codes(occt));
    for (const auto& d : fast.diagnostics.with_code("validate.connection.off_boundary")) {
      if (d->subjects.front().label == "Join") ++off_boundary;  // the Seat is always off
    }
    // The connection rules' messages are the same, numbers aside for a
    // patch off its boundary: a patch wholly inside a box is as far from
    // its surface as its nearest point, which kernel::min_distance()
    // doesn't always find (1.33 mm for 0.5 mm, decision 0032).
    for (std::size_t k = 0; k < fast.diagnostics.size(); ++k) {
      const auto& d = fast.diagnostics[k];
      if (d.code == "validate.connection.off_boundary") {
        CHECK(words(d.message) == words(occt.diagnostics[k].message));
      } else if (d.code.rfind("validate.connection.", 0) == 0) {
        CHECK(d.message == occt.diagnostics[k].message);
      }
    }

    REQUIRE(fast.overlaps.size() == occt.overlaps.size());
    for (std::size_t k = 0; k < fast.overlaps.size(); ++k) {
      const auto& f = fast.overlaps[k];
      const auto& o = occt.overlaps[k];
      CHECK(f.a == o.a);
      CHECK(f.b == o.b);
      CHECK(near(f.depth, o.depth, tol));
      // OCCT's fuzzy boolean snaps sides closer than the tolerance: a
      // volume may move by the tolerance times the region's surface.
      CHECK(near(f.volume, o.volume, tol * surface(o.region)));
      CHECK(near(f.region, o.region, tol));
      ++overlaps;
    }
    REQUIRE(fast.contacts.size() == occt.contacts.size());
    for (std::size_t k = 0; k < fast.contacts.size(); ++k) {
      const auto& f = fast.contacts[k];
      const auto& o = occt.contacts[k];
      CHECK(f.a == o.a);
      CHECK(f.b == o.b);
      CHECK(f.face_a == o.face_a);
      CHECK(f.face_b == o.face_b);
      CHECK(near(f.area, o.area, tol * surface(o.region)));
      if (f.face_a.empty()) {
        // Anywhere along where the boxes meet: both reports' points lie
        // on both boxes, within the tolerance.
        const dbox3 box_a = box_of(doc, build, f.a);
        const dbox3 box_b = box_of(doc, build, f.b);
        for (const dbox3& region : {f.region, o.region}) {
          CHECK(box_a.contains(region, tol));
          CHECK(box_b.contains(region, tol));
        }
        ++edges;
      } else {
        CHECK(near(f.region, o.region, tol));
        ++faces;
      }
    }
  }
  // The trials reach every outcome.
  CHECK(overlaps > 10);
  CHECK(faces > 10);
  CHECK(edges > 10);
  CHECK(off_boundary > 10);
  CHECK(skipped * 10 < trials);
}

TEST_CASE("The box path and OCCT agree on the shared models", "[validate][box]") {
  // Boxes, curved and notched boundaries, keep-outs and nesting: pairs with
  // anything but two boxes still go through OCCT, so codes and subjects
  // agree wherever both paths run.
  const auto settings = vs::core::ProductSettings::defaults();
  std::vector<Document> docs;
  for (const auto& f : fixtures::all()) docs.push_back(f.project.document);
  docs.push_back(bracket::make_bracket().project.document);
  docs.push_back(curved::make_curved().project.document);
  docs.push_back(nested::make_nested().project.document);
  for (const Document& doc : docs) {
    const DocumentBuild build = build_layout(doc, settings);
    const auto fast = vs::validate::validate_layout(doc, build, settings);
    const auto occt = vs::validate::validate_layout(doc, build, settings, {}, {false});
    CHECK(codes(fast) == codes(occt));
    CHECK(fast.overlaps.size() == occt.overlaps.size());
    CHECK(fast.contacts.size() == occt.contacts.size());
  }
}
