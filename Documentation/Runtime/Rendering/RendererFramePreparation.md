# Renderer Frame Preparation and Render Graph Execution

Summary: Define immutable per-view preparation, resolved geometry, transient
target ownership, typed pass outcomes, render-graph scheduling, telemetry, and
output transactions.

Modules: Engine, Renderer, RenderCore, RHI

Last reviewed: 2026-09-23

## Ownership Boundary

One render command prepares one `FSceneRenderPlan`. Only the private scene renderer and its preparation
pipeline may inspect this outer value. It owns command-local logical
partitions for the fitted view, optional environment, selected lighting,
receiver geometry, optional directional
shadow, and optional volumetric cloud. The temporal transaction, resolved
execution resources, final feature decisions, pass results, and telemetry
occupy the `Transaction`, `Resolved`, `Features`, and `Observation` partitions
of the same submission-owned `FSceneFrameContext`, not fields of the logical plan.

Logical geometry is immutable after publication. StaticMesh and SplineMesh
providers submit `FMeshBatch` values through `CollectMeshBatches`. Engine owns
asset LOD/residency and section/material interpretation. RenderCore input bindings
retain vertex declarations and streams; prepared draws retain checked draw
ranges and buffer views instead of borrowing asset LOD/section/factory objects.
Provider LOD values remain diagnostic data. Material and deformation snapshots
are immutable for the prepared frame.

`CaptureStaticMeshLODSelection` copies screen-size thresholds and readiness into
`FMeshLODSelectionSnapshot`, without retaining resource or proxy pointers. Resource
and snapshot overloads of LOD selection share the same algorithm: threshold
equality chooses higher detail, invalid size/threshold input requests LOD zero,
and availability searches lower detail before higher detail. A snapshot remains
stable across later readiness changes. Capturing requires stable source resources;
the values alone do not retain geometry or certify later resource readiness.

Providers may opt into `CaptureLODSelection_RenderThread`; static and spline mesh
providers return owned threshold/readiness snapshots, while other providers
default to their original collection-time selection. Renderer captures world
bounds alongside these snapshots, then `PrepareMeshViewFacts` computes projected
size and requested/available LODs without scene or proxy access. At 256 primitives
it uses independent CPU tasks in waves of at most eight 128-primitive chunks;
small views run inline. Ordered facts are passed through `FMeshCollectionContext`
to collection, which still resolves materials, spline bindings and current
geometry readiness on the rendering thread. Capture and collection occur within
one render command without intervening scene mutation. A failed pure task discards
partial facts and retries inline, draining all launched work before returning.

`CollectStaticMeshView_RenderThread` finishes scene/proxy reads before publishing
an immutable `FCollectedStaticMeshView`. It owns primitive IDs and classification,
projection fallback facts, collection outcomes, admitted batch values, and the
view matrices/policy consumed by preparation. It does not retain scene-info or
proxy pointers. `PrepareCollectedStaticMeshView_RenderThread` can replay those
values after source destruction or geometry republication. The ordinary view
entrypoint composes collection and replay. Collection also captures the canonical
input binding, factory/layout compatibility, per-element pass support and dynamic
input-validation outcome. Replay performs no factory registry lookup or virtual
capability calls and does not repeat geometry input validation. Published geometry
keeps its validated-publication path. Preparation counters attribute consumed
collection facts to the resulting view; replay is not another physical validation.
`ResolveCollectedStaticMeshView_RenderThread` resolves shared transform, material
and command caches into an immutable `FStaticMeshPreparationInputs` owner. It
retains the collected view, copied transform facts, selected command templates,
material indices and the material-table extent. Template build/reuse facts remain
per-element attribution, not shared mutable counters. `PrepareStaticMeshInputs`
uses only this owner to form and sort pass lists, assign dense indices/groups,
and accumulate result-local telemetry; it has no command list or cache parameter
and no rendering-thread affinity.
Input owners must outlive every consumer. Dynamic providers retain their existing
binding-resource retirement contract; arbitrary external mutation of published
bindings is not supported by this boundary.

