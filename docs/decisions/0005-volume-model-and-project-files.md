# 0005: The volume model, connection surfaces and project files

Nov 19, 2026 · M5

## Questions

1. The plan describes `vs_model` in a paragraph. Several things it leaves
   open: what a boundary is made of, how keep-outs relate to the keep-out
   flag, how a parametric volume's tree reaches its boundary, how published
   outputs feed anything, and what a dependency loop is made of.
2. The spec leaves the connection surface types and their parameters open;
   the plan says "parameters fixed in M5".
3. The plan asks for a project format that is "a folder or zip"; it doesn't
   say how to zip, or what "saves and loads with identical hashes" hashes.

## Decisions

**One level of volumes, owned by the root.** A `Document` holds the layout
(a feature tree with the global variables) and the volumes directly under
it. Connection surfaces, keep-outs and links live on the document, because
the root is the common parent of every pair of volumes. Composite volumes
(L3) will move these onto a parent volume without changing their shape.

**Boundaries are built from layout features or two points.** A
`FeatureBoundary` is the union of the solids that layout extrudes, revolves,
sweeps or lofts add (their `tool`, not the layout body, so sibling
boundaries stay separate even though the layout body fuses them). A
`BoxBoundary` spans two layout points (datum or sketch points): reference
geometry without a solid feature.

**Keep-outs.** A `KeepOut` cuts listed volumes' boundaries with layout
solids or keep-out volumes before contents generate. A keep-out volume
(`keep_out = true`) reserves its boundary as empty space: it may not have
contents or connection surfaces, and only keep-out volumes may be used as
cutters. Siblings still may not overlap it (M6 checks that); a keep-out is
how a volume makes room for one.

**Connection surface types (answers the spec's open question for the MVP).**

| Type | Parameters |
| --- | --- |
| `bonded` | none |
| `clearance` | `gap` (mm) |
| `bolted_flange` | hole pattern (a bolt circle: count, pitch diameter, start angle; or a grid: columns, rows, x and y pitch; both centred at (u, v) in the patch frame), `thread` (ISO metric coarse, M2 to M12, with ISO 273 medium clearance holes), `min_wall` (mm) |

Every number is a `Param`, so variables can drive it. Patches are planar:
a layout sketch's single profile (holes allowed) or a rectangle on a layout
plane. A surface joins two volumes, or one volume and the outside. The seam
definition is a placeholder (`spacing`, 0 = derive in M7). Whether the
parameters make sense together (the minimum wall against the hole pattern)
is M6's type-mismatch check.

**Contents see their inputs through vs_feature's new `ExternalInputs`.** A
parametric volume's tree is rebuilt with stand-ins for features it can't
contain: the boundary (by the volume's id) with its faces named
`<layout feature id>:<face name>` (or `-x` … `+z` for a box boundary), each
joining connection surface (by its id) as a plane with one face `patch`,
and the volume's parameters as values its Params can use by name. This is
the only change to vs_feature, besides `references_of()`, which lists what a
feature refers to for the checks below. Variables inside the tree can't use
parameters; features can.

**Encapsulation is one function.** `resolve()` classifies an id (layout
feature, layout variable, volume, connection surface, keep-out, link,
parameter, published output, or something inside a volume's contents) and
judges it from a scope (the layout, or inside one volume). `check()` runs it
over every reference in the document, including variable names in
expressions, and `edit()` applies a change only if `check()` finds no errors.
So references into contents fail when authored, as the plan asks.

**The dependency graph has one node per volume's contents.** Nodes are
layout variables and features, boundaries, keep-outs, connection surfaces,
parameters, contents, published outputs and links. Contents depend only on
the volume's boundary, connection surfaces and parameters, which is
encapsulation seen as data flow. Published outputs feed things only through
`Link`s (to a layout variable or a volume parameter), and only scalar
outputs can. A link into anything upstream of its volume closes a loop,
reported with the whole path and the link first, since that's what to undo.
M5 records links; applying the output's value is M9's (until then the
target's own expression stands in).

**Status is runtime, not saved.** The spec's volume statuses (current,
stale, regenerating, failed) come from building, not from the file, so
`VolumeStatus` lives on the build result. M5's `build()` makes boundaries,
patches and parametric contents, and imports STEP/IGES static contents; it
leaves procedural and mesh contents stale for M8 to M10.

**Content hash is the BLAKE3 hash of the document's JSON.** The JSON writer is
deterministic (fixed key order, nlohmann's shortest round-trip numbers), and
a structural `hash_append` for every feature and sketch type would double
that code for no gain yet. A project's hash adds each blob's path and bytes.
Saving and loading keep the hash, and the tests also compare the documents
for equality. M9's cache keys may want a structural hash; they can add one.

**Zips are written "stored", by our own ~250 lines, not a library.**
(Superseded by decision 0006: miniz is now vendored and zips are deflated.) The
first attempt added miniz through vcpkg. In this cloud session vcpkg couldn't
download its source, because the session's network policy only reaches the
project's own GitHub repository, and the shared binary cache only holds what
CI on `main` has built (decision 0004). So a new vcpkg dependency can't be
introduced from a cloud session at all; it has to land from a desktop or CI
first. Rather than block M5 on that, `src/zip.cpp` writes uncompressed zips
with fixed timestamps (the same project makes the same bytes) and reads
them back with CRC checks. It refuses deflated entries
(`model.project.zip_compressed`), so a project re-zipped by Explorer won't
open until unzipped; the folder form is the everyday one anyway. If that
proves annoying, add a deflate library from a desktop session.

**Files are checked as if authored.** Loading a project checks blob sizes and
hashes, then runs `check()` and rejects the file on the first error, like
vs_feature does for variable loops. Format version 1; newer versions are
refused with `model.project.version`.

## Consequences

- vs_model is the only place that judges cross-volume references. M6's
  validation and M9's scheduler build on `DependencyGraph` and `build()`.
- The spec's open question on connection surface types is answered for the
  MVP; curved patches remain L2.
- Adding a vcpkg dependency needs a desktop or CI run first, then a cache
  refresh, before cloud sessions can build it (noted in CLAUDE.md).
