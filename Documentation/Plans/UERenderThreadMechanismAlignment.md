# UE Render Thread Mechanism Alignment Plan

Summary: Reduce rendering CPU critical paths through persistent preparation caches, dependency-driven preparation, conservative visibility and UE-style asynchronous command dispatch and parallel native translation.

Last reviewed: 2026-11-22

Status: Active
Completed:

## Current Status

This plan consolidates the four remaining findings from the local UE comparison:
serial preparation and delayed task replenishment, preparation-cache churn across
views, missing occlusion rejection, and single-threaded native RHI translation.
Stage 0 is in progress. The source inventory, route decisions and diagnostic
receipts cover mixed geometry, disjoint-view cache churn and offscreen frame
attribution. The remaining workloads, windowed attribution and qualified
timing budgets are open.

The one-off frame-attribution probes are archived in `2956bdd8f` and have been
removed from the working renderer and backends. The disjoint-view fixture still
provides frame timing, churn counts and an optional cache-memory census for
Stage 1. On 2026-11-19, Stage 1 was authorized for functional delivery before
timing budgets are frozen. Scene-owned retention now uses the normal rendering
path. Its functional checks are complete. Exclusive timing and workload-wide
budgets remain open under Stages 0 and 5.

Stage 2 is functionally complete. Stage 3 passed opt-in correctness and completed
the initial high/low occlusion comparison. GPU bounds batches now return indexed
visibility to the main view. History applies the UE 5.8 camera thresholds to
the actual producing view and resolves reordered candidates by component ID.
Occlusion consumes scene primitive identity, eligibility and bounds. Material,
geometry and instance publications invalidate stale feedback before visibility.
Mesh preparation captures its own published batches. On 2026-11-22, rollout was
approved for eligible persistent main views with an explicit per-view opt-out.
The low-occlusion overhead gate remains open.
Stage 2 has removed render-thread task replenishment. Static mesh merges and
sorting follow chunk workers through a completion dependency. Each view selects
LOD independently and publishes validated ordinary static batches directly for sibling
cascades in the same submission. CPU stream validation and
spline winding use captured inputs after admission. Shadow cascades reuse
resolved ordinary static batches on compatible section hits and join at their
individual GPUScene consumers after receiver collection. The receiver and cascades
share Begin/Finish entry points, with a synchronous adapter for HitProxy.
Proxies publish reusable static inputs through `DrawStaticElements`. Water
uses the ordinary StaticMesh publication and each view retains its LOD and pass selection.
Only translucent sections compute sort depth. Shadow preparation skips GBuffer
capability queries for both cached static sections and per-view factory inputs.
Scene registration creates one SceneInfo per owned proxy, so the submission
deduplicates capture by SceneInfo alone. Material resolution stays command-local.
The primitive proxy owns its local-to-world transform and derives the inverse
when the transform changes. Ordinary static batches capture that result at
admission. Instance and custom batches still validate their own transforms.
GPUScene now consumes the primitive builder's validated upload record directly.
Default and dynamic instances reuse the packed primitive transform and bounds.
Release packing regression coverage compares default-instance words with
independently packed instance data for a mirrored transform at a large origin.
Scene/proxy access, mutable
cache decisions, factory callbacks and RHI resource resolution stay on the render
thread. Shutdown and source teardown have functional coverage.

The prerequisite main-pass recording change is committed as `345a2fc30`.
GBuffer and opaque/masked forward and retained-forward draws now use
`FParallelMeshDrawCommandPass::DispatchDraw`, `FParallelCommandListSet` and
`FRHISubCommandList`. Recording is partitioned with a 64-command default grain,
bounded by worker count and recording width. Children preserve one parent render
pass and transfer resource ownership and PSO dependencies in draw order.
Recording still joins on the rendering thread before synchronous observations
are published; native translation/replay remains serial. These remaining limits
are explicitly included in Stage 4 rather than treated as full UE dispatch parity.

Recorded prerequisite validation covers Win64-Debug-DurinEditor: workspace `all`
build, 155 RHI tests, 177 renderer contracts, 256 Core concurrency tests and two
forced-parallel Vulkan pixel/culling regressions. These are correctness/build
receipts, not Release performance evidence. Reuse only where the final relevant
inputs remain applicable under the [test workflow](../Agents/Testing.md).

The original comparison used UE 5.8.0, CL 53629095, at
`E:/Programs/Epic Games/UE_5.8`. That installation was absent at Stage 0
execution. The working reference is now the installed source at
`D:/Programs/UE_5.8`, whose `Engine/Build/Build.version` reports UE 5.8.2,
CL 56702186, branch `++UE5+Release-5.8`. Reconfirm the version and source
symbols at each stage. Existing qualification receipts retain their original
provenance.

## Goal

Reduce actual rendering-thread work and its dependency critical path for large
static/dynamic scenes, multiple views and heavily occluded scenes. Distinguish
CPU execution from task joins, RHI queue pressure, native translation, GPU waits
and presentation pacing. Do not infer a rendering-thread compute bottleneck from
an inclusive thread duration alone.

Align interface responsibilities and execution semantics with local UE:
scene-owned retained facts, dependency-driven preparation, conservative
visibility, asynchronous recording prerequisites, independent translation
contexts and ordered submission. Similar class names or additional worker tasks
alone do not establish alignment.

## Scope and Selected Decisions

- Retain the committed main-pass recorder and its serial comparison path.
  Preserve sorted translucency order, pass clear/load/store behavior, indirect
  argument addressing, optional statistics and source/resource retirement.
- Replace per-view cache sweeping with scene-aware, frame-aged retention under
  explicit byte/entry budgets. A view not visiting an entry does not invalidate
  that entry. Structural dependency changes still do.
- Separate owner-thread snapshot/publication from immutable worker preparation.
  Use dependency-driven chunks or continuously replenished worker ranges; do not
  require a rendering-thread consumer to launch every subsequent wave.
- Add conservative, asynchronous occlusion state. Unknown, stale, failed or
  discontinuous visibility remains visible. Never wait for a GPU readback on the
  normal frame path. Late GPU culling alone does not satisfy CPU-work reduction.
- Separate recording completion, native translation and ordered submission.
  Translation workers use independent backend state, not concurrent replay into
  today's shared default context. Preserve explicit inline/serial fallbacks.
- GPU async compute, multiple physical GPU queues, Nanite, bindless rendering,
  shadow-map content caching and presentation-policy changes are outside scope.
  Native translation parallelism does not require multiple GPU queues.

Existing plan boundaries:

- [UE-Aligned GPU Scene](UEAlignedGPUScene.md) owns persistent scene data,
  command identity, instance grouping and its remaining qualification gates.
  Reuse those contracts and tests; this plan does not close its open gates.
- [Shadow Preparation Optimization](ShadowPreparationOptimization.md) owns
  receiver/cascade fact and resource reuse. Stage 0 reconciles overlapping
  pending work and records one implementation owner for each change. Schedule
  already reusable shadow inputs without duplicating their cache machinery.
- [RHI Creation Contract Tightening](RHICreationContractTightening.md) owns native
  creation and compilation-failure semantics. Pending/failed PSO requests must
  retain their accepted-work outcomes through asynchronous dispatch.

## Evidence and Execution References

| Area | Current implementation | Local UE comparison symbols/files |
| --- | --- | --- |
| Visibility and preparation | [SceneVisibility.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp), [StaticMeshRenderPreparation.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.cpp) | `SceneVisibility.cpp`: `FVisibilityTaskData`, relevance, dynamic mesh gathering and mesh-pass setup tasks |
| Preparation-cache lifetime | [SceneRenderPreparation.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPreparation.cpp), `FMeshDrawDescriptionCache::BeginSubmission/EndSubmission` | `PrimitiveSceneInfo.cpp`: scene-owned static mesh command caching and invalidation |
| Occlusion and GPU culling | [InstanceCulling.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/InstanceCulling.cpp), [InstanceCulling.slang](../../Engine/Shaders/Slang/InstanceCulling.slang) | `SceneVisibility.cpp`: occlusion processing; instance-culling and HZB consumers appropriate to the chosen visibility route |
| Recording and translation | [MeshDrawCommandInstancing.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/MeshDrawCommandInstancing.cpp), [RHICommandList.cpp](../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp) | `MeshDrawCommands.cpp`: `DispatchDraw`/`Dispatch`; `SceneRendering.cpp`: command-list sets; RHI `RHICommandList.cpp`: Dispatch/Translate/Submit pipes |

Implemented authorities remain [frame preparation](../Runtime/Rendering/Pipeline/RendererFramePreparation.md),
[GPUScene](../Runtime/Rendering/Scene/GPUScene.md),
[RHI command execution](../Runtime/Rendering/RHI/RHICommandExecution.md),
[RDG](../Runtime/Rendering/Pipeline/RenderGraph.md) and the
[CPU task system](../Runtime/Core/TaskSystem.md).
Use the [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md)
workflows for selection, execution and result reuse. Performance acceptance follows
the [rendering baseline protocol](../Development/Build/RenderingPerformanceBaseline.md).
Keep raw profiles, CSVs and logs under ignored `Build/` or retrievable CI artifacts;
record concise receipts here, not raw reports.

