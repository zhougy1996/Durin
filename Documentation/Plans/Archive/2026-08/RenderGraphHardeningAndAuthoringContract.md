# Render Graph Hardening and Authoring Contract Plan

Summary: Harden typed graph authoring, add immutable frame inspection, and freeze compile and execution regression budgets.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Milestone 4 is complete. RenderCore now exposes deterministic structural
budgets, observational compile/execute CPU statistics, and an owning
pointer-free capture. The scene-frame executor publishes captures only after
graph execution, and the scene/contact graphs carry named production budgets.

Validation passed 55 `RenderContractTests`, 31
`RendererSceneContractTests`, 77 `EditorRenderingTests`, the full `all` build,
and the complete directional-shadow Vulkan qualification including a real
scene capture under its 12-pass/24-edge/5 ms compile/250 ms execute ceilings.
The GBuffer qualification recorded full-frame 397.120/398.912 microseconds
(median/p95) and contact routes around 382.7-384.5 microseconds; its aggregate
case remained red only at the pre-existing unrelated half-resolution GTAO
ratio and emitted the known `DefaultWhiteArray` comparison-sampling diagnostic.
No graph, full-frame, contact, or migrated-route budget failed.

## Goal

Make the render graph the enforceable default for new inter-pass renderer work,
with deterministic structural budgets, immutable captures, actionable failures,
and explicit CPU regression telemetry.

## Scope

- Add structural and CPU budget declarations to graph compilation.
- Add immutable compiled-graph captures and statistics.
- Publish completed scene-frame captures through a read-only observer seam.
- Apply production budgets to the scene-frame and contact-shadow graphs.
- Freeze the authoring rules and regression gates in lasting documentation.
- Add contract and renderer integration coverage for the new inspection feature.

## Non-Goals

- Transient allocation or aliasing.
- Async-compute queue scheduling.
- Replacing the existing renderer feature timing and image-capture seams.
- Failing a rendered frame because an observed wall-clock budget was exceeded.

## Design Decisions and Invariants

- Structural limits are deterministic compile gates; CPU limits are reported in
  captures and tests but never control rendering correctness.
- A capture owns copies of all public diagnostic data and remains valid after the
  compiled graph is destroyed.
- Capture sinks receive only immutable snapshots and cannot alter scheduling,
  transitions, resource preparation, or pass execution.
- Every production inter-pass dependency is a typed texture, buffer, token, or
  explicit dependency; every culled graph has an explicit output/effect root.
- Production graph budgets are named constants beside their authoring site.

## Current Foundations and Gaps

- The compiler already exposes passes, dependencies, lifetimes, culling, dumps,
  and compile duration, but these views borrow the compiled graph lifetime.
- The scene-frame graph has no frozen size/transition budget and no whole-frame
  inspection seam.
- Contact-shadow production graphs have typed resource declarations but no
  structural budget.
- The runtime documentation does not yet define the mandatory new-feature path.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Add budgets and immutable capture

- [x] Add public budget, statistics, and owning capture value types.
- [x] Enforce pass, dependency, and transition limits deterministically.
- [x] Measure compile and execute CPU duration for observational regression data.
- [x] Cover structural failures, capture lifetime, and deterministic dump parity.

#### Acceptance Gate

- Over-budget graphs fail with the exceeded dimension and actual/limit values.
- Captures remain usable after graph destruction and do not expose callbacks or
  mutable RHI resources.

### Stage 2: Wire the production inspection feature

- [x] Publish immutable captures after scene-frame execution.
- [x] Freeze budgets for the scene-frame and contact-shadow production graphs.
- [x] Prove that enabling capture observes the migrated graph without bypassing
  it or changing output/control flow.

#### Acceptance Gate

- A renderer integration test receives the expected scene-frame capture while
  the normal graph execution path remains authoritative.

### Stage 3: Qualify the authoring contract

- [x] Document the mandatory authoring path and budget policy.
- [x] Run RenderCore contracts, renderer contracts, Vulkan qualification, full
  build, and documentation validation.
- [x] Update the roadmap evidence and close this child plan.

#### Acceptance Gate

- A post-migration inspection feature lands entirely through the graph path,
  errors/dumps are actionable, and frozen regression gates pass.

## Validation Matrix

| Concern | Validation |
| --- | --- |
| Compiler/capture contracts | `RenderContractTests` |
| Scene-frame observer and budgets | `RendererSceneContractTests` |
| Migrated renderer integration | directional-shadow/GBuffer Vulkan qualification |
| Build integrity | full `all` target |
| Documentation | plans, links, and index validators |

## Definition of Done

- All stages and acceptance gates are complete.
- The roadmap marks milestone 4 complete with measured evidence.
- Changes are committed with plan/stage provenance.

## Deferred Follow-ups

- Transient allocation/aliasing remains milestone 5.
- Async-compute scheduling remains milestone 6.
- Legacy cleanup remains milestone 7.

## Related Documentation

- `Documentation/Roadmaps/Archive/2026-08/RenderGraphArchitecture.md`
- `Documentation/Runtime/Rendering/RenderGraph.md`

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRendererProfiling.*`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
