# Renderer Frame Preparation and Render Graph Execution

Summary: Define immutable per-view preparation, resolved geometry, transient
target ownership, typed pass outcomes, render-graph scheduling, telemetry, and
output transactions.

Modules: Renderer, RenderCore, RHI

Last reviewed: 2026-09-03

## Ownership Boundary

One render command prepares one `FSceneRenderPlan`. Only the private scene-render
execution pipeline may inspect this outer value. It owns command-local logical
partitions for the fitted view, optional environment, selected lighting,
receiver geometry, optional directional
shadow, and optional volumetric cloud. The temporal transaction, resolved
execution resources, final feature decisions, pass results, and telemetry
occupy the `Transaction`, `Resolved`, `Features`, and `Observation` partitions
of the same stack-owned `FSceneFrameContext`, not fields of the logical plan.

Logical geometry is immutable after publication. StaticMesh and SplineMesh
logical draws contain visibility, LOD, material and
pipeline identity, geometry facts, sort keys, and shadow membership. Separate
resolved values own fallible shader, pipeline, material binding, geometry,
palette, upload, and directional-shadow resources. Resolution and
execution consume logical values as `const`; they do not write readiness,
execution phases, target pointers, bindings, or counters back into a logical
draw.

After each family's final sort, logical draws receive contiguous
`ResolvedIndex` values shared by receiver, GBuffer, retained-forward, and
shadow execution. Resolved StaticMesh/SplineMesh views store one index-aligned
record per draw; each record co-locates the optional material binding and
readiness bit. Submission-local geometry performs no pointer-keyed draw,
primitive, material-binding, or batch-readiness lookup.

Preparation/resource/execution measurements live in family-specific
observation values rather than the resolved correctness record surface. Common
conservation helpers finalize those observations, and the scene telemetry
reducer reads them once after successful Scene Color execution. Rendering
policy and draw readiness inspect only prepared identities and resolved records.

Qualification-only route selection is not part of public
`FSceneViewRenderOptions` or the logical plan. Tests and development tools may
install one Renderer-private `FScopedRendererQualificationPolicy` on the
render thread; the frame pipeline snapshots it once when the submission starts.
That bounded value can request isolated GBuffer/deferred/GTAO work or force the
contact/cloud fragment comparison routes. It cannot change scene preparation,
and the former mutable volumetric-cloud preparation callback no longer exists.
Supported GBuffer, deferred, and GTAO debug modes remain explicit public view
options and execute through the same compiled graph schedule. Timing and
capture sinks observe completed typed pass results only.

The frame pipeline's preparation stage builds the plan from the fitted
`FSceneView` and render-thread `FScene` snapshot. The plan never contains a
reflected or game-thread object and remains bounded by the render command's
existing SceneInfo/proxy lifetime. Optional environment and cloud partitions
are complete values: absence is valid, while a selected but invalid required
environment fails the view.

Preparation returns either one complete plan or a typed failure. The published
plan is then held as `const`. A distinct resolution stage allocates the packed
lighting uniform, resolves receiver and shadow resources, and resolves the cloud
sampler. The pipeline then publishes one immutable `FSceneFrameFeaturePlan`.
Purpose flags explain production, debug, qualification, and dependency demand;
feature-specific decisions carry the exact Contact Visibility, cloud-shadow,
and cloud route plus reason. Route preparation resolves persistent payloads and
selects against expected graph target capabilities without invoking recording
with empty targets. Scene
Color, depth, GBuffer, and output receive graph identities before
physical target creation; compiled execution allocates their exact retained
descriptions through the Renderer RDG allocator. There is no resolved frame-target
container or scene-name publication step. Directional-shadow command recording follows
both resolution boundaries as an explicit graph pass; it is never performed by
logical preparation.

## Resource Lifetime Classes

| Class | Owner | Examples | Frame rule |
| --- | --- | --- | --- |
| External | Caller, asset, or shared resource owner plus graph execution | Window/offscreen output, material and environment textures, default textures | `RegisterExternalTexture` retains a counted RHI reference by physical identity and declares exact boundary access. |
| Persistent | Feature/shared Renderer owner or view state | Shader maps, PSOs, samplers, fullscreen geometry, material/geometry caches, cloud history | Generation invalidation and ordered owner shutdown remain authoritative. Committed history is never placed in the RDG allocator cache. |
| Frame-transient | `FRendererRDGAllocator` | Scene Color/depth, GBuffer, GTAO, contact/cloud visibility, deferred/debug output, cloud spatial/composite textures | After compile and culling, exact retained descriptions allocate as one batch. Culled resources never allocate, and pass execution performs no target creation. |

The Renderer RDG allocator keys retained entries by the complete allocation-
compatible description: dimension, flags, format, extent, depth, array size,
mip/sample counts, and clear binding/value for textures, or size, stride, and
usage for buffers. Debug names, graph IDs, pass names, and feature routes are
excluded. Equal descriptions reserve distinct entries within one execution;
inactive compatible entries may be reused by a later execution. The 640 MiB
named graph-wide structural policy rejects an oversized active batch and evicts
the oldest inactive entries when retained storage exceeds the ceiling. Active
allocation IDs are tracked directly, retained bytes are updated incrementally,
and stable allocator sequence IDs preserve deterministic reuse and eviction. A
successful allocation publishes a nonzero ID across graph executions;
external resources remain outside that identity and publish ID zero. Allocation
publishes only after the entire batch succeeds, and the single-use builder keeps
every returned RHI reference alive through recording.