## Stage 0 Source Inventory and Route Decisions

The inventory uses Durin `ffe2f169a`, which contains `345a2fc30`. The current
workspace projects are Engine, Sandbox and RoadWeaver.

| Change owner | Durin producer and consumers | UE 5.8.2 comparison |
| --- | --- | --- |
| Stage 1, Renderer cache lifetime | `SceneRenderPreparation.cpp` opens and closes `Service.MeshCommandCache` per preparation submission. `FMeshDrawDescriptionCache::BeginSubmission/EndSubmission` reset use flags and trim unused entries while each table exceeds 512. Account for static facts, prepared sections and commands together. Validate scene, main-pass and shadow consumers, including resource replacement and dynamic providers. GPUScene keeps ownership of persistent scene records, command identity and grouping. | `PrimitiveSceneInfo.cpp`, `FPrimitiveSceneInfo::CacheMeshDrawCommands`, `AddToScene` and `RemoveFromScene`. Durin will retain scene-keyed facts with frame aging and explicit capacity limits. |
| Stage 2, Renderer task dependencies | `PrepareSceneVisibility` and static-mesh preparation capture, classify and assemble ahead of `CollectStaticMeshView_RenderThread` and mesh-pass preparation. Both task paths have RT-owned replenishment and result joins. Stage 2 owns scheduling and immutable input handoff. Shadow Preparation Optimization retains ownership of shared receiver/cascade fact layout, primitive/resource validation, repeated batch materialization and cascade projection/LOD. | `SceneVisibility.cpp`, `FVisibilityTaskData::LaunchVisibilityTasks`, `ProcessRenderThreadTasks`, `FinishGatherDynamicMeshElements` and `FComputeAndMarkRelevance::AddPrimitives/Finish/Finalize`. |
| Stage 3, Renderer visibility | `PrepareSceneVisibility` feeds collection. The later `FInstanceCullingContext::BuildRenderingCommands` and `CullMain` in `InstanceCulling.slang` compact eligible instanced draws after CPU preparation. Apply validated main-view history before collection. Keep shadow caster admission independent of receiver visibility, and preserve direct, CPU-instanced and indirect fallback consumers. | `SceneVisibility.cpp`, `FGPUOcclusionPacket::OcclusionCullPrimitive` and `FPrimitiveOcclusionHistory`. `SceneOcclusion.cpp`, `FHZBOcclusionTester::AddBounds`, `Submit` and `MapResults`. |
| Stage 4, Renderer and RHI execution | Renderer owns recording results for GBuffer and opaque/masked forward/retained-forward work. `FParallelCommandListSet::Dispatch` currently joins worker completion before child insertion and result observation. RenderCore owns RDG prerequisite and resource-lifetime integration. RHI owns translation and submission ordering. `FRHICommandList::InsertParallelRenderPass` appends children for serial replay through the default backend context. VulkanRHI and MetalRHI own independent native encoder state. RHI Creation Contract Tightening retains PSO terminal and compilation-failure semantics. | `MeshDrawCommands.cpp`, `FParallelMeshDrawCommandPass::DispatchDraw`; `SceneRendering.cpp`, `FParallelCommandListSet::Dispatch`; `RHICommandList.cpp`, Dispatch/Translate/Submit pipes, `RHIGetParallelCommandContext` and `RHISubmitCommandLists`. |

Use HZB bounds tests off the main-view opaque depth and read back compact
results through an RDG copy into a bounded per-view ring of
`FRHIGPUBufferReadback` slots. Each result identifies its producing frame,
scene/device generation and candidate identity/generation. UE's `MapResults`
can block on staging. Durin will consume only already-published ready data and
skip producers when the ring is full. Unsupported or unavailable depth, shader,
feedback or readback capacity leaves frustum admission in effect.

Initially accept occluded results only from a prior frame within a bounded
feedback age and with matching persistent view, view/projection, viewport,
origin, depth convention and opaque-occluder revision. Freeze the maximum age
against measured feedback latency and retain the always-visible fallback after
expiry. Camera or occluder motion, cuts and
unknown or changed bounds or geometry invalidate the result. The HZB test must
prove full projected coverage using the farthest depth in each covered cell
and a strict depth margin. Near-plane intersections, invalid projections and
ambiguous coverage remain visible. Re-test rejected candidates with bounded
feedback and compare rendered output frame by frame against frustum-only
admission, including newly revealed objects and non-occluding materials. Treat
the eligible occluded count and downstream visits as separate measures.

For translation, select independent Vulkan secondary command buffers with
worker-owned pools, descriptors and temporary allocations, and explicit render
pass inheritance. The parent remains responsible for attachment operations and
executes children in logical draw order. UE's Vulkan implementation is
`FVulkanDynamicRHI::RHIGetParallelCommandContext` in `VulkanSubmission.cpp`.
Durin currently allocates primary buffers in `VulkanCommandBuffer.cpp`. Its
Metal command context holds a regular render encoder. UE's Metal counterpart is
`FMetalDynamicRHI::RHIGetParallelCommandContext` in `MetalRHI.cpp`, using native
parallel render encoding. Keep Metal on the serial path until its independent
state and native qualification are available. Unsupported Vulkan pass or
capability combinations, inline mode and small workloads retain serial replay.
Carry owned recording inputs and terminal completion into translation with
separate ordered submission. Preserve synchronous observation where a caller
requires the result and publish optional statistics only after completion.

The primary timing lane is Windows Vulkan on the NVIDIA GeForce RTX 3090.
Host discovery found driver `616.64`, Vulkan device API `1.4.351` and loader
`1.4.357`. Required correctness lanes are dedicated and inline Vulkan, with
native macOS Metal build/runtime coverage when backend code or execution
contracts change. Adapter discovery does not establish an exclusive timing lane.

### Workload and measurement gaps

Use the following fixture inventory to build and freeze the Stage 0 workload
set. The new cases must exercise normal scene submission and retain deterministic
scene inputs and camera sequences for the final A/B comparison.

| Workload | Starting fixture and remaining coverage |
| --- | --- |
| Small scene and shadow-heavy control | Reuse `DirectionalShadowBaselineVulkanTests` five-box and frozen high-overlap cases. Record separate receiver and cascade timings and keep the existing output checks. Add a no-shadow small-scene control for per-view overhead. |
| Unique sections/materials and disjoint view revisits | Use the disjoint-view fixture below for unique geometry/section keys, warm A/A and B/B controls, alternation and a delayed revisit in one scene. The large distinct-material workload remains open. |
| Repeated instances | Render a compatible repeated mesh/material group through the same camera path with dynamic instancing on and off. Compare pixels, native draws and uploaded bytes. Keep the unique-section fixture as the negative grouping control. |
| Dynamic/Spline providers | Start from `StaticMeshRenderPreparationQualificationTests` with 64 primitives and 256 draws. Add production frames with deterministic geometry updates and a workload large enough to span several former task waves. |
| Occluded and exposed views | Use a fixed interior/exterior scene with opaque occluders and hidden objects. Measure static-camera plateaus and replay a deterministic motion/cut/disocclusion path. Include a low-occlusion control and compare each image against frustum-only admission at the same camera pose. |

`StaticMeshRenderPreparationQualificationTests` measures a preparation call,
and `VulkanCreationQualificationTests.BindingUpdateAndSubmission` measures
synthetic producer and queue/replay spans. Neither supplies end-to-end frame
attribution. Add frame-correlated observations for GT/RT active work and waits,
then distinguish worker joins, RHI admission and replay, native submit, GPU
retirement, and acquire/present in a windowed workload. Record release frame
samples with tracing disabled and collect attribution traces separately. Include
cache and visit counts, temporary and retained memory, upload bytes and native
draws. Save per-frame rows in run order for A/A noise and percentile comparisons.
Freeze numeric latency and memory budgets only after the fixture and exclusive
lane provide applicable baseline evidence.

The diagnostic trace can reuse `Renderer.PrepareView`,
`Renderer.PrepareDirectionalShadows`, `RHI.Enqueue.BackpressureWait`,
`RHI.Commands.Replay`, `Vulkan.Submission.QueueSubmit`,
`Vulkan.BeginFrame.FrameSlotWait`, `Vulkan.Completion.WaitFenceNative`,
`Vulkan.Presentation.AcquireImage` and `VulkanViewport.Present`. Correlate by
frame and thread, and instrument preparation-task execution and joins separately
inside the inclusive preparation zones. Use GPU timestamps or completion events
for GPU execution/retirement and report CPU thread waits independently.

### Mixed geometry diagnostic receipt

On 2026-11-19, `StaticMeshRenderPreparationQualificationTests` passed with
`Win64-Release-DurinEditor`, MSVC `14.44.35207`, `DURIN_WITH_TRACY=0` and
`DURIN_VULKAN_VALIDATION=off` on the RTX 3090. The tested inputs were
`ffe2f169a139f91bbf2ddef5643de31e303f1710` plus this change to retain raw
preparation samples. The test input SHA-256 was
`3d6b29609aea73dbae8326d5be5678d5f46ee2ce210c22cd1162394249e3018d`.

