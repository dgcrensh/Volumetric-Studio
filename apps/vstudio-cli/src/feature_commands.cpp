#include "feature_commands.hpp"
#include "files.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/feature/serialize.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::cli {

int run_build(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.empty() || args.size() > 2) {
    err << "usage: vstudio-cli build <in.json> [<out.step>]\n";
    return 2;
  }
  std::ifstream in(to_path(args[0]), std::ios::binary);
  if (!in) {
    err << "vstudio-cli: can't read '" << args[0] << "'\n";
    return 1;
  }
  std::ostringstream text;
  text << in.rdbuf();
  auto tree = feature::feature_tree_from_json(text.str());
  if (!tree) {
    err << tree.error().to_string() << "\n";
    return 1;
  }

  const auto settings = core::ProductSettings::defaults();
  const auto result = feature::rebuild(*tree, settings);

  out << fmt::format("{} variables, {} features\n", tree->variables().variables().size(),
                     tree->features().size());
  for (const auto& v : tree->variables().variables()) {
    const auto value = result.variables.get(v.name);
    out << fmt::format("  {:<16} = {:<20} {}\n", v.name, v.expression,
                       value ? fmt::format("{:.6g}", *value) : std::string("(no value)"));
  }
  for (const auto& f : result.features) {
    out << fmt::format("  {:<11} {:<10} {}\n", feature::to_string(tree->find(f.feature)->kind()),
                       feature::to_string(f.status), tree->label(f.feature));
  }
  for (const auto& d : result.diagnostics) out << "  " << d.to_string() << "\n";

  if (result.body.is_empty()) {
    out << "body            none\n";
  } else {
    const auto mass = kernel::mass_properties(result.body);
    out << fmt::format("body            {} solid{}, valid {}\n", result.body.solid_count(),
                       result.body.solid_count() == 1 ? "" : "s",
                       kernel::check_validity(result.body).has_errors() ? "no" : "yes");
    if (mass) out << fmt::format("  volume        {:.6f} mm³\n", mass->volume);
  }
  if (!result.ok()) return 1;

  if (args.size() == 2) {
    if (result.body.is_empty()) {
      err << "vstudio-cli: nothing to write; the tree makes no body\n";
      return 1;
    }
    const auto problems = kernel::write_step(result.body, to_path(args[1]));
    if (problems.has_errors()) {
      for (const auto& d : problems) err << d.to_string() << "\n";
      return 1;
    }
    out << "wrote " << args[1] << "\n";
  }
  return 0;
}

}  // namespace vs::cli
