# Async Asset Build Orchestration Plan

Summary: Generalize editor-side asynchronous asset build completion, commit, rollback, and recovery orchestration.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

The texture editor no longer waits synchronously while replacing a shared
source, but the first non-blocking implementation placed a complete
prepare/build/commit/rollback/rebuild state machine in a TextureEditor-owned
`FSharedSourceReplacementWorkflow`. That removes the UI stall while leaving two
architectural gaps: build completion is still expressed by a Texture2D-specific
boolean callback, and reusable compensating-operation semantics live in an
asset-family UI module.

The implemented replacement is a three-layer boundary. AssetBuildCore owns a
family-neutral terminal build result and completion signature; DurinEd owns a
completion-driven compensating asynchronous operation; TextureEditor owns only
the mounted-source and Texture2D adapter. `EditorOperationTests` passes 5/5,
`TextureTests` passes 81/81, and `EditorPropertyTests` passes 33/33.
`EditorAssetWorkflowTests` passes 89 cases with its existing Windows
directory-symlink privilege skip, and the Debug DurinEditor `all` build passes.
The lasting ownership and completion contract is recorded in
`Documentation/Editor/Architecture/AsyncAssetOperations.md` and
`Documentation/Runtime/Assets/AssetDataLifecycle.md`.

## Goal

Provide reusable editor infrastructure for asynchronous asset mutations that
must commit only after a build succeeds and must restore the prior derived state
after a failure, without blocking the editor thread or encoding Texture2D,
mounted-source, or Widget policy into the infrastructure.

## Scope

- Define a family-neutral asynchronous build terminal result and completion
  callback in AssetBuildCore.
- Guarantee one terminal completion for accepted Texture2D authoring requests,
  including failure, cancellation, and supersession.
- Add a DurinEd operation coordinator for
  `prepare -> async apply -> commit` and
  `rollback -> async compensation`.
- Move mounted-source replacement and Texture2D reimport wiring out of
  `MTextureEditor` into a TextureEditor-private adapter.
- Preserve the existing user workflow, document-save semantics, source-index
  invalidation, diagnostics, and explicit user cancellation behavior.

## Non-Goals

- Replacing family-owned worker schedulers with one global build scheduler.
- Making AssetBuildCore interpret Texture, Geometry, Terrain, or other typed
  products.
- Converting synchronous StaticMesh, TextureCube, or import providers to async
  execution in this change.
- Redesigning undo/redo history or making shared-source replacement undoable.
- Removing the explicitly user-invoked `Wait for Build` diagnostic action.

## Design Decisions and Invariants

- AssetBuildCore owns only the family-neutral terminal vocabulary and callback;
  TextureBuild continues to own request admission, worker priority,
  cancellation, supersession, publication, and completion pumping.
- An accepted build request completes exactly once with `Succeeded`, `Failed`,
  `Canceled`, or `Superseded`. Starting a newer request must complete the older
  observer instead of silently discarding its callback.
- DurinEd's coordinator consumes callbacks and never polls a family service or
  waits on a worker. Apply and compensation completions execute on the owning
  editor thread; inline completion is supported.
- Preparation owns rollback until commit succeeds. Rollback executes at most
  once. A failed apply or commit preserves its primary diagnostic even when
  compensation also fails.
- Destroying or aborting an active coordinator detaches future callbacks,
  requests cancellation, and rolls back prepared external state when apply has
  not reached compensation.
- TextureEditor adapts the generic contracts through weak object references.
  The Widget owns presentation state only: active asset identity, progress
  label, conflict disabling, and final error display.
- Package save precedes mounted-source commit. A save failure restores source
  bytes and rebuilds the texture from the restored source.

## Current Foundations and Gaps

- AssetBuildCore already owns the family-neutral build host, while its recipe
  session intentionally remains synchronous and worker-agnostic.
- TextureBuild already owns an asynchronous Texture2D coordinator and pumps
  completions on the game thread, but exposes
  `FTexture2DAuthoringCompletion(bool, string)` and silently drops a superseded
  request's callback.
