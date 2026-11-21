#pragma once

// The model-wide dependency graph (spec, design rule 6, "One-way data flow").
//
// Nodes are the things of a document whose values flow somewhere:
//
//   Variable           a layout variable
//   Feature            a layout feature
//   VolumeBoundary     a volume's boundary (after keep-outs)
//   KeepOut            a keep-out region
//   ConnectionSurface  a connection surface
//   Parameter          a volume's parameter
//   VolumeContents     a volume's contents (its feature tree, graph or file,
//                      as one node: what's inside is private)
//   PublishedOutput    one output of a volume
//   Link               a published output feeding a variable or parameter
//
// and an edge runs from a node to each node it uses:
//
//   Variable  -> variables its expression names; the link driving it
//   Feature   -> features it refers to; variables its expressions name; for a
//                feature whose result depends on the body above it (a
//                through-all extrude, which reaches as far as the body does;
//                a cut; a shell, fillet or chamfer; a pattern), every
//                unsuppressed feature above it that changed the body
//   VolumeBoundary -> its features or points (or, inside a composite, the
//                composite's boundary and joining surfaces they name); the
//                keep-outs that cut it
//   KeepOut   -> its cutter features; the boundaries of keep-out volumes it uses
//   ConnectionSurface -> its sketch or plane; variables its parameters name
//   Parameter -> variables its expression names; the link driving it
//   VolumeContents -> its volume's boundary, connection surfaces and parameters
//   PublishedOutput -> its volume's contents
//
// (Loops among layout variables alone are left to the variable table,
// which reports them as feature.variable.loop.)
//   Link      -> the output it carries
//
// Composite volumes (L3): a composite's layout variables and features are
// Variable and Feature nodes too (owned by the composite), with the same
// edges as the root layout's, plus edges to the composite's boundary and
// joining surfaces where its features refer to them; its children's
// boundaries, parameters and surfaces use them as top-level ones use the
// root layout; and a composite's contents use each child's contents.
// Contents use every surface joining them, delegated ones included.
//
// Contents depend only on what reaches them through the boundary,
// connection surfaces and parameters, which is the encapsulation rule seen
// as data flow. A published output may therefore feed only things that
// aren't upstream of its volume: feeding one closes a loop.
//
// check() reports each loop once as "model.dependency.loop", with the loop
// as a path ("link 'x' → output 'mass' of 'Base' → contents of 'Base' → ...
// → link 'x'"), read "uses". Its subjects are the nodes along the loop,
// starting with a link when the loop has one (the usual thing to undo),
// else with the first node of the loop in document order.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "vs/model/document.hpp"

namespace vs::model {

enum class NodeKind {
  Variable,
  Feature,
  VolumeBoundary,
  KeepOut,
  ConnectionSurface,
  Parameter,
  VolumeContents,
  PublishedOutput,
  Link,
};
std::string_view to_string(NodeKind kind);

struct DependencyNode {
  NodeKind kind = NodeKind::Variable;
  Id id;      // the volume's id for VolumeBoundary and VolumeContents
  Id volume;  // the owning volume, for VolumeBoundary, Parameter, VolumeContents, PublishedOutput
  SubjectRef subject;
  // For messages: "variable 'width'", "boundary of 'Base'", ...
  std::string description;
};

class DependencyGraph {
 public:
  static DependencyGraph build(const Document& document);

  [[nodiscard]] const std::vector<DependencyNode>& nodes() const { return nodes_; }
  [[nodiscard]] std::optional<std::size_t> find(NodeKind kind, const Id& id) const;
  // The nodes `node` uses, in the order found, each once.
  [[nodiscard]] const std::vector<std::size_t>& inputs(std::size_t node) const {
    return inputs_[node];
  }
  // The nodes that use `node`.
  [[nodiscard]] std::vector<std::size_t> dependants(std::size_t node) const;
  // True when `node` uses `other`, directly or through others.
  [[nodiscard]] bool depends_on(std::size_t node, std::size_t other) const;

  // Each loop once, as the nodes along it (the first not repeated at the
  // end), rotated as described at the top of this header.
  [[nodiscard]] std::vector<std::vector<std::size_t>> loops() const;
  [[nodiscard]] Diagnostics check() const;

  // Every node after the nodes it uses (ties in node order); nodes on or
  // downstream of a loop are left out.
  [[nodiscard]] std::vector<std::size_t> order() const;

  // "a → b → c → a" for a loop.
  [[nodiscard]] std::string path(const std::vector<std::size_t>& loop) const;

 private:
  std::vector<DependencyNode> nodes_;
  std::vector<std::vector<std::size_t>> inputs_;
};

}  // namespace vs::model
