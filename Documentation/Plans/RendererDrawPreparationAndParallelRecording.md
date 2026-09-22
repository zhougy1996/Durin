# Renderer Draw Preparation and Parallel Recording Plan

Summary: Replace repeated per-view mesh interpretation with persistent draw records, prepared binding batches, and task-based CPU recording while preserving RDG resource and submission contracts.

Last reviewed: 2026-09-22

Status: Active
Completed:

## Current Status

Planning only; no stage is accepted and no performance improvement is claimed.
The source audit follows `f3c835e51`, which introduced shared preparation facts,
dense uniform groups and separate view/material descriptor sets. The user
reports no obvious reduction in render-thread time. This is qualitative feedback,
not a controlled benchmark. That commit's correctness results do not establish
the performance baseline or acceptance of this plan.

Next work is Stage 0: establish comparable captures and freeze the lifetime,
invalidation and command-list ownership contracts before changing execution.
Do not expand the existing per-draw caches as a substitute for this work.

## Goal

Move stable mesh, material and shader interpretation out of the per-frame path.
Make frame preparation produce compact visible-command lists and binding batches;
make recording consume those records without rediscovering draw state. Reduce
both total CPU work and the rendering critical path, then parallelize remaining
work where the workload justifies task overhead.

## Evidence and Scope

The audited implementation has the following concrete remaining costs:

| Area | Current evidence | Intended change |
| --- | --- | --- |
| Mesh collection | `PrepareStaticMeshView_RenderThread` collects batches, validates geometry and constructs pipeline keys for each view | Retain view-independent records; perform visibility and LOD selection per view |
| Material preparation | `ResolvePreparedMaterialBinding` calls `TryGetMaterialRenderBinding`, which validates layout and copies payload/resource arrays for each section | Resolve immutable material bindings once per accepted revision |
| Shared facts | `FStaticMeshPreparationCache` hashes material contents and compares representations during collection | Propagate stable revisioned identities; use content comparison at publication/deduplication boundaries |
| Shader binding | `BindCompiledSurfaceMaterial` walks reflection and creates a resource vector on each draw | Compile binding layouts once; prepare concrete bindings before recording |
| Vulkan descriptors | `GetOrCreateDescriptorSetsForDraw` sorts, validates and hashes pending resources per draw | Reuse prepared per-set state; update only changed state and dynamic offsets |
| RDG recording | `FRDGBuilder::Record` invokes pass callbacks sequentially on one immediate command list | Introduce owned recording chunks and deterministic assembly |

Moving uniforms into descriptor set 0 does not itself reduce binding frequency.
Uniform grouping is also not draw-call merging. The existing Vulkan descriptor
key already excludes dynamic-uniform offsets; fewer uniform allocations alone
need not reduce descriptor allocation or lookup work.

Preserve the implemented [frame preparation contract](../Runtime/Rendering/RendererFramePreparation.md),
[render graph contract](../Runtime/Rendering/RenderGraph.md) and
[resource recovery contract](../Runtime/Rendering/RendererResourceRecovery.md).
RDG already provides dependency analysis, culling, lifetime planning and barriers;
this plan extends its execution boundary rather than replacing the graph.

The [Geometry Submission Refactor](GeometrySubmissionRefactor.md) owns generic
geometry and vertex-factory extensibility. Preserve independent geometry providers,
multiple batches and dynamic factories. The [RDG and RHI Multi-Queue Execution
Plan](RdgRhiMultiQueueExecution.md) owns GPU queue scheduling, split barriers and
aliasing. CPU parallel recording here must preserve its submission and retirement
contracts and must not silently enable production async compute.

Non-goals: a full UE renderer port, Nanite, GPU-driven visibility, a mandatory
bindless backend, or changing rendering features and image quality. A scene-wide
primitive buffer and instancing are later work, not prerequisites for eliminating
repeated CPU interpretation. Do not reorder transparent draws to increase batching.

## Selected Architecture

Names below describe responsibilities; Stage 0 freezes concrete API names.

| Lifetime | Owned data | Update trigger |
| --- | --- | --- |
| Shader/pipeline generation | Binding layout, shader selection metadata, pipeline identity | Shader/layout generation or device/resource generation changes |
| Scene/asset revision | Geometry records per supported LOD, material binding records, eligible mesh-pass command templates | Accepted geometry/material updates or relevant pass-policy changes |
| Frame/view | Visible command indices, selected LODs, sort keys, view/pass constants, instance data | Each frame and view |
| Recording chunk | Immutable command span, binding state, owned command memory and resource references | Each recording task |
| GPU submission | Submitted command/resource ownership and retirement proof | Completion or defined failure handling |

The intended flow is:

