# Versioning

Summary: Define engine release, Archive, authored package, custom-version, and compatibility contracts.

Modules: Core, CoreDObject, Engine, AssetRegistry, AssetMaintenance

Last reviewed: 2026-09-21

Durin's engine release version is defined once in `Engine/Build/Build.version`.
CMake validates that file, exposes the numeric core as the workspace project
version, and generates the header consumed by Core's public version API.
Runtime code uses `GetEngineVersion()` and `GetEngineVersionString()` rather
than duplicating the value in module sources, UI, integrations, or scripts.

## Independent Version Domains

The engine release version identifies a Durin release for users, logs, source
tags, and distributable binaries. It does not define serialization
compatibility. Independently versioned domains include `.dasset` packages,
transient object graphs, DurinHeaderTool schemas, shader/cache records, editor
settings, project descriptors, Cook manifests, and asset-family payloads.

Changing the engine release alone must not rewrite assets or invalidate caches.
A format version changes only when that format's byte/semantic contract changes.
Readers decide compatibility from the selected format plus package-local custom
versions, never from the saved-by engine release.

## Archive Version Context

`FArchiveVersionContext` carries a named format version separately from
GUID-keyed custom versions. Object-graph Archives report object-graph v2;
authored and cooked package Archives report DAST v10. Property snapshots are
process-local and unversioned.

`FCustomVersion` contains only a valid GUID and a nonnegative `int32` version.
`FArchiveCustomVersion` and the package linker use that same record. Absence is
represented by a missing record, never by substituting the current version.

Core's `FCustomVersionRegistry` owns copied, immutable process-lifetime definitions
(GUID, current version, diagnostic name). Modules register before serialization,
using `FCustomVersionRegistration` or checked `Register` calls. Identical
registration is idempotent; invalid or conflicting registrations fail. Definitions
survive module unload and cannot be changed or removed during the process lifetime.

`FArchive::UsingCustomVersion` records the registered current version when saving,
including discovery, and is a no-op when loading. Saving an unregistered GUID or
overriding its current version fails. Package capture unions declarations across
objects independently of default-value omission, sorts the records, and rejects
version-manifest drift between discovery and emission. Authored and cooked capture
share this contract. Other Archive users must persist their version context in
their own format; declaration alone does not inject bytes into a raw stream.

The v10 writer emits GUID, version, and a zero reserved framing byte. Nonzero
framing flags, invalid GUIDs, negative/out-of-range versions, and duplicate
records fail parsing; legacy emission/support/codec metadata is unsupported.
Read-only package inspection does not require local version registration. Before
constructing objects, Engine rejects unknown GUIDs and versions newer than the
local registered current version. Required-version absence and old-version
migration/rejection belong to the consuming serializer. There is no generic
minimum-version policy, downgrade writer, or migration registry.

Cook build inputs conservatively include all registered GUID/current-version
pairs, including formats used only by cooked serializers, before deciding reuse.
Changing a local format version therefore invalidates Cook reuse even when source
asset bytes are unchanged; unrelated registered format changes can also invalidate
reuse. Diagnostic names do not affect the fingerprint.

Struct `PostDeserialize` receives Archive purpose, source DAST version, and the
complete custom-version context. An object's pre-publication `PostLoad` sees the
same already validated package state. Neither derives compatibility from the
engine release.

Reflected `_DEPRECATED` routes are independent of custom-version domains. They
match historical fields by declaring type, inferred stored name, and logical
type signature; semantic conversion belongs to `PostLoad` or
`PostDeserialize`. Custom versions remain appropriate when one unchanged
schema shape has different meanings across package generations, but that
version contract is not encoded in `DPROPERTY(Deprecated)`.

Authored fields removed from a still-known declaring type are automatically
discarded on load and omitted on the next explicit save. This applies through
nested structs and containers but not to unavailable classes, malformed wire
values, or incompatible current/historical field types. Keep an explicit
deprecated route when old values still need semantic conversion rather than
discarding. Cooked native serializer fields remain strict.

## Material Package Versions

The package domains are registered when Engine loads. Their source authority is
[`MaterialCustomVersion.h`](../../../Engine/Source/Runtime/Engine/Public/Materials/MaterialCustomVersion.h).

| Domain | Current version | Authored data |
| --- | ---: | --- |
| `FMaterialGraphVersion` | 4 | `DMaterial` and `DMaterialFunction` expression graphs |
| `FMaterialInstanceVersion` | 1 | Typed instance parameter storage |
| `FMaterialFunctionVersion` | 1 | Terminal-owned function port definitions |
| `FMaterialOutputVersion` | 3 | Terminal-owned material output data |