`PrepareStaticMeshInputChunk` consumes a contiguous primitive range and retains
its input owner. It produces unsorted local pass vectors, primitive indices,
material-table indices and local counters. `MergeStaticMeshPreparationChunks`
validates common input identity and exact non-overlapping coverage before moving
any outputs, then concatenates in original primitive order and offsets primitive
indices. Only the final combined view performs global ordering, dense resolved
indices, material grouping and transition accounting. Arrival order cannot change
these results. Missing, duplicate, invalid or mixed-input ranges return explicit
errors without publishing a partial view; an empty view uses one empty chunk.
The inline path shares the same range builder and finalizer.

`StartStaticMeshPreparation` runs small views inline and dispatches views with at
least 256 primitives as independent CPU tasks. Each chunk covers at most 128
primitives; an owning work object retains at most eight active tasks and refills
that window during `FinishStaticMeshPreparation`. Workers capture immutable
inputs and range bounds only. The rendering thread joins through the task
system's explicit independent-CPU contract. Results carry task count and join
duration; chunk counters remain local until deterministic merge. The first failed
or canceled chunk in range order cancels and drains remaining tasks, and no partial
view is returned. Abandoning the owning work object also cancels and drains it.
Pre-canceled inline work returns cancellation without replay. A missing scheduler
uses the inline path. `DURIN_MESH_PREPARATION=inline` or `tasks` overrides the
ordinary view wrapper's size policy for diagnosis. These fixed thresholds are
correctness defaults, not measured performance acceptance.

The production frame starts receiver preparation immediately after visibility
collection and joins it before combined translucent geometry consumes the result.
Environment, light, shadow and cloud preparation can proceed while its initial
task window runs. Shadow preparation starts every cascade before joining them in
cascade order for raster bias and telemetry. Collection and shared-cache resolution
remain rendering-thread operations; subsequent cache growth cannot change an
already frozen input. With three cascades, one frame retains at most 32 active
mesh preparation tasks. Required receiver failure rejects resource resolution;
shadow failure uses the existing all-cascade readiness fallback.

Successful static-mesh resource initialization publishes one immutable
`FMeshGeometryRecord` per LOD, with a process-unique nonzero record ID.
It owns section draw ranges, bounds, material-slot indices, vertex/index buffer
views and a retained local input binding. Receiver and cascade collection share
the publication while independently selecting LOD and resolving current materials.
Collection does not reread asset section geometry or rebuild local declarations
and stream arrays. Prepared primitives retain the publication. Resource release
withdraws the asset's record; replacement or initialization after release publishes
new IDs. Repeated initialization of an unchanged ready LOD preserves its ID.
Previously prepared records retain their old RHI resources until their consumers
release them, under the existing submission retirement contract. There is no
global registry or retained history; storage is bounded by current asset LODs and
live consumers. Published CPU geometry must be replaced through the resource
publication boundary rather than mutated in place.

Publication consumes a unique input binding and validates element identity,
draw/stream ranges, resource usage, instance inputs and declaration compatibility.
Only read-only spans and bindings are exposed. An admitted batch contains either
dynamic elements or a publication plus an aligned material array. Mixing these
forms, substituting the publication's input binding or mismatching material counts
is rejected. Published batches and Renderer preparation read the validated span
directly, without copying or revalidating geometry per view. Primitive transforms,
bounds, LOD selection, duplicate batch identity and pass eligibility are still
checked at collection/preparation. Independent providers can explicitly opt into
the same publication API; view-dependent providers retain dynamic validation.

Spline proxies retain at most one publication per LOD, keyed by geometry record ID
and the accepted deformation state. Accepted deformation updates clear those
bindings; rejected revisions leave them intact. Material updates do not invalidate
geometry. A binding revision shares the original immutable geometry span and its
geometry ID while assigning a new record ID and validating the new inputs once.
These caches are render-thread-only and are not worker-safe mutable state.

Published geometry may also use the renderer-owned mesh command-template cache.
Each immutable template owns its geometry publication, material publication,
logical pipeline identity, geometry views and stable ordering facts. The key uses
geometry record ID, element index, material record ID and the selected raster,
depth, preparation-mode, winding and deformation-domain policy. Material planning
identity and compiled-program ownership are checked on lookup. Dynamic providers
without a geometry publication construct a fresh template for the selected view.
Templates contain no view matrix, view depth, uniform range or transient RDG
resource. Shader/device readiness is resolved separately for every submission.

