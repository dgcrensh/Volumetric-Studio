#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/types.hpp"

using namespace vs;
using namespace vs::proc;

TEST_CASE("Graphs round-trip through JSON exactly", "[proc][graph][json]") {
  auto s = scenes::shell_and_ball();
  Graph& g = s.graph;
  g.parameters.push_back({"tiny", 0.1 + 0.2});
  g.find_node(s.ball)->settings["radius"] = 1.0 / 3.0;
  g.add("input.surface", "Joint", {{"surface", Setting(core::Id::random().to_string())}});
  const std::string text = to_json(g);
  const auto back = graph_from_json(text);
  REQUIRE(back);
  CHECK(*back == g);
  CHECK(to_json(*back) == text);
}

TEST_CASE("Bad graph JSON is reported", "[proc][graph][json]") {
  CHECK(graph_from_json("{").error().code == "proc.json.parse");
  CHECK(graph_from_json(R"({"format": "other", "version": 1})").error().code == "proc.json.version");
  CHECK(graph_from_json(R"({"format": "vstudio.graph", "version": 99, "nodes": []})").error().code ==
        "proc.json.version");
  CHECK(graph_from_json(R"({"format": "vstudio.graph", "version": 1})").error().code ==
        "proc.json.parse");
  CHECK(graph_from_json(R"({"format": "vstudio.graph", "version": 1,
                           "nodes": [{"id": "nope", "type": "output"}]})")
            .error()
            .code == "proc.json.parse");
  CHECK(graph_from_json(R"({"format": "vstudio.graph", "version": 1,
                           "nodes": [{"id": "0123456789abcdef0123456789abcdef", "type": "output",
                                      "settings": {"x": [1, 2]}}]})")
            .error()
            .code == "proc.json.parse");
  const auto empty = graph_from_json(R"({"format": "vstudio.graph", "version": 1, "nodes": []})");
  REQUIRE(empty);
  CHECK(empty->nodes.empty());
}

TEST_CASE("Graph lookups", "[proc][graph]") {
  auto s = scenes::shell_and_ball();
  CHECK(s.graph.find_node("Ball")->id == s.ball);
  CHECK(s.graph.find_node(s.shell)->type == "field.shell");
  CHECK(s.graph.edge_into(s.join, "b")->from.node == s.ball);
  CHECK(s.graph.edge_into(s.join, "radius") == nullptr);
  CHECK(s.graph.find_parameter("wall")->value == 2.0);
  CHECK(node_subject(*s.graph.find_node(s.ball)).label == "Ball");
  CHECK(type_of(Value(2.0)) == PortType::Scalar);
  CHECK(port_type_from_string("frames") == PortType::Frames);
  CHECK(to_string(PortType::Curves) == "curves");
  CHECK_FALSE(port_type_from_string("number"));
}
