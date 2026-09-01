# Single Package Intermediate Representation Plan

Summary: Remove the production PackageObjectStream model and make CoreDObject linker tables the only format-neutral package intermediate representation.

Last reviewed: 2026-09-01

Status: Archived
Completed: 2026-09-01

## Current Status

The cutover is complete. Engine now captures live `DPackage` graphs directly
into CoreDObject-owned `ObjectPackage::FLinkerTables` and applies validated
linker tables directly to unpublished object graphs. CoreDObject remains the
sole owner of DAST v9 encoding/decoding, while AssetRegistry exposes only
Registry, catalog, scan/cache, and dependency projections.

The production `PackageObjectStream` DTO, its AssetRegistry reader/writer and
canonical-map-key codec, the Engine bidirectional linker adapter, and the
independent wire/reference-model tests were deleted. Runtime and native-test
source-absence checks find no retired package-object-stream vocabulary.

The directly replaced Runtime surface comprised 6,394 physical lines; its
linker-native replacement totals 3,283 lines, a net Runtime reduction of 3,111
lines. Removing the
independent object-stream test model and registrations reduces tests/build
declarations by another 3,338 net lines, for a total net reduction of 6,449
lines. Line count is evidence of duplicate-model removal, not a correctness
gate.

Qualification passed on 2026-09-01: `asset-package`, `reflection`, and
`asset-cook` bounded domains; the repository's affected-test selection against
`dev`; and the complete configured target graph. The affected gate exposed and
then verified the fix for order-dependent Struct schema discovery when an empty
container instance preceded a populated instance. Canonical writer/reader,
Registry front matter, BulkData, Cook, mutation, dependency, and rollback
coverage all run through the direct linker path. Runtime/test source-absence
checks confirm that no retired vocabulary, filenames, compatibility switch, or
test-private historical fixture remains.

## Goal

Establish exactly one format-neutral package graph:

```text
DObject graph
    <-> Engine Archive capture / unpublished graph application
FLinkerTables
    <-> CoreDObject DAST codec
Package bytes
```

`FLinkerTables` is the only detached model that can represent a complete
package, its object topology, schemas, tagged values, imports, dependencies,
and top-level assets. A phase-local capture buffer or resolved live-schema view
may exist only as a private implementation detail: it must not be
round-trippable, cross a module API, own a second package/schema/value
vocabulary, or acquire a codec or reference model.

## Scope

- Direct Engine capture from one live `DPackage` graph into
  `ObjectPackage::FLinkerTables`, including default-delta provenance, custom
  versions, cooked filtering, save overrides, imports, soft references, and
  inline/external BulkData.
- Direct Engine application from validated `FLinkerTables` into one unpublished
  `DPackage` graph, including live reflection binding, deprecated-route
  evidence, authored-ledger restoration, dependency loading, rollback, and
  final publication.
- Removal of the production `PackageObjectStream` types, codecs, adapters,
  headers, diagnostics, and test seams.
- Removal of AssetRegistry's public object value, schema, reader, and writer
  surface while preserving Registry projection, catalog, dependency snapshot,
  scan, and cache behavior.
- Linker-native replacement of still-valuable behavior coverage and deletion of
  legacy object-stream wire/reference-model coverage.

## Non-Goals

- Changing DAST v9 bytes, canonical ordering, format identity, package version,
  BulkData placement, or the maintained asset corpus.
- Moving Engine object construction, class lookup, residency, dependency
  loading, or publication policy into CoreDObject.
- Moving Registry/catalog/cache policy into CoreDObject or Engine.
- Preserving an internal source-compatibility layer for
  `PackageObjectStream`, `FPackageInput`, or `FDecodedPackage`.
- Reworking general `FArchive`, default-delta, authored-ledger, or asset
  mutation semantics beyond the adaptations required by the single linker
  model.

## Selected Decisions

- **One package IR.** `ObjectPackage::FLinkerTables` and its
  `FSerializedType`, `FSerializedSchema`, `FPropertyTag`, and
  `FSerializedValue` vocabulary are authoritative. No replacement DTO may
  duplicate that graph under Engine or AssetRegistry ownership.
- **One-way module ownership.** Engine owns live graph capture/application,
  CoreDObject owns linker validation plus DAST read/write, and AssetRegistry
  consumes only CoreDObject's bounded Registry projection.
- **No format bump.** This is an in-memory ownership refactor. Existing valid
  DAST v9 main and bulk bytes must remain byte-identical for the same logical
  package.
- **Immutable linker application.** The load path reads a validated linker and
  may build a private binding plan containing resolved classes, properties,
  indices, and diagnostics. It must not copy the package into another detached
  value graph before object construction.
