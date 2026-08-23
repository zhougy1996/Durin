# Renderer Frame Preparation and Fixed Execution

Summary: Define immutable per-view preparation, resolved geometry, transient
target ownership, typed pass outcomes, fixed scheduling, telemetry, and output
transactions.

Modules: Renderer, RenderCore, RHI

Last reviewed: 2026-08-24

## Ownership Boundary

One render command prepares one `FSceneRenderPlan`. Only fixed Renderer
orchestration may inspect this outer value. It owns command-local logical
partitions for the fitted view, optional environment, selected lighting,
receiver geometry and its shared Skeletal pose table, optional directional
shadow, and optional volumetric cloud. The temporal transaction, resolved
execution resources, pass results, and telemetry are sibling executor state,
not fields of the logical plan.

Logical geometry is immutable after publication. StaticMesh, SplineMesh,
SkeletalMesh, and Terrain logical draws contain visibility, LOD, material and
pipeline identity, geometry facts, sort keys, and shadow membership. Separate
resolved values own fallible shader, pipeline, material binding, geometry,
palette, upload, terrain, and directional-shadow resources. Resolution and
execution consume logical values as `const`; they do not write readiness,
execution phases, target pointers, bindings, or counters back into a logical
draw.

After each family's final sort, logical draws receive contiguous
`ResolvedIndex` values shared by receiver, GBuffer, retained-forward, and
shadow execution. Resolved StaticMesh/SplineMesh, SkeletalMesh, and Terrain
views store one index-aligned record per draw; each record co-locates the
optional material binding and readiness bit. Skeletal palette ranges align
with prepared primitive indices, while Terrain batches have their own bounded
contiguous indices. Submission-local geometry performs no pointer-keyed draw,
primitive, material-binding, palette-range, or batch-readiness lookup.

Preparation/resource/execution measurements live in family-specific
observation values rather than the resolved correctness record surface. Common
conservation helpers finalize those observations, and the scene telemetry
reducer reads them once after successful Scene Color execution. Rendering
policy and draw readiness inspect only prepared identities and resolved records.

Qualification-only route selection is not part of public
`FSceneViewRenderOptions` or the logical plan. Tests and development tools may
install one Renderer-private `FScopedRendererQualificationPolicy` on the
render thread; the fixed executor snapshots it once when the submission starts.
That bounded value can request isolated GBuffer/deferred/GTAO work or force the
contact/cloud fragment comparison routes. It cannot change scene preparation,
and the former mutable volumetric-cloud preparation callback no longer exists.
Supported GBuffer, deferred, and GTAO debug modes remain explicit public view
options and execute through the same fixed schedule. Timing and capture sinks
observe completed typed pass results only.

The fixed executor's preparation stage builds the plan from the fitted
`FSceneView` and render-thread `FScene` snapshot. The plan never contains a
reflected or game-thread object and remains bounded by the render command's
existing SceneInfo/proxy lifetime. Optional environment and cloud partitions
are complete values: absence is valid, while a selected but invalid required
environment fails the view.

Preparation returns either one complete plan or a typed failure. The published
plan is then held as `const`. A distinct resolution stage allocates the packed
lighting uniform, resolves receiver and shadow resources, uploads shared
Skeletal palettes into a resolved palette table, and resolves the cloud
sampler. The executor then derives one immutable `FSceneFrameRequirements`
value and resolves Scene Color/depth plus every requested feature bundle into
`FResolvedSceneFrame::Targets`. Directional-shadow command recording follows
both resolution boundaries as an explicit fixed-schedule step; it is never
performed by logical preparation.

## Resource Lifetime Classes

| Class | Owner | Examples | Frame rule |
| --- | --- | --- | --- |
| Imported | Caller, asset, or shared resource owner | Window/offscreen output, material and environment textures, default textures | Pass inputs retain RHI references and declare required access; the fixed executor does not release them. |
| Persistent | Feature/shared Renderer owner or view state | Shader maps, PSOs, samplers, fullscreen geometry, material/geometry caches, cloud history | Generation invalidation and ordered owner shutdown remain authoritative. Committed history is never placed in the transient pool. |
| Frame-transient | `FRendererTransientTargetPool` | Scene Color/depth, GBuffer, GTAO, contact/cloud visibility, deferred/debug output, cloud spatial/composite textures | Frame setup acquires a complete typed lease. Pass execution receives resolved targets and performs no target lookup or creation. |

