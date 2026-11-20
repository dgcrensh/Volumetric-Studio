#include "vs/kernel/exchange.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <locale>
#include <sstream>
#include <string>

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

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

// --- named parts -------------------------------------------------------------------

namespace {

TCollection_ExtendedString extended(std::string_view utf8_text) {
  return TCollection_ExtendedString(std::string(utf8_text).c_str(), /*theIsMultiByte=*/true);
}

std::string from_extended(const TCollection_ExtendedString& s) {
  std::string out(static_cast<std::size_t>(s.LengthOfCString()) + 1, '\0');
  char* p = out.data();
  const int n = s.ToUTF8CString(p);
  out.resize(static_cast<std::size_t>(std::max(n, 0)));
  return out;
}

occ::handle<TDocStd_Document> new_xcaf_document() {
  occ::handle<TDocStd_Document> doc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
  return doc;
}

std::string label_name(const TDF_Label& label) {
  occ::handle<TDataStd_Name> name;
  if (label.FindAttribute(TDataStd_Name::GetID(), name)) return from_extended(name->Get());
  return {};
}

}  // namespace

Diagnostics write_step(std::span<const NamedShape> parts, const std::filesystem::path& path,
                       std::string_view assembly_name) {
  Diagnostics out;
  if (parts.empty()) {
    out.add(Diagnostic::error("kernel.step.empty_shape", "There is nothing to write"));
    return out;
  }
  for (const auto& p : parts) {
    if (p.shape.is_empty()) {
      out.add(Diagnostic::error("kernel.step.empty_shape",
                                fmt::format("Part '{}' has no shape", p.name)));
      return out;
    }
  }
  auto result = detail::guarded<Result<bool>>(
      "kernel.step.write_failed", "Writing STEP", [&]() -> Result<bool> {
        const std::lock_guard lock(exchange_mutex());
        STEPControl_Controller::Init();
        occ::handle<TDocStd_Document> doc = new_xcaf_document();
        occ::handle<XCAFDoc_ShapeTool> tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        const TDF_Label assembly = tool->NewShape();
        TDataStd_Name::Set(assembly, extended(assembly_name));
        for (const auto& p : parts) {
          const TDF_Label part = tool->AddShape(topo(p.shape), /*makeAssembly=*/false);
          TDataStd_Name::Set(part, extended(p.name));
          const TDF_Label component = tool->AddComponent(assembly, part, TopLoc_Location());
          TDataStd_Name::Set(component, extended(p.name));
        }
        tool->UpdateAssemblies();

        DESTEP_Parameters params;
        params.WriteSchema = DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
        params.WriteUnit = UnitsMethods_LengthUnit_Millimeter;
        // Mesh shapes (faces with only a triangulation) go out as AP242
        // tessellated geometry; B-rep faces as B-rep.
        params.WriteTessellated = DESTEP_Parameters::RWMode_Tessellated_OnNoBRep;
        STEPCAFControl_Writer writer;
        writer.SetNameMode(true);
        if (!writer.Transfer(doc, params, STEPControl_AsIs)) {
          return Diagnostic::error("kernel.step.write_failed",
                                   "The parts could not be translated to STEP");
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

Result<std::vector<NamedShape>> read_step_parts(const std::filesystem::path& path) {
  using Parts = std::vector<NamedShape>;
  return detail::guarded<Result<Parts>>(
      "kernel.step.read_failed", "Reading STEP", [&]() -> Result<Parts> {
        if (!std::filesystem::exists(path)) {
          return Diagnostic::error("kernel.step.not_found",
                                   fmt::format("STEP file '{}' does not exist", display(path)));
        }
        const std::lock_guard lock(exchange_mutex());
        STEPControl_Controller::Init();
        occ::handle<TDocStd_Document> doc = new_xcaf_document();
        STEPCAFControl_Reader reader;
        reader.SetNameMode(true);
        if (reader.ReadFile(utf8(path).c_str()) != IFSelect_RetDone || !reader.Transfer(doc)) {
          return Diagnostic::error("kernel.step.read_failed",
                                   fmt::format("Could not read STEP file '{}'", display(path)));
        }
        occ::handle<XCAFDoc_ShapeTool> tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        NCollection_Sequence<TDF_Label> roots;
        tool->GetFreeShapes(roots);
        Parts parts;
        for (int i = 1; i <= roots.Length(); ++i) {
          const TDF_Label& root = roots.Value(i);
          if (!XCAFDoc_ShapeTool::IsAssembly(root)) {
            parts.push_back({label_name(root), detail::wrap(XCAFDoc_ShapeTool::GetShape(root))});
            continue;
          }
          NCollection_Sequence<TDF_Label> components;
          XCAFDoc_ShapeTool::GetComponents(root, components, /*getsubchilds=*/false);
          for (int k = 1; k <= components.Length(); ++k) {
            const TDF_Label& c = components.Value(k);
            TDF_Label referred;
            std::string name;
            if (XCAFDoc_ShapeTool::GetReferredShape(c, referred)) name = label_name(referred);
            if (name.empty()) name = label_name(c);
            parts.push_back({std::move(name), detail::wrap(XCAFDoc_ShapeTool::GetShape(c))});
          }
          break;  // the first assembly
        }
        if (parts.empty()) {
          return Diagnostic::error("kernel.step.no_shapes",
                                   fmt::format("STEP file '{}' holds no shapes the kernel can use",
                                               display(path)));
        }
        return parts;
      });
}

Result<Shape> mesh_shape(std::span<const core::fvec3> positions,
                         std::span<const std::uint32_t> indices) {
  if (indices.empty() || indices.size() % 3 != 0 || positions.empty()) {
    return Diagnostic::error("kernel.mesh.invalid", "A mesh shape needs whole triangles");
  }
  for (const auto& p : positions) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      return Diagnostic::error("kernel.mesh.invalid", "A mesh position isn't finite");
    }
  }
  for (const std::uint32_t i : indices) {
    if (i >= positions.size()) {
      return Diagnostic::error("kernel.mesh.invalid", "A mesh index is out of range");
    }
  }
  return detail::guarded<Result<Shape>>("kernel.mesh.invalid", "Making a mesh shape",
                                        [&]() -> Result<Shape> {
    const int nodes = static_cast<int>(positions.size());
    const int triangles = static_cast<int>(indices.size() / 3);
    occ::handle<Poly_Triangulation> tri = new Poly_Triangulation(nodes, triangles, false);
    for (int i = 0; i < nodes; ++i) {
      const auto& p = positions[static_cast<std::size_t>(i)];
      tri->SetNode(i + 1, gp_Pnt(static_cast<double>(p.x), static_cast<double>(p.y),
                                 static_cast<double>(p.z)));
    }
    for (int t = 0; t < triangles; ++t) {
      const std::size_t k = 3 * static_cast<std::size_t>(t);
      tri->SetTriangle(t + 1, Poly_Triangle(static_cast<int>(indices[k]) + 1,
                                            static_cast<int>(indices[k + 1]) + 1,
                                            static_cast<int>(indices[k + 2]) + 1));
    }
    TopoDS_Face face;
    BRep_Builder builder;
    builder.MakeFace(face, tri);
    return detail::wrap(face);
  });
}

std::size_t stored_triangle_count(const Shape& shape) {
  if (shape.is_empty()) return 0;
  std::size_t n = 0;
  for (TopExp_Explorer e(topo(shape), TopAbs_FACE); e.More(); e.Next()) {
    TopLoc_Location loc;
    const occ::handle<Poly_Triangulation> tri = BRep_Tool::Triangulation(TopoDS::Face(e.Current()), loc);
    if (!tri.IsNull()) n += static_cast<std::size_t>(tri->NbTriangles());
  }
  return n;
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

std::string write_brep(const Shape& shape) {
  if (shape.is_empty()) return {};
  std::ostringstream out;
  out.imbue(std::locale::classic());
  BRepTools::Write(topo(shape), out, /*theWithTriangles=*/false, /*theWithNormals=*/false,
                   TopTools_FormatVersion_CURRENT);
  return std::move(out).str();
}

Result<Shape> read_brep(std::string_view text) {
  if (text.empty()) return Shape{};
  return detail::guarded<Result<Shape>>("kernel.brep.read_failed", "Reading BRep", [&]()
                                                                                      -> Result<Shape> {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    TopoDS_Shape shape;
    BRep_Builder builder;
    BRepTools::Read(shape, in, builder);
    if (shape.IsNull()) {
      return Diagnostic::error("kernel.brep.read_failed", "The BRep text holds no shape");
    }
    return detail::wrap(shape);
  });
}

core::Hash256 shape_hash(const Shape& shape) {
  const std::string text = write_brep(shape);
  core::Hasher h;
  h.add_bytes(text.data(), text.size());
  return h.finalize();
}

}  // namespace vs::kernel
