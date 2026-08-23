# Renderer Frame Preparation and Fixed Execution

Summary: Define immutable per-view preparation, resolved geometry, transient
target ownership, typed pass outcomes, fixed scheduling, telemetry, and output
transactions.

Modules: Renderer, RenderCore, RHI

Last reviewed: 2026-08-24

## Ownership Boundary

One render command prepares one `FSceneRenderPlan`. Only fixed Renderer
orchestration may inspect this outer value. It owns command-local partitions
for the fitted view and temporal metadata, optional environment, lighting,
receiver geometry and its shared Skeletal palette table, optional directional
shadow, optional volumetric cloud, resolved execution resources, pass results,
and telemetry.

Logical geometry is immutable after publication. StaticMesh, SplineMesh,
SkeletalMesh, and Terrain logical draws contain visibility, LOD, material and
pipeline identity, geometry facts, sort keys, and shadow membership. Separate
resolved values own fallible shader, pipeline, material binding, geometry,
palette, upload, terrain, and directional-shadow resources. Resolution and
execution consume logical values as `const`; they do not write readiness,
execution phases, target pointers, bindings, or counters back into a logical
draw.

`FSceneFramePreparation` builds the plan from the fitted `FSceneView` and
render-thread `FScene` snapshot. The plan never contains a reflected or
game-thread object and remains bounded by the render command's existing
SceneInfo/proxy lifetime. Optional environment and cloud partitions are
complete values: absence is valid, while a selected but invalid required
environment fails the view.

## Resource Lifetime Classes

| Class | Owner | Examples | Frame rule |
| --- | --- | --- | --- |
| Imported | Caller, asset, or shared resource owner | Window/offscreen output, material and environment textures, default textures | Pass inputs retain RHI references and declare required access; the fixed executor does not release them. |
| Persistent | Feature/shared Renderer owner or view state | Shader maps, PSOs, samplers, fullscreen geometry, material/geometry caches, cloud history | Generation invalidation and ordered owner shutdown remain authoritative. Committed history is never placed in the transient pool. |
| Frame-transient | `FRendererTransientTargetPool` | Scene Color/depth, GBuffer, GTAO, contact/cloud visibility, deferred/debug output, cloud spatial/composite textures | Frame setup acquires a complete typed lease. Pass execution receives resolved targets and performs no target lookup or creation. |

The transient pool keys every texture by semantic group and complete creation
description: debug identity, dimension, flags, format, extent, depth, array
size, mip/sample counts, and clear binding/value. A bundle is visible only
when every requested texture resolves. Entries use device-generation-aware
creation slots, so a failed request is suppressed in the same generation and
is eligible after manual or device invalidation. Per-group retained-byte
budgets evict the oldest inactive description while preserving every active
lease. RHI references held by a lease remain valid if its pool entry is later
evicted.

Feature release clears feature-local views and persistent payloads; the pool
owner performs deterministic transient release before the shared coordinator
is released. Device invalidation reconstructs demanded textures under the new
generation. The provider does not alias physical memory, infer scheduling, or
introduce synchronization.

## Fixed Frame Schedule

`FSceneRenderer::RenderView_RenderThread` delegates to
`FFixedSceneFrameExecutor`. The executor is the sole production scheduler and
keeps this order:

1. Validate output extent and persistent startup resources; acquire Scene
   Color/depth.
2. Fit the view, reject an interleaved view-state submission, and begin the
   temporal transaction.
3. Prepare environment, visibility, lighting, receiver/shadow logical draws,
   shared palettes, and optional cloud inputs; resolve geometry resources.
4. Render directional-shadow cascades, then acquire and execute GBuffer if the
   production or qualification route requires it.
5. Build typed deferred inputs and run GTAO, contact visibility, cloud-shadow
   visibility, and explicit isolated debug/qualification branches.
6. Bootstrap/produce Scene Color, render retained unlit opaque/masked
   geometry, transition depth for sampling, then render, reconstruct, and
   composite clouds.
7. Restore Scene Color attachment access and render combined translucent
   geometry in the prepared stable order.
8. `FSceneFrameFinalization` selects debug or Scene Color output, performs post
   process, optional editor assistance, restores the output viewport/scissor,
   and finishes in `Present` for window output or `ShaderReadOnly` offscreen.
9. On complete success, commit temporal state and publish telemetry. Every
   early return aborts pending view state and publishes no partial statistics.

Qualification routes are optional branches over the same prepared plan,
transient targets, and typed results. They do not build a second frame model or
execute an alternate production scheduler.

## Typed Results and Failure Policy

Geometry resolution and execution return family-specific resolved/result
values. `FGBufferPassResult` establishes completeness and whether any geometry
was rendered directly from execution outcomes; no correctness branch derives
either fact from counters. Deferred, GTAO, contact-shadow, cloud-shadow, cloud,
post-process, and geometry boundaries expose their exact RHI inputs, output
resources, status, route/fallback, and execution measurements.

Required output, Scene targets, required environment, or required production
resources fail the view. Optional compute routes may fall back to fragment;
missing optional cloud inputs disable that feature; unavailable optional debug
targets do not publish a partially valid output. Directional-shadow and
environment bindings use their documented complete fallback resources.

Manual texture and buffer transitions remain the only resource-state
authority. Typed pass boundaries use backend-neutral RHI resources and preserve
the established render-pass load/store and initial/final access contracts.

## Telemetry and Observation

Preparation, resolution, execution, memory, and timing telemetry have
feature-owned sources. `SceneRenderTelemetry` performs one saturated reduction
into the stable public `FSceneViewStatistics` contract after command recording.
Rendering policy and pass success never read telemetry. Profiling owns timing
sink registration and scoped GPU queries; capture and qualification observers
receive immutable named pass resources/results and cannot mutate the frame
plan.

## Render Graph Handoff Inventory

The bounded Render Graph follow-up may wrap, in order, directional shadow,
GBuffer, GTAO, contact visibility, cloud-shadow visibility, isolated deferred
and debug, Scene Color/deferred production, cloud spatial/reconstruction/
composite, translucency, post process, editor assistance, and output
finalization. Its first consumers are the typed preparation partitions,
resolved geometry values, transient leases, and pass results described above.

The graph must preserve imported initial/final access, persistent view-state
history, optional fallback policy, the temporal/output transaction, and the
manual transition semantics before it may synthesize equivalent barriers. It
does not need a new scene-preparation model, mutable blackboard, public pass
registry, or alternate feature interface. Physical aliasing, asynchronous
compute, multiple queues, pass merging/culling, and PSO centralization remain
separate measured decisions.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [Renderer Scene Representation](SceneRepresentation.md)
- [Persistent View State](PersistentViewState.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [Minimal GBuffer Contract](GBuffer.md)
- [Volumetric Cloud Spatial Rendering](VolumetricCloudSpatialRendering.md)
