#include "vs/validate/validate.hpp"

#include "vs/model/check.hpp"
#include "vs/model/configuration.hpp"
#include "vs/validate/containment.hpp"

namespace vs::validate {

ValidationReport validate(const model::Document& document, const ProductSettings& settings,
                          const model::BlobStore* blobs) {
  ValidationReport report;
  report.diagnostics.append(model::check(document));
  if (!report.diagnostics.has_errors()) {
    // L5: the active configuration, as build() builds it.
    const model::Document built = model::configured(document);
    report.build = model::build(built, settings, blobs);
    report.diagnostics.append(report.build->diagnostics);
    report.layout = validate_layout(built, *report.build, settings);
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
