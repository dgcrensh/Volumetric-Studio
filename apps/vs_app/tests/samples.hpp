#pragma once

// The sample projects the app is tested against: every .vsproj the library
// suites keep (M5 bracket, M6 validation fixtures, M10–M12 procedural
// projects), plus helpers to evaluate them. Qt-free: vs_app_core_tests
// uses it too (wait.hpp has the Qt event-loop helper).

#include <filesystem>
#include <string>
#include <vector>

#include "vs/eval/engine.hpp"
#include "vs/model/project.hpp"

namespace samples {

std::filesystem::path source_dir();

// Every sample project, sorted by path.
std::vector<std::filesystem::path> projects();

// A sample by its folder name, e.g. "bracket.vsproj"; loaded without the
// authoring check, as the app opens files.
vs::model::Project load(const std::string& name);

// A fresh engine with vs_proc's generator, evaluated once.
vs::eval::Evaluation evaluate(const vs::model::Project& project);

// shell_and_ball.vsproj with its output sampled at `cell` mm instead of 1:
// a procedural volume that takes seconds to regenerate.
vs::model::Project long_procedural(double cell);

// A static volume: libs/vs_mesh/tests/data/hex_prism.stl imported into a
// box boundary around it (M8's import path), next to nothing else.
vs::model::Project static_mesh();

std::string codes(const vs::core::Diagnostics& diagnostics);

}  // namespace samples
