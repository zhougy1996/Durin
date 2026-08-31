# Versioning

Summary: Define engine release, Archive, authored package, custom-version, and compatibility contracts.

Modules: Core, CoreDObject, Engine, AssetRegistry, AssetMaintenance

Last reviewed: 2026-08-31

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
authored and cooked package Archives report DAST v9. Property snapshots are
process-local and unversioned.

CoreDObject linker tables own the package-local custom-version list, canonical
GUID order, discovery freeze, known-codec flags, emitted value, optional maximum
supported value, and whether a version is required for interpretation. The v9
writer freezes these facts with all other linker tables. The reader rejects
duplicates, malformed flags, unsupported required values, or late-discovery
drift before linker or object publication.

Struct `PostDeserialize` receives Archive purpose, source DAST version, and the
complete custom-version context. An object's pre-publication `PostLoad` sees the
same already validated package state. Neither derives compatibility from the
engine release.

Reflected `_DEPRECATED` routes use one stable domain GUID, an exclusive
`DeprecatedBefore` bound, and a domain `LatestVersion`. Current saves discover
and emit `LatestVersion`. Missing tags resolve to the domain's
`BeforeCustomVersionWasAdded` value (`-1` by convention), not the current
version. A source value above the runtime's supported maximum fails before
object publication. Version discovery and emission must agree exactly.

## Authored Package Policy

DAST has one permanent nonzero format GUID and current production wire version
9. Core's bounded DURF validation selects DAST identity; CoreDObject's sole v9
reader/writer owns all package tables and tagged-value semantics. Engine's
immutable ordinary codec policy selects v9 only for header reads, validation,
inspection, schema probes, reference projection, live load, serialization,
relocation, fix-up, redirectors, Cook, and canonical resave.

Every production entry requires exact package identity and, when present, the
complete main/raw-bulk closure. Unknown format identities, non-v9 DAST versions,
required features, legacy prefixes, noncanonical bytes, or invalid closure
facts fail before object construction, mutation, catalog publication, or Dirty
state changes. Read-only entry points never select a writer.

Package format version is independent of reflected field evolution. Tagged
field addition/removal and compatible default changes normally use schemas and
custom versions without changing DAST version. A wire-layout or canonical-value
change requires a new DAST version and a separately planned corpus transition.
The Registry cache fingerprints exact source bytes/format and is invalidated by
any relevant main/bulk change.

Raw `.dbulk` is not a DURF format. DAST v9 Registry and Bulk Directory own its
extent, whole-segment digest, field ranges, alignments, and per-value digests.
Asset-family payload schemas and DDC/Cook keys version independently.

## Retired And Offline Package Formats

The maintained repository baseline is canonical v9. Production discovery,
load, save, inspection, mutation, Cook, and resave select v9 only. V8 remains a
bounded offline input to AssetMaintenance's explicit `asset migrate`
preview/apply workflow and focused conversion fixtures; it is never selected by
ordinary runtime policy. V7 has no supported reader or migration path. A
mounted non-v9 package fails Registry admission before it can publish catalog
state.

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
