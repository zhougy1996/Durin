# Core Object Package Linker Final Qualification Plan

Summary: Publish the canonical DAST v8 linker contracts and prove the retired production package routes remain absent.

Last reviewed: 2026-08-31

Status: Completed
Completed: 2026-08-31

- 2026-08-31: Stage 0 assigned each lasting package/linker rule to one owning
  document and froze `PackageMigrationTests`, `test all`, `build --target all`,
  documentation validators, maintained-corpus checks, and retired-route
  searches as the final gates.
- 2026-08-31: Stage 1 replaced the mixed v7/v8 package specification with the
  canonical nine-section v8 contract, published format-neutral linker and live
  capture/application boundaries, rewrote BulkData around v8 directory and
  exact raw-segment binding, and updated atomic closure publication.
- 2026-08-31: Stage 2 published package-level catalog and transient occurrence
  ownership, v8 authored/Cook/resave/source-control and VolumeTexture workflows,
  current-version policy, bounded offline v7 conversion, DevTool routing, and
  final CoreDObject/Registry/Engine/AssetMaintenance module boundaries.
- 2026-08-31: Stage 3 closed qualification defects in exact package identity,
  Cook relocation, external BulkData resource binding and recovery, inactive
  mount isolation, and canonical-resave reporting. All native, build,
  maintained-corpus, retired-route, and documentation gates pass.

## Current Status

P0 through P7 are complete. CoreDObject owns canonical v8 linker read/write,
AssetRegistry persists package-level metadata, Engine applies exact v8 closures,
and retained mutation and Cook workflows pass. Lasting Runtime, Editor,
Development, and Workspace contracts describe the final ownership boundaries;
all repository-wide qualification gates passed on 2026-08-31.

## Goal

Make the owning documentation describe only the implemented production v8
architecture, keep v7 terminology bounded to the offline converter and its
fixtures, and finish the roadmap with complete test, build, documentation, and
repository-search evidence.

## Scope

- Canonical v8 envelope/section/linker/value/BulkData contracts and ownership.
- Package-level Registry scanning and dependency state, plus transient exact
  Engine occurrence inspection for mutation tools.
- Authored, Cook, canonical-resave, migration, publication, and module-routing
  documentation affected by the production cutover.
- Repository-wide retired-symbol and maintained-corpus searches.
- Routine native aggregates, full build, and documentation lifecycle gates.

## Non-Goals

- Changing the v8 wire format, linker semantics, or retained higher-level
  workflows after their P6 qualification.
- Removing the bounded AssetRegistry/AssetMaintenance offline v7 converter or
  its focused fixtures.
- Rewriting archived plans and roadmaps as current contracts.
- Launching an editor, game, or GPU workload.

## Qualification Boundaries

- Production Runtime and Editor documentation may name v7 only as retired or
  as input to the explicit offline converter; it must not describe v7 live
  admission, save, load, Cook, mutation, or canonical-resave behavior.
- CoreDObject is the sole package-table, tagged-value, linker, and canonical v8
  codec owner. AssetRegistry reads bounded front matter and owns package-level
  catalog state. Engine owns live graph capture/application and exact tools,
  without another package-wire interpreter.
- A production package is one exact canonical identity plus main bytes and the
  validated raw `.dbulk` closure selected by v8 linker BulkData descriptors.
- Historical plan evidence is excluded from retired-route source searches;
  production source, tests, maintained assets, and lasting documentation are
  included according to the gate being proven.

## Authority And Gate Matrix

| Concern | Owning documentation |
| --- | --- |
| Package identity, v8 sections, live save/load, construct-free validation, offline converter boundary | `Runtime/Assets/AssetPackages.md` |
| Format-neutral linker tables, tagged values, Archive capture/application, canonical value semantics | `Runtime/Core/Serialization.md` |
| Inline/external field state, v8 bulk directory, raw `.dbulk` binding and resource ranges | `Runtime/Assets/BulkData.md` |
| Main/bulk atomic publication and rollback | `Runtime/Core/FileIO.md` |
| Front-matter discovery, package-level dependencies, transient exact occurrences, mutation transactions | `Runtime/Assets/AssetCatalogAndMutation.md` |
| Authored/Cook storage policy and family payload ownership | `Runtime/Assets/AssetDataLifecycle.md` plus family contracts |
| Current version policy and offline v7 input | `Runtime/Assets/Versioning.md` |
| User/tool/source-control workflows | `Editor/Guides/CanonicalResave.md`, `Editor/Guides/SourceFileWorkflows.md`, `Development/Tooling/DurinDevTool.md`, and `Development/VersionControl/ContentVersionControl.md` |
| Cross-module implementation ownership | `Workspace/CodeModules.md` |

