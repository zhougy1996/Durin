# Asset Package Compatibility Simplification Plan

Summary: Separate current-format package decoding from offline compatibility audit and remove migration machinery with no repository consumer.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-15

## Current Status

This is the completed M2 child plan of the
[Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md).
M0 catalog/load ownership and M1 mutation ownership are complete. M2 now owns a
strict one-decode live-load boundary and leaves canonical comparison to explicit
construct-free maintenance.

## Outcome

- Ordinary authored load validates and decodes the current DAST format without
  reserializing the package to prove canonical encoding.
- Canonical byte audit and rewrite remain explicit offline/editor operations.
- Migration/upgrader registries, legacy field adapters, and partial compatibility
  package state are absent because no tracked fixture or production caller
  requires them.
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

- [x] Inventory every production and test caller of compatibility decode,
  canonical audit/resave, migration context, structure upgrader registration,
  compatibility-risk state, and migration-aware Engine load behavior.
- [x] Inventory tracked authored/cooked fixtures and classify every package by
  format/schema version and whether construction requires migration machinery.
- [x] Characterize current-format decode failures, unsupported versions,
  noncanonical bytes, unknown fields/types, partial construction rollback,
  restart, and Cook behavior.
- [x] Assign every retained compatibility surface a production owner or a
  deletion stage.

### Acceptance Gate

- No compatibility or migration mechanism is removed without a caller and
  fixture disposition.
- Current-format success and strict failure behavior have focused baseline
  evidence.

### Frozen inventory

| Surface | Production/test ownership | Destination |
| --- | --- | --- |
| v4 `DecodePackage` canonical re-emission | Every full v4 load, inspection, reference extraction, compatibility probe, and byte mutation currently re-encodes decoded bytes | Stage 1 splits bounded decode from explicit canonical validation; ordinary load takes the decode-only path while validation/audit retains canonical comparison |
| Compatibility probe/report and canonical resave | `DurinAssetTool` audit/baseline/canonical-resave plus AssetCore and import-record tests | Retain as read-only/offline current-format maintenance; repair its JSON stdout/schema contract |
| Package migration registry/apply/journal | `DurinAssetTool --operation=migrate` and synthetic AssetCore tests only | Remove in Stage 2: the built-in registry contains zero edges and no repository package needs a format transition |
| Synthetic package codec `0xffff0004` | Migration characterization only; no authored/cooked fixture or runtime caller | Remove with the migration subsystem after decode/audit separation is qualified |
| Structure upgrader registry and `FAssetMigrationContext` | Public declaration and private implementation only; zero production or test registrations | Remove in Stage 2 |
| Unknown-field compatibility state and save opt-in | Current v4 live load, mutation guards, canonical-resave admission, and tests | First repair the five stale-schema packages; then change current live load to reject incompatible schemas before publication and remove partial risk state |
| `IsAssetMigrationLoad` | Nine Engine call sites across animation, material, mesh, terrain, environment, and texture loading | Remove with source-aware migration load; Stage 3 preserves authored/cooked behavior directly |
| Authored/cooked `EPackageLoadMode` | Runtime construction, Cook, and Engine payload policy | Retain for M2; M4 owns immutable runtime-domain simplification |

The repository/project audit discovered 28 packages: all are DAST v4, 23 are
schema-compatible, and five retain the removed material
`ParameterSchemaVersion` field (two Engine packages and three Sandbox material
packages). There is no tracked older-format package. The seven checked-in
compatibility hex fixtures are current-format success/corruption/schema cases;
the only alternate format is synthesized in tests. The asset baseline therefore
correctly rejects the five stale-schema packages and gives Stage 2 a concrete
corpus-repair prerequisite.

Stage 0 repaired two pre-existing audit-tool contract drifts: initialization
logs no longer pollute the JSON stdout channel, and the DevTool v2 schema now
accepts the native report's canonicalization evidence. The focused tool suite
passes 24/24, `AssetPackageTests` passes 100/100, and the M1 qualification
already established `AssetCookTests` 13/13 plus restart coverage.

## Stage 1: Separate Decode From Canonical Audit

Dependencies: Stage 0 complete.

- [x] Make ordinary load perform bounded validation and one decode without
  canonical reserialization.
- [x] Keep canonical comparison/resave behind explicit offline/editor APIs.
- [x] Preserve structured format/schema/corruption diagnostics and failure-
  atomic package publication.
- [x] Remove duplicate readers, byte passes, and compatibility state used only
  by the former load-time audit.

### Acceptance Gate

- Current packages load through one read/decode path and never re-encode during
  ordinary load.
- Explicit canonical audit still detects and repairs noncanonical current data.

The v4 reader now has a bounded structure-decode primitive and keeps
`DecodePackage` as the explicit canonical-validation operation. Live load calls
the structure decoder directly; inspection, compatibility audit, reference
projection, and byte mutation retain byte-for-byte re-emission where their
contracts require canonical input. A test-only counter proves live load performs
zero re-encodes while explicit decode performs one. `AssetPackageTests` remains
green at 100/100 with the existing malformed input, failure-atomic publication,
compatibility, restart, and canonical validation matrix.

## Stage 2: Remove Unused Migration And Upgrader Machinery

Dependencies: Stage 1 complete.

