# Selected Local Authored Payload Publication Plan

Summary: Implement DAST v5 dual-read loading and failure-atomic opt-in publication with external DABK v1 companions.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

[Authored Package Trailer Foundation](AuthoredPackageTrailerFoundation.md)
qualified the separately versioned EOF trailer/footer and the sole
`ExternalDabkV1` placement without enabling production v5. DAST v4/DABK v1 is
still the ordinary writer and complete rollback route. This plan activates a
reader-complete DAST v5 codec and an explicit opt-in writer while preserving
companion-first publication, full-file atomic package replacement, and the
existing Git/LFS boundary.

## Goal

Make DAST v5 packages with trailer-indexed external DABK v1 payloads fully
readable and safely publishable through an explicit opt-in policy, including
all package operations and failure recovery, without changing the default
writer or any domain schema.

## Scope

- Define the v5 object stream as the canonical v4 logical package body with a
  v5 preamble and section extents ending at the trailer offset rather than EOF.
- Register a complete v5 reader and mutator surface for header, validation,
  inspection, references, compatibility, load, relocation, and redirectors.
- Add an explicit per-operation opt-in v5 writer route; retain v4 as the
  ordinary/default writer and canonical rollback target.
- Move external physical placement facts to the v5 trailer while preserving
  logical payload identity and verifying exact agreement at codec boundaries.
- Reuse DABK v1 generation-named companions, `.dabulk` Git LFS policy,
  companion-first staging, whole-file atomic package replacement, cleanup, and
  catalog publication.
- Cover save/reload, bundles, move/copy/delete, Fix Up, inspection, repair,
  canonical v5-to-v4 resave, failure injection, and catalog recovery.

## Non-Goals

- Making v5 the ordinary/default writer or converting tracked assets.
- Package-local payload bytes, tail rewrite, append generations, compaction,
  compression, deduplication, virtualization, or a DABK v2 wire.
- Changing `FEditorBulkData`, texture or volume schemas, DDC/Cook contracts, or
  runtime resource ownership.
- Moving `.dasset` to Git LFS, rewriting history, or retiring v4/DABK reads or
  writes.

## Design Decisions and Invariants

- V5 reuses the proven v4 logical tables and tagged value semantics. The
  section directory's final extent equals `TrailerOffset`, not physical EOF;
  the trailer foundation exclusively owns all later bytes.
- External v5 `BulkData` values retain payload id and logical authored facts
  but do not independently select a companion generation. Trailer lookup is
  mandatory and duplicate/missing/disagreeing identities fail before bytes are
  exposed. Inline values remain in the object stream and have no trailer entry.
- The v5 codec is reader-complete before its writer is callable. The opt-in is
  explicit in package-authoring options and cannot be inferred from an
  existing file, domain type, file size, or environment variable.
- Publication stages and validates a new immutable DABK v1 generation first,
  builds and validates the complete detached v5 package second, then atomically
  replaces the package and publishes catalog state. Failure preserves the last
  complete package/companion closure; cleanup is last and reachability-based.
- Package mutation either preserves v5 plus its validated trailer or uses the
  explicit canonical rollback path to v4. It never strips or copies a trailer
  without rebuilding hashes and entries.
- V4 remains readable and writable throughout. V5 is never emitted by ordinary
  save, bundle, relocation, Fix Up, or redirector code unless the originating
  operation explicitly selected it.

## Current Foundations and Gaps

| Area | Foundation | Plan gap |
| --- | --- | --- |
| Object package | Complete v4 codec surface and codec registry | Generalize the bounded body extent and provide a complete v5 codec |
| Trailer | Qualified detached v1 build/inspect contract | Bind trailer entries to v5 bulk descriptors and package inspection |
| Publication | DABK-first staging and atomic full-file replacement | Select v5 explicitly and validate the combined candidate before publish |
| Operations | Save, bundle, relocation, Fix Up, deletion, repair, and resave understand v4 closures | Preserve or deliberately roll back v5 closures across every operation |
| Compatibility | V4 reader/writer and canonical resave are qualified | Add new-reader v4/v5 matrix and explicit old-reader v5 rejection |
| Source control | `.dasset` ordinary Git and `.dabulk` LFS | Prove the selected route does not change path or submit-closure policy |

## Implementation Stages

### Stage 0: Freeze v5 body and opt-in contracts

- [ ] Freeze v5 preamble, bounded body extent, bulk-descriptor/trailer binding,
  codec capabilities, diagnostics, and v5-to-v4 rollback behavior.
- [ ] Add an explicit authoring selection that defaults to v4 and propagates
  through single and bundle staging without ambient policy.
- [ ] Record the compatibility and failure matrix before registering the v5
  writer.

#### Acceptance Gate