Final qualification runs `PackageMigrationTests` plus the complete ordinary
`test all` aggregate, then `build --target all`. Documentation gates are
changed/all validation and all-plan/all-roadmap lifecycle validation. Search
gates inspect production source and lasting documentation for retired v7
codec/object-stream/payload-directory routes, and inspect every maintained
`.dasset`/`.dbulk` closure under `Engine/Content` and `Sandbox/Content`.

## Implementation Stages

### Stage 0: Freeze authorities and final gates

- [x] Inventory every lasting document that still describes v7 production,
  payload-directory ownership, temporary cutover state, or occurrence storage.
- [x] Map each implemented linker/format/Registry/migration rule to one owning
  Runtime, Editor, Development, or Workspace document.
- [x] Freeze exact native aggregates, full-build target, documentation
  validators, corpus scan, and retired-symbol searches.

#### Acceptance Gate

Every stale claim and final proof has a bounded owner; offline migration
evidence is distinguishable from a production compatibility route.

### Stage 1: Publish the package and linker contracts

- [x] Update the Asset Packages, Serialization, BulkData, File I/O, and related
  lifecycle contracts to canonical v8 ownership and exact closure semantics.
- [x] Document the nine-section front directory, construct-free validation,
  live graph capture/application boundary, package identity, and bulk binding
  without duplicating the wire implementation across domains.
- [x] Remove or rewrite stale v7/payload-directory production guidance and
  repair routing metadata and links.

#### Acceptance Gate

The owning Runtime contracts completely describe implemented v8 production
save, scan, load, publication, and BulkData behavior with no live v7 claim.

### Stage 2: Publish catalog, workflow, migration, and ownership contracts

- [x] Update catalog/mutation, authored lifecycle, Cook, canonical-resave,
  source-control, volume-texture, tooling, and module-ownership guidance.
- [x] Bound v7 conversion to explicit offline AssetMaintenance operations with
  deterministic, stale-checked, non-mutating failures.
- [x] Prove lasting documentation contains no persistent occurrence cache,
  Engine wire-parser ownership, or v7 production workflow claim.

#### Acceptance Gate

Users and maintainers are routed to the implemented v8 workflows, and module
ownership has no temporary cutover language.

### Stage 3: Execute final qualification and complete the roadmap

- [x] Run the selected routine native aggregates and offline migration tests.
- [x] Run the full repository build gate without launching an application.
- [x] Run changed/all documentation, plan, and roadmap validators.
- [x] Run maintained-corpus and retired-production-route searches; disposition
  every match as offline fixture, historical evidence, or defect.
- [x] Complete this plan and the Core Object Package Linker roadmap with exact
  validation evidence.

#### Acceptance Gate

All program completion criteria pass, no retired production route remains, and
the roadmap and every child plan are completed or explicitly dispositioned.

## Validation Evidence

Completed on 2026-08-31 without launching an application:

- `./DevTool test PackageMigrationTests`: 3/3 passed.
- `./DevTool test EditorAssetWorkflowTests`: 35/35 passed, including the v8
  canonical-resave target regression.
- `./DevTool test all`: 81/81 ordinary native targets passed.
- `./DevTool build --target all`: passed.
- `./DevTool asset migrate-v8 --all --json`: read-only plan completed with all
  25 maintained `.dasset` packages reported `AlreadyV8`; the eight required
  `.dbulk` companions retained exact extent and digest identity.
- Retired-route searches found no payload-directory match and no v7 reader,
  writer, or converter reference outside the bounded AssetRegistry converter,
  AssetMaintenance offline migration entrypoint, and their focused fixtures.
  The search exposed and removed one stale v7 target-report constant from the
  live canonical-resave planner.
- `./DevTool doc validate --scope changed`, `./DevTool doc validate --scope all`,
  `./DevTool doc plan validate --scope all`, and
  `./DevTool doc roadmap validate --scope all`: passed.

## Related Code and Documentation

- [Core Object Package Linker roadmap](../Roadmaps/CoreObjectPackageLinker.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Bulk Data](../Runtime/Assets/BulkData.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Testing Workflow](../Agents/Testing.md)