The cache is shared by receiver and cascade preparation and retains only entries
touched by the latest submission. End-of-preparation cleanup also runs on failure;
device-resource release clears all entries. During preparation, storage is bounded
by the union of the preceding and current submission's templates. Prepared frames
retain their selected templates independently, so eviction cannot invalidate old
frames. Per-view depth and primitive/LOD ordering identities are recomputed while
instantiating a visible draw; camera changes do not invalidate stable templates.

Material time is also submission state: `PrepareMeshViewUniform` uploads the
current view's `MaterialTimeSeconds` independently of cached geometry, material
publications and command templates. Reusing those records must preserve animated
shader inputs. Explicit time values remain deterministic across repeated renders;
an unspecified time is resolved by `RenderView` before preparing the view.

Visible mesh draws contain compact template references, dense indices, world sort
center/depth and optional per-view raster bias; they do not copy template geometry,
material, pipeline or layout arrays. A compile-time bound limits this record to
128 bytes. Visible ordering keys retain an alias to immutable template sort state
and keep primitive/batch/LOD tie-breakers separately. Combined translucent lists
share the same sort state. Comparison preserves pipeline, material, vertex input,
geometry, primitive, batch, LOD and section order, with translucent depth first.
Cascade raster-bias magnitudes remain dynamic draw state and do not enter the
pipeline-cache identity or mutate templates shared with another view.

Recording shares a complete binding group across adjacent compatible draws.
The group identifies the retained pipeline, vertex batch and fragment batch;
changing any member submits the whole group again. A failed bind clears the
group. The state is local to one uninterrupted mesh recording span and starts
empty at every pass boundary or after non-mesh pipeline/parameter work. Forward,
retained-forward, sorted translucency, GBuffer and cascades use this rule without
reordering draws. Geometry inputs and dynamic shadow bias are still submitted
per draw. Hit-proxy draws retain their distinct prepared ID bindings. This does
not imply per-descriptor-set reuse when only one part of a group changes.

`FStaticMeshRenderer::RecordShadow` consumes const prepared/resolved views on a
regular command list and returns local draw observations. It does not inspect
renderer caches or modify the shared resolved view. Geometry and prepared surface
binding replay also accept regular lists. `CaptureCascade_RenderThread` retains
the exact shadow target and attachment view; `RecordCascade` consumes that value
and owns the complete render-pass scope, including graphics pipeline selection.
Directional-shadow RDG passes declare one exact array layer per cascade and
produce typed local observations. A serial consumer reads every cascade value,
merges telemetry and invokes depth capture only after ordered list assembly.
Prepared/resolved view references remain valid until synchronous graph execution
has joined every recording task; command payloads retain native resources for
later replay. Workers do not access renderer state or publish global telemetry.

The automatic shadow policy enables independent recording at 256 total cascade
draws; smaller workloads use owned serial lists. A single cascade or unavailable
scheduler remains inline. `DURIN_SHADOW_RECORDING=immediate|serial|parallel` selects
diagnostic comparison paths; unset/other values use the automatic policy. An
active shadow GPU timing sink always selects the immediate path because its
query spans all cascades. `ShadowWorkerRecordingChunks` counts actual worker
recordings, reduced from local observations on the rendering thread. Each list
clears/stores only its own layer; no intra-pass render-pass splitting is enabled.

Renderer-owned mesh vertex-factory implementations supply compatible vertex
shader types and typed vertex parameter preparation. Shader-type compilation
metadata supplies the same options to authored material maps and cook. Forward,
shadow and GBuffer execution use the registered factory/layout identity; pass
state and material policy remain with each pass. Pipeline identities include
factory/layout, vertex declaration and topology. Registered implementations are
retained for the Renderer module lifetime, and registration is synchronized.
New contributions must be installed during initialization before shader inventory
freeze. Each supported factory/pass contributes a named cooked vertex request.
Opaque-shadow fixed fragments have their own cooked request; material maps
link independently compiled sources with stage/reflection validation intact.
Scene membership, visibility and cascade candidates use the same generic
primitive list. One provider may publish multiple batches with independent pass
participation. Checked 64-bit batch/element IDs remain deterministic sort ties.
Vertex input validation covers declaration compatibility, stream bounds and
instance-rate ranges. Missing inputs fail receiver resolution; shadow collection
resource failures use the shadow transaction's existing failure/retry path.
Optional GBuffer exclusions retain forward routing. Separate
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