Set `DURIN_TEST_KEEP_WORK=1` and run
`.\DevTool.bat test StaticMeshRenderPreparationQualificationTests --mode qualification --preset Win64-Release-DurinEditor --test-jobs 1 --report`.
The case records three runs of 30 warm-ups and 120 measured samples of
`PrepareStaticMeshView_RenderThread` over 64 primitives and 256 draws. The
verified per-run medians were 259.05, 256.10 and 255.55 us, with p95 values of
293.40, 302.50 and 327.60 us. Peak top-level container capacity was 89,672 bytes
in each run. These results describe preparation-call wall time and top-level
capacity. Exclusive timing authority, full-frame attribution and retained
cache memory were unverified.

The test writes `GeometryPreparationBaseline/samples.csv` in its retained work
directory. The 360 sequential samples, full CTest log, JUnit report and device
inventory are saved under
`Build/Qualification/UERenderThreadMechanismAlignment/20261119/`.

### Disjoint-view cache fixture

`FStaticMeshRenderPreparationQualificationTests.RecordsDisjointViewCacheChurnBaseline`
renders one unchanged scene with two disjoint 32x32 grids of opaque triangles
using two 192x192 orthographic unlit cameras. Each grid has 1,024 published
geometry records with one section and its own static-input publication per
primitive. The material is shared. The test verifies distinct geometry record
IDs and distinct static-input keys across both sets and checks the visible
primitive IDs against the intended grid. Every measured `Renderer.RenderView`
submission visits 2,048 primitives, admits 1,024 sections and records 1,024
native draws. Dynamic instancing and GPU culling are disabled.

After resource preparation for both views, each of three runs measures fixed A,
fixed B and alternating A/B. Each sequence has 30 warm-up frames followed by
120 measured frames. After alternation, eight additional B frames precede the
first return to A and an immediate second A frame. CSV rows identify each view
and report description reuse/builds, which include the retained-section shortcut.
The draw-description, static-fact and static-section tables each report counts
at submission start, immediately before trimming and immediately after trimming.
The difference across trimming is the eviction count for that table.

The 2026-11-19 baseline used `cdfb4e54b9933b677e580ed5bff7dd1919f7ae22` plus
this fixture and Renderer telemetry change. The test source SHA-256 was
`001b24b71b95a0ce57367d849ff374c5d4c9c76969b8be1c7941dfc8c644923c`.
Use the mixed geometry receipt's DevTool command, with the same Release preset,
Tracy-off and validation-off settings. The target passed both cases on the
RTX 3090 with driver 616.64, Vulkan device API 1.4.351 and dedicated RHI
execution. The disjoint case starts a two-worker scheduler for shader
preparation and task execution. Both views are prepared before sampling.

| Sequence | Reused / built descriptions per frame | Evictions per frame (descriptions / facts / sections) | Per-run `RenderView` median range | Per-run p95 range |
| --- | ---: | ---: | ---: | ---: |
| Fixed A | 1,024 / 0 | 0 / 0 / 0 | 15.47–16.22 ms | 16.67–19.07 ms |
| Fixed B | 1,024 / 0 | 0 / 0 / 0 | 15.49–15.72 ms | 16.79–17.43 ms |
| Alternating A/B | 0 / 1,024 | 1,024 / 1,024 / 1,024 | 16.88–17.24 ms | 18.56–19.12 ms |

After the eight additional B frames, the first A rebuilt all 1,024
descriptions and evicted 512 descriptions, 1,024 facts and 1,024 sections. The
immediate second A reused all 1,024 descriptions through retained sections.
It trimmed the separate description table from 1,024 to 512 entries while
retaining 1,024 facts and sections. Alternating frames held 2,048 entries per
table before trimming and 1,024 per table after trimming. These are table
entry counts. Sections can still own descriptions outside the description
table, so the counts cannot be summed into a retained-memory estimate.

The 1,086 verified frame samples, per-run summary, CTest log and JUnit report
are in `Build/Qualification/UERenderThreadMechanismAlignment/20261119/disjoint-views/`.
`RenderView` wall time excludes frame begin/end and presentation and can
include RHI waits. The telemetry-on run establishes repeatable churn counts.
The subsequent offscreen frame receipt covers active CPU/wait attribution and
retained bytes for this fixture.

`RendererSceneContractTests` on the same Release preset passed 173 of 176
cases with this change. The unchanged `cdfb4e54b` Renderer and contract inputs
reproduced the same three failures in indexed culling arguments, dynamic
instancing state matching and shared pipeline invalidation. The two paired
contract logs and XML reports are retained with the cache receipt.

### Disjoint-view offscreen frame attribution

This is a historical receipt. Run its two modes from `2956bdd8f` to reproduce
the thread, join, RHI and GPU timing columns. The maintained fixture uses
`DURIN_TEST_DISJOINT_VIEW_MEMORY=0` for timing or `1` for the cache census and
the same target and Release preset. It retains GT/RT wall time and the three
cache tables' observations. Use these maintained modes for matched Stage 1
control and candidate runs. Keep the census out of timing runs.

The second receipt preserves the scene, camera sequence and warm-up/sample
counts from the cache fixture. The GameThread now submits one render command
per frame and uses a render-thread fence before issuing the next frame. The
GameThread active time is the test producer's thread CPU time, covering
submission and fence bookkeeping. It has no world tick or gameplay workload.
The RenderThread interval spans frame begin, `RenderView` and frame end.
The CSV records active thread CPU time using the operating system's thread
accounting, plus producer submission wall time, dispatch-to-RT time, GT fence
wait, and RT wall time. On Windows, use summed thread CPU samples across a
phase divided by frame count. Individual CPU deltas are quantized. The short
GT producer additionally records Windows thread CPU cycles to show scheduled
work below the CPU time counter's resolution. CPU cycles retain their native
units and producer submission wall time is reported separately.

Run the qualification once with `DURIN_TEST_DISJOINT_VIEW_ATTRIBUTION=0` for
the frame timing control, then separately with `1` for detailed attribution.
Both retain lightweight draw statistics, CPU clocks and executor counters. The
detailed run adds measured visibility, LOD, section assembly and draw-recording
join calls, cache accounting and Vulkan timestamps. RT serial wait and producer
backpressure have separate columns. Frame pressure is a subset of backpressure.
Phase-level snapshots correlate dedicated RHI thread CPU time, executor replay
wall time, Vulkan `Queue.submit` wall time, native Vulkan fence waits and upload
bytes with the measured frames. Replay can contain backend waiting or nested
submission. These spans overlap across threads and cannot be summed into frame
latency. The timestamp interval brackets the view's graphics commands. GPU
idle is used to read timestamps only after the final measured frame and after
phase counter capture. Offscreen frames have no acquire or Present calls.

The cache census charges table key/value storage, distinct cache-created
descriptions, facts and section lists, and their owned vector capacity. A
description retained by both a table and a section list is charged once. The
before-trim value captures the phase's overlap between the two view working
sets. The after-trim value measures retained cache storage. Bucket and node
overhead, allocator metadata, reference-count control blocks and externally
owned geometry/material/RHI resources are outside this accounted byte figure.

On 2026-11-19, both lanes passed on the RTX 3090 with `Win64-Release-DurinEditor`,
MSVC 14.44.35207, `DURIN_WITH_TRACY=0`, `DURIN_VULKAN_VALIDATION=off` and
`DURIN_RHI_EXECUTION=threaded`. Run the same DevTool target and keep work as in
the disjoint fixture receipt. The timing lane ran both cases. The detailed lane
selected `FStaticMeshRenderPreparationQualificationTests.RecordsDisjointViewCacheChurnBaseline`.
The tested base was `03da03324697f317f8960d9059b7dddfc59cb3c7` plus this change.
The fixture SHA-256 was
`c29d2bee3143b08e0fc62dfde906963e62eab5348d6e4a5022ed2ea7cab62d32`.

| Timing control | RT frame median range | RT frame p95 range |
| --- | ---: | ---: |
| Fixed A/B | 15.17–16.12 ms | 16.41–18.88 ms |
| Alternating A/B | 16.30–17.22 ms | 17.88–18.15 ms |

The alternating timing-control phases showed 11.59–12.11 ms of accounted RT
CPU per frame by phase mean. GT submit wall medians were 5.95–6.70 us, with
48,682–53,434 GT CPU cycles by per-run median and 16.32–17.25 ms of GT fence
wall time by per-run median. GT CPU nanoseconds stayed below observable
resolution. The GT fence duration overlaps the RT frame.

In the detailed alternating phases, per-run median join durations were
0.097–0.099 ms for visibility, 0.120–0.122 ms for LOD, 0.146–0.175 ms for
section assembly and 4.49–4.62 ms for draw recording. Each frame issued four,
eight, eight and two joins respectively. Per-frame phase means were
2.21–2.60 ms for RHI CPU, 1.79–1.92 ms for inclusive replay and 0.051–0.058 ms
for Vulkan queue submission. Per-run GPU timestamp medians were 0.267–0.296 ms.
Both lanes recorded zero RT serial, backpressure and frame-pressure waits,
and zero Vulkan fence waits or upload bytes in the sampled phases. Draw
recording dominates the measured join intervals in this fixture.

