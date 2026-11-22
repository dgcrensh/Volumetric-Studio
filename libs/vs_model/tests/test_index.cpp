// DocumentIndex (decision 0031) answers exactly what the list searches
// answer, on the shared fixtures and on malformed documents.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "curved.hpp"
#include "nested.hpp"
#include "rack.hpp"
#include "vs/feature/expression.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/generated.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/index.hpp"
#include "vs/model/references.hpp"

using namespace vs::model;
using vs::core::dbox3;
using vs::core::Id;

namespace {

std::string text(const Result<Target>& r) {
  if (!r) return r.error().code + " | " + r.error().message;
  return std::to_string(static_cast<int>(r->kind)) + " " + r->id.to_string() + " " +
         r->owner.to_string();
}

std::string text(const Diagnostics& ds) {
  std::string out;
  for (const auto& d : ds) out += d.code + " | " + d.message + "\n";
  return out;
}

// Every id the document holds, plus one it doesn't and the null id.
std::vector<Id> all_ids(const Document& doc) {
  std::vector<Id> out{Id{}, Id::random()};
  const auto tree = [&](const vs::feature::FeatureTree& t) {
    for (const auto& e : vs::feature::tree_ids(t)) out.push_back(e.id);
  };
  tree(doc.layout);
  for (const auto& v : doc.volumes) {
    out.push_back(v.id);
    for (const auto& p : v.parameters) out.push_back(p.id);
    for (const auto& o : v.outputs) out.push_back(o.id);
    if (const auto* t = v.tree()) tree(*t);
  }
  for (const auto& c : doc.connections) out.push_back(c.id);
  for (const auto& k : doc.keep_outs) out.push_back(k.id);
  for (const auto& l : doc.links) out.push_back(l.id);
  return out;
}

// Every name an expression of the document could use, and one it can't.
std::vector<std::string> all_names(const Document& doc) {
  std::vector<std::string> out{"nothing_by_this_name"};
  for (const auto& v : doc.layout.variables().variables()) out.push_back(v.name);
  for (const auto& v : doc.volumes) {
    for (const auto& p : v.parameters) out.push_back(p.name);
    if (const auto* t = v.tree()) {
      for (const auto& var : t->variables().variables()) out.push_back(var.name);
    }
  }
  return out;
}

void same_answers(const Document& doc) {
  const DocumentIndex ix(doc);
  const auto ids = all_ids(doc);
  std::vector<Id> volumes{Id{}, Id::random()};
  for (const auto& v : doc.volumes) volumes.push_back(v.id);

  for (const Id& id : ids) {
    INFO(id.to_string());
    CHECK(ix.volume(id) == doc.find_volume(id));
    CHECK(ix.connection(id) == doc.find_connection(id));
    CHECK(ix.keep_out(id) == doc.find_keep_out(id));
    CHECK(ix.link(id) == doc.find_link(id));
    CHECK(ix.classify(id) == classify(doc, id));
    CHECK(ix.volume_subject(id) == doc.volume_subject(id));
    CHECK(ix.boundary_subject(id) == doc.boundary_subject(id));
    CHECK(ix.connection_subject(id) == doc.connection_subject(id));
    CHECK(ix.keep_out_subject(id) == doc.keep_out_subject(id));
  }
  for (const Id& v : volumes) {
    INFO(v.to_string());
    CHECK(ix.ancestors(v) == ancestors(doc, v));
    CHECK(ix.depth(v) == depth(doc, v));
    CHECK(ix.level_layout(v) == level_layout(doc, v));
    CHECK(ix.children(v) == children(doc, v));
    std::vector<std::size_t> joined;
    for (std::size_t k = 0; k < doc.connections.size(); ++k) {
      const auto& c = doc.connections[k];
      CHECK(ix.joins(c, v) == joins(doc, c, v));
      CHECK(ix.delegate_of(v, c.id) == delegate_of(doc, v, c.id));
      if (joins(doc, c, v)) joined.push_back(k);
    }
    CHECK(ix.joining(v) == joined);
    for (const Id& level : volumes) {
      CHECK(ix.is_within(v, level) == is_within(doc, v, level));
      CHECK(ix.child_toward(level, v) == child_toward(doc, level, v));
    }
  }
  for (const auto& c : doc.connections) CHECK(ix.level_of(c) == level_of(doc, c));
  for (const auto& k : doc.keep_outs) CHECK(ix.level_of(k) == level_of(doc, k));
  for (const auto& l : doc.links) CHECK(ix.level_of(l) == level_of(doc, l));

  // References, from every scope.
  const SubjectRef by{vs::core::SubjectKind::Feature, Id::random(), "by"};
  for (const Id& scope : volumes) {
    const Scope s{scope};
    for (const Id& id : ids) {
      INFO(scope.to_string() << " -> " << id.to_string());
      CHECK(text(resolve(ix, s, id, &by)) == text(resolve(doc, s, id, &by)));
    }
    for (const auto& name : all_names(doc)) {
      CHECK(text(check_variable_name(ix, s, name, by)) ==
            text(check_variable_name(doc, s, name, by)));
    }
  }
}

// The dependency graph as text: nodes and inputs in order.
std::string graph_text(const DependencyGraph& g) {
  std::string out;
  for (std::size_t i = 0; i < g.nodes().size(); ++i) {
    out += g.nodes()[i].description + " <-";
    for (const std::size_t j : g.inputs(i)) out += " " + std::to_string(j);
    out += "\n";
  }
  return out;
}

}  // namespace