Serializers declare applicable domains during discovery/save and require exactly
the current version on authored/cooked loads. Missing records are never inferred
from defaults or obsolete fields. A material declares graph and output domains;
a function declares graph and port domains. Domains can coexist in a multi-asset
package, but an instance does not declare its parent's graph domain unless that
graph is serialized in the same package. No legacy-reader or converter fallback
exists; ordinary non-version `AlwaysSerialize` fields keep their normal behavior.

ObjectGraph, Duplicate, PropertySnapshot, and EditableCopy do not require package
version records. Cooked dispatch performs the same checks through
`DObject::SerializeCooked` and virtual `Serialize`, even when graph fields are
stripped. Incompatible changes must advance the owning domain and define an
explicit migration policy.

Standard functions are shipped Engine assets. Loading validates interfaces without
creating or saving assets. Removed authoring-provenance fields follow ordinary
field discard and canonical resave; package versions govern compatibility
independently of function revisions. Legacy DecodeNormalRG expressions and normal
sample output index 8 have no runtime decoder, port alias, or migration branch.

## Static Mesh Source Versions

`FStaticMeshSourceVersion` versions authored static mesh source metadata at
version 1. `DStaticMesh::Serialize` declares it during authored discovery/save
and requires the exact current file version on load, including meshes with
default or empty source data. Missing, old and future records are rejected.
Construct-free `FAssetPackageInspection::CustomVersions` exposes file records
without consulting the local registry; static mesh diagnostics use those records.

The Source field is EditorOnly. Cooked discovery and serialization omit its
custom-version domain, and cooked runtime loads do not require it. ObjectGraph,
Duplicate, PropertySnapshot and EditableCopy also do not require package records.

Geometry bulk remains independently self-describing through
`StaticMeshSourceGeometryPayloadVersion` (1). Changing the authored package domain
does not implicitly change this bulk codec, source identity, or DDC keys.
Incompatible changes must advance the owning domain; no legacy load fallback exists.

## Authored Package Policy

DAST has one permanent nonzero format GUID and current production wire version
10. Core's bounded DURF validation selects DAST identity; CoreDObject's sole v10
reader/writer owns all package tables and tagged-value semantics. Engine's
immutable ordinary codec policy selects v10 only for header reads, validation,
inspection, schema probes, reference projection, live load, serialization,
relocation, fix-up, redirectors, Cook, and canonical resave.
The maintained `Engine/Content`, `Sandbox/Content`, and `RoadWeaver/Content`
corpus is canonical v10; older package versions have no reader or migration path.

Every production entry requires exact package identity and, when present, the
complete main/raw-bulk closure. Unknown format identities, non-v10 DAST versions,
required features, legacy prefixes, noncanonical bytes, or invalid closure
facts fail before object construction, mutation, catalog publication, or Dirty
state changes. Read-only entry points never select a writer.

Package format version is independent of reflected field evolution. Tagged
field addition/removal and compatible default changes normally use schemas and
custom versions without changing DAST version. A wire-layout or canonical-value
change requires a new DAST version and a separately planned corpus transition.
The Registry cache fingerprints exact source bytes/format and is invalidated by
any relevant main/bulk change.

Raw `.dbulk` has no independent DURF version; its format is owned by the
[package BulkData contract](BulkData.md#dast-v10-authored-placement).
Asset-family payload schemas and DDC/Cook keys version independently. A
builder-version change invalidates production identity without necessarily
changing readable family bytes; a family schema change requires a supported
reader or an explicit unsupported-version result.

## Early-Development Compatibility

Until Durin makes an explicit external compatibility commitment, the
repository keeps one authored `.dasset` baseline. A future format change first
inventories real source content and receives a scoped plan. If conversion is
required, that plan adds only the exact temporary offline converter justified
by the source corpus, rewrites the tracked corpus explicitly, verifies the
restart and baseline boundary, and removes the converter and obsolete readers
after the baseline is proven. Runtime never retains a data-loss save permission
or partial-compatibility object graph.

External-project support windows, release deprecation policy, and downloadable
migration bundles require a separate release-level decision.

## Release Convention

Durin follows Semantic Versioning for the engine release identifier. While API
and long-lived content policy remain under development, releases stay below
major version one. Development builds use a prerelease channel such as
`0.1.0-dev`; release tags use the corresponding stable form when ready.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Serialization](../Core/Serialization.md)
- [Package Bulk Data](BulkData.md)
