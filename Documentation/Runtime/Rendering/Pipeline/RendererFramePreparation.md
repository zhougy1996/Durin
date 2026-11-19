# Renderer Frame Preparation and Render Graph Execution

Summary: Define submission ownership, CPU preparation, resource resolution, graph execution and transactional output.
Modules: Engine, Renderer, RenderCore, RHI
Last reviewed: 2026-11-19

## Ownership Boundary

`FSceneRenderingService` owns persistent renderers, feature resources, view state
and the allocator. `FScene` owns its mesh draw command storage and retained mesh
preparation data through `GetCachedMeshDrawCommands`.
Each render command creates one `FSceneRenderPipeline` and `FSceneRenderer` with a
submission-local `FSceneFrameContext`. The context separates logical plan, resolved
resources, feature policy, temporal transaction and observations. Only the private
renderer/pipeline inspect the whole context; features receive narrow inputs.

`FSceneRenderPlan` owns the fitted view and immutable environment, lighting,
receiver/shadow geometry and cloud inputs. It contains no game-thread objects,
execution resources or telemetry. Material time is resolved once in the fitted
view. Preparation returns a complete plan or typed failure; resource resolution
does not mutate published logical draws. Final component admission filters lists
before graph authoring, preserving stable resolved indices.

[Load preparation](LoadPreparation.md) pumps the same resource phases with a fresh
snapshot and stops before temporal history or graph submission. Pending resources
remain retryable; terminal errors and capacity exhaustion fail explicitly.

Geometry preparation has three boundaries:

1. Render-thread collection finishes scene/proxy reads and captures LOD, bounds,
   input compatibility, material/deformation snapshots and source identity.
2. Shared-cache resolution publishes immutable `FStaticMeshPreparationInputs`.
   Workers consume these owned facts without scene, proxy, command-list or cache access.
3. Pure chunks merge in original primitive order, then perform one final sort,
   dense indexing and material grouping. Mixed inputs, missing/overlapping ranges
   and cancellation publish no partial view. All launched work is drained.

LOD threshold equality selects higher detail; invalid inputs request LOD zero.
Availability searches lower detail before higher detail. Collection/resource
readiness still validates the selected geometry. Published `FMeshGeometryRecord`
values retain checked ranges and bindings; release/replacement publishes new record
IDs while existing consumers retain their resources. Dynamic providers retain
collection-time validation. Spline deformation changes invalidate their binding
publication; material-only changes preserve geometry.

The scene's preparation cache retains static facts, prepared sections and draw
descriptions on the render thread. Successful lookups refresh a render-frame
stamp. Receiver and shadow submissions in the same frame share that clock.
Entries outside a view stay eligible for revisits. Cache admission enforces
8,192 entries and 8 MiB of charged CPU storage per scene across all three
tables. Section charges include the facts and descriptions they hold, so shared
values can be charged multiple times. Scene assets, backend allocations and
allocator overhead have separate accounting. An oversize entry or a full active
working set uses the prepared result directly. Pressure can evict rows idle for
two frames, once per scene per frame. An age sweep every 120 frames removes
rows idle for at least 120 frames.

Material rebinding, structural material publication, instance-list changes
and primitive removal clear the affected scene's preparation cache. Geometry
replacement publishes a fresh record and changed publication IDs select fresh
entries. Shader, device and manual resource invalidation clear the registered
preparation caches. Scene teardown releases the cache with its draw storage.
Borrowed values keep their existing reference-counted frame lifetime through
all these transitions.

Preparation uses independent CPU tasks at 256 primitives, with 128-primitive chunks
and at most eight active tasks per view. Missing scheduler/small views run inline;
`DURIN_MESH_PREPARATION=inline|tasks` is a diagnostic override. Start receiver and
cascade work before joining it; cancel/drain abandoned work. These policy constants
are not performance acceptance evidence.

### Required recording and invalidation contracts

- Collected view facts and resolved command handles are frame-local. Scene mutation or
  source release follows joined preparation/recording tasks; snapshots do not
  authorize later historical submissions.
- Resolved views retain fallible material, geometry and pipeline resources. Draws
  borrow retained commands/bindings. Recording performs no shader/reflection work,
  pipeline-cache lookup, material traversal or deformation allocation inside a pass.
- Persistent material, Primitive and Collection uniforms update in command order.
  View/pass values have frame owners. Same-layout updates preserve command identity;
  layout/shader/buffer replacement invalidates affected source slots.
- Primitive removal/rebinding invalidates its own cached locations. Old metadata
  becomes invalid before slot reuse; retained command memory cannot revive a source.
  Structural material publication invalidates scenes using that proxy. Texture
  target changes use the source uniform/resource table and preserve commands.