TEST_CASE("DocumentIndex answers as the list searches do on the fixtures", "[index]") {
  same_answers(bracket::make_bracket().project.document);
  same_answers(curved::make_curved().project.document);
  same_answers(nested::make_nested().project.document);
  const rack::Rack r = rack::make_rack();
  same_answers(r.project.document);

  // The rack expanded by hand: generated children, surfaces and a
  // delegation to the first child.
  GeneratedLayout g;
  for (int i = 0; i < 3; ++i) {
    GeneratedChild c;
    c.key = Id::derive(Id{}, std::to_string(i));
    c.name = "Cell";
    c.template_name = "Cell";
    c.box = dbox3::from_min_max({20.0 * i, 0, 0}, {20.0 * i + 20, 20, 10});
    g.children.push_back(c);
  }
  for (std::size_t i = 0; i + 1 < 3; ++i) {
    GeneratedSurface s;
    s.key = Id::derive(Id{}, "s" + std::to_string(i));
    s.name = "Join";
    s.a = i;
    s.b = i + 1;
    s.axis = 0;
    s.offset = 20.0 * static_cast<double>(i + 1);
    s.lo = {1, 1};
    s.hi = {19, 9};
    g.surfaces.push_back(s);
  }
  g.delegations = {{r.seat, 0}};
  auto expanded = expand(r.project.document, r.rack, g);
  REQUIRE(expanded);
  same_answers(*expanded);
}

TEST_CASE("DocumentIndex answers as the list searches do on malformed documents", "[index]") {
  nested::Nested n = nested::make_nested();
  Document& doc = n.project.document;

  SECTION("a repeated volume id: the first wins") {
    Volume copy = *doc.find_volume(n.left);
    copy.name = "Second Left";
    copy.parent = Id{};
    doc.volumes.push_back(copy);
    same_answers(doc);
  }
  SECTION("a loop of parents") {
    doc.find_volume(n.housing)->parent = n.core;
    same_answers(doc);
  }
  SECTION("a missing parent, and a volume its own parent") {
    doc.find_volume(n.cap)->parent = Id::random();
    doc.find_volume(n.right)->parent = n.right;
    same_answers(doc);
  }
  SECTION("a delegation to a volume that isn't the composite's child") {
    auto& comp = std::get<CompositeContents>(doc.find_volume(n.housing)->contents);
    for (auto& d : comp.delegations) d.child = n.core;  // a grandchild
    comp.delegations.push_back({n.seat, n.base});
    same_answers(doc);
  }
  SECTION("a repeated surface id and a repeated delegation") {
    ConnectionSurface again = *doc.find_connection(n.middle);
    again.name = "Again";
    doc.connections.push_back(again);
    auto& comp = std::get<CompositeContents>(doc.find_volume(n.housing)->contents);
    if (!comp.delegations.empty()) comp.delegations.push_back(comp.delegations.front());
    same_answers(doc);
  }
}

TEST_CASE("check() and the dependency graph are unchanged by the index", "[index]") {
  // The graph through an index is the graph through the document; check()
  // reports the same on a document with every kind of problem it shares
  // with the list searches (duplicate names and ids, a parent loop).
  nested::Nested n = nested::make_nested();
  Document& doc = n.project.document;
  const DependencyGraph a = DependencyGraph::build(doc);
  const DependencyGraph b = DependencyGraph::build(DocumentIndex(doc));
  CHECK(graph_text(a) == graph_text(b));
  for (std::size_t i = 0; i < a.nodes().size(); ++i) {
    CHECK(a.find(a.nodes()[i].kind, a.nodes()[i].id).has_value());
    for (std::size_t j = 0; j < a.nodes().size(); ++j) {
      CHECK(a.upstream(i)[j] == a.depends_on(i, j));
    }
  }

  Volume twin = *doc.find_volume(n.right);
  twin.id = Id::random();
  doc.volumes.push_back(twin);  // same name as Right
  ConnectionSurface again = *doc.find_connection(n.middle);
  again.id = Id::random();
  doc.connections.push_back(again);  // same name as Middle
  const Diagnostics found = check(doc);
  CHECK(found.with_code("model.volume.duplicate_name").size() == 1);
  CHECK(found.with_code("model.connection.duplicate_name").size() == 1);
}
