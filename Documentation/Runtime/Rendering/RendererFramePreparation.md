# Renderer Frame Preparation and Render Graph Execution

Summary: Define immutable per-view preparation, resolved geometry, transient
target ownership, typed pass outcomes, render-graph scheduling, telemetry, and
output transactions.

Modules: Renderer, RenderCore, RHI

Last reviewed: 2026-08-24

## Ownership Boundary

One render command prepares one `FSceneRenderPlan`. Only the private scene-frame
execution pipeline may inspect this outer value. It owns command-local logical
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
render thread; the graph executor snapshots it once when the submission starts.
That bounded value can request isolated GBuffer/deferred/GTAO work or force the
contact/cloud fragment comparison routes. It cannot change scene preparation,
and the former mutable volumetric-cloud preparation callback no longer exists.
Supported GBuffer, deferred, and GTAO debug modes remain explicit public view
options and execute through the same compiled graph schedule. Timing and
capture sinks observe completed typed pass results only.

The graph executor's preparation stage builds the plan from the fitted
`FSceneView` and render-thread `FScene` snapshot. The plan never contains a
reflected or game-thread object and remains bounded by the render command's
existing SceneInfo/proxy lifetime. Optional environment and cloud partitions
are complete values: absence is valid, while a selected but invalid required
environment fails the view.

Preparation returns either one complete plan or a typed failure. The published
plan is then held as `const`. A distinct resolution stage allocates the packed
lighting uniform, resolves receiver and shadow resources, uploads shared
Skeletal palettes into a resolved palette table, and resolves the cloud
sampler. The pipeline then derives one immutable `FSceneFrameTopology` value.
Mutually exclusive Contact Visibility, cloud-shadow, and cloud routes use
`Disabled`/`Fragment`/`Compute` states rather than independent booleans. Scene
Color, depth, GBuffer, and output receive graph identities before
physical target creation; the compiled retained request resolves their backing
into `FResolvedSceneFrame::Targets` as one publication. Directional-shadow command recording follows
both resolution boundaries as an explicit graph pass; it is never performed by
logical preparation.

## Resource Lifetime Classes

| Class | Owner | Examples | Frame rule |
| --- | --- | --- | --- |
| Imported | Caller, asset, or shared resource owner | Window/offscreen output, material and environment textures, default textures | Pass inputs retain RHI references and declare required access; the graph executor does not release them. |
| Persistent | Feature/shared Renderer owner or view state | Shader maps, PSOs, samplers, fullscreen geometry, material/geometry caches, cloud history | Generation invalidation and ordered owner shutdown remain authoritative. Committed history is never placed in the transient pool. |
| Frame-transient | `FRendererTransientTargetPool` | Scene Color/depth, GBuffer, GTAO, contact/cloud visibility, deferred/debug output, cloud spatial/composite textures | After compile and culling, retained logical requests derive the exact target families and publish one complete backing table before recording. Culled resources never allocate, and pass execution performs no target creation. |

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

## Render Graph Frame Schedule

`FSceneRenderer::RenderView_RenderThread` delegates to the thin
`FRenderGraphSceneFrameExecutor`. The executor owns the compile/execute/capture
boundary. `FSceneFrameGraphComposer` constructs the sole production graph in
stable order through renderer-private named feature contributors:

1. Validate output extent and persistent startup resources.
2. Fit the view, reject an interleaved view-state submission, and begin the
   temporal transaction.
3. Prepare environment, visibility, lighting, receiver/shadow logical draws,
   shared poses, combined translucency, and optional cloud inputs; publish the
   immutable plan, then resolve geometry, palette, shadow, lighting, and cloud
   resources into `FResolvedSceneFrame`. Derive frame topology once.
4. Compile explicit top-level dependencies and output roots, cull unreachable
   versions, then resolve retained logical resources as one complete-or-null
   backing table before any command records.
5. Render directional-shadow cascades, then execute GBuffer from its resolved
   target bundle if the production or qualification route requires it.