Preparation shares validated transform and exact material-representation facts
between the receiver and directional-shadow cascades within one submission.
Providers still collect batches independently for each view and LOD; batch
identity alone never authorizes reuse of a changed transform or material.
Repeated material publications are identified by immutable record IDs; unchanged
IDs bypass content comparison, and content hashes are computed at publication.
Distinct IDs still undergo exact comparison when entering a submission's uniform
groups, preserving deduplication of independently published equal materials.
Prepared bindings and resolved surface uniform views retain the immutable material
publication instead of copying its arrays or revalidating its layout per section.
Sort keys retain that publication too; identical IDs compare immediately and
distinct publications preserve byte-wise uniform ordering without payload copies.
After draw sorting, a separate dense material-group schedule assigns uniform
indices without changing translucent ordering. Resource preparation uploads
one transform per prepared primitive/view and one material payload per group;
recording reads these arrays directly, without lazy uniform-cache searches.

Mesh material descriptor set 0 contains view/pass controls and lighting resources;
set 1 contains primitive transforms, deformation, material parameters/textures,
and hit-proxy IDs. Material time, the view lighting switch, and specular-AA control
are uploaded once per required view/pass, independently of material payloads.
Material shading models remain shader permutations. The reserved material header
is retained for layout stability and is no longer overwritten with view controls.
Forward, GBuffer, masked-shadow, and hit-proxy fragments use the same binding
contract; changing it advances the material pass-contract version so stale cooked
programs cannot be accepted. Uniform storage belongs to the current submission
and is rebuilt for new views, frames, and resource-resolution attempts.

Surface shader instances compile reflection into immutable binding layouts.
Before recording, receiver and cascade resolution builds concrete fragment
bindings once per material uniform group, pass and selected shader; compatible
draws retain the same batch. Hit-proxy preparation also resolves its fragment
bindings before opening the render pass. Missing resources, unsupported layouts
and invalid uniform ranges fail preparation. Recording consumes the retained
batch without reflection traversal or material resource-vector construction.

`FRHIShaderParameterBatch` canonicalizes resource views before recording and owns
the shader and every canonical resource. Recorded commands share its immutable
parameter span, preserving ownership through submission retirement. Reusing a
batch does not recreate views or copy its parameter array. Command payload
accounting conservatively charges the retained array storage for each command
reference. Backend descriptor processing remains separate from this binding
contract.

Vertex factories prepare immutable RHI parameter batches before recording too.
Their preparation operation may upload deformation data, but cannot bind a
pipeline or emit draws. Its inputs are the selected immutable factory binding,
shader and prepared transform range. A submission shares its result across
sections with the same primitive and selected vertex shader; distinct views
prepare independent ranges. Spline and independent dynamic factories upload
their deformation data here. Forward, shadow, GBuffer, retained-forward and hit
proxy recording consume the resulting batches without constructing typed
parameter arrays or allocating deformation uniforms inside a render pass.

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

Static-mesh resource resolution retains immutable pipeline payloads in each
resolved draw record. Forward, GBuffer and shadow records retain their selected payload;
hybrid retained-forward preparation stores a separate variant. Section recording
consumes these references directly without rebuilding a pipeline key or searching
the renderer cache. Cache growth, eviction, or replacement cannot invalidate a
prepared reference. Each new resolution attempt clears prior draw references;
shader/device generation checks still occur during resource resolution. These
references belong to the current submission and do not cache draw state across
frames. GBuffer recording binds its retained payload without a pipeline-cache
lookup; the payload also retains the resolved fragment RHI shader.

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
named graph-wide structural policy rejects an oversized active batch. Before any
creation, the allocator reserves all reusable entries for the complete batch,
then evicts the oldest unreserved entries until retained bytes plus missing
allocation bytes fit the ceiling. Texture and buffer requests share this budget.
After eviction, RHI processes CPU retirement and completed native deletions
before creation; it never waits for GPU idle. This is a logical pool budget,
not a hard bound on device memory: in-flight deletions, exports, other owners,
and backend allocation overhead remain outside the retained-byte count. Active
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