Fixed-view accounted cache storage was 1,129,472 bytes before and after trim.
Alternation reached 2,324,480 bytes before trim and retained 1,162,240 bytes
after trim. The first revisit to A reached 2,291,712 bytes and retained
1,162,240 bytes. The next A retained 1,129,472 bytes. These values reproduced
over all three runs in the detailed lane.

Each lane retained 1,086 frame rows and 12 phase rows, with summaries and test
receipts under `Build/Qualification/UERenderThreadMechanismAlignment/20261119/frame-attribution/`.
The same Release preset passed all 17 `RHIThreadTests` and three focused
Renderer contracts covering visibility and description-cache retention.
The test runner used one GPU test slot. Host GPU activity was present outside
the samples, and exclusive timing authority was unverified. These figures
are the Release offscreen diagnostic baseline. Isolated timing, windowed
acquire/Present, broader workload coverage and numeric budgets remain open.

### Stage 1 invalidation inventory

| Dependency | Source boundary and required retention behavior |
| --- | --- |
| Material proxy or accepted program | `FStaticMeshSceneProxy::UpdateMaterialBinding_RenderThread` clears its static batch publication. `DrawStaticElements` compares geometry and accepted material record IDs, including fallback when needed. `FScene::UpdateStaticDrawListsForMaterials` clears the corresponding native draw ownership. Stage 1 must reject old preparation publications and bound their retained owners separately from that storage. |
| Geometry, layout and instance eligibility | `FStaticMeshRenderData::CaptureBatchGeometry_RenderThread` compares LOD readiness, screen size and published geometry records. `ReleaseResources` invalidates that snapshot. `FStaticMeshSceneProxy::SetRenderInstances` clears static batches and instance providers continue through the dynamic route. Keep geometry record identity, factory/layout compatibility, material identity and pass policy in the retained lookup. |
| View and transform | Frustum membership and LOD selection remain per view. Raster mode, depth convention, preparation mode, winding and orientation affect the description/section policy key. Visibility and ordinary camera moves leave otherwise eligible cached entries available for a revisit. |
| Shader, device and manual generations | `FRendererResourceCoordinator::Apply_RenderThread` advances the corresponding generation. `FSceneRenderingService::ReleaseDeviceResources_RenderThread` resets the current service-wide cache. A scene-aware replacement must retire obsolete owned entries on all three generations and keep previously published in-flight values alive for their existing consumers. |
| Primitive and scene lifetime | `FScene::DetachPrimitive` removes GPUScene and native command ownership before registry removal. `FScene::Release` serializes resource release, scene clear and final deletion. Attach retained preparation to the same lifetime and invalidate the removed primitive or changed publication. Test a new scene and reused primitive IDs without sharing keys across scenes. |

### Maintained cache control after probe removal

The maintained fixture passed its memory lane and two timing-only A/A launches
on 2026-11-19. All three used the unchanged production cache on
`Win64-Release-DurinEditor` with Tracy off, dedicated RHI, validation off and the
RTX 3090. Each launch retained 1,086 verified rows. The tested base was
`2956bdd8f` plus the probe removal. Fixture SHA-256 was
`fd366dbb26c371f9ec1ede9d1be9ce2a54e4dd58daa2d2a839376f1ccede4691`.

| Alternating-view control | Per-run RT median range | Per-run RT p95 range |
| --- | ---: | ---: |
| First timing launch | 18.18–26.52 ms | 23.36–36.23 ms |
| Repeat timing launch | 17.78–18.48 ms | 19.74–21.47 ms |

All runs reproduced 1,024 description builds and 1,024 evictions per table
per alternating frame. The memory lane reproduced 2,324,480 accounted bytes
before trim and 1,162,240 bytes afterward. The two timing launches measured
the same implementation with the census disabled. The host lacked an exclusive
GPU window and the observed A/A variation leaves latency and p95 acceptance
budgets open. This is the control for Stage 1. Samples,
summaries, GPU snapshots and reports are retained under
`Build/Qualification/UERenderThreadMechanismAlignment/20261119/cache-retention/`.

Three focused `StaticMeshRenderPreparationVulkanTests` passed on the same
Release preset. They cover per-view facts, geometry resource replacement, and
rejection of stale transform and material facts. The existing tests establish
the correctness control for Stage 1's later cache changes.

## Implementation Stages

### Stage 0: Establish attribution, UE contracts and acceptance budgets

Depends on: prerequisite `345a2fc30`; no additional runtime optimization.

Outcome: a reproducible baseline and a fixed contract/ownership inventory for
the four remaining mechanisms.

- [x] Reconcile current source and overlapping plans; map every proposed change
  to its owner, consumers and exact local UE symbols. Record deliberate
  adaptations, including current synchronous recording observations.
- [ ] Capture quiet Release, Tracy-off timing baselines with separate diagnostic
  traces. Separate GT/RT active CPU, scheduling/join time, RHI backpressure,
  translation/submission, GPU completion and Present/acquire waits.
  The offscreen disjoint-view receipt covers this fixture's CPU, join and
  RHI/GPU spans. Windowed attribution and an exclusive timing lane are pending.
- [ ] Freeze workloads for small scenes; large unique sections/materials;
  repeated instances with dynamic instancing on/off; two disjoint views each
  exceeding the old 512-entry working set; camera revisits; dynamic/Spline
  providers; shadow-heavy views; and occluded interior/exterior camera paths.
- [ ] Record cache hits/misses/evictions, visited primitives/sections, task
  occupancy and joins, allocation/upload bytes, native draws, translation time
  and retained/peak memory alongside frame median/p95.
  The disjoint-view fixture now has churn, visits, join and queue timings,
  upload bytes, draws and accounted retained/peak bytes.
- [ ] Fix workload-specific latency, p95 and memory acceptance budgets using
  A/A noise and the qualified device. Name the primary Vulkan device and
  required backend correctness lanes. The Stage 1 implementation can proceed
  before this qualification.
- [x] Resolve the exact UE-referenced occlusion route and conservative history
  validity oracle, and the Vulkan/Metal translation-context capabilities, before
  implementation. Record the chosen interfaces and unsupported-route fallbacks.

Completion: baseline artifacts are retrievable; attribution identifies work
versus waits; ownership, workloads and numeric acceptance budgets are recorded.

### Stage 1: Make preparation-cache retention stable across views

Depends on: Stage 0 cache baseline and invalidation inventory.

Outcome: alternating views and camera revisits reuse unchanged facts without
unbounded retention or stale resource selection.

- [x] Replace service-wide per-submission used-bit reset/trim with scene-aware
  frame access stamps and a bounded retention/eviction policy. Account for all
  retained fact, section and draw-description tables and their strong owners.
- [x] Preserve dependency-based invalidation for material/layout/geometry,
  shader/device/manual generation, primitive removal and scene teardown.
- [x] Test alternating disjoint large views, revisits, scene changes, structural
  mutation, resource replacement and teardown; retain dynamic-provider checks.
- [x] Compare steady-state and revisit misses, evictions, diagnostic RT time
  and retained bytes against the Stage 0 control. Exercise age, entry and byte
  bounds under explicit pressure. Keep qualified warm-up and timing comparisons
  with the outstanding Stages 0 and 5 performance gates.

Completion: below-budget alternating views stop mutually evicting their working
sets; true invalidations still rebuild; pressure remains bounded and measured.

#### Stage 1 evidence

The local UE 5.8.2 CL 56702186 comparison uses
`FPrimitiveSceneInfo::CacheMeshDrawCommands`, `RemoveCachedMeshDrawCommands`,
`RemoveFromScene` and `UpdateStaticMeshes` in `PrimitiveSceneInfo.cpp`.
`FScene::GetCachedMeshDrawCommands` now centralizes creation and generation
synchronization for Durin's existing scene draw storage. The same storage owns
preparation facts, sections and descriptions. Native command locations retain
their primitive-scoped invalidation. Preparation invalidation is conservative
at scene granularity for primitive removal, structural material changes,
rebinding and instance-list replacement. Immutable publication keys and
per-view checks cover geometry, layout, LOD and transform changes. The
[frame preparation contract](../Runtime/Rendering/Pipeline/RendererFramePreparation.md)
defines the frame-age policy, budgets and RHI lifetime boundary.

On 2026-11-19, `aa9c239ad` plus the Stage 1 changes passed the
`Win64-Release-DurinEditor` qualification target on RTX 3090 Vulkan with
Tracy off, validation off, threaded RHI and
`DURIN_TEST_DISJOINT_VIEW_MEMORY=1`. The 360 measured alternating frames and
six revisit frames all reused the complete 1,024-description working set.
The old policy built 122,880 descriptions and evicted 368,640 rows per
120-frame alternating run. The new policy built zero descriptions and recorded
one sweep of 1,024 redundant description-table rows across all three runs.
The section and fact tables retained both views through that sweep. Measured
accounted retention peaked at 2,299,904 bytes across the alternating runs,
compared with 1,162,240 bytes for the old policy. Diagnostic RT frame medians
were 16.12–16.26 ms in the candidate and 17.43–17.77 ms in the legacy memory
lane. The measurements use the cache census and a shared host.