```text
Accepted scene/material/geometry changes
    -> publish revisioned records and invalidate affected commands
Visibility + per-view LOD selection
    -> visible command references + dynamic-provider commands
Pass preparation
    -> ordered command spans + material batches + view/primitive data
RDG setup and compile
    -> resource dependencies + recording plan
Recording tasks
    -> independent command chunks
Ordered assembly and RHI translation
    -> existing GPU submission and retirement contracts
```

Persistent commands contain view-independent draw state, not a saved view matrix
or a transient RDG resource pointer. View/pass resources are supplied at execution
through declared pass parameters. Stable resources remain alive through their
consumers; frame-ring updates must not overwrite data still used by an older frame.

Use explicit revision/generation dependencies rather than raw pointer identity.
Material scalar updates, texture replacement, geometry streaming, spline changes,
shader reload, pass-policy changes and device reset must either update referenced
data safely or invalidate exactly the dependent commands. Retain a bounded dynamic
path for providers whose bindings depend on the view. Cache eligibility is an
explicit capability, not an assumption about every geometry provider.

Cache construction/deduplication may use hashing. The hot recording path uses
prepared indices and spans, not content hashing or a general cache lookup per draw.
Pass constants, material bindings and primitive data have separate update rates;
descriptor-set numbering alone is not the frequency contract. Backend layout
compatibility and dynamic-offset ordering remain explicit.

CPU recording may run ahead without changing GPU dependency order. GPU async
compute, CPU recording parallelism and RHI translation parallelism are distinct
capabilities. The first implementation retains ordered backend translation;
parallel backend contexts require separate ownership and capability evidence.

## Implementation Stages

### Stage 0: Baseline and ownership contracts

Dependencies: none. Outcome: reproducible cost attribution and reviewed data boundaries.

- [ ] Capture the user workload and bounded static-heavy, material-diverse,
  shadow-heavy and dynamic-update scenes. Record scene revisions, camera, viewport,
  features, cascade count, build configuration, hardware, warm-up and instrumentation.
- [ ] Measure render-thread busy time separately from waits, worker CPU time,
  RHI translation, GPU time and end-to-end frame time. Separate graph compilation
  from callback recording; inclusive `RDG.Record` time is not graph overhead alone.
- [ ] Count material/layout validation, bytes copied, allocations, command builds,
  binding updates, descriptor lookup/update, draw calls and retained memory.
- [ ] Use repeated warmed runs on an exclusive timing lane; record median, p95
  and variability. Keep startup, shader compilation and resource streaming separate.
- [ ] Freeze per-scenario improvement and regression budgets from those measurements
  before evaluating implementations. Specify a minimum improvement exceeding noise;
  moving work from the render thread to another thread alone is not acceptance.
- [ ] Define persistent-record ownership, invalidation keys, dynamic-provider
  eligibility, command-list state boundaries and supported execution modes.

Gate: baseline artifacts and numeric budgets are recorded; every proposed cache
has a documented owner, revision dependency, retirement rule and bounded size.
The screenshot and previous test passes cannot close this gate.

### Stage 1: Persistent geometry and material records

Dependencies: Stage 0. Outcome: unchanged scenes stop repeating stable preparation.

- [ ] Publish immutable geometry records per eligible batch/LOD and accepted
  material binding records at scene-update boundaries; preserve fallback diagnostics.
- [ ] Carry record IDs through collection; eliminate repeated layout validation,
  material-array copying and content hashing for unchanged published records.
- [ ] Share stable records across receiver and cascade views while retaining
  independent visibility, LOD, pass eligibility and transparent ordering.
- [ ] Add targeted invalidation for material parameters/resources, geometry/LOD
  replacement, spline deformation and shader/device generations.
- [ ] Exercise update/removal while older frames still retain records; bound memory.

Gate: after warm-up, unchanged eligible records cause zero rebuilds and zero
repeated material-layout validation. Mutations update only dependent records;
dynamic providers and all existing mesh consumers preserve their behavior.

### Stage 2: Prepared mesh draw commands and binding batches

Dependencies: Stage 1. Outcome: recording consumes fully interpreted draw records.

- [ ] Build eligible pass-specific command templates with geometry, pipeline
  identity and binding references; keep transient view/pass data external.
- [ ] Produce compact visible-command arrays, sort opaque/masked state where legal,
  and group adjacent compatible bindings without changing transparent order.
- [ ] Compile shader binding layouts at shader creation; prepare binding payloads
  before recording. Remove reflection traversal and material-vector construction
  from the steady-state draw loop.
- [ ] Resolve readiness, fallback and pipeline availability before recording;
  preserve output failure/publication contracts rather than creating resources mid-pass.
- [ ] Migrate forward, GBuffer, shadow, retained-forward, hit proxy, preview and
  independent geometry consumers, with an explicit dynamic path where needed.

Gate: warmed eligible draw recording performs no material resolution, shader
reflection parsing or pipeline discovery. Counters scale with changed records and
binding batches rather than visible sections; correctness and Stage 0 budgets pass.

