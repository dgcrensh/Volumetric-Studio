#pragma once

// Configurations and design tables (L5).

#include <ostream>
#include <span>
#include <string_view>

#include "vs/model/project.hpp"

namespace vs::cli {

// Makes the configuration named `name` the project's active one ("-": the
// authored values). Says why on `err` and returns false when there is no
// such configuration. What `--config <name>` does for rebuild, validate,
// export, seams and info.
bool use_configuration(model::Project& project, std::string_view name, std::ostream& err);

// `configs <project> [--build] [--threads N] [--activate <name>|-]`: lists
// the configurations and their values; --build evaluates each (one engine,
// so volumes they share come from the cache) and prints statuses, what
// regenerated and the published outputs; --activate makes one the active
// configuration and saves the project. Exits 1 on errors.
int run_configs(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `design-table <project> [--all] [--out <table.csv>]` writes the design
// table as CSV (to standard output without --out); `design-table <project>
// --import <table.csv> [--out <project>]` makes a table the project's
// configurations and saves it (in place without --out). Exits 1 when the
// table or the result is refused.
int run_design_table(std::span<const std::string_view> args, std::ostream& out,
                     std::ostream& err);

}  // namespace vs::cli