6. Build typed deferred inputs and run GTAO, contact visibility, cloud-shadow
   visibility, and explicit isolated debug/qualification branches.
7. Produce opaque Scene Color, then render cloud spatial work through its
   preselected compute or graphics domain and reconstruct/composite the result.
   Graph declarations own every color/depth handoff between these passes.
8. Render combined translucent geometry in the prepared stable order; its
   managed attachment declarations publish the final color/depth access.
9. The frame finalization stage selects debug or Scene Color output,
   performs post process, optional editor assistance, restores the output
   viewport/scissor, and finishes in `Present` for window output or
   `ShaderReadOnly` offscreen.
10. On complete success, commit temporal state and publish telemetry. Every
   early return aborts pending view state and publishes no partial statistics.

Qualification routes are optional branches over the same prepared plan,
transient targets, and typed results. They do not build a second frame model or
execute an alternate production scheduler.

## Typed Results and Failure Policy

`FSceneFrameGraphExecutionChannels` owns the submission's typed
directional-shadow, GBuffer, GTAO, contact-shadow, cloud-shadow,
isolated-deferred, Scene Color/cloud, and post-process results. Each channel is
a `TSceneFrameGraphValue<TResult>` containing both its graph token and its
execution-lifetime result, so distinct non-RHI results cannot be interchanged
and callbacks do not communicate through unrelated external locals. Each
fallible producer publishes `NotRequested`,
`Complete`, or `Failed`. Graph-owned directional shadow, GBuffer, GTAO,
contact/cloud visibility, isolated-deferred, Scene Color/cloud, debug, and
post-process results carry status and logical selection only; consumers resolve
physical textures from their declared handles.

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

The render graph is the inter-pass declaration and transition-planning
authority. Typed pass boundaries use backend-neutral RHI resources and preserve
the established render-pass load/store and imported initial/final access
contracts. RHI and the active backend remain authoritative for validating and
committing physical execution state; migrated edges contain no competing
feature-local manual transition.

## Telemetry and Observation

Preparation, resolution, execution, memory, and timing telemetry have
feature-owned sources. `SceneRenderTelemetry` performs one saturated reduction
into the stable public `FSceneViewStatistics` contract after command recording.
Rendering policy and pass success never read telemetry. Profiling owns timing
sink registration and scoped GPU queries; capture and qualification observers
receive immutable named pass resources/results and cannot mutate the frame
plan.

## Render Graph Integration Boundary

The production graph declares, in order, directional shadow, GBuffer, GTAO,
contact visibility, cloud-shadow visibility, deferred lighting, opaque Scene
Color, cloud spatial/reconstruction/composite, translucency, post process,
editor assistance, and output finalization. Its inputs are typed preparation
partitions, resolved geometry values, imported/persistent graph handles,
retained logical target descriptions, and non-RHI pass results described above.

Each stable pass identity is owned by one named contributor type in
`SceneFrameGraphContributors.h`; contributors add passes only to the
caller-owned builder and never compile or execute a graph. The composer wires
their handles, declared resources, and typed channels. It converts the complete
prepared plan into feature-specific shadow, geometry, visibility, cloud, or
view inputs; neither contributors nor their callbacks can discover the whole
plan or the execution pipeline. `FSceneFrameFeatureRecorders` owns command
recording through the same narrow contracts. Retained allocation
policy is a separate `FSceneFrameGraphBackingProvider`: graph descriptions use
the typed `ESceneFrameBackingClass` boundary, retained requests are converted
into a request-bounded topology, and logical-to-physical publication succeeds
only as one complete candidate table. Human-readable backing names remain
diagnostics rather than allocation policy.

The graph preserves imported initial/final access, persistent view-state
history, optional fallback policy, and the temporal/output transaction while
compiling equivalent RHI transition batches. It does not introduce a second
scene-preparation model, mutable blackboard, public pass registry, or alternate
feature interface. Physical aliasing, asynchronous compute, multiple queues,
pass merging, and PSO centralization remain separate measured decisions.

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
