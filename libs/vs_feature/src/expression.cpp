#include "vs/feature/expression.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <span>

#include <fmt/format.h>

namespace vs::feature {

namespace {

// --- built-ins --------------------------------------------------------------

using Fn = double (*)(std::span<const double>);

struct Function {
  std::string_view name;
  int min_args;
  int max_args;  // -1: any number from min_args up
  Fn fn;
};

const std::array<Function, 19> kFunctions{{
    {"sin", 1, 1, [](std::span<const double> a) { return std::sin(a[0]); }},
    {"cos", 1, 1, [](std::span<const double> a) { return std::cos(a[0]); }},
    {"tan", 1, 1, [](std::span<const double> a) { return std::tan(a[0]); }},
    {"asin", 1, 1, [](std::span<const double> a) { return std::asin(a[0]); }},
    {"acos", 1, 1, [](std::span<const double> a) { return std::acos(a[0]); }},
    {"atan", 1, 1, [](std::span<const double> a) { return std::atan(a[0]); }},
    {"atan2", 2, 2, [](std::span<const double> a) { return std::atan2(a[0], a[1]); }},
    {"sqrt", 1, 1, [](std::span<const double> a) { return std::sqrt(a[0]); }},
    {"abs", 1, 1, [](std::span<const double> a) { return std::abs(a[0]); }},
    {"exp", 1, 1, [](std::span<const double> a) { return std::exp(a[0]); }},
    {"ln", 1, 1, [](std::span<const double> a) { return std::log(a[0]); }},
    {"log10", 1, 1, [](std::span<const double> a) { return std::log10(a[0]); }},
    {"floor", 1, 1, [](std::span<const double> a) { return std::floor(a[0]); }},
    {"ceil", 1, 1, [](std::span<const double> a) { return std::ceil(a[0]); }},
    {"round", 1, 1, [](std::span<const double> a) { return std::round(a[0]); }},
    {"pow", 2, 2, [](std::span<const double> a) { return std::pow(a[0], a[1]); }},
    {"hypot", 2, 2, [](std::span<const double> a) { return std::hypot(a[0], a[1]); }},
    {"min", 1, -1,
     [](std::span<const double> a) { return *std::min_element(a.begin(), a.end()); }},
    {"max", 1, -1,
     [](std::span<const double> a) { return *std::max_element(a.begin(), a.end()); }},
}};

const Function* find_function(std::string_view name) {
  for (const auto& f : kFunctions) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

struct Unit {
  std::string_view name;
  double factor;  // into mm or radians
};

const std::array<Unit, 9> kUnits{{
    {"mm", 1.0},
    {"cm", 10.0},
    {"m", 1000.0},
    {"um", 0.001},
    {"in", 25.4},
    {"ft", 304.8},
    {"deg", std::numbers::pi / 180.0},
    {"rad", 1.0},
    {"\xC2\xB0", std::numbers::pi / 180.0},  // °
}};

const Unit* find_unit(std::string_view name) {
  for (const auto& u : kUnits) {
    if (u.name == name) return &u;
  }
  return nullptr;
}

constexpr std::string_view kPiName = "pi";

// --- syntax tree ------------------------------------------------------------

enum class Op { Number, Name, Negate, Add, Subtract, Multiply, Divide, Power, Call };

struct Node {
  Op op = Op::Number;
  double value = 0.0;
  std::string name;                   // Name
  const Function* function = nullptr;  // Call
  std::vector<int> args;               // operand node indices
};

bool is_name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Result<int> parse_all() {
    auto root = expression();
    if (!root) return root;
    skip_space();
    if (pos_ < text_.size()) return error_here("Expected an operator");
    return root;
  }

  std::vector<Node> nodes;

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
  int depth_ = 0;

  Diagnostic error_here(std::string_view what) const {
    const std::string near =
        pos_ < text_.size() ? fmt::format("'{}'", text_.substr(pos_, 10)) : "the end";
    return Diagnostic::error("feature.expression.syntax",
                             fmt::format("{} at position {} (near {}) in '{}'", what, pos_ + 1,
                                         near, text_));
  }

  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) ++pos_;
  }