Stage 2 starts by repairing the only five incompatible current-format packages
identified by the frozen inventory. A one-use, apply-only compatibility-loss
permission removed the obsolete `ParameterSchemaVersion` field, then the
permission was deleted rather than retained as a production escape hatch. The
canonical-resave tool now publishes a complete validated Asset Catalog before
loading selected packages, so dependency resolution uses the same catalog
contract as ordinary runtime loading. The resulting corpus audit reports 28/28
compatible packages and the baseline reports 28/28 current DAST v4 packages.

Package residency follows the UE model throughout this work: residency is the
fact that a package is loaded, while dirty state records unsaved edits. They are
orthogonal. `UnloadPackage` is therefore the sole lifetime operation, with
`RejectUnsaved` or explicit `DiscardUnsaved` policy; “draft” is not a second
package store or an unload-shaped API.

The dormant migration framework is now removed: the native and DevTool migrate
commands, registry, journal/recovery writer, migration report schema, synthetic
format codec, registry publication backdoor, and migration-only tests had no
tracked package or production transition to own them. Live v4 loading now
checks every serialized class and field against the captured reflection catalog
immediately after its single structure decode, before constructing or
publishing a package skeleton. The retained offline probe still reports exact
unknown-field and signature diagnostics without constructing objects.

- [x] Remove unconsumed structure-upgrader registrations and legacy field
  adapters proven unnecessary by the Stage 0 fixture audit.
- [x] Remove partial compatibility package state when no retained transition
  requires it.
- [x] Collapse compatibility policy to the smallest strict current-format and
  explicitly supported-transition contract.
- [x] Move any still-required fixture conversion to an explicit offline tool or
  narrowly owned adapter with restart evidence.

### Acceptance Gate

- Production code contains no dormant migration registry or unowned upgrader.
- Unsupported content fails before object publication; supported content has no
  partial compatibility state.

The live-load report no longer carries legacy fields or compatibility-risk
items, save APIs have no data-loss bypass, and the resident store has no risk
side table. The old editor compatibility rejection wrapper and its startup
timing phase were removed because a strict load failure is now the single
boundary. Level activation rollback retains an explicit package-load snapshot,
independent of compatibility policy. Validation passes: DevTool contracts
31/31, AssetPackageTests 94/94, DurinAssetTool builds, and
EditorAssetWorkflowTests passes 77 tests with one existing platform skip.

## Stage 3: Simplify Engine Load Consumers

Dependencies: Stage 2 complete.

- [x] Remove Engine `PostLoad` and payload-policy branches that depend on mutable
  migration/load mode rather than authored versus cooked construction.
- [x] Remove `LoadPackageForMigration`, `IsAssetMigrationLoad`, and their scoped
  mutable load state after the Engine consumers are gone.
- [x] Preserve source/DDC fallback rules and Cooked-runtime fail-closed behavior
  for every affected asset class.
- [x] Update affected restart, source-missing, DDC-missing, and cooked payload
  tests around the simplified domain contract.

### Acceptance Gate

- Engine load behavior depends only on explicit immutable runtime domain and
  asset payload state.
- No production class queries migration-specific package state.

All nine Engine migration branches and AssetCore's thread-local scoped
migration loader are gone. Canonical resave uses ordinary `LoadAsset`; it has no
privilege to bypass authored payload or resource validation. The former
skeletal migration test now proves missing required DDC fails the authored load
and rolls back the entire dependency residency set. The former material legacy
map test now proves incompatible fields fail before either package becomes
resident. Focused validation passes: SkeletalAssetTests 34/34, TextureTests 66
with two platform skips, TerrainHeightmapTests 11/11,
EnvironmentLightingTests 3/3, MaterialTests 74/74, StaticMeshTests 68/68, and
AssetCookTests 13/13.

## Stage 4: Qualify And Publish Compatibility Ownership

Dependencies: Stages 0-3 complete.

- [x] Run focused AssetCore package/compatibility/restart/Cook and affected
  Engine asset suites.
- [x] Run complete native qualification, full build, and hidden-window editor
  smoke without concurrent build processes.
- [x] Search for retired migration/upgrader/load-audit symbols and duplicate
  compatibility policy.
- [x] Update Asset Packages, Asset Versioning, Asset Data Lifecycle, and roadmap
  contracts with the implemented decode/audit boundary.
- [x] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record evidence for the M2 exit gate.

### Acceptance Gate

- Ordinary load is a strict one-decode current-format path.
- Offline audit owns canonical comparison and any retained conversion.
- Lasting documentation and validation evidence satisfy the M2 roadmap exit
  gate and activate only dependency-ready M3/M4 work with child plans.

Qualification passed on 2026-08-15. The default `all` build completed, the
complete native target suite passed, and DurinEditor remained alive through an
8-second hidden-window Sandbox smoke. The authored baseline reports 28/28
current DAST v4 packages. Focused evidence from Stages 2-3 includes
AssetPackageTests 94/94, EditorAssetWorkflowTests 77 with one platform skip,
AssetCookTests 13/13, and all affected material, mesh, skeletal, texture,
terrain, and environment suites. Retired-symbol search found no production
migration command, upgrader, migration-load mode, partial compatibility-risk
state, or discard-draft API. Changed-document validation passed for nine files;
all 171 active/completed/archived plan records, all 16 roadmaps, and all 112
active repository documents validated.

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
- [`AssetSystem.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp)
- [`AssetPackageV4Reader.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp)
- [`PackageTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp)