- Every physical fact has one authority, ordinary writes remain v4, and all
  v5 operation outcomes are deterministic.

### Stage 1: Implement reader-complete DAST v5

- [ ] Factor the v4 logical reader so a caller-supplied bounded object-stream
  extent can be decoded without relaxing v4 EOF rules.
- [ ] Register v5 header, validation, inspection, reference, compatibility,
  load, relocation, and redirector behavior with mandatory trailer validation.
- [ ] Resolve every external descriptor through a unique matching trailer
  entry and validate the referenced DABK v1 generation before exposure.

#### Acceptance Gate

- Construct-free and live paths accept valid v4 and v5 packages, reject every
  split-brain or malformed v5 closure, and leave v4 golden bytes unchanged.

### Stage 2: Implement opt-in companion-first publication

- [ ] Extend detached package capture/build to emit canonical v5 object-stream
  and trailer bytes only when explicitly selected.
- [ ] Reuse DABK v1 candidate validation, immutable publication, atomic package
  replacement, catalog publication, and reachability cleanup in the required
  order.
- [ ] Add failure hooks at companion staging/publication, trailer build,
  package validation/replacement, catalog publication, and cleanup boundaries.

#### Acceptance Gate

- Every injected failure retains a loadable previous closure or a fully
  loadable new closure, and no ordinary save emits v5.

### Stage 3: Integrate package operations and rollback

- [ ] Preserve explicit v5 intent through bundle save, move/rename, copy,
  delete, Fix Up, redirectors, inspection/repair, and reference projection.
- [ ] Add canonical v5-to-v4 resave that publishes v4/DABK first and deletes no
  reachable generation before package and catalog publication succeed.
- [ ] Prove interrupted operations and catalog refresh recover from physical
  package facts without DDC or object construction.

#### Acceptance Gate

- The complete package-operation matrix passes for v4 and opt-in v5, including
  rollback and every publication failure point.

### Stage 4: Qualify and hand off the VolumeTexture pilot

- [ ] Add exact v5 wire/closure goldens, corruption fixtures, source-control
  closure checks, and selected native/regression coverage.
- [ ] Document production-supported v5 read and opt-in write boundaries in the
  owning package, lifecycle, version-control, serialization, and File IO
  contracts.
- [ ] Update roadmap Milestone 2 and activate exactly one VolumeTexture trailer
  migration plan only after code, tests, and documentation validation pass.

#### Acceptance Gate

- V5 opt-in publication is qualified end to end, v4 remains default and fully
  readable, and the next plan owns only the consumer pilot.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Wire | Exact v5 object-stream/trailer/footer golden bytes and v4 golden stability |
| Reader | Header, validation, inspection, references, compatibility, live load, and catalog refresh for v4/v5 |
| Binding | Missing, duplicate, extra, mismatched id/size/hash/container/placement, corrupt and unavailable DABK rejection |
| Publication | Companion-first ordering and failures at build, flush, replace, catalog, cleanup, and bundle member boundaries |
| Operations | Save/reload/unload, bundle, move, rename, copy, delete, Fix Up, redirector, repair, orphan cleanup, and reference projection |
| Rollback | Canonical v5-to-v4 resave and explicit old-reader v5 rejection |
| Policy | Default/ordinary writes remain v4; only explicit opt-in emits v5 |
| Source control | `.dasset` ordinary Git, `.dabulk` LFS, submit closure, rename, partial-sync, and rollback unchanged |
| Native | Focused v5 codec/publication tests plus package, storage, catalog, relocation, Fix Up, resave, and bundle aggregates |
| Documentation | Changed/all plus all-plan/all-roadmap validation |

## Definition of Done

- DAST v5 is reader-complete and safely publishable only by explicit opt-in.
- Trailer entries are the sole external-placement authority and every DABK v1
  companion is verified before payload exposure.
- All package operations and injected failures preserve one complete reachable
  package/companion closure.
- DAST v4 remains the default writer, supported reader, and canonical rollback
  format; no tracked asset or domain schema changes.
- Milestone 2 is complete and exactly one VolumeTexture pilot plan is active.

## Deferred Follow-ups

- VolumeTexture production pilot and tracked-asset qualification.
- Corpus conversion and default-writer selection.
- Persistent virtualization, optimization, and DABK retirement remain
  roadmap-owned and evidence-gated.

## Related Documentation

- [Authored Package Storage Evolution](../Roadmaps/AuthoredPackageStorageEvolution.md)
- [Authored Package Trailer Foundation](AuthoredPackageTrailerFoundation.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File IO](../Runtime/Core/FileIO.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Writer.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetRuntime.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageTrailer.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageTrailer.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Tests/Native/AssetCoreTests/CMakeLists.txt`