Four focused Renderer contracts passed for capacity pressure and age,
scene/generation isolation, source lifetime, material invalidation and key
validation. Three focused Vulkan integration cases passed for per-view facts,
resource replacement and dynamic/material validation. Raw measurements,
comparisons and receipts are under
`Build/Qualification/UERenderThreadMechanismAlignment/20261119/stage-1/`.

### Stage 2: Build a dependency-driven preparation pipeline

Depends on: Stages 0 and 1; immutable-input and cache-ownership contracts fixed.

Outcome: useful preparation work overlaps without serial collection/resolve
chains or rendering-thread-driven task replenishment dominating the critical path.

- [x] Split visibility input capture, classification, LOD/relevance, section
  preparation, material/pass facts and sorting/grouping into explicit ownership
  and dependency boundaries. Keep mutable caches and publication on their owner.
- [x] Remove fixed-wave replenishment that waits for RT result consumption;
  schedule all bounded ranges or use worker-driven range acquisition. Preserve
  deterministic assembly, cancellation/failure handling and bounded task storage.
- [x] Reuse captured scene/material/geometry facts across receiver, views and
  cascades only where identities and generations permit; retain per-view LOD,
  winding, bias, bindings and dynamic validation where required.
  The ordinary static receiver/cascade path now shares one admitted batch per
  primitive and selected LOD. Compatible shadow views also share its immutable
  resolved transform and sections. Separate submissions start a fresh capture.
  Release Vulkan regressions verify retained Water batches and custom per-view
  collection on non-instanced geometry alongside per-view LOD and pass selection.
- [x] Move measured eligible collection/resolve/finalization work to immutable
  worker inputs; avoid merely parallelizing cheap classification while serial
  traversal, allocation and sorting remain the dominant work.
  For collections of at least 256 candidates with a running scheduler, ordinary
  single-LOD or forced-LOD0 meshes now project captured bounds in the existing
  preparation chunks. Admission resolves their available LOD without a projection
  prerequisite, following the forced-LOD ordering in UE `SceneManagement.cpp`.
- [ ] Qualify rendering-thread active work and critical-path cost against the
  Stage 0 budgets once those budgets are frozen.
- [x] Validate serial/task output parity, changing inputs, scheduler shutdown,
  task failure, source teardown and large workloads spanning many former waves.

The scheduling boundary follows local UE 5.8.2 CL 56702186
`FVisibilityTaskData::LaunchVisibilityTasks` and
`FComputeAndMarkRelevance::Finish/Finalize`: publish prerequisites at launch and
consume the completed result at the owner boundary. Durin reuses its independent
CPU tasks for both the asynchronous dependency chain and synchronous indexed
facts. The ordinary `ParallelFor` worker waits are rejected on the render thread. See the
[frame preparation contract](../Runtime/Rendering/Pipeline/RendererFramePreparation.md).

The snapshot gathering split follows `FRelevancePacket::LaunchComputeRelevanceTask`
and `FVisibilityTaskData::GatherDynamicMeshElements` in local UE 5.8.2. Durin
hands workers retained publications and descriptor facts. Provider callbacks and
factory support queries stay with the render-thread owner.

Functional receipt on 2026-11-19 (`b0170ab87` plus this Stage 2 change):
`Win64-Release-DurinEditor` built `StaticMeshRenderPreparationVulkanTests` and
passed `QualifiesIndependentMultiBatchGeometryAndFactory` and
`HitProxyIdsRespectDepthBackgroundAndCacheInvalidation` on RTX 3090 Vulkan.
The multi-batch case compares serial and tasked visibility, LOD and mesh output,
observes all 12 mesh chunks finalized before calling finish, and exercises
cancellation, a failed completion and abandoned-work drain.
Raw results are under `Build/NativeTestResults/Win64-Release-DurinEditor/`.

The 2026-11-19 snapshot migration receipt (`57fab7805` plus this change)
uses the same Release preset and RTX 3090 Vulkan. The selected
`ClassifiesResolvedSectionsAndRecomputesPerViewFacts` and
`QualifiesIndependentMultiBatchGeometryAndFactory` cases cover retained LOD and
geometry identity over 256 inputs, serial/task stream-validation parity, spline
winding, and the existing draw/cache assertions. The measured-work checkbox
stays open pending RT attribution and timing acceptance.

On 2026-11-21, `2934ba6e3` plus the Stage 2 tests passed on the same Release
preset. `SchedulerShutdownDrainsAdmittedMeshPreparation` closes scheduler
admission with both chunks queued and verifies their output and input release.
`QualifiesIndependentMultiBatchGeometryAndFactory` also prepares all four
sections after scene destruction and verifies the last geometry binding expires
after the prepared output is released.

Two diagnostic runs of `DirectionalShadowBaselineVulkanTests` per workload
used automatic preparation, threaded RTX 3090 Vulkan, Tracy and validation off,
30 warm-up frames and 120 measured frames. The table reports the range of run
medians in ms for RT mesh collection, resolution, launch/inline work and finish
combined. The shadow column sums the three cascades within each frame.

| Three-cascade workload | Main view | Shadows | Main collection | Shadow collection |
| --- | ---: | ---: | ---: | ---: |
| Five boxes | 0.024–0.049 | 0.037–0.061 | 0.011–0.024 | 0.011–0.020 |
| 128 overlapping primitives | 0.428–0.616 | 0.880–1.085 | 0.283–0.411 | 0.550–0.656 |
| 512 overlapping primitives | 0.967–0.988 | 1.916–2.000 | 0.750–0.765 | 1.311–1.379 |

The 512-primitive variant ran four preparation chunks per view. Its p95 totals
were 1.33–1.60 ms for the receiver and 2.92–3.58 ms for shadows. Shadow joins
accounted for 0.158–0.163 ms at the median. This selected repeated static
capture and batch materialization inside `CollectStaticMeshView_RenderThread`
for the next change. Raw frame rows, probe patch, summaries
and test receipts are in
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2/`.
To reproduce the diagnostic, apply `timing.patch`, set `DURIN_PREP_SAMPLE_FILE`
to a fresh absolute output path and `DURIN_VULKAN_VALIDATION=off`, then run
`test DirectionalShadowBaselineVulkanTests --mode qualification --preset Win64-Release-DurinEditor --report`
via DevTool. The 512 case changes only the high-overlap fixture's primitive count.

The 2026-11-21 collection change follows local UE 5.8.2 CL 56702186:
`PrimitiveSceneInfo.cpp` retains static meshes from `DrawStaticElements`,
`SceneVisibility.cpp` selects the view LOD, and `ShadowSetup.cpp` applies
`ShouldDrawStaticMesh` before selecting the shadow pass. Durin shares an
immutable admitted batch within a single submission. The multi-LOD regression
checks a receiver requesting LOD 2, a cascade requesting unavailable LOD 1
and falling back to 2, and another cascade selecting LOD 0. Mixed-material
and dynamic-provider cases retain their pass and stream-validation checks.

Matched diagnostic runs compared `a57788134` and the change on the same
Release RTX 3090 setup with the preceding probe and 30 warm-up/120 measured
frames. Values in ms are medians across runs of each per-frame statistic.
The combined column sums receiver and three-cascade RT preparation per frame.

| Workload | Shadow collection before → after | Combined median before → after | Combined p95 before → after |
| --- | ---: | ---: | ---: |
| Five boxes | 0.010 → 0.007 | 0.060 → 0.067 | 0.084 → 0.105 |
| 128 overlapping | 0.354 → 0.154 | 0.838 → 0.635 | 1.008 → 0.858 |
| 512 overlapping | 1.325 → 0.639 | 2.984 → 2.364 | 4.376 → 3.642 |

Each revision has two 128-object and four 512-object runs, with the five-box
control after each run. Five-box combined medians improved following the 128
case and worsened following the 512 case. Receiver-only p95 also rose in
both overlap workloads. Small-scene and tail acceptance remain open for the
exclusive lane. Both uninstrumented qualification targets passed. One earlier
instrumented full shadow run observed two worker recordings against an expected
three in the forced-parallel case. Raw rows, configuration patches, comparison
script and XML receipts are under
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-static-collection/final/`.

Fixed-LOD projection retest against `0b6db3d99`, same Release setup and probe.
Times in ms sum the receiver and three cascades per frame (30 warm-up, 120
measured frames, two runs per overlap workload and four five-box controls).

| Primitives | Combined median before → after | Combined p95 before → after |
| ---: | ---: | ---: |
| 5 | 0.053 → 0.052 | 0.075 → 0.072 |
| 128 | 0.597 → 0.618 | 0.720 → 0.744 |
| 512 | 2.820 → 2.635 | 6.898 → 5.570 |