- DurinEd property editing independently implements deferred completion,
  cancellation, lifetime detachment, and inline-completion handling. Those
  invariants inform the shared coordinator but property snapshot policy remains
  separate.
- The current TextureEditor workflow polls `HasPendingTexture2DBuild` and owns
  generic compensation sequencing under a shared-source-specific public name.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm the three-layer ownership boundary and dependencies.
- [x] Separate generic orchestration from typed build and mounted-source policy.
- [x] Define non-goals for scheduler, provider, and undo/redo expansion.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Generalize asynchronous build completion

- [x] Add the AssetBuildCore terminal result and completion callback contract.
- [x] Migrate Texture2D authoring submission and AssetForge forwarding APIs.
- [x] Complete superseded observers exactly once and preserve cancellation and
  failure diagnostics.
- [x] Add focused completion-status and supersession tests.

#### Acceptance Gate

- Texture2D callers no longer depend on a family-specific boolean completion,
  and every accepted request has one observable terminal result.

### Stage 2: Add generic compensating editor orchestration

- [x] Implement the DurinEd completion-driven coordinator with safe owner
  detachment, cancellation, inline completion, and at-most-once rollback.
- [x] Cover success, apply failure, commit failure, compensation failure,
  cancellation, and inline completion.

#### Acceptance Gate

- The coordinator has no asset, build-family, mounted-source, package, or Widget
  dependency and its focused contract tests pass.

### Stage 3: Migrate TextureEditor shared-source replacement

- [x] Add a TextureEditor-private adapter for mounted-source preparation,
  Texture2D build submission, document save, commit, rollback, and recovery.
- [x] Replace Widget-owned callback wiring and polling with the generic
  coordinator and adapter.
- [x] Keep conflicting controls and close behavior disabled only while the
  operation is active, and surface apply versus compensation progress.
- [x] Remove the shared-source-specific public workflow and its obsolete tests.

#### Acceptance Gate

- Shared-source replacement contains no synchronous build wait and
  `MTextureEditor` contains no transaction sequencing or family-service polling
  for this workflow.

### Stage 4: Validate and document the implemented contract

- [x] Run focused coordinator, Texture2D build, and editor workflow tests.
- [x] Run the complete affected test targets and the Debug DurinEditor `all`
  build under the repository agent workflows.
- [x] Record the lasting ownership and terminal-completion contract in the
  authoritative asset lifecycle documentation.

#### Acceptance Gate

- All required checks pass, any environment skip is identified, lasting
  documentation matches the implementation, and the plan is complete.

## Validation Matrix

| Concern | Validation | Required result |
| --- | --- | --- |
| Generic operation transitions | Focused DurinEd operation tests | All phases and inline completion pass |
| Texture2D terminal completion | Focused TextureBuild tests | Success, failure/cancel, and supersession complete once |
| Shared-source editor behavior | `EditorAssetWorkflowTests` focused and whole target | No regression beyond documented environment skips |
| Module/API integration | Debug DurinEditor `all` build | Successful |
| Plan and lasting docs | Changed-doc and all-plan validation | Successful |

## Definition of Done

- No shared-source replacement path calls `WaitForTexture2DBuild`.
- Generic orchestration is owned by DurinEd and typed policy by TextureEditor.
- Asset build completion has a family-neutral result and exactly-once terminal
  semantics for accepted Texture2D requests.
- Required tests, documentation validators, and the editor build pass.
- Changes and plan status are committed together with exact plan/stage
  provenance.

## Deferred Follow-ups

- Adopt the generic terminal result in future asynchronous GeometryBuild or
  Terrain authoring coordinators when those APIs expose caller-owned requests.
- Evaluate request-specific generic observation/cancellation handles if more
  than one asset family needs external polling or cross-editor progress UI.
- Consider consolidating DurinEd property-deferred lifetime plumbing on the
  generic coordinator only after its different snapshot/apply semantics are
  proven compatible.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Agent Build And Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildHost.h`
- `Engine/Source/Developer/TextureBuild/Public/Texture/Texture2DAuthoringService.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/CompensatingAsyncOperation.h`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/MountedSource.h`