Feature release clears feature-local views and persistent payloads; the allocator
owner performs deterministic transient release before the shared coordinator
is released. Device invalidation reconstructs demanded textures under the new
generation. The allocator does not alias physical memory, infer scheduling, or
introduce synchronization.

## Render Graph Frame Schedule

`FSceneRenderer::RenderView_RenderThread` creates one temporary
`FSceneRenderPipeline`. The pipeline owns preparation, the stack-local
five-part `FSceneFrameContext`, execution/capture, and final transaction
publication. `FSceneRenderGraphComposer` constructs the sole production graph
in stable order through renderer-private, feature-owned `AddPasses` entries:

1. Validate output extent and persistent startup resources.
2. Fit the view, reject an interleaved view-state submission, and begin the
   temporal transaction.
3. Prepare environment, visibility, lighting, receiver/shadow logical draws,
   shared poses, combined translucency, and optional cloud inputs; publish the
   immutable plan, then resolve geometry, palette, shadow, lighting, and cloud
   resources into `FResolvedSceneResources`. Derive dependency closure and the
   final feature plan once.
4. Compile explicit top-level dependencies and output roots, cull unreachable
   versions, then allocate retained logical descriptions as one complete-or-null
   strong-reference table before any command records.
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

Each producer creates its own graph-owned typed completion value and returns
its `TRDGValueHandle<TResult>` in a feature-specific output. The
composer passes that output directly to the exact downstream input; there is
no frame-wide execution-channel lookup or mutable channel bag. Payload
storage, one writer, declared readers, dependency lifetime, and callback
access are owned by RenderCore. Distinct non-RHI results cannot be
interchanged, and callbacks do not communicate through mutable side payloads. Each
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

Scene Color and post-process callbacks copy only their final transactional
publication into `FSceneRenderGraphComposition`; intermediate payloads never
leave graph storage. Editor assistance reads the post-process value and
publishes its adjusted final result without becoming a second writer. The
pipeline commits view state and telemetry only from these final publications
after successful graph execution. Compilation failure maps to the existing
compile-failure status; preparation failure maps to execution failure. The
pipeline reads compiler diagnostics and observational budgets from the builder
after Execute, publishes a capture only when requested and compilation succeeded,
and leaves temporal/output transactions uncommitted on failure. A later frame
authors a new builder; it never retries a consumed graph.

Required output, Scene targets, required environment, or required production
resources fail the view. Optional compute routes may fall back to fragment;
missing optional cloud inputs disable that feature; unavailable optional debug
targets do not publish a partially valid output. Directional-shadow and
environment bindings use their documented complete fallback resources. The
environment irradiance, prefiltered, and BRDF selection is completed by the
composer before registration: either the complete candidate set and sampler or
the complete cube/cube/black fallback set is selected. Only those selected
physical textures receive the stable `Scene.Environment.*` graph identities;
the deferred callback resolves those handles and cannot query or switch to the
unselected set.

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

Each stable pass identity, parameter metadata definition, graph setup,
callback, and private command-recording function is owned by its rendering
unit. The composer wires returned typed outputs into narrow downstream inputs
and passes only the exact renderer services, immutable decisions, and upstream
capabilities required by that feature. Features add parameterized passes only
to the caller-owned builder and never compile or execute a graph; neither they
nor their callbacks can discover the complete frame context or execution
pipeline. Routes and
fallbacks are selected before immutable pass parameters are published; callbacks
validate the selected capabilities and fail the feature if they no longer match,
rather than switching routes. Physical textures are resolved only from declared
pass fields. Parameter lowering and callback authority follow
[Render Graph](RenderGraph.md#typed-pass-parameters).

Feature memory telemetry uses the request's numeric observation tag only for
attribution; changing the tag cannot change compatibility, allocation success,
transitions, culling, or command recording.

The graph preserves imported initial/final access, persistent view-state
history, optional fallback policy, and the temporal/output transaction while
compiling equivalent RHI transition batches. It does not introduce a second
scene-preparation model, mutable blackboard, public pass registry, or alternate
feature interface. Physical aliasing, asynchronous compute, multiple queues,
pass merging, and PSO centralization remain separate measured decisions.

## Scene Budgets and Capture

`FSceneRenderGraphComposer` sets observational regression ceilings of 12 declared
passes, 28 dependencies, and 32 physical texture transitions. Structural limits
are 256 passes and 4096 dependencies, buffer transitions, and texture
transitions. No cross-pass buffers are currently declared, so no measured buffer
regression ceiling is selected. Debug CPU ceilings are 5 milliseconds to compile
and 250 milliseconds to record callbacks. Budget interpretation is defined by
[Render Graph diagnostics](RenderGraph.md#diagnostics-and-budgets).

`SetSceneRenderGraphCaptureSink` is the development/test observer. A `RenderView`
submission may also request an explicit owning capture for its exact viewport.
No capture is constructed without a consumer; with both consumers, one owning
snapshot is published to both. Neither can mutate resources, scheduling,
callbacks, or commit state. Viewport publication is defined by
[Viewport Rendering](ViewportRendering.md#viewport-rendering-statistics).

Historical migration and extension decisions remain in the
[Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md).

## Related Documentation

- [Render Graph](RenderGraph.md)
- [Viewport Rendering](ViewportRendering.md)
- [Renderer Scene Representation](SceneRepresentation.md)
- [Persistent View State](PersistentViewState.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)
- [RHI Resource Transitions](RHIResourceTransitions.md)
- [Minimal GBuffer Contract](GBuffer.md)
- [Volumetric Cloud Spatial Rendering](VolumetricCloudSpatialRendering.md)
- [Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
