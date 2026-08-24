# Render Graph Renderer Pilot Plan

Summary: Migrate one closed renderer production slice to Render Graph with fixed-path parity and single transition authority.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

All stages are complete. Production contact visibility now executes compute
and fragment routes through one graph with no manual transition calls. The
directional-shadow Vulkan qualification, 77 editor-rendering tests, and 30
renderer-scene contracts pass. The latest quiet GBuffer qualification records
contact compute/fragment medians of 555.280/597.104 microseconds with all
contact gates passing; its aggregate remains rejected only by unrelated GTAO
half-resolution/resolve timing gates and pre-existing DefaultWhiteArray Vulkan
diagnostics, so no baseline was changed.

## Goal

Make Render Graph the sole inter-pass transition authority for production
contact-shadow visibility while preserving route selection, factor-one
fallback, images, draw/dispatch identity, timing, telemetry, and failure policy.

## Scope

- Compute and fragment contact-visibility routes after existing preparation
  selects a complete route and target.
- Imported GBuffer/depth inputs, transient visibility output and dynamic
  uniform, plus persistent fullscreen geometry declarations.
- The attachment-to-sampling and compute-to-graphics handoffs, including
  restoration of all imported boundary states.
- Existing production rendering, qualification captures, telemetry, and Vulkan
  state validation.

## Non-Goals

- Any contact-shadow algorithm, shader, quality, target-pool, or route-policy
  change.
- GBuffer, deferred lighting, other features, whole-frame scheduling, culling,
  or logical transient allocation.

## Design Decisions and Invariants

- Existing route/resource preparation completes before graph construction;
  graph compile failure publishes the existing factor-one result without
  partially executing a route.
- The compute route deletes its manual texture/uniform transitions and declares
  exact initial/final states once. The fragment route changes its render-pass
  layout to preserve graph-owned attachment state rather than duplicate an
  implicit transition.
- Pass callbacks resolve every declared texture/buffer through graph resources;
  renderer state owns shaders, pipelines, views, and feature policy.
- Production has one contact-visibility execution path. Tests compare existing
  route, image, telemetry, timing, and barrier outcomes rather than retaining a
  runtime fixed/graph toggle.

## Current Foundations and Gaps

`FContactShadowVisibilityRenderer` already resolves compute then fragment then
factor-one fallback and owns stable GPU qualification. Its compute branch
manually transitions one uniform, five inputs, and one output in and out; its
fragment render-pass layout implicitly transitions the output from discard to
shader read. These are the pilot edges to migrate.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Migrate the closed production slice

- [x] Build one graph after route selection with typed imported, persistent,
  and transient declarations.
- [x] Execute compute or fragment work from graph-bounded resources.
- [x] Delete the migrated manual and render-pass-owned transition authority.

#### Acceptance Gate

- Both routes compile and execute through one production graph; factor-one
  remains a pre-execution outcome and migrated edges have one owner.

### Stage 2: Prove production parity

- [x] Pass focused compute/fragment image and route qualification.
- [x] Preserve failure, fallback, draw/dispatch, telemetry, timing, and retained
  target behavior.
- [x] Validate Vulkan transition state and bounded Renderer/RHI integration.

#### Acceptance Gate

- Existing fixed-baseline fixtures pass unchanged and no validation error or
  material CPU/GPU regression is introduced.

### Stage 3: Publish the pilot boundary

- [x] Record exact migrated edges and authoring lessons in the lasting graph
  contract.
- [x] Complete documentation validation, milestone status, and the pilot plan.

#### Acceptance Gate

- Milestone 2 exit evidence is recorded and only milestone 3 becomes ready.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Route/fallback | Contact route decision and production compute/fragment fixtures |
| Output | Existing contact-shadow bounded image/readback qualifications |
| Identity | Existing telemetry draw/dispatch/timing assertions |
| State | Vulkan validation and exact final GraphicsShaderRead inputs/output |
| Recovery | Existing shader/target failure and factor-one behavior |

## Definition of Done

- Stages 1-3 pass without a runtime fixed/graph switch.
- The roadmap marks milestone 2 complete and links this plan.
- The stage is committed with exact plan provenance.

## Deferred Follow-ups

- Logical target descriptions, graph-owned pool acquisition, culling, and
  complete frame migration belong to milestone 3.

## Related Documentation

- [Render Graph Architecture Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation](../../../Runtime/Rendering/RendererFramePreparation.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
