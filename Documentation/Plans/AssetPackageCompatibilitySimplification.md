# Asset Package Compatibility Simplification Plan

Summary: Separate current-format package decoding from offline compatibility audit and remove migration machinery with no repository consumer.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

This is the active M2 child plan of the
[Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md).
M0 catalog/load ownership and M1 mutation ownership are complete. Stage 0 now
freezes the package-format, fixture, compatibility, upgrader, and Engine load
caller inventory before any compatibility path is removed.

## Outcome

- Ordinary authored load validates and decodes the current DAST format without
  reserializing the package to prove canonical encoding.
- Canonical byte audit and rewrite remain explicit offline/editor operations.
- Migration/upgrader registries, legacy field adapters, and partial compatibility
  package state remain only when a tracked fixture or production caller proves
  they are required.
- Unsupported or corrupt packages fail with one structured, strict policy before
  partial package publication.
- Engine asset loading consumes an immutable authored/cooked runtime domain and
  does not branch on migration-specific load state.

## Scope

- AssetCore authored package reader, compatibility policy, migration context,
  structure upgrader registry, canonical audit/resave, and load reports.
- Repository and test fixtures for current and historical DAST encodings.
- Engine `PostLoad` and load-time code that observes compatibility or migration
  state.
- Focused package, compatibility, Engine asset, restart, Cook, and documentation
  qualification.

## Non-Goals

- Designing a new DAST version or bulk-data format.
- Changing redirector, catalog, residency, mutation, import, or build ownership.
- Silently accepting unknown formats or weakening corruption diagnostics.
- Keeping unused migration abstractions for hypothetical future formats.

## Stage 0: Freeze Compatibility Inventory And Baseline

Dependencies: M0 and M1 complete.

- [ ] Inventory every production and test caller of compatibility decode,
  canonical audit/resave, migration context, structure upgrader registration,
  compatibility-risk state, and migration-aware Engine load behavior.
- [ ] Inventory tracked authored/cooked fixtures and classify every package by
  format/schema version and whether construction requires migration machinery.
- [ ] Characterize current-format decode failures, unsupported versions,
  noncanonical bytes, unknown fields/types, partial construction rollback,
  restart, and Cook behavior.
- [ ] Assign every retained compatibility surface a production owner or a
  deletion stage.

### Acceptance Gate

- No compatibility or migration mechanism is removed without a caller and
  fixture disposition.
- Current-format success and strict failure behavior have focused baseline
  evidence.

## Stage 1: Separate Decode From Canonical Audit

Dependencies: Stage 0 complete.

- [ ] Make ordinary load perform bounded validation and one decode without
  canonical reserialization.
- [ ] Keep canonical comparison/resave behind explicit offline/editor APIs.
- [ ] Preserve structured format/schema/corruption diagnostics and failure-
  atomic package publication.
- [ ] Remove duplicate readers, byte passes, and compatibility state used only
  by the former load-time audit.

### Acceptance Gate

- Current packages load through one read/decode path and never re-encode during
  ordinary load.
- Explicit canonical audit still detects and repairs noncanonical current data.

## Stage 2: Remove Unused Migration And Upgrader Machinery

Dependencies: Stage 1 complete.

- [ ] Remove unconsumed structure-upgrader registrations and legacy field
  adapters proven unnecessary by the Stage 0 fixture audit.
- [ ] Remove partial compatibility package state and migration-load mode when no
  retained transition requires them.
- [ ] Collapse compatibility policy to the smallest strict current-format and
  explicitly supported-transition contract.
- [ ] Move any still-required fixture conversion to an explicit offline tool or
  narrowly owned adapter with restart evidence.

### Acceptance Gate

- Production code contains no dormant migration registry or unowned upgrader.
- Unsupported content fails before object publication; supported content has no
  partial compatibility state.

## Stage 3: Simplify Engine Load Consumers

Dependencies: Stage 2 complete.

- [ ] Remove Engine `PostLoad` and payload-policy branches that depend on mutable
  migration/load mode rather than authored versus cooked construction.
- [ ] Preserve source/DDC fallback rules and Cooked-runtime fail-closed behavior
  for every affected asset class.
- [ ] Update affected restart, source-missing, DDC-missing, and cooked payload
  tests around the simplified domain contract.

### Acceptance Gate

- Engine load behavior depends only on explicit immutable runtime domain and
  asset payload state.
- No production class queries migration-specific package state.

## Stage 4: Qualify And Publish Compatibility Ownership

Dependencies: Stages 0-3 complete.

- [ ] Run focused AssetCore package/compatibility/restart/Cook and affected
  Engine asset suites.
- [ ] Run complete native qualification, full build, and hidden-window editor
  smoke without concurrent build processes.
- [ ] Search for retired migration/upgrader/load-audit symbols and duplicate
  compatibility policy.
- [ ] Update Asset Packages, Asset Versioning, Asset Data Lifecycle, and roadmap
  contracts with the implemented decode/audit boundary.
- [ ] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record evidence for the M2 exit gate.

### Acceptance Gate

- Ordinary load is a strict one-decode current-format path.
- Offline audit owns canonical comparison and any retained conversion.
- Lasting documentation and validation evidence satisfy the M2 roadmap exit
  gate and activate only dependency-ready M3/M4 work with child plans.

## Completion Criteria

- All stages and acceptance gates pass with evidence recorded here.
- No production migration/upgrader or load-time canonical re-encode remains
  without a proven current consumer.
- Strict failure, restart, Cook, and Engine payload behavior remain qualified.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Versioning](../Runtime/Assets/Versioning.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`AssetCompatibility.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetCompatibility.h)
- [`AssetMigration.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetMigration.h)
- [`AssetSystem.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp)
- [`AssetMigration.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetMigration.cpp)
- [`AssetPackageV4Reader.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp)
- [`PackageTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp)