The transient pool partitions entries by bounded
`ERendererTransientTargetGroup` identity and keys each group by the complete
creation description: debug identity, dimension, flags, format, extent, depth,
array size, mip/sample counts, and clear binding/value. Lookup is bounded to
one semantic group. A bundle is visible only when every requested texture
resolves. Failed bundle acquisition discards newly created successful siblings,
enforces the group budget, and retains the failed generation-aware creation
slot so an immediate retry is suppressed until manual or device invalidation.
Per-group retained-byte budgets evict the oldest inactive description while
preserving every active lease. RHI references held by a lease remain valid if
its pool entry is later evicted.

Feature release clears feature-local views and persistent payloads; the pool
owner performs deterministic transient release before the shared coordinator
is released. Device invalidation reconstructs demanded textures under the new
generation. The provider does not alias physical memory, infer scheduling, or
introduce synchronization.

## Fixed Frame Schedule

`FSceneRenderer::RenderView_RenderThread` delegates to
`FFixedSceneFrameExecutor`. The executor is the sole production scheduler and
keeps this order:

1. Validate output extent and persistent startup resources.
2. Fit the view, reject an interleaved view-state submission, and begin the
   temporal transaction.
3. Prepare environment, visibility, lighting, receiver/shadow logical draws,
   shared poses, combined translucency, and optional cloud inputs; publish the
   immutable plan, then resolve geometry, palette, shadow, lighting, and cloud
   resources into `FResolvedSceneFrame`. Derive the frame requirements once and
   resolve all requested frame-transient bundles before execution begins.
4. Render directional-shadow cascades, then execute GBuffer from its resolved
   target bundle if the production or qualification route requires it.
5. Build typed deferred inputs and run GTAO, contact visibility, cloud-shadow
   visibility, and explicit isolated debug/qualification branches.
6. Bootstrap/produce Scene Color, render retained unlit opaque/masked
   geometry, transition depth for sampling, then render, reconstruct, and
   composite clouds.
7. Restore Scene Color attachment access and render combined translucent
   geometry in the prepared stable order.
8. The executor's finalization stage selects debug or Scene Color output,
   performs post process, optional editor assistance, restores the output
   viewport/scissor, and finishes in `Present` for window output or
   `ShaderReadOnly` offscreen.
9. On complete success, commit temporal state and publish telemetry. Every
   early return aborts pending view state and publishes no partial statistics.

Qualification routes are optional branches over the same prepared plan,
transient targets, and typed results. They do not build a second frame model or
execute an alternate production scheduler.

## Typed Results and Failure Policy

`FSceneFrameOutcome` owns the submission's typed directional-shadow, GBuffer,
GTAO, contact-shadow, cloud-shadow, isolated-deferred, Scene Color/cloud, and
post-process results. Each fallible producer publishes `NotRequested`,
`Complete`, or `Failed` with the resources required by its consumers. A status
alone is insufficient: result predicates reject `Complete` values whose
required output is absent.

Geometry resolution and execution return family-specific resolved/result
values. `FGBufferPassResult` establishes completeness and whether any geometry
was rendered directly from execution outcomes; no correctness branch derives
either fact from counters. GTAO, contact visibility, and cloud-shadow execution
return independent results and never mutate a shared deferred parameter block.
After all producers finish, one `BuildDeferredParameters` boundary constructs
the complete binding set from their outputs or documented white/array
fallbacks. Isolated diagnostics receive a copy, production receives a separate
copy with production diagnostic policy, and cloud composition receives cloud-
shadow visibility explicitly rather than through executor member state. Scene
Color and post process return explicit output/result values instead of
rewriting caller-owned texture variables.

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

The cross-plan sequencing, required migration milestones, and evidence gates
for those conditional extensions are owned by the
[Render Graph Architecture Roadmap](../../Roadmaps/RenderGraphArchitecture.md).

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [Renderer Scene Representation](SceneRepresentation.md)
- [Persistent View State](PersistentViewState.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [Minimal GBuffer Contract](GBuffer.md)
- [Volumetric Cloud Spatial Rendering](VolumetricCloudSpatialRendering.md)
- [Render Graph Architecture Roadmap](../../Roadmaps/RenderGraphArchitecture.md)
