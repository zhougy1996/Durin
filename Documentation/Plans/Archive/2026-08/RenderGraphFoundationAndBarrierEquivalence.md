# Render Graph Foundation and Barrier Equivalence Plan

Summary: Implement deterministic graph compilation, resource declarations, transition planning, diagnostics, and structural validation.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

All stages are complete. `RenderCore` compiles deterministic graph structure
and exact RHI transition batches, 50 focused RenderCore contract tests pass,
and the Vulkan graph replay fixture passes through the backend state tracker.
The 128-pass Debug synthetic gate is bounded below 250 milliseconds. The
lasting contract is published in Runtime Rendering and milestone 2 is ready.

## Goal

Provide a deterministic, backend-neutral Render Graph builder and compiler that
rejects invalid declarations before command recording and emits the exact RHI
buffer/texture transition batches required by declared resource uses.

## Scope

- Frame-local texture and buffer handles with imported ownership-neutral views.
- Stable pass declarations, explicit dependencies, exact ranges/subresources,
  initial/final access, attachment semantics, and graphics/compute/copy intent.
- Deterministic hazard compilation, cycle detection, transition batches,
  immutable diagnostics, execution, and bounded compile telemetry.
- Synthetic contract tests for failure, dependency, transition, and ordering
  behavior.

## Non-Goals

- Production renderer migration, transient allocation, pass culling, aliasing,
  multi-queue scheduling, pass reordering, or PSO ownership changes.
- A process-global resource registry or string-based frame blackboard.

## Design Decisions and Invariants

- Handles are builder-local typed values; invalid kind/index/generation use is a
  compile error.
- Declaration order is the stable tie-breaker. Explicit and resource-hazard
  edges are topologically compiled without performance reordering.
- Imported resources retain external ownership and declare exact initial/final
  access. Graph-created resources begin discarded and require a producer before
  a read.
- A use declares one exact buffer range or texture subresource range. Partial
  overlap between differently shaped declarations is rejected until interval
  splitting has a measured production consumer; exact matches and disjoint
  ranges compile independently.
- The graph emits existing `FRHIBufferTransition` and
  `FRHITextureTransition` values. RHI remains the execution-state authority.
- Compilation completes before any callback or command-list mutation.

## Current Foundations and Gaps

`ERHIAccess`, exact transition descriptors, command-list replay, Vulkan state
validation, and immutable scene preparation already exist. `RenderCore` lacks
logical resource identities, pass declarations, dependency compilation,
pre-recording validation, and graph diagnostics.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Implement graph declarations and deterministic compilation

- [x] Add typed frame-local handles, imported resource declarations, and pass
  uses with exact ranges/subresources.
- [x] Compile explicit and hazard dependencies with stable topological order.
- [x] Reject invalid handles, missing producers, illegal access declarations,
  partial overlaps, and cycles before execution.

#### Acceptance Gate

- Synthetic graphs produce exact stable dependency order and invalid graphs
  return deterministic pass/resource diagnostics.

### Stage 2: Compile and execute RHI transition batches

- [x] Derive per-pass buffer and texture transitions from declared initial,
  use, discard, and final states.
- [x] Execute immutable batches and callbacks only after successful compilation.
- [x] Expose deterministic dumps and compile counters without allowing
  telemetry to affect correctness.

#### Acceptance Gate

- Graphics/compute/copy fixtures emit exact expected transitions and final
  states using existing RHI descriptors.

### Stage 3: Qualify the foundation

- [x] Cover read/write hazards, stable independent ordering, cycles, discard,
  attachment uses, exact ranges/subresources, and final states.
- [x] Record bounded synthetic compile-cost evidence and validate the focused
  RenderCore target.
- [x] Publish the lasting foundation contract and complete the milestone.

#### Acceptance Gate

- Focused tests and documentation validation pass, diagnostics are stable, and
  the measured synthetic compile cost is recorded.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Structure | Focused compiler tests for handles, producers, cycles, overlap, and ordering |
| Dependencies | Exact RAW/WAR/WAW and explicit-edge assertions |
| Transitions | Exact buffer/texture pre-pass and final batches |
| Diagnostics | Repeated compile/dump equality and named failure assertions |
| Performance | Bounded synthetic graph compile median/upper-bound test |

## Definition of Done

- Stages 1-3 and their acceptance gates pass.
- The roadmap marks milestone 1 complete and activates only the pilot plan.
- Changes are validated and committed with this plan and stage provenance.

## Deferred Follow-ups

- Renderer pilot selection and boundary adapters belong to milestone 2.
- Transient allocation/culling and whole-frame migration belong to milestone 3.
- Aliasing and queue scheduling remain evidence-gated roadmap work.

## Related Documentation

- [Render Graph Architecture Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Renderer Frame Preparation](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