  bool accept(char c) {
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  int add(Node node) {
    nodes.push_back(std::move(node));
    return static_cast<int>(nodes.size() - 1);
  }

  int binary(Op op, int a, int b) {
    Node n;
    n.op = op;
    n.args = {a, b};
    return add(std::move(n));
  }

  Result<int> expression() {
    if (++depth_ > 200) return error_here("The expression nests too deeply");
    auto left = term();
    if (!left) return left;
    int node = *left;
    while (true) {
      Op op;
      if (accept('+')) {
        op = Op::Add;
      } else if (accept('-')) {
        op = Op::Subtract;
      } else {
        break;
      }
      auto right = term();
      if (!right) return right;
      node = binary(op, node, *right);
    }
    --depth_;
    return node;
  }

  Result<int> term() {
    auto left = unary();
    if (!left) return left;
    int node = *left;
    while (true) {
      Op op;
      if (accept('*')) {
        op = Op::Multiply;
      } else if (accept('/')) {
        op = Op::Divide;
      } else {
        break;
      }
      auto right = unary();
      if (!right) return right;
      node = binary(op, node, *right);
    }
    return node;
  }

  Result<int> unary() {
    if (++depth_ > 200) return error_here("The expression nests too deeply");
    Result<int> out = 0;
    if (accept('-')) {
      auto operand = unary();
      if (!operand) return operand;
      Node n;
      n.op = Op::Negate;
      n.args = {*operand};
      out = add(std::move(n));
    } else if (accept('+')) {
      out = unary();
    } else {
      out = power();
    }
    --depth_;
    return out;
  }

  Result<int> power() {
    auto base = primary();
    if (!base) return base;
    if (accept('^')) {
      auto exponent = unary();
      if (!exponent) return exponent;
      return binary(Op::Power, *base, *exponent);
    }
    return base;
  }

  std::string_view read_name() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && is_name_char(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }

  // A unit name after a number, if one follows; leaves the position alone
  // otherwise.
  const Unit* read_unit() {
    const std::size_t save = pos_;
    skip_space();
    if (text_.substr(pos_).starts_with("\xC2\xB0")) {
      pos_ += 2;
      return find_unit("\xC2\xB0");
    }
    if (pos_ < text_.size() && is_name_start(text_[pos_])) {
      const std::size_t name_start = pos_;
      const auto name = read_name();
      if (const Unit* u = find_unit(name)) return u;
      pos_ = name_start;
    }
    pos_ = save;
    return nullptr;
  }

  Result<int> number() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
    }
    // An exponent only when digits follow, so "2em" isn't misread.
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      std::size_t p = pos_ + 1;
      if (p < text_.size() && (text_[p] == '+' || text_[p] == '-')) ++p;
      if (p < text_.size() && is_digit(text_[p])) {
        pos_ = p;
        while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
      }
    }
    const std::string_view digits = text_.substr(start, pos_ - start);
    double value = 0.0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || end != digits.data() + digits.size()) {
      pos_ = start;
      return error_here("Malformed number");
    }
    if (const Unit* unit = read_unit()) value *= unit->factor;
    Node n;
    n.op = Op::Number;
    n.value = value;
    return add(std::move(n));
  }

  Result<int> primary() {
    skip_space();
    if (pos_ >= text_.size()) return error_here("Expected a number, name or '('");
    const char c = text_[pos_];
    if (is_digit(c) || (c == '.' && pos_ + 1 < text_.size() && is_digit(text_[pos_ + 1]))) {
      return number();
    }
    if (c == '(') {
      ++pos_;
      auto inner = expression();
      if (!inner) return inner;
      if (!accept(')')) return error_here("Expected ')'");
      return inner;
    }
    if (is_name_start(c)) {
      const std::size_t name_pos = pos_;
      const std::string name(read_name());
      if (accept('(')) {
        const Function* fn = find_function(name);
        if (!fn) {
          return Diagnostic::error(
              "feature.expression.unknown_function",
              fmt::format("'{}' is not a function (in '{}')", name, text_));
        }
        Node n;
        n.op = Op::Call;
        n.function = fn;
        if (!accept(')')) {
          do {
            auto arg = expression();
            if (!arg) return arg;
            n.args.push_back(*arg);
          } while (accept(','));
          if (!accept(')')) return error_here("Expected ',' or ')'");
        }
        const int count = static_cast<int>(n.args.size());
        if (count < fn->min_args || (fn->max_args >= 0 && count > fn->max_args)) {
          const std::string wanted =
              fn->max_args < 0 ? fmt::format("at least {}", fn->min_args)
              : fn->min_args == fn->max_args
                  ? fmt::format("{}", fn->min_args)
                  : fmt::format("{} to {}", fn->min_args, fn->max_args);
          return Diagnostic::error(
              "feature.expression.arity",
              fmt::format("{}() takes {} argument{}, not {} (in '{}')", name, wanted,
                          wanted == "1" ? "" : "s", count, text_));
        }
        return add(std::move(n));
      }
      if (name == kPiName) {
        Node n;
        n.op = Op::Number;
        n.value = std::numbers::pi;
        return add(std::move(n));
      }
      if (find_function(name) || find_unit(name)) {
        pos_ = name_pos;
        return error_here(fmt::format("'{}' is a built-in name and can't be used here", name));
      }
      Node n;
      n.op = Op::Name;
      n.name = name;
      return add(std::move(n));
    }
    return error_here("Expected a number, name or '('");
  }
};

}  // namespace

