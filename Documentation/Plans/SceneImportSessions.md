# Scene Import Sessions Plan

Summary: Replace blocking scene import and material preview with a reusable asynchronous scene session and visible progress while preserving asset publication semantics.

Last reviewed: 2026-09-22

Status: Completed
Completed: 2026-09-22

## Current Status

All stages are complete. LevelEditor uses the session, automatically prepares material
configuration, and shows cancellable phase/count feedback. Existing synchronous
callers share the session implementation with yielding disabled for compatibility.

Validation on `Win64-Debug-DurinEditor`: all build passed; SceneImportTests 20/20,
AssetPackageTests 189/189, MaterialCompileLifecycleTests 4/4, and
SceneImportVulkanTests 1/1 passed. The Vulkan fixture now imports asynchronously and
explicitly settles reloaded texture compilation/upload before image comparisons;
without that wait both synchronous and asynchronous imports produced the same
premature fallback image. Changed-document validation, all-plan validation and
whitespace/diff review passed. No manual editor interaction or large-scene frame-time
qualification was performed; material policy, package capture and atomic commit
remain owner-thread work without a hard per-package frame-time guarantee.

The selected persistence API is `FPreparedAssetSave`: capture private candidate bytes
on GameThread, stage through the existing protected save owner, then prepare reference
replacement immediately before synchronous commit. Preparing replacement before I/O
would reject unrelated object creation during the wait because replacement snapshots
track object-array revisions. Add a nonblocking material-owner work query to include
canceled requests awaiting reap. Test successful import, preview reuse, cancellation,
changed sources, reimport preservation and injected partial persistence failures.

The LevelEditor host owns and ticks the session; teardown explicitly cancels and drains.
No new global tick service is needed. Changing coordinate settings invalidates parsing;
destination/material changes reuse the parsed scene. Per-package capture and graph
commit remain indivisible owner-thread operations; timing qualification is separate.

## Goal

Keep editor frames responsive during scene preparation, compilation and persistence.
Reuse captured scene data between material configuration and import. Expose phase,
completed work, cancellation and partial persistence results. Preserve unrelated
assets, existing material edits and bindings, private candidates and per-package
rollback semantics.

## Selected Design

- A scene-specific session owns request values, detached preparation, progress and
  terminal results; widgets render state and submit intent.
- Workers access only detached values. Object lookup, policy validation, candidate
  creation, reference replacement and publication remain on the owner thread.
- Owner-thread continuation uses bounded steps. Compilation and disk staging must
  return to normal editor ticks rather than synchronously finish unfinished work.
- Preview and import share an immutable source snapshot and decoded scene. Source
  changes invalidate reuse; destination and material changes only rebuild policy.
- Cancellation discards unpublished candidates after outstanding work settles.
  Already committed packages remain committed and are included in the result.
- One active scene mutation owns publication admission. Shutdown drains or cancels
  accepted work before module teardown; UI destruction cannot leave callbacks to
  destroyed widgets. No generic import framework is introduced.

## Implementation Stages

### Stage 0: Establish asynchronous publication and lifetime boundaries

- [x] Inspect task, compilation, private graph and protected save contracts and
  select the smallest safe integration points.
- [x] Record required API changes and test coverage.
- Completion: implementation boundaries preserve existing rollback and registry
  guarantees without worker access to live objects.

### Stage 1: Implement reusable scene preparation and import sessions

Depends on Stage 0.

- [x] Separate source capture/decoding, material policy and detached product build.
- [x] Add session lifecycle, cancellation, progress and source-result reuse.
- [x] Advance candidate preparation and compilation without blocking waits.
- [x] Integrate asynchronous protected persistence and incremental publication.
- [x] Cover failure, cancellation, stale inputs and partial persistence.
- Completion: a session reaches a terminal result through normal owner-thread
  ticks, with worker ownership and private candidate cleanup verified.

### Stage 2: Integrate the editor and validate the complete flow

Depends on Stage 1.

- [x] Start preparation after source selection and replace mandatory manual
  material preview with session-driven configuration and validation.
- [x] Show phases, work counts, cancellation and terminal diagnostics; guard
  conflicting controls and handle close/shutdown safely.
- [x] Migrate affected callers and document the implemented ownership contract.
- [x] Run relevant native tests and affected builds; complete an all build if a
  shared Engine API changes. Validate documentation and review the final diff.
- Completion: successful import, reuse, cancellation and failure paths are tested;
  remaining timing or UI limitations are stated with evidence.

## Required References

- [Async asset operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Asset compilation](../Runtime/Assets/AssetCompilation.md)
- [Package persistence](../Runtime/Core/PackagePersistence.md)
- [Asset catalog and mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Task system](../Runtime/Core/TaskSystem.md)
- [Build workflow](../Agents/BuildAndRun.md)
- [Test workflow](../Agents/Testing.md)
