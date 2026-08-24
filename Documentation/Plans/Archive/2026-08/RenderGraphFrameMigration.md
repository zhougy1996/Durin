# Render Graph Frame Migration Plan

Summary: Integrate logical transient lifetimes and culling, migrate the complete scene frame schedule, and retire fixed orchestration.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

All stages are complete. The compiler now exposes logical token lifetimes,
explicit-root culling, deterministic reasons, and a complete-or-abort
execution preparation gate. `FRenderGraphSceneFrameExecutor` owns the sole
production schedule and the former fixed executor symbol/files are gone.
Validation passed 53 RenderCore contracts, 30 renderer contracts, 77 editor
rendering tests, 7 EditorGrid Vulkan tests, resource reload, volumetric-cloud
scene Vulkan, and directional-shadow qualification. The full-frame GBuffer
gate recorded total median/p95 398.576/401.792 microseconds with all migrated
frame/contact gates passing; its aggregate remained rejected only by the
unrelated GTAO half/full ratio and pre-existing DefaultWhiteArray diagnostics.

## Goal

Make one Render Graph the sole production scene-frame scheduler from
directional shadow through final output, with observable logical lifetimes,
graph-owned transient lease preparation, explicit side effects, deterministic
culling, and no surviving fixed orchestration authority.

## Scope

- Generic graph lifetimes, explicit roots/effects, deterministic backward
  reachability culling, and allocation preparation before callbacks.
- Renderer target declarations backed by the existing transient pool and its
  budgets/recovery generations.
- Top-level scene passes, optional feature branches, temporal/output
  transactions, and final presentation under one compiled graph.
- Removal of the former fixed executor and migrated inter-pass transitions.

## Non-Goals

- Physical aliasing, placed resources, multi-queue execution, async compute,
  pass reordering, feature algorithm changes, or target-pool replacement.
- Treating persistent/history/output resources as graph-owned transients.

## Design Decisions and Invariants

- Graph compilation computes reachability and lifetimes on logical identities;
  complete transient lease preparation occurs after successful compile and
  before the first callback. Allocation failure executes nothing.
- Present, temporal publication, readback/capture, timing, and other
  non-resource work are explicit roots/effects. Fallback selection remains a
  graph-construction decision.
- Top-level feature callbacks may own internal implementation steps, but every
  resource crossing callbacks is declared once and transitioned by the graph.
- The existing pool remains the physical budget/recovery authority and leases
  remain alive through recorded-command retention.
- Production is renamed to graph scheduling in one commit; no runtime fixed
  path or migration toggle survives.

## Current Foundations and Gaps

The pilot proves graph execution against real Renderer/RHI/Vulkan behavior.
The compiler previously lacked lifetimes, roots, culling, and an execution
preparation gate. The renderer resolved every optional target before scheduling
and sequenced feature methods imperatively in the former fixed executor.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Add lifetimes, effects, culling, and allocation preparation

- [x] Compile first/last use for logical transient resources.
- [x] Add explicit roots/effects and deterministic unused-branch culling.
- [x] Resolve all required physical resources as one complete preparation
  before execution and diagnose allocation/culling decisions.

#### Acceptance Gate

- Synthetic graphs prove lifetimes, pure-branch culling, effect retention,
  allocation abort, deterministic dumps, and no callback on failure.

### Stage 2: Migrate the complete frame schedule

- [x] Declare top-level resources and optional passes from prepared policy.
- [x] Execute directional shadow through final output under one compiled graph.
- [x] Preserve typed outcomes, temporal commit/abort, multi-view isolation,
  resize/recovery, telemetry, images, and runtime modes.

#### Acceptance Gate

- Production frames use one graph and every cross-pass resource edge has one
  scheduling/transition authority.

### Stage 3: Retire fixed orchestration and qualify

- [x] Remove/rename the fixed executor and its contract references.
- [x] Pass focused, aggregate, Vulkan, image, multi-view, resize, recovery, and
  performance gates.
- [x] Publish lasting lifetime/culling/authoring contracts and complete the
  milestone.

#### Acceptance Gate

- No fixed production scheduler or alternate graph path remains; milestone 4
  alone becomes ready.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Lifetimes/culling | RenderCore structural tests and deterministic dumps |
| Allocation/recovery | Renderer transient pool failure, budget, invalidation, shutdown tests |
| Frame correctness | Renderer contracts plus image/readback feature qualifications |
| Temporal/isolation | Resize, multi-view, duplicate submission, Begin/Commit/Abort fixtures |
| State | RHI/Vulkan exact transition validation in inline/threaded modes |
| Performance | Frozen graph build/compile/execute and full-frame median/p95 gates |

## Definition of Done

- Stages 1-3 pass, fixed scheduling is removed, and lasting contracts are
  updated.
- The roadmap marks milestone 3 complete and activates only milestone 4.
- Changes are committed with exact plan provenance.

## Deferred Follow-ups

- Stable feature-authoring helpers, capture UI, and frozen aggregate budgets
  belong to milestone 4.
- Aliasing and queue-aware scheduling remain evidence-gated roadmap work.

## Related Documentation

- [Render Graph Architecture Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Persistent View State](../../../Runtime/Rendering/PersistentViewState.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.cpp`