- **Diagnostics stop at their owning boundary.** CoreDObject codec diagnostics
  are translated once at the Engine or AssetRegistry boundary. Engine capture
  and application failures use Engine's asset result/diagnostic vocabulary;
  there is no intermediate object-stream failure enum.
- **Legacy object-stream codec is deleted by default.** Current production
  readers accept DAST v9 only and the maintained corpus is v9, so the v5-style
  object-stream encoder/decoder and its independent reference model have no
  migration role. If implementation discovers an exact historical byte fixture
  that is still required, it may survive only beneath `Engine/Tests`, with
  fixture-specific naming and no Runtime header, source, symbol, or module
  dependency.
- **Temporary coexistence is stage-bounded.** A private direct linker path may
  live beside the old bridge while one capture or apply stage is being
  qualified. The old path is not extended, and the plan cannot complete with a
  runtime switch, bidirectional converter, or compatibility facade remaining.

## Implementation Stages

### Stage 0: Freeze linker-native behavior and deletion inventory

- [x] Record every Runtime and native-test consumer of
  `PackageObjectStream`, `AssetRegistry/ObjectStream.h`,
  `FPackageInput`, `FDecodedPackage`, `CaptureLivePackageLinker`, and
  `ApplyLivePackageLinker`; classify each as direct-linker migration, deletion,
  or test-only fixture.
- [x] Add or move focused behavior coverage onto `FLinkerTables` before
  changing production call sites. Cover multi-export/Outer topology, internal
  and imported hard references, soft references, Struct/Array/Map values,
  default-delta provenance, custom-version/deprecated routes, cooked filtering,
  save overrides, inline/external BulkData, injected load failures, and atomic
  rollback.
- [x] Preserve exact DAST v9 byte fixtures and write-read-write identity in
  CoreDObject package writer/reader tests; do not use the legacy object-stream
  writer as an oracle.
- [x] Establish source-absence checks for the retired vocabulary and capture a
  before-change source/test line baseline for the final deletion report.

Completion condition: linker-native tests cover every behavior that justifies
surviving the old capture/apply implementations, and every legacy consumer has
an explicit disposition without introducing a second reference model.

### Stage 1: Capture live graphs directly into linker tables

- [x] Replace `AdaptObjectStreamType`, object-stream schema discovery, and
  object-stream value materialization with direct construction of
  `FSerializedType`, `FSerializedSchema`, `FPackageImport`, `FPackageExport`,
  `FPropertyTag`, and `FSerializedValue` values.
- [x] Preserve the existing two-pass Archive discovery/emission checks, frozen
  object topology, canonical object ordering, default-delta planning, cooked
  reachability pruning, save overrides, custom versions, dependency capture,
  and detached BulkData without exposing the phase-local capture buffer.
- [x] Build top-level asset records and hard/soft package dependencies directly
  in `FPackageSummary`; resolve internal and external hard references directly
  to checked `FPackageIndex` values.
- [x] Make the DAST v9 save and mutation paths call the direct linker capture
  capability, then `ObjectPackage::WritePackageV9`, with no intermediate
  object-stream diagnostic or value conversion.
- [x] Delete the Engine `PackageObjectStream` capture API and writer header once
  the direct path passes its focused tests.

Completion condition: every production save/cook/mutation capture produces
`FLinkerTables` directly, emits byte-identical canonical DAST v9 closures, and
no Engine capture code constructs `FPackageInput`, `FTypeDescriptor`, or
`FValue`.

### Stage 2: Apply linker tables directly to unpublished object graphs

- [x] Replace loader helpers over `FDecodedPackage`, decoded table ids, and
  object-stream opcodes with helpers over `FLinkerTables`, checked
  `FPackageIndex`, `FSerializedType`, `FPropertyTag`, and `FSerializedValue`.
- [x] Resolve reflection aliases, classes, declaring schemas, fields, and
  deprecated routes into a private non-owning binding plan; keep the validated
  linker values authoritative and unmodified.
- [x] Construct all package/export skeletons before resolving references, apply
  tagged values through the existing authored-load Archive contract, restore
  authored override ledgers, run `PostLoad`, and publish only after the complete
  graph succeeds.
- [x] Preserve dependency-cycle admission, injected failure phases, rollback of
  skeleton publication and packages loaded since the operation snapshot, load
  reports, canonicalization evidence, cooked target state, and unknown-class or
  type-mismatch failures.
- [x] Replace `PackageObjectStream::FLiveLoadOptions` and
  `FLoadedAssetPackage` with narrowly named Engine-private linker-application
  state; remove the object-stream testing counter and namespace from Engine's
  public testing header.

Completion condition: `ObjectPackage::ReadPackageV9` feeds the Engine graph
application capability directly, no load path constructs `FPackageInput` or
`FDecodedPackage`, and all failure points leave no published or leaked partial
graph.

