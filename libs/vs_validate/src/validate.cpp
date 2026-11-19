#include "vs/validate/validate.hpp"

#include "vs/model/check.hpp"
#include "vs/validate/containment.hpp"

namespace vs::validate {

ValidationReport validate(const model::Document& document, const ProductSettings& settings,
                          const model::BlobStore* blobs) {
  ValidationReport report;
  report.diagnostics.append(model::check(document));
  if (!report.diagnostics.has_errors()) {
    report.build = model::build(document, settings, blobs);
    report.diagnostics.append(report.build->diagnostics);
    report.layout = validate_layout(document, *report.build, settings);
    report.diagnostics.append(report.layout.diagnostics);
    for (std::size_t i = 0; i < document.volumes.size(); ++i) {
      const auto& vb = report.build->volumes[i];
      if (vb.boundary.is_empty() || vb.contents.is_empty()) continue;
      auto c = check_containment(vb.boundary, vb.contents,
                                 document.volume_subject(document.volumes[i].id), settings);
      report.diagnostics.append(c.diagnostics);
    }
  }
  report.routed = route_all(report.diagnostics, document);
  return report;
}

}  // namespace vs::validate