`FSceneRenderingService::RenderView_RenderThread` creates one temporary
`FSceneRenderPipeline`. The service owns persistent feature renderers, caches,
view states, allocator and startup/invalidation duties. Each submission
constructs a noncopyable, nonmovable `FSceneRenderer` with its own in-place
`FSceneFrameContext`. Pipeline prepares that context, then calls
`FSceneRenderer::Render(FRDGBuilder&)` exactly once. Render registers scene
resources and wires typed outputs through named `Add<Feature>Passes(Graph, Inputs)`
functions. It does not execute the graph or commit history.
`GetOutputTexture()` exposes the registered output handle after authoring so
callers can append consumers to the same builder; the declared imported final
access still applies.

The pipeline owns the builder, execution/capture and final transaction publication.
The submission and service outlive Execute and publication/abort; graph callbacks
borrow their stable records. Telemetry and view-state guards are destroyed before
the submission context. Logical preparation runs once. View resources, scene
resources and feature resources each collect required PSO requests, join them at
one common waiting boundary, and resolve their results before advancing. Dependent
requests may require another wave within the same resource phase; previous phases
and scene collection are not repeated. Each phase is bounded by 4,096 joined
observations and returns failure if waiting is unavailable or capacity is exceeded.
Per-phase resolved state and observations are restored before resolving a completed
wave so partial draws and counters are not accumulated across attempts.

Feature resource preparation reads the successful history sequence without opening
a temporal transaction. Only after all required resources are ready does the
outer submission establish its view-state guard and begin history mutation.
Feature policy is stored directly in `Context.Features`; preparation-only flags
stay local. Pure viewport fitting lives in `FitSceneViewToOutput`.

1. Validate output extent and fit the view.
2. Prepare environment, visibility, lighting, receiver/shadow logical draws,
   shared poses, translucency and cloud inputs once; derive feature policy.
3. Collect, join and resolve resource phases; reject an interleaved view-state
   submission and read its sampling sequence for cloud preparation. Begin the
   temporal transaction only after feature resources are ready.

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
scene renderer passes that output directly to the exact downstream input; there is
no frame-wide execution-channel lookup or mutable channel bag. Payload
storage, one writer, declared readers, dependency lifetime, and callback
access are owned by RenderCore. Distinct non-RHI results cannot be
interchanged, and callbacks do not communicate through mutable side payloads.

Unrequested GBuffer, AO, contact visibility, and cloud-shadow features declare no pass,
value, or target. Their optional completion handles remain absent; deferred
lighting and cloud composition declare reads only for present completions and
interpret absence as `NotRequested`. Requested features retain their failure
reporting even when the selected resources are unavailable.
Unrequested cloud spatial work also declares no pass, value, or target; feature
authoring records the disabled-view and unneeded-route observation. Disabled cloud
routes declare no composite pass or value. A requested but disabled spatial route
retains its producer as an explicit diagnostic root, preserving its failed result
and route observations even without a composite consumer. Scene Color treats an
absent composite completion as `NotRequested`. GBuffer consumers that are always
present use optional reads; demanded AO and visibility branches require GBuffer
through feature-plan dependency closure.
Their recording functions assume the request was accepted by feature authoring;
resource readiness and execution failures remain checked at the recording boundary.
GBuffer color handles cross feature boundaries as one optional set of four required
handles. AO handles form one optional Raw/Scratch set with an optional, complete
Selector/Resolved reconstruction pair. Pass parameters lower those sets into exact
per-resource declarations; callbacks never select a different feature route. Each
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
scene renderer before registration: either the complete candidate set and sampler or
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

Cloud stages share density/detail/weather handles and weather range metadata
through `FCloudDensityInputs`. Environment authoring inputs group the selected handles, physical range metadata
and sampler in `FSceneEnvironmentInputs`. These pointers are setup metadata;
callbacks continue to resolve physical resources only from their declared pass
parameters. GBuffer keeps its four attachments as one typed texture set.

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
unit. The scene renderer wires returned typed outputs into narrow downstream inputs
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

`FSceneRenderer::PrepareGraphResources` sets observational regression ceilings of 15 declared
passes, 36 dependencies, and 34 physical texture transitions. These include
independent shadow-layer passes and their typed observation consumer. Structural limits
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