### Stage 3: Descriptor frequency and backend state reuse

Dependencies: Stage 2. Outcome: low-frequency data has low-frequency binding work.

- [ ] Prepare pass-common and material set state independently; use stable layout
  identities and explicit compatibility when pipelines change.
- [ ] Avoid sorting and hashing unchanged complete descriptor state per draw.
  Update primitive offsets without rebuilding material or pass resource ownership.
- [ ] Preserve sparse sets, optional bindings, dynamic-offset ordering, descriptor
  pool lifetime, device reset and chunk-boundary state initialization.
- [ ] Validate structural bindings when constructed and changed; retain necessary
  draw-dependent range/access checks and a diagnostic full-validation mode.

Gate: identical consecutive binding batches cause no redundant descriptor update;
offset-only changes do not perform full descriptor reconstruction. Pipeline/set
changes and resource retirement pass backend tests. Allocation and retained-memory
budgets remain bounded, with measured CPU improvement over Stage 2.

### Stage 4: Parallel frame and mesh-pass preparation

Dependencies: Stage 2; Stage 3 may complete independently. Outcome: safe preparation tasks.

- [ ] Freeze a frame snapshot before worker access; avoid mutable scene/proxy reads
  and shared cache writes inside preparation tasks.
- [ ] Partition visibility/LOD and pass-list work using bounded task sizes and
  thread-local scratch; merge deterministically before consumption.
- [ ] Schedule tasks early and join at their first real consumer; preserve an
  inline path and avoid parallelizing tiny workloads.
- [ ] Define local telemetry accumulation and deterministic failure propagation.

Gate: inline and task paths produce equivalent ordered commands and outputs;
mutation/lifetime stress passes and render critical-path savings exceed scheduling
overhead without violating total CPU and memory budgets.

### Stage 5: Parallel RDG command recording

Dependencies: Stages 3 and 4. Outcome: independent command chunks with ordered assembly.

- [ ] Replace the all-pass immediate-list assumption with an owned recording
  interface; classify callbacks by parallel safety and retain serial-only support.
- [ ] Make graph metadata and resolved resources immutable during recording;
  callbacks cannot mutate shared renderer caches, graph state or global telemetry.
- [ ] Record eligible passes or large mesh-pass chunks on independent lists.
  Define how graphics render-pass scope spans chunks before enabling intra-pass
  splitting; workers must not independently corrupt attachment load/store behavior.
- [ ] Keep barrier/submission assembly in the deterministic execution plan, and
  preserve typed-value producer/consumer dependencies, including CPU dependencies.
- [ ] Publish outputs only after successful required recording. Define partial
  failure, cancellation, resource retention and fallback without duplicate submission.

Gate: serial and parallel paths match images, resource transitions and externally
observable ordering. Failure and delayed-completion tests pass; current multi-queue
contracts remain valid. Large workloads improve under frozen budgets and small
workloads select an appropriate serial/task-granularity policy.

### Stage 6: Qualification and contract handoff

Dependencies: Stages 0–5. Outcome: measured production acceptance and maintained contracts.

- [ ] Compare against the frozen baseline on identical workloads and instrumentation;
  report median/p95 frame critical path, total CPU, GPU time, allocations and memory.
- [ ] Validate camera/LOD changes, negative transforms, masked/translucent materials,
  spline and independent factories, material animation, reload and resource failure.
- [ ] Verify editor preview/hit proxy and applicable Engine, Sandbox and RoadWeaver
  consumers; remove superseded interpretation paths only after equivalent coverage.
- [ ] Update owning runtime documents, record evidence and retire temporary controls
  that are no longer required for diagnosis or supported fallback.

Gate: all frozen performance, correctness and lifecycle budgets pass. Follow the
[build workflow](../Agents/BuildAndRun.md) and [test workflow](../Agents/Testing.md);
shared Engine API migrations require an `all` build and affected consumer validation.
Passing functional tests alone does not complete this plan.

## Follow-Up Boundary

After this plan, evaluate scene-wide primitive data and compatible instancing as
a separate measured change. Their purpose is to reduce per-object binding and
draw count; they are not substitutes for command lifetime and invalidation design.
Optimize RDG compiler data structures only when the isolated compile measurements
identify a material cost. Preserve graph correctness rather than removing dependency
or lifetime checks to make inclusive recording timings appear smaller.

## Reference Basis

- [Epic: Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
  motivates persistent draw state, explicit invalidation and visible-command processing.
- [Epic: Render Dependency Graph](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine)
  motivates separation of setup from recording and pass-scoped resource declarations.
- [Epic: Parallel Rendering Overview](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine)
  motivates separating command recording from backend translation and GPU submission.

These are architectural references, not proof of a particular UE release's full
implementation. The selected Durin design and acceptance gates above are proposals
grounded in the audited local paths; do not import version-specific UE limitations
or assume matching class names provide matching performance.