### Stage 3: Remove the duplicate model and restore the AssetRegistry boundary

- [x] Delete the bidirectional `PackageObjectStream <-> FLinkerTables` adapter
  and both conversion directions in one change after Stages 1 and 2 are live.
- [x] Remove `AssetRegistry/ObjectStream.h`, the AssetRegistry object-stream
  reader/writer and canonical-map-key adapter, and any object-stream-only module
  dependencies or exported symbols.
- [x] Remove the Engine object-stream reader/writer private headers and rename
  surviving capture/application sources and symbols around linker ownership.
- [x] Delete `DastObjectStreamVersion`, re-encode counters, legacy failure
  enums, section-directory types, and other constants or test seams whose only
  owner was the retired codec.
- [x] Delete the object-stream wire-contract and independent reference-model
  suites. Port only behavior that is not already proven by linker, DAST v9, or
  asset-package tests, using linker-native fixtures.
- [x] Confirm AssetRegistry public headers expose only Registry projection,
  catalog, references, scan/cache, and result values; no object property value,
  schema, package writer, full-package reader, or canonical Map-key API remains.

Completion condition: Runtime source contains no `PackageObjectStream`,
`FPackageInput`, `FDecodedPackage`, object-stream opcode, or object-stream codec;
AssetRegistry cannot construct or interpret object values; and all production
package paths cross the Engine/CoreDObject boundary exactly once through
`FLinkerTables`.

### Stage 4: Qualify the single-IR cutover and publish evidence

- [x] Run the focused `PackageLinkerContractTests`,
  `PackageWriterContractTests`, `PackageRegistryContractTests`,
  `AssetPackageTests`, and `AssetBulkContainerTests` selections while iterating,
  following the repository testing workflow.
- [x] Run the bounded `asset-package` and `asset-cook` domain coverage needed by
  save, load, Cook, mutation, Registry, and BulkData callers, followed by the
  standard affected-test handoff gate.
- [x] Build the complete configured target graph because a public Runtime header
  and exported AssetRegistry/Engine symbols are removed.
- [x] Verify canonical v9 main/bulk byte identity, maintained-corpus load and
  resave, metadata-only inspection, front-matter Registry scan boundaries,
  exact mutation, dependency cycles, and failure rollback.
- [x] Run source-absence checks across Runtime and tests; if a private historical
  fixture remains, prove its files live only under `Engine/Tests` and no Runtime
  target includes or links it.
- [x] Update this plan's status and evidence, update lasting package/serialization
  documentation only where implementation names changed, and report before/after
  production and test line counts without making line count a correctness gate.

Completion condition: focused, bounded-domain, affected, full-build, corpus,
documentation, and retired-symbol gates pass; the lasting contracts describe
the implemented code; and the repository has one format-neutral package IR.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Ownership | CoreDObject is the sole owner of linker types and DAST v9 read/write; Engine alone captures/applies live graphs; AssetRegistry exposes only projections. |
| Single IR | No complete package/schema/value DTO exists outside `FLinkerTables`, and no production conversion copies a linker graph into another detached graph. |
| Bytes | Existing canonical DAST v9 fixtures and maintained packages re-emit byte-identically for unchanged logical inputs; no version bump occurs. |
| Save parity | Authored, no-delta, cooked, override, custom-version, reference, multi-export, and BulkData capture cases pass through the direct linker path. |
| Load parity | Class/schema binding, dependency cycles, references, deprecated routes, ledgers, PostLoad, reports, and all injected rollback points pass directly from linker tables. |
| Registry boundary | Ordinary and full-validation scans remain bounded to declared front matter and publish only immutable package metadata/dependency state. |
| Retirement | Runtime and production tests contain no object-stream types, codecs, adapters, namespaces, counters, or compatibility switches. Any proven historical fixture is test-private and explicitly named as such. |
| Integration | Focused targets, bounded asset domains, affected tests, the full configured build, corpus qualification, and documentation validators pass according to repository workflows. |

## Related Code

- [CoreDObject package linker](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageLinker.h)
- [CoreDObject package format](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [Engine-private linker boundary](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinker.h)
- [Engine direct linker capture](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerCapture.cpp)
- [Engine direct linker application](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp)
- [Engine DAST v9 integration](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageV9Codec.cpp)

## Related Documentation

- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Core Object Package Linker roadmap](../../../Roadmaps/Archive/2026-08/CoreObjectPackageLinker.md)
- [Core Object Package Linker Foundation plan](../2026-08/CoreObjectPackageLinkerFoundation.md)
- [Build and Run workflow](../../../Agents/BuildAndRun.md)
- [Testing workflow](../../../Agents/Testing.md)