At 512, combined collection fell from 1.700 to 1.326 ms while the join rose
from 0.207 to 0.303 ms. The two candidate run medians were 1.915 and 3.356 ms,
so exclusive-lane confirmation remains open. Raw measurements and qualification receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-projection/final/`.

At `9ebf0cf05`, six fresh diagnostic runs used the same Release Vulkan fixture,
30 warm-up and 120 measured frames per case (three runs each at 128 and 512,
six five-box controls). Times below are medians of run medians in µs.
Resolve times cover `ResolveCollectedStaticMeshView_RenderThread`; wait times
cover only the RT call to `Work.Completion.WaitDetailed()`. Inline work has zero
wait. Cascade times are summed per frame before computing percentiles.

| Primitives | Main resolve | Main wait | Three cascades resolve | Three cascades wait |
| ---: | ---: | ---: | ---: | ---: |
| 5 | 3.0 | 0 | 4.85 | 0 |
| 128 | 48.95 | 0 | 79.05 | 0 |
| 512 | 240.4 | 0.1 | 516.3 | 266.6 |

At 512, cascade wait medians in submission order were 0.3, 15.15 and 219.4 µs.
Cascade aggregate p95 was 1,292.1 µs for resolve and 658.9 µs for wait.
Prioritize the repeated per-batch traversal in shadow resolution. Measure the
retained-section cache-hit path there first, and track the final join and total
RT time together as earlier task admission can change overlap. Exclusive timing
acceptance is still open. Samples, per-cascade percentiles and replay patches:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-resolve-join/`.

The shadow resolve change follows UE 5.8.2 `FProjectedShadowInfo::ShouldDrawStaticMesh`
and `AddCachedMeshDrawCommandsForPass`: each cascade selects its caster and LOD,
then references a compatible pass record. Durin indexes the admitted batch
within the submission. On repeat hits this skips the transform lookup and copy,
the persistent and submission-local section lookups, and per-primitive batch allocation.

Matched Release Vulkan runs against `c9e71daae` used the preceding fixture,
probe and sampling settings. Values in µs are medians across runs. Total covers
RT collection, resolution, start and finish across the receiver and three cascades.

| Primitives | Main resolve p50 | Cascade resolve p50 | Final wait p50 | Total p50 | Total p95 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 3.0 → 2.9 | 4.7 → 4.0 | 0 → 0 | 54.3 → 52.0 | 75.2 → 73.0 |
| 128 | 46.5 → 43.3 | 83.2 → 48.5 | 0 → 0 | 635.6 → 592.1 | 786.1 → 760.5 |
| 512 | 252.5 → 300.7 | 563.9 → 298.7 | 276.7 → 134.8 | 2663.8 → 2579.6 | 8162.5 → 7254.6 |

At 512, cascade 2/3 resolve fell from 160.4/137.6 to 24.4/26.5 µs.
Wait shifted earlier: the aggregate cascade wait rose from 312.7 to 371.6 µs.
Run-to-run variation and the non-exclusive lane leave timing acceptance open. Raw samples,
probe patches, per-run summaries and test receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-shadow-resolve/final/`.

The cascade wait change follows local UE 5.8.2's early shadow setup dispatch
and later synchronization in `DeferredShadingRenderer.cpp`. All cascades already
launched before the first join. Durin now retains the existing handles through
feature planning, view resources and receiver GPUScene collection. It joins and
applies bias immediately before collecting each cascade's GPUScene primitives.
Frame teardown drains outstanding handles on an early exit.

Diagnostic A/B against `5b0f17bfa` used the same Release Vulkan fixtures, three
runs at each overlap size and six five-box controls per revision (30 warm-up,
120 measured frames). Times in µs are medians across runs. Total sums receiver
and cascade collection, resolution, start and finish on the render thread.

| Primitives | Cascade 1 wait p50 | Cascade 2 wait p50 | Cascade 3 wait p50 | Total p50 | Total p95 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 0 → 0 | 0 → 0 | 0 → 0 | 55.4 → 61.7 | 78.5 → 99.0 |
| 128 | 0 → 0 | 0 → 0 | 0 → 0 | 599.2 → 617.9 | 768.6 → 751.1 |
| 512 | 0.45 → 0.1 | 139.15 → 0.1 | 117.15 → 0.1 | 1874.6 → 1538.3 | 5789.2 → 2492.5 |

At 512, individual wait p95 fell from 209/394/352 to 0.6/131/56 µs.
The inline controls had zero wait in both revisions and higher total medians.
Repeat those controls on the exclusive lane before changing scheduling policy.
Timing patches, samples, per-run summaries and validation receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-cascade-consumption/`.

A blocked A/B/B/A recheck of `5b0f17bfa` and `c11e50986` used matching
probes and the same fixtures, with six runs per overlap size per revision.
Another application used the RTX 3090, so exclusive acceptance remains open.
Total preparation p50 (µs) was 56.7 → 55.7 at 5 objects, 614.6 → 597.3 at
128, and 1813.1 → 2018.0 at 512. The inline slowdown did not recur consistently.
At 512, aggregate cascade wait fell from 264.5 to 0.5 µs and total p95 fell
from 4870.1 to 3825.8 µs, but only three of six paired total medians improved.
Keep the existing policy. Profile the receiver's collection next: its p50 was
840.7 µs, compared with 257.1 µs for receiver resolution. Raw samples and
pair comparisons: `Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-inline-recheck/`.

Four receiver-only breakdowns of the 512-object fixture put first-use batch
admission at 298–467 µs (45–51% of instrumented collection in every run).
Publication capture took 100–141 µs and view LOD took about 0.3 µs. Three
additional runs singled out cold bounds/custom-data validation (112–206 µs)
and transform resolution (150–213 µs). These inner probes add per-batch clock
reads. The original 841 µs and the comparison below use outer timers only.

Local UE 5.8.2 retains static elements in `FPrimitiveSceneInfo::AddStaticMeshes`
and references eligible pass commands in `FDrawCommandRelevancePacket::AddCommandsForMesh`.
Durin now validates the ordinary static batch in place and moves it directly
to its submission owner. The collector uses the same checks for dynamic batches.
Transform validation relies on `TryInverse` for finite input and inverse,
and still rejects overflow when narrowing for rendering.

Against `f50da7b9d`, six runs per overlap size per revision bracketed the
candidate with baseline runs, with a five-box control each time. Same Release
Vulkan fixture, three cascades, 30 warm-up and 120 measured frames; times in µs.

| Primitives | Main collection p50 | Total preparation p50 | Total preparation p95 |
| ---: | ---: | ---: | ---: |
| 5 | 11.1 → 11.0 | 56.7 → 58.6 | 88.4 → 99.5 |
| 128 | 190.7 → 166.3 | 615.4 → 593.7 | 784.9 → 740.1 |
| 512 | 622.1 → 540.3 | 1485.6 → 1460.4 | 2118.3 → 2438.1 |

Main collection improved in 6/6 pairs at 128 and 5/6 at 512. At 512 total
p50 improved in 3/6 pairs and total p95 rose. Exclusive tail and critical-path
acceptance remain open. Samples, probe patches and receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-receiver-collection/`.

The three-interface-change comparison brackets `72c27df17` with `9c21bf580`
in A/B/B/A order on RTX 3090 Release Vulkan. Each size ran six times per
revision with a five-box control, 30 warm-up frames and 120 measured frames.
Discarding both sides of pairs with an external or unidentified GPU process
in the sampled test window leaves 9/4/5 pairs. Three-cascade results below
are medians of run statistics in µs. Total sums RT Begin and Finish across views.

| Objects | Main collect p50, before → after | Total p50 | Total p95 |
| ---: | ---: | ---: | ---: |
| 5 | 8.9 → 8.8 | 53.6 → 53.8 | 73.3 → 71.3 |
| 128 | 139.2 → 137.1 | 542.8 → 531.1 | 672.1 → 629.8 |
| 512 | 553.6 → 740.2 | 1480.9 → 2026.9 | 4515.1 → 3202.3 |

The inline cases had zero cascade join time. At 512, C1/C2/C3 join p50
was 0.2/0.1/0.1 → 0.2/0.2/0.1 µs and p95 was
0.5/0.6/0.4 → 0.8/79.2/46.7 µs. C2 and C3 p95 rose in all five pairs.
Total p50 rose in three pairs and fell in two. Next isolate the 512-object
C2/C3 tail by comparing dispatch, worker completion and GPUScene consumption
on the same frames before changing scheduling. The clean shadow suite passed.
Raw frames, process samples, exclusions and analysis:
`Build/Qualification/UERenderThreadMechanismAlignment/20261121/stage-2-three-refactors/`.

At `4c65c1919`, a 512-object, three-cascade timeline aligned first worker
dispatch, finalizer body exit, RT wait entry/return and GPUScene collection
entry on one clock per frame. Six of 50 Release Vulkan runs passed the
process-level quiet-window filter (30 warm-up, 120 measured frames each).
With a wait above 40 µs, C2 accounted for 50 frames and C3 for nine, including
two overlapping frames. In every case the finalizer exited after wait entry.
Median wait / time to finalizer exit on those frames was 91.3 / 74.0 µs for
C2 and 83.6 / 62.0 µs for C3.

Against the nearest frame in the same run with both waits at or below 40 µs,
43 of 57 long-wait frames had greater summed RT Begin/Finish time (median
paired difference +227.2 µs, combined wait +91.2 µs). Elapsed time from
receiver Begin to C3 GPUScene entry rose in 40 pairs (median +212.3 µs).
Only seven of 42 frames at or above their run's total-preparation p95 had a
long wait. Retain the GPUScene joins. Next split the C2 dispatch-to-finalizer
interval into queue delay, chunk execution and finalization before changing
task policy. Per-frame timestamps, monitor logs, exclusions and receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-cascade-timeline/`.

