#pragma once

// Kernel shape handles.
//
// A Shape is an immutable, reference-counted handle to a B-rep shape owned by
// the kernel implementation (Open CASCADE today). Copying a handle is cheap
// and never copies geometry. No kernel type appears in this header: the rest
// of the studio sees only these handles and vs_core types.
//
// Typed handles (Solid, Shell, Face, Edge) carry an invariant about what they
// hold, checked when they are made from a Shape with `from()`:
//
// - Solid: one solid, or a compound whose leaves are all solids (a boolean
//   can split a body into several lumps; `solid_count()` says how many).
// - Shell: one shell.
// - Face: one face.
// - Edge: one edge.
//
// Kernel operations never throw. They return vs::core::Result<T> carrying a
// Diagnostic with a "kernel.*" code when they fail.

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "vs/core/diagnostic.hpp"

namespace vs::kernel {

using core::Diagnostic;
using core::Diagnostics;
using core::Result;

enum class ShapeKind {
  Empty,
  Compound,
  CompSolid,
  Solid,
  Shell,
  Face,
  Wire,
  Edge,
  Vertex,
};

std::string_view to_string(ShapeKind kind);

namespace detail {
struct ShapeData;  // defined by the implementation
struct ShapeAccess;
}  // namespace detail

class Shape {
 public:
  // An empty shape.
  Shape() = default;

  [[nodiscard]] ShapeKind kind() const;
  [[nodiscard]] bool is_empty() const { return kind() == ShapeKind::Empty; }

  // True when both handles refer to the same underlying topology, ignoring
  // orientation.
  [[nodiscard]] bool is_same(const Shape& other) const;

 private:
  friend struct detail::ShapeAccess;
  std::shared_ptr<const detail::ShapeData> data_;
};

// One solid or a compound of solids.
class Solid : public Shape {
 public:
  Solid() = default;
  // Fails with "kernel.shape.wrong_kind" when `shape` holds anything else.
  static Result<Solid> from(const Shape& shape);
  // The number of solids (lumps) held: 1 for a single solid.
  [[nodiscard]] std::size_t solid_count() const;
};

class Shell : public Shape {
 public:
  Shell() = default;
  static Result<Shell> from(const Shape& shape);
};

class Face : public Shape {
 public:
  Face() = default;
  static Result<Face> from(const Shape& shape);
};

class Edge : public Shape {
 public:
  Edge() = default;
  static Result<Edge> from(const Shape& shape);
};

}  // namespace vs::kernel
