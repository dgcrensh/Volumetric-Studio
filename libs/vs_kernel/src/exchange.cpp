#include "vs/kernel/exchange.hpp"

#include <mutex>
#include <string>

#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <UnitsMethods_LengthUnit.hxx>

#include "occt.hpp"

namespace vs::kernel {

using detail::topo;

namespace {

// OCCT's data exchange keeps global state (static parameters, controller
// registration), so calls into it are serialised.
std::mutex& exchange_mutex() {
  static std::mutex m;
  return m;
}

// OCCT takes file names as UTF-8 byte strings.
std::string utf8(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::string display(const std::filesystem::path& path) { return utf8(path); }

template <typename Reader>
Result<Shape> read_with(Reader& reader, const std::filesystem::path& path,
                        std::string_view format, std::string_view code_prefix) {
  if (!std::filesystem::exists(path)) {
    return Diagnostic::error(fmt::format("{}.not_found", code_prefix),
                             fmt::format("{} file '{}' does not exist", format, display(path)));
  }
  if (reader.ReadFile(utf8(path).c_str()) != IFSelect_RetDone) {
    return Diagnostic::error(fmt::format("{}.read_failed", code_prefix),
                             fmt::format("Could not read {} file '{}'", format, display(path)));
  }
  if (reader.TransferRoots() <= 0) {
    return Diagnostic::error(fmt::format("{}.no_shapes", code_prefix),
                             fmt::format("{} file '{}' holds no shapes the kernel can use",
                                         format, display(path)));
  }
  return detail::wrap(reader.OneShape());
}

}  // namespace

Result<Shape> read_step(const std::filesystem::path& path) {
  return detail::guarded<Result<Shape>>("kernel.step.read_failed", "Reading STEP", [&] {
    const std::lock_guard lock(exchange_mutex());
    STEPControl_Controller::Init();
    STEPControl_Reader reader;
    return read_with(reader, path, "STEP", "kernel.step");
  });
}

Diagnostics write_step(const Shape& shape, const std::filesystem::path& path) {
  Diagnostics out;
  if (shape.is_empty()) {
    out.add(Diagnostic::error("kernel.step.empty_shape", "There is nothing to write"));
    return out;
  }
  auto result = detail::guarded<Result<bool>>(
      "kernel.step.write_failed", "Writing STEP", [&]() -> Result<bool> {
        const std::lock_guard lock(exchange_mutex());
        STEPControl_Controller::Init();
        DESTEP_Parameters params;
        params.WriteSchema = DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
        params.WriteUnit = UnitsMethods_LengthUnit_Millimeter;
        STEPControl_Writer writer;
        if (writer.Transfer(topo(shape), STEPControl_AsIs, params) != IFSelect_RetDone) {
          return Diagnostic::error("kernel.step.write_failed",
                                   "The shape could not be translated to STEP");
        }
        if (writer.Write(utf8(path).c_str()) != IFSelect_RetDone) {
          return Diagnostic::error("kernel.step.write_failed",
                                   fmt::format("Could not write STEP file '{}'", display(path)));
        }
        return true;
      });
  if (!result) out.add(result.error());
  return out;
}

Result<Shape> read_iges(const std::filesystem::path& path) {
  return detail::guarded<Result<Shape>>("kernel.iges.read_failed", "Reading IGES", [&] {
    const std::lock_guard lock(exchange_mutex());
    IGESControl_Controller::Init();
    IGESControl_Reader reader;
    return read_with(reader, path, "IGES", "kernel.iges");
  });
}

Diagnostics write_iges(const Shape& shape, const std::filesystem::path& path) {
  Diagnostics out;
  if (shape.is_empty()) {
    out.add(Diagnostic::error("kernel.iges.empty_shape", "There is nothing to write"));
    return out;
  }
  auto result = detail::guarded<Result<bool>>(
      "kernel.iges.write_failed", "Writing IGES", [&]() -> Result<bool> {
        const std::lock_guard lock(exchange_mutex());
        IGESControl_Controller::Init();
        // Mode 1 writes B-rep solids (IGES 5.3 MSBO entities) rather than
        // trimmed surfaces.
        IGESControl_Writer writer("MM", 1);
        if (!writer.AddShape(topo(shape))) {
          return Diagnostic::error("kernel.iges.write_failed",
                                   "The shape could not be translated to IGES");
        }
        writer.ComputeModel();
        if (!writer.Write(utf8(path).c_str())) {
          return Diagnostic::error("kernel.iges.write_failed",
                                   fmt::format("Could not write IGES file '{}'", display(path)));
        }
        return true;
      });
  if (!result) out.add(result.error());
  return out;
}

}  // namespace vs::kernel