At `7d3f7146f`, eight of nine 512-object Release Vulkan runs had quiet GPU
measurement windows. Each used 30 warm-up and 120 measured frames. C2 waited
over 40 µs in 90 of 960 frames. The critical chunk is the last of four
128-object workers to finish. Differences pair each long frame with the nearest
ordinary C2 frame in the same run.

| C2 wall time (µs, median) | Ordinary | Long wait | Paired difference |
| --- | ---: | ---: | ---: |
| Critical chunk launch call to worker entry | 123.3 | 263.2 | +81.1 |
| Critical chunk execution | 65.8 | 144.9 | +28.1 |
| Last worker exit / finalizer submission (whichever is later) to finalizer entry | 148.1 | 339.4 | +65.5 |
| Merge call, including final sorting | 127.5 | 253.8 | +35.9 |

Both entry delays grew in all eight runs by paired median. Combined per pair,
they added 171.3 µs versus 85.5 µs for chunk execution and merge. C2
dispatch-to-completion rose 250.9 µs, RT wait 110.0 µs and total RT mesh
preparation 378.8 µs. The probes include task submission and prerequisite
notification overhead in the entry intervals. Retain scheduling and GPUScene
joins; distinguish scheduler-ready time from worker pickup before attempting
a priority or chunk-size change. Frames, worker timestamps and exclusions:
`Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-c2-phases/`.

At `6d377fc2a`, the same 512-object fixture stamped each C2 task's activation,
last prerequisite notification, queue claim, pool insertion and removal, and
body entry. Eight of nine measurement windows passed the GPU activity filter.
Of 960 measured frames, 78 exceeded the 40 µs C2 wait threshold.

| C2 interval (µs, median) | Ordinary | Long wait | Same-run paired difference |
| --- | ---: | ---: | ---: |
| Last-finishing chunk, insertion to removal | 97.8 | 209.4 | +98.2 |
| Finalizer submission through activation (overlaps chunk work) | 1.4 | 1.3 | −0.1 |
| Last prerequisite closure to final dependency notification | 0.1 | 0.1 | 0.0 |
| Finalizer, insertion to removal | 125.1 | 154.8 | +32.3 |

Combined queue residence added 129.1 µs per pair at the median and increased
in all eight runs. Total RT mesh preparation added 176.8 µs. In 29 long frames
the wait-assist path removed the finalizer; all chunk pickups used the worker
loop. Among the other 49 long frames the combined queue difference was still
+84.1 µs. Retain current dispatch and join points. Next measure pool occupancy
and work ahead of C2 at insertion to distinguish busy workers from wake-up
latency before selecting a scheduler change. Samples, exclusions and receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-c2-queue/`.

At `ad3a93461`, snapshots taken under the pool lock immediately before C2
insertion recorded physical worker assignments and already queued tasks by
view, task name and priority. The 512-object fixture normally initializes two
workers. A second series used the editor startup scheduler default (18 on
this host), with the same workload, probes and 40 µs C2 wait threshold.

| Pool | Quiet runs | Long frames | C2 wait p95 | Total RT mesh preparation p50 / p95 |
| --- | ---: | ---: | ---: | ---: |
| Fixture, 2 workers | 8/10 | 139/960 | 137.9 µs | 1383 / 3639 µs |
| Host default, 18 workers | 8/8 | 0/960 | 0.1 µs | 1344 / 1578 µs |

In the two-worker long frames both workers were occupied at the last-finishing
chunk's insertion in 114/139 frames and at finalizer insertion in 131/139.
The median queue already contained three and four tasks respectively. Every
long frame had C3 chunks ahead of the C2 finalizer. All recorded physical
worker assignments and queue entries at C2 insertion belonged to shadow
preparation. With 18 workers, the corresponding median free capacities were
15 and 17; median insertion-to-removal times were 52.0 and 6.5 µs.
Keep the bounded fixture as a contention case and retain the shared pool.
These sequential series describe this workload; mixed editor/background
workloads need their own evidence before reserving foreground workers.
Snapshots, exclusions, probe patches and receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-c2-occupancy/`.

At `760de3322`, the Release three-cascade fixture used the host default of 18
workers. Six runs per series passed the external GPU activity filter (120 measured
frames per run). At 512 objects the median of run medians was 481 µs for main-view
collection and 195 µs for resolve. Total RT preparation p50/p95 was 1364/1898 µs,
versus 534/622 µs at 128 objects and 54/78 µs in its five-object control.
Separate per-batch probes measured 52 µs building the 512 new batches, 212 µs
validating them and 81 µs publishing them. All cascades reused admission. C1
resolved the shadow pass and C2/C3 each reused all 512 resolved shadow batches.
This preserves the UE 5.8 split between `DrawStaticElements` publication and
view-specific `ComputeLODForMeshes` and `AddCommandsForMesh`.

Skipping redundant first-use transform initialization produced no reproducible
gain. The 512-object main collection medians before, during and after the trial
were 481, 759 and 1029 µs (6/6/3 runs). Two slow restored-baseline runs sampled
only 0–16% host CPU utilization. Core frequency and RT scheduling were unmeasured.
The trial was reverted. The subsequent proxy-owned transform change removes
per-submission inverse builds for ordinary static batches. Its timing impact
and the correlation of slow frames with RT on-CPU time remain open.
Samples, probe patches and receipts:
`Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-default-collection/`.

Release regression coverage verifies zero command-local transform builds for
ordinary static meshes, rejection of a singular transform, recovery to a mirror
and preservation of the earlier snapshot. Static mesh, Water and directional
shadow suites passed. The scene contract suite reproduced the same three
pipeline equivalence failures as the unchanged `e15c314f6` baseline (175/178).
Reports: `Build/Qualification/UERenderThreadMechanismAlignment/20261122/stage-2-proxy-transform/`.
Zero-initializing unused vertex declaration fields resolved those failures.
The Release scene contract suite now passes all 178 cases.
At `e2ef02cee`, the Release static mesh suite (28 cases), scene contract suite
(178 cases) and directional shadow behavior regression passed.

Completion: receipts show reduced RT active work/critical-path cost within Stage 0
budgets, continuous task progress and unchanged scene/resource lifetime behavior.

### Stage 3: Reject occluded work conservatively

Depends on: Stages 0-2 visibility identities and preparation boundaries.

Outcome: occluded scenes reduce downstream CPU preparation as well as GPU work.

- [x] Implement the route selected in Stage 0: GPU HZB/query production plus
  delayed, nonblocking history consumption for CPU admission, with the selected
  capability fallback. Keep history per persistent
  view and identify the producing frame and primitive/instance generation.
  The initial route read back one conservative depth level and tested bounds on
  the CPU. The follow-up based on `23ad86143` reuses immutable projected bounds
  and reads GPU results by batch, producing frame and candidate index. Per-view
  exact input comparison covers material and geometry changes without a scene
  revision bump. A delayed-result regression reverses the candidate order and
  restores it before consuming the old result. Release passed 180 scene contracts,
  28 Vulkan static mesh preparation tests and the indexed occlusion pixel/admission
  regression. The first implementation
  qualified stable, opaque ordinary static meshes. The provisional age limit is two render frames.
  On the Stage 3 changes based on `cf07299dd`, the Vulkan occlusion fixture
  checks main-view draw removal, shadow caster identity, exact pixel parity,
  cuts, expiry, separate view states and occluder replacement. Release correctness
  regressions passed for scene contracts (179), Vulkan static mesh preparation
  (28), both selected directional shadow cases and RoadWeaver viewport qualification.
  The follow-up based on `27c334b1d` accepts transient disocclusion errors
  under the UE camera movement thresholds. Pose changes refresh the GPU bounds.
  Component IDs preserve test indices across traversal changes. Idle views,
  cuts and large camera steps drop previous results. Depth-input changes still
  invalidate the view without consulting the HitProxy revision. Release regression
  covers reordered IDs, unchanged visibility publication, rotation and translation
  boundaries, idle time and time rewind.
  The follow-up based on `066f4f476` passed
  `ConsumesReadyMainViewOcclusionAndKeepsShadowCasters` in
  Win64-Debug-DurinEditor on Vulkan (RTX 3090). It covers a sparse referenced
  material slot, shared geometry capture, and a two-frame feedback result whose
  producing camera has moved beyond the threshold.
  The same preset passed static mesh preparation Vulkan (28), renderer scene
  contracts (180), Sandbox gameplay (15), and RoadWeaver viewport qualification
  (2).
  The follow-up based on `888af4b7a` moved eligibility to scene primitive
  records and uses the scene's occlusion revision for depth publication
  invalidation. Win64-Debug-DurinEditor Vulkan passed the first-frame pixel
  and draw ID comparisons after in-place material, LOD readiness and instance
  updates. The producing primitive handle and world bounds stay equal in
  each case. The same preset passed renderer contracts (180), static mesh
  preparation Vulkan (28), material render proxy (14), renderer scene view
  (32), Water Vulkan (2), Sandbox gameplay (15), and RoadWeaver viewport
  qualification (2).
