#pragma once

// Authoring checks: what a document must satisfy at every moment, so that
// errors are caught when they are made (spec: loops "are rejected when they
// are authored, not at rebuild time"; references into contents "fail at
// authoring time").
//
// check() runs, in order:
//
//   structure      model.document.duplicate_id     two things share an id
//                  model.document.null_id          an authored object has the null id
//                  model.volume.duplicate_name     two volumes share a name
//                  model.volume.keep_out_contents  a keep-out volume has contents
//                  model.volume.bad_parameter      a parameter name isn't legal
//                                                  (feature::is_valid_name) or repeats
//                  model.volume.duplicate_output   two outputs of a volume share a name
//                  model.volume.bad_output         a Mass output's density isn't positive
//                  model.volume.bad_format         static contents in an unknown format
//                  model.boundary.empty            a feature boundary with no features
//                  model.boundary.not_solid_feature  a boundary feature isn't an extrude,
//                                                  revolve, sweep or loft that adds
//                  model.keep_out.no_cutters / .no_volumes / .not_volume
//                  model.keep_out.bad_cutter       a cutter isn't a solid feature or a
//                                                  keep-out volume
//                  model.connection.duplicate_name
//                  model.connection.volumes        not one or two distinct volumes
//                  model.connection.not_volume     a joined id isn't a volume
//                  model.connection.keep_out       it joins a keep-out volume
//                  model.connection.unknown_thread the thread isn't an ISO size
//                  model.connection.curved_flange  a bolted flange on a cylinder patch
//                  model.volume.parent             (L3) the parent is missing or isn't a
//                                                  composite, or the parents loop
//                  model.composite.delegation      a delegation names a surface not joining
//                                                  the composite, a volume not its child,
//                                                  or a surface twice
//                  model.composite.undelegated     (warning) a bonded or bolted surface joins
//                                                  a composite and no child meets it
//                  model.composite.template        (L4) a template has no name, shares one,
//                                                  or has an illegal or repeated parameter
//                  model.level.mixed               a surface or keep-out names volumes of
//                                                  different levels
//                  model.link.not_output           the source isn't a published output
//                  model.link.not_scalar           the source isn't one number
//                  model.link.bad_target           the target isn't a layout variable or
//                                                  a volume parameter
//                  model.link.duplicate_target     two links drive the same target
//                  feature.*                       the layout's and contents' own
//                                                  structural checks (FeatureTree::validate)
//   references     model.reference.*               (references.hpp) for every reference:
//                                                  layout features and variables,
//                                                  boundaries, keep-outs, connection
//                                                  surfaces, parameters, links, and
//                                                  every feature inside each volume
//   dependencies   model.dependency.loop           (dependency.hpp)
//                  model.composite.generated_link  (L4) a link feeds the inputs of a composite
//                                                  with a generator, from outside it
//   configurations model.configuration.*         (L5, configuration.hpp) every configuration
//                                                  must build: names, targets, expressions,
//                                                  loops, the active one; .linked warns
//
// Generated volumes (L4, generated.hpp) are checked like authored ones,
// except that their names (and their surfaces') needn't be distinct.
//
// A diagnostic names the thing to change first: the feature, volume,
// connection surface, keep-out, parameter or link holding the reference.

#include <functional>

#include "vs/model/document.hpp"

namespace vs::model {

[[nodiscard]] Diagnostics check(const Document& document);

// Authoring: applies `change` to a copy of the document and keeps it only
// if check() finds no errors in the result. Returns check()'s findings
// (warnings alone don't block the change). Every edit of a document in the
// studio goes through here.
Diagnostics edit(Document& document, const std::function<void(Document&)>& change);

}  // namespace vs::model
