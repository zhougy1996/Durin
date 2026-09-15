# Package Property Load Refactor Plan

Summary: Separate ordinary default-relative serialization from opt-in authored replacement state and eliminate exhaustive loaded-property ledgers.

Last reviewed: 2026-09-14

Status: Archived
Completed: 2026-09-14

## Current Status

Implemented and validated on 2026-09-14. Ordinary Explicit values no longer
allocate a ledger; opt-in Forced replacements are sparse field boundaries.
Full emission and replacement intent are independent. The v9 adapter writes
complete selected composites, matching its shared type descriptors.

Validation on Win64-Debug-DurinEditor:

- CoreObjectTests: 87 passed; AssetPackageTests: 151 passed.
- `build --target all`: passed, including Engine, Sandbox, and RoadWeaver targets
  (`Build/.agent-state/logs/20260914-123534-832229-30008-cmake.log`).
- `test affected`: 82 of 83 targets passed, including MaterialTests, package
  reload, editor transactions, and material/asset integration coverage
  (`Build/.agent-state/logs/20260914-123810-465575-39568-ctest.log`).
- The only failing target was StaticMeshThumbnailTests; after the test-only async
  correction below, all 9 tests passed in a focused rerun
  (`Build/.agent-state/logs/20260914-124321-971303-37964-StaticMeshThumbnailTests.log`).
  Other target results and the production all build remain applicable.
- The 65-node material regression passes two save/load/duplicate cycles with
  exact program equality and no allocated ledger. This is correctness and
  retained-state evidence, not a GUI latency or GPU timing claim.
- Changed-document, all-plan, and diff-whitespace validation passed.

The earlier investigation measured 7,552 override entries for the 82-node
ImportedSurface asset, with a representative Debug restoration/publication time
of 1,587 ms (including 967 ms sorting and 426 ms recapture/shape validation).
These overlapping, local measurements motivate eliminating unnecessary state;
they are not acceptance timings or a claim about the current asset revision.

## Goal

Ordinary authored load/save operates on values and defaults without allocating
an override ledger. Explicit replacement remains opt-in through the object API
and survives package save/load and duplication. Full serialization is an Archive
policy, not an instruction to manufacture persistent authored overrides.

## Selected Design and Compatibility

- Keep Core byte Archives, CoreDObject reflection/default planning, and Engine
  package application as separate layers. Keep canonical DAST v9 parsing,
  validation, deprecated-field migration, and atomic publication.
- Remove LoadedExplicit from the runtime override API. A Forced mark means
  replacement of the complete selected field. Parents needed to carry a nested
  Struct mark are emitted normally, not promoted into independent overrides.
- Validate explicit paths, then normalize indexed Array, fixed-array, and Map
  routes to the owning container field. Store replacement marks only at field
  boundaries; parent replacement subsumes children. This avoids persistent
  positional element identity and redundant parent/child state.
- Ordinary wire Explicit values restore values only. Restore wire Forced values
  as sparse replacement marks, stopping below an already replaced field.
  Historical Forced tags cannot distinguish old NoDelta output from intentional
  overrides; preserve them conservatively as replacement state. Do not guess
  user intent or rewrite shipped assets.
- The existing SavePackage entry uses NoDelta; retain that full-value asset
  policy. Default-relative Enabled planning is tested independently. Switching
  SavePackage to Enabled also requires dynamic Outer-graph/default coverage and
  is not necessary to eliminate loaded ledgers.
- New NoDelta output writes ordinary Explicit tags except where the caller has
  opted into a replacement. Cooked loading continues to omit override state.
- V9 uses complete shared Struct descriptors. Materialize every child of an
  emitted composite, while Enabled planning selects object fields. The previous
  bridge omitted children without narrowing descriptors and failed canonical
  writing; cover that correction with Enabled and NoDelta package round trips.
- Preserve v9 default reconstruction: top-level fields compare with the CDO;
  emitted Structs compare against their type default, including in containers.
  Changing this to paired nested archetype defaults requires a separately
  versioned writer/reader contract and is not part of this compatible refactor.
- Retain existing validated copy-on-write publication. No unchecked loader API,
  parallel loader, trie, custom material blob, or new editor override UI is
  required. General paths remain available for validation and diagnostics.
- Values explicitly saved equal to defaults may be omitted on ordinary resave;
  this is the intentional semantic change. Default evolution follows omitted
  values. Old bytes remain readable; resaved bytes need not remain identical.

## Implementation Stages

### Stage 0: Select serialization and compatibility semantics

Dependency: none. Outcome: the selected design above supersedes exhaustive
LoadedExplicit preservation without claiming a private UE implementation.

- [x] Audit loading, planning, copying, and editor/project override consumers.
- [x] Separate NoDelta emission from persistent intent and record legacy policy.
- [x] Keep nested v9 default reconstruction compatible with existing packages.

### Stage 1: Implement and validate sparse authored replacements

Dependency: Stage 0. Outcome: ordinary assets load without a ledger and explicit
replacements remain complete, stable, validated, and serializable.

- [x] Migrate runtime override API and every source/test consumer in Engine,
  Sandbox, and RoadWeaver; normalize and coalesce replacement paths.
- [x] Separate ordinary, complete, and explicitly replaced planning; restore
  only forced field boundaries during package loading.
- [x] Cover reset-to-default/resave, nested replacement, container edits,
  NoDelta, old forced tags, malformed paths, copy, reload, and material graphs.
- [x] Run focused and affected native tests plus an all build, following
  [testing](../../../Agents/Testing.md) and [build guidance](../../../Agents/BuildAndRun.md).
- [x] Update [Serialization](../../../Runtime/Core/Serialization.md), validate changed
  documentation and all plans, and record exact results before completion.

## Validation Findings

The 83-target affected run passed 82 targets, including MaterialTests and the
65-node graph regression, but exposed three stale StaticMeshThumbnailTests
assertions. A standalone rerun reproduced them. The pool uses background cache
reads, while those tests assume one EndFrame completes disk I/O and count corrupt
bytes as a disk hit. Update only these tests to wait for observable completion
with a bounded deadline and assert the current async retry/load counters. Keep
all thumbnail production code unchanged and rerun that target; retain passing
results for the other unchanged targets.

## Unreal Engine Reference

Epic separates [tagged serialization with defaults](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UStruct/SerializeTaggedProperties)
from [opt-in overridable serialization](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject/FOverridableSerializationLogic?application_version=5.5).
This plan borrows that separation; it does not adopt UE's experimental override
implementation or claim an editor/GPU performance result.