- [x] Define conservative invalidation for cuts, camera/occluder motion,
  resolution/depth-convention changes, newly added/moved objects, scene/device
  changes and unavailable results. Never hide content on an uncertain result.
  Release regressions on the changes based on `31684738e` cover a cut on a
  rejected submission, camera and primitive motion, additions, target and
  fitted viewport changes, depth convention, scene switches with reused IDs,
  and renderer device invalidation. Each Vulkan case starts with confirmed
  occlusion, then compares first-frame draw IDs and pixels with frustum-only
  rendering. Preparation retries keep the same logical frame to isolate
  invalidation from expiry.
- [x] Apply validated history before expensive collection/material/section work;
  integrate GPU visibility with existing direct/instanced/indirect and fallback
  paths without assuming that an indirect draw eliminates earlier CPU costs.
  On the changes based on `a550a46be`, the Vulkan fixture compares direct,
  CPU-instanced, GPU-indirect and forced fallback submissions against their
  frustum-only images and draw IDs. A fully hidden instance group remains a
  shadow caster, and a group with one exposed instance stays in the main view.
  The material proxy regression checks that inspecting a dirty binding cannot
  trigger resolution and a shader using world-position offset cannot qualify.
  Release correctness passed for 14 material proxy cases, 28 Vulkan mesh
  preparation cases and both selected shadow cases. Scene contracts passed
  179/179 on three consecutive reruns after one uniform-buffer cleanup
  assertion during render-thread shutdown.
- [x] Test disocclusion, fast motion, thin geometry, near-plane cases, multiple
  views, dynamic/Spline instances, origin changes, unavailable/delayed results
  and teardown. Check rendered output and bounded history/feedback memory.
  Release regressions based on `45e38a6ad` compare Vulkan draw IDs and pixels
  with frustum-only rendering during fast camera turns, origin rebasing, near
  clipping, thin occlusion, instance movement and spline deformation. Controlled
  RHI completions cover ring saturation, failure, timely and expired results
  and teardown with a pending copy. Forward and reversed perspective tests
  cover a subpixel bound crossing a tile edge. The mesh-pass fixture now drains
  nested resource cleanup before closing admission. The previously failing
  material publication case passed 100 consecutive repetitions.
- [x] Measure CPU visits and preparation time, GPU cost and visibility latency
  on occluded and low-occlusion controls before enabling an automatic policy.
  On 2026-11-22, `ProfilesOccludedAndExposedMainViews` tested renderer
  `f4c9972a63` with Win64 Release Vulkan on an RTX 3090 (616.64). Both
  1080p views contain 512 single-section quads with a stationary camera. Moving
  one opaque wall behind the grid makes the low-occlusion control. The separate
  eight-pose-frame replay matched the history-off pixels in both scenes.
  Three fresh processes alternated six blocks per scene, reversing the order
  in the second process. Each block warmed 30 frames and recorded 120.

  | Scene | History | Frustum candidates / prepared primitives (median) | Main-view CPU preparation p50 / p95 (µs) | View GPU interval p50 / p95 (µs) |
  | --- | --- | --- | --- | --- |
  | High occlusion | Off | 512 / 512 | 1287 / 1502 | 590 / 655 |
  | High occlusion | On | 512 / 1 | 337 / 1491 | 648 / 660 |
  | Low occlusion | Off | 512 / 512 | 1278 / 1501 | 617 / 632 |
  | Low occlusion | On | 512 / 512 | 1509 / 1709 | 646 / 717 |

  CPU timing ends at draw admission before graph construction. GPU timestamps
  span the main-view graph, including feedback when enabled. In 831/1080
  high-occlusion enabled frames, 511 primitives skipped section preparation.
  The other 249 prepared all 512. Every low-occlusion frame prepared all 512.
  At frame entry, readback readiness first became observable at a median and
  p95 of three frames in both scenes. The renderer checks again during view
  preparation and accepts only results at most two frames old. All three runs
  showed higher low-occlusion CPU and GPU medians with history enabled.
  Reproduce with the named Vulkan test, `DURIN_OCCLUSION_CSV` set to an output
  path and `DURIN_OCCLUSION_REVERSE=1` for reverse order. Per-frame samples
  and GPU engine observations are in `Build/Qualification/Stage3-20261122/stage3-samples.zip`.
  The indexed GPU route repeated the same controls three times on the follow-up
  based on `23ad86143`. Low-occlusion CPU medians were 1328/1416 µs (off/on)
  and GPU medians were 615/646 µs. High-occlusion CPU medians were 1324/174 µs
  and GPU medians were 589/641 µs. Concurrent browser GPU use makes these
  diagnostic only. Samples and engine observations are in
  `Build/Qualification/Stage3-20261122-Bounds/stage3-samples.zip`.

Completion: conservative visibility passes the declared output oracle; no normal
frame waits on feedback; occluded workload CPU work falls within budget and
low-occlusion overhead is accepted against the frozen controls.

### Stage 4: Separate asynchronous recording, native translation and submission

Depends on: Stage 2 immutable task inputs and Stage 0 RHI attribution; Stage 3
supplies visibility integration cases before this stage can close.

Outcome: RT can hand off eligible recording work without immediate per-pass joins,
and independent backend contexts translate eligible command ranges concurrently.

- [ ] Align recording-completion/prerequisite interfaces with UE dispatch:
  transfer owned inputs and completion handles to RDG/RHI rather than borrowing
  stack/scene data until an immediate RT join. Define asynchronous functional
  outcomes and optional statistics publication before migrating callers.
- [ ] Preserve failure/cancellation propagation, deferred cleanup and terminal
  completion on shutdown. Do not relax independent-CPU wait rules to permit RT
  waits on external events, GameThread work or RHI/GPU progress.
- [ ] Introduce capability-gated independent translation contexts and backend
  command-pool/allocator ownership; isolate bindings/descriptors and transient
  allocations. Support inherited render-pass children without splitting clears
  or violating attachment load/store and ordering semantics.
- [ ] Separate translation dependencies from ordered native submission. Preserve
  graphics/compute ownership, PSO readiness, serial/fence meaning, resource and
  command-storage lifetime, frame limits and queue backpressure.
- [ ] Migrate Vulkan and audit Metal consumers. Retain explicit serial/inline
  behavior for unsupported routes and small workloads. An unsupported parallel
  backend must neither silently share a mutable context nor claim native parity.
- [ ] Test out-of-order recording/translation completion with ordered draws,
  translucent continuation, timing/capture scopes, indirect fallback, failed PSOs,
  cancellation, queue pressure, frame retirement and shutdown on changed backends.

Completion: traces demonstrate eligible recording/translation overlap and reduced
critical-path wait; pixel/order/lifetime checks pass; changed backends have native
build/runtime evidence and policy promotion satisfies the frozen budgets.

### Stage 5: Integrate, qualify and publish lasting contracts

Depends on: Stages 1-4 and their applicable receipts.

- [ ] Compare the final implementation with the Stage 0 baseline using identical
  fixtures, cache state, instrumentation and alternating run order. Report RT
  active CPU and waits separately, RHI/GPU costs, median/p95, A/A noise and memory.
- [ ] Verify combined cases: multiple views with distinct occlusion histories,
  dynamic content and cascades, cache pressure plus asynchronous submission,
  resource replacement, failure and shutdown. Qualify the Stage 1 cache default
  and require passing budgets before promoting later automatic policies.
- [ ] Validate owning modules and affected consumers in every project declared
  by `Durin.dworkspace`. Require one final workspace `all` build for the public
  command-list/context layout and build-graph integration risk; reuse applicable
  passing results instead of repeating the gate after documentation-only edits.
- [ ] Complete changed-backend correctness coverage, including native Metal when
  its code/contracts change. Keep unavailable required lanes explicitly open;
  do not substitute Vulkan or reinterpret other plans' outstanding gates.
- [ ] Publish final task, visibility, cache and execution contracts in their
  owning Runtime documents. Record exact revisions, configurations, commands,
  artifacts and remaining limits here; validate documentation and plan lifecycle.

Completion: all required correctness, build and measured performance gates have
applicable evidence. Update lifecycle metadata only then; no timing, backend or
failure gate is silently waived to close the plan.