- Shader bindings, vertex/index state and bias are cached within an uninterrupted
  recording span. Pipeline changes, pass boundaries and foreign state changes reset
  the relevant cache. Source arguments and binding arrays stay immutable.
- Recorded RHI payloads retain resources through replay; backend retirement retains
  native allocations through GPU completion independently of scene cache lifetime.

GPUScene owns supported Primitive/Instance attributes; dirty uploads precede mesh
binding resolution. Every drawing graph declares its exact reads and registers
terminal completion for retirement. Pass-owned visible lists, CPU/GPU compaction,
source lookup and optional statistics follow [GPUScene](../Scene/GPUScene.md#cpu-dynamic-instancing).
That contract is the authority for mesh submission; this document does not duplicate
its command-storage or culling tables. Unsupported factories use the explicit
primitive-uniform fallback.

GBuffer and opaque/masked forward recording partition the finalized visible list
through `FParallelMeshDrawCommandPass::DispatchDraw`. Each worker records a
subcommand list inside the same parent render pass, with independent state and
statistics; joining and ordered insertion preserve scene-data lifetime and the
single clear/load/store boundary. Small ranges and sorted translucency remain
serial. The list ownership, prerequisite and comparison-policy contract lives in
[RHI command execution](../RHI/RHICommandExecution.md#command-list-roles).

Shadow recording consumes const prepared/resolved values and returns local
observations on regular command lists. Each cascade owns its complete render pass
and exact array layer; ordered list assembly precedes telemetry/depth capture.
Workers neither access renderer caches nor publish global telemetry. The graph
joins tasks before submission records expire. Automatic independent recording starts
at 256 total cascade draws with multiple cascades and a scheduler;
`DURIN_SHADOW_RECORDING=immediate|serial|parallel` selects comparison paths. A shadow
GPU timing observer uses the immediate path to span all cascades.

Factory/layout identity determines supported shaders and pipeline compatibility.
Registration completes before shader inventory freeze. Local/Spline factories opt
into generated material vertex stages; independent factories keep their registered
shader contract. Material bindings and uniform ownership follow
[Material System](../Materials/MaterialSystem.md) and
[source-owned resources](RenderResourceLifecycle.md#source-owned-draw-resources).

## Resource Lifetime Classes

| Class | Owner | Rule |
| --- | --- | --- |
| External | Caller/asset/shared owner and graph | Register counted physical resources with exact entrance/final access. |
| Persistent | Feature, service or view state | Ordered generation invalidation/release; committed history is outside the transient pool. |
| Frame-transient | `FRendererRDGAllocator` | Allocate retained descriptions as one complete batch after compile/culling and before callbacks. |

Allocation compatibility uses full texture/buffer descriptions, excluding debug
names, graph identities, feature routes and observation tags. Equal descriptions
need distinct entries unless compiler-proven opt-in aliasing permits shared backing.
Aliasable and ordinary resources occupy separate compatibility buckets.

The allocator reserves reusable entries, evicts oldest unreserved entries and
publishes only a complete successful batch. Texture and buffer requests share the
640 MiB retained-pool policy; oversized active batches reject. This does not bound
in-flight deletions, exports, other owners or backend overhead. Allocation IDs are
nonzero/stable across reuse; external resources have ID zero. The builder retains
returned references through recording. Reuse/eviction never waits for GPU idle.

Compiler-selected alias handoffs and backend alias support remain authoritative;
the allocator does not infer synchronization. Aliased backing is counted once.
Failed alias/allocation creation publishes no resources or callbacks. Transient
release precedes coordinator shutdown; generation invalidation reconstructs demanded
resources. See [resource recovery](RendererResourceRecovery.md).

## Render Graph Frame Schedule

The pipeline owns graph execution/capture and final commit/abort. The renderer's
`Render(FRDGBuilder&)` authors exactly once using `Add<Feature>Passes(Graph, Inputs)`;
features never compile or execute another graph. `GetCompletion()` exposes the final
same-builder result and `GetOutputTexture()` permits appended consumers. Submission
records and services outlive synchronous Execute and publication.

1. Validate output, fit the view, snapshot qualification policy and prepare logical inputs.
2. Resolve resources and admit each receiver/cascade component independently. An
   unready section withholds that component, without discarding unrelated components.
3. Publish immutable feature routes; begin temporal candidates only after readiness.
4. Author shadow/GBuffer, optional visibility and lighting dependencies, opaque/base
   color, cloud reconstruction/composition, stable sorted translucency, postprocess
   and optional editor assistance.
5. Compile/cull, allocate exact retained resources, and record declared dependencies.
   Forward clears color/depth. Hybrid bootstrap loads GBuffer depth; production
   samples depth and loads color; retained geometry and translucency load attachments.
6. Restore viewport/scissor and finish imported output in Present (window) or shader
   read access (offscreen). Inspect completion only after successful Execute and
   publish statistics only on complete view success.

Qualification/debug work uses the same prepared model and graph. The private scoped
qualification policy is captured once at entry and passed explicitly; resource
pools and callbacks do not reread changing global policy. Preparation-only calls
never open temporal candidates. Capture owners wait for readiness first; recording
and readback have no preparation-time budget. Request ownership and readiness are defined by
[Load Preparation](LoadPreparation.md#scene-resource-readiness).

## Typed Results and Failure Policy

Each producer returns typed graph-owned completion handles. One writer, declared
readers and dependency lifetime belong to RenderCore. Values carry logical outcomes,
not physical texture/buffer bindings. Callbacks resolve only declared resource groups
and cannot discover the whole frame context, query undeclared fallback resources or
switch the preselected feature route. See [typed parameters](RenderGraph.md#typed-pass-parameters).

Unrequested optional features declare no pass, value or target. Consumers read only
present handles and interpret absence as NotRequested. Requested features preserve
failure reporting; a disabled requested cloud route retains its diagnostic producer.
GBuffer's four colors cross boundaries as one set; optional reconstruction pairs
must be complete. Persistent physical aliases resolve through their declared member.

| Boundary | Failure policy |
| --- | --- |
| Preparation/readiness | Optional AO/cloud/shadow resources choose documented fallback/disabled routes before authoring; GPU culling can use direct draws. Required output/environment/base-lighting failures reject the view. |
| Authored pass | Recording failure propagates; readiness fallback does not hide execution errors. GBuffer geometry presence comes from execution outcome, never statistics. |
| Hybrid chain | Bootstrap → production → retained geometry → Scene Color preserve predecessor failure before raster work. |
| Final output | Postprocess preserves exact upstream error; editor assistance reads the same completion. No partial statistics publication. |
| Graph failure | Do not read CPU result values. Retry uses a fresh builder; a consumed graph is never retried. |
| Feature history | Successful feature history publication is independent of later final-output failure; abort only unfinished candidates. |

Environment selection completes before registration: either a complete candidate
set/sampler or the complete cube/cube/black fallback receives the declared identities.
Explicit environment overrides retain required-texture failure semantics. Sky
selection is frozen before recording. Cloud density/weather owners and normalized
fallbacks become declared graph inputs, without side-channel physical bindings.

RDG owns inter-pass declarations and transition planning; RHI/backends validate and
commit physical execution state. Features preserve attachment load/store and
imported boundary access without competing manual transitions.

## Telemetry and Observation

Feature-owned observations reduce once into `FSceneViewStatistics` after successful
view completion. Policy, readiness and pass success never depend on telemetry.
Native draw counts, optional per-draw statistics and asynchronous GPU visibility
follow the [GPUScene statistics contract](../Scene/GPUScene.md#cpu-dynamic-instancing).
Capacity, admitted upload traffic and backend residency are distinct measurements.
Timing/capture observers receive immutable results; absent observers allocate no
capture and read no observer clock. Capture/timing effects are explicit graph roots.

## Render Graph Integration Boundary

Rendering units own their pass identity, metadata, authoring, callbacks and recording.
The scene renderer wires narrow typed outputs; features receive only the services,
immutable decisions and upstream capabilities they need. Routes are selected before
parameter publication. Managed raster merging, transient aliasing and async compute
follow graph/backend contracts; aliasing and async compute remain explicit opt-ins.

## Scene Budgets and Capture

Structural budgets reject invalid/oversized graphs; observational ceilings report
regressions without changing rendering policy. Exact route adjustments live in
`FSceneRenderer::PrepareGraphResources`; they must reflect declared buffers as well
as textures. Interpretation follows [graph diagnostics](RenderGraph.md#diagnostics-and-budgets).

`SetSceneRenderGraphCaptureSink` and explicit viewport capture requests share one
owning snapshot when both observe an execution. No consumer means no capture.
Publish only after compilation succeeds; observers cannot mutate scheduling,
resources or commit state. See [viewport statistics](ViewportRendering.md#viewport-rendering-statistics).

## Related Documentation

- [Render Graph](RenderGraph.md)
- [Scene representation](../Scene/SceneRepresentation.md) and [GPUScene](../Scene/GPUScene.md)
- [Persistent view state](../Scene/PersistentViewState.md)
- [Viewport rendering](ViewportRendering.md)
- [Resource lifecycle](RenderResourceLifecycle.md) and [recovery](RendererResourceRecovery.md)
- [RHI transitions](../RHI/RHIResourceTransitions.md)
- [GBuffer](GBuffer.md) and [cloud spatial rendering](../Features/VolumetricCloudSpatialRendering.md)