struct Expression::Impl {
  std::string text;
  std::vector<Node> nodes;
  int root = -1;
  std::vector<std::string> references;
};

// A null impl means the constant 0; see the accessors.
Expression::Expression() = default;

Result<Expression> Expression::parse(std::string_view text) {
  Parser parser(text);
  auto root = parser.parse_all();
  if (!root) return root.error();
  auto impl = std::make_shared<Impl>();
  impl->text = std::string(text);
  impl->nodes = std::move(parser.nodes);
  impl->root = *root;
  for (const auto& n : impl->nodes) {
    if (n.op == Op::Name) impl->references.push_back(n.name);
  }
  std::sort(impl->references.begin(), impl->references.end());
  impl->references.erase(std::unique(impl->references.begin(), impl->references.end()),
                         impl->references.end());
  Expression e;
  e.impl_ = std::move(impl);
  return e;
}

const std::string& Expression::text() const {
  static const std::string kZero = "0";
  return impl_ ? impl_->text : kZero;
}

const std::vector<std::string>& Expression::references() const {
  static const std::vector<std::string> kNone;
  return impl_ ? impl_->references : kNone;
}

Result<double> Expression::evaluate(const Lookup& lookup) const {
  if (!impl_) return 0.0;
  const auto& nodes = impl_->nodes;
  // Nodes are stored children first, so one forward pass evaluates them all.
  std::vector<double> values(nodes.size(), 0.0);
  std::vector<double> args;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    auto arg = [&](std::size_t k) { return values[static_cast<std::size_t>(n.args[k])]; };
    double v = 0.0;
    switch (n.op) {
      case Op::Number: v = n.value; break;
      case Op::Name: {
        const auto found = lookup ? lookup(n.name) : std::nullopt;
        if (!found) {
          return Diagnostic::error(
              "feature.expression.unknown_name",
              fmt::format("There is no variable named '{}' (in '{}')", n.name, impl_->text));
        }
        v = *found;
        break;
      }
      case Op::Negate: v = -arg(0); break;
      case Op::Add: v = arg(0) + arg(1); break;
      case Op::Subtract: v = arg(0) - arg(1); break;
      case Op::Multiply: v = arg(0) * arg(1); break;
      case Op::Divide: v = arg(0) / arg(1); break;
      case Op::Power: v = std::pow(arg(0), arg(1)); break;
      case Op::Call: {
        args.clear();
        for (std::size_t k = 0; k < n.args.size(); ++k) args.push_back(arg(k));
        v = n.function->fn(args);
        break;
      }
    }
    if (!std::isfinite(v)) {
      return Diagnostic::error(
          "feature.expression.not_finite",
          fmt::format("'{}' doesn't evaluate to a finite number (a division by zero, or a "
                      "function outside its domain?)",
                      impl_->text));
    }
    values[i] = v;
  }
  return values[static_cast<std::size_t>(impl_->root)];
}

Result<double> evaluate_expression(std::string_view text, const Expression::Lookup& lookup) {
  auto e = Expression::parse(text);
  if (!e) return e.error();
  return e->evaluate(lookup);
}

bool is_reserved_name(std::string_view name) {
  return name == kPiName || find_function(name) != nullptr || find_unit(name) != nullptr;
}

bool is_valid_name(std::string_view name) {
  if (name.empty() || name.size() > 64 || !is_name_start(name.front())) return false;
  if (!std::all_of(name.begin(), name.end(), is_name_char)) return false;
  return !is_reserved_name(name);
}

}  // namespace vs::feature
