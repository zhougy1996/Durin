# UE Render Thread Mechanism Alignment Plan

Summary: Reduce rendering CPU critical paths through persistent preparation caches, dependency-driven preparation, conservative visibility and UE-style asynchronous command dispatch and parallel native translation.

Last reviewed: 2026-11-19

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
Stage 1. The maintained fixture exercises the existing production cache. The
qualified GPU lane is currently unavailable. Stage 1 implementation depends on
the applicable baseline and acceptance budgets.

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
| Material proxy or accepted program | `FStaticMeshSceneProxy::UpdateMaterialBinding_RenderThread` clears its static batch publication. `CaptureStaticMeshBatches_RenderThread` compares geometry and accepted material record IDs, including fallback when needed. `FScene::UpdateStaticDrawListsForMaterials` clears the corresponding native draw ownership. Stage 1 must reject old preparation publications and bound their retained owners separately from that storage. |
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
budgets open. The cache retention policy has yet to be implemented. Samples,
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
- [ ] Fix workload-specific latency, p95 and memory acceptance budgets before
  changing runtime policy, using A/A noise and the qualified device. Name the
  primary Vulkan device and required backend correctness lanes.
- [x] Resolve the exact UE-referenced occlusion route and conservative history
  validity oracle, and the Vulkan/Metal translation-context capabilities, before
  implementation. Record the chosen interfaces and unsupported-route fallbacks.

Completion: baseline artifacts are retrievable; attribution identifies work
versus waits; ownership, workloads and numeric acceptance budgets are recorded.

### Stage 1: Make preparation-cache retention stable across views

Depends on: Stage 0 cache baseline and invalidation inventory.

Outcome: alternating views and camera revisits reuse unchanged facts without
unbounded retention or stale resource selection.

- [ ] Replace service-wide per-submission used-bit reset/trim with scene-aware
  frame access stamps and a bounded retention/eviction policy. Account for all
  retained fact, section and draw-description tables and their strong owners.
- [ ] Preserve dependency-based invalidation for material/layout/geometry,
  shader/device/manual generation, primitive removal and scene teardown.
- [ ] Test alternating disjoint large views, revisits, scene changes, structural
  mutation, resource replacement and teardown; retain dynamic-provider checks.
- [ ] Compare warm-up/steady-state misses, evictions, RT time and retained bytes
  against Stage 0, including explicit capacity pressure.

Completion: below-budget alternating views stop mutually evicting their working
sets; true invalidations still rebuild; pressure remains bounded and measured.

### Stage 2: Build a dependency-driven preparation pipeline

Depends on: Stages 0 and 1; immutable-input and cache-ownership contracts fixed.

Outcome: useful preparation work overlaps without serial collection/resolve
chains or rendering-thread-driven task replenishment dominating the critical path.

- [ ] Split visibility input capture, classification, LOD/relevance, section
  preparation, material/pass facts and sorting/grouping into explicit ownership
  and dependency boundaries. Keep mutable caches and publication on their owner.
- [ ] Remove fixed-wave replenishment that waits for RT result consumption;
  schedule all bounded ranges or use worker-driven range acquisition. Preserve
  deterministic assembly, cancellation/failure handling and bounded task storage.
- [ ] Reuse captured scene/material/geometry facts across receiver, views and
  cascades only where identities and generations permit; retain per-view LOD,
  winding, bias, bindings and dynamic validation where required.
- [ ] Move measured eligible collection/resolve/finalization work to immutable
  worker inputs; avoid merely parallelizing cheap classification while serial
  traversal, allocation and sorting remain the dominant work.
- [ ] Validate serial/task output parity, changing inputs, scheduler shutdown,
  task failure, source teardown and large workloads spanning many former waves.

Completion: receipts show reduced RT active work/critical-path cost within Stage 0
budgets, continuous task progress and unchanged scene/resource lifetime behavior.

### Stage 3: Reject occluded work conservatively

Depends on: Stages 0-2 visibility identities and preparation boundaries.

Outcome: occluded scenes reduce downstream CPU preparation as well as GPU work.

- [ ] Implement the route selected in Stage 0: GPU HZB/query production plus
  delayed, nonblocking history consumption for CPU admission, with the selected
  capability fallback. Keep history per persistent
  view and identify the producing frame and primitive/instance generation.
- [ ] Define conservative invalidation for cuts, camera/occluder motion,
  resolution/depth-convention changes, newly added/moved objects, scene/device
  changes and unavailable results. Never hide content on an uncertain result.
- [ ] Apply validated history before expensive collection/material/section work;
  integrate GPU visibility with existing direct/instanced/indirect and fallback
  paths without assuming that an indirect draw eliminates earlier CPU costs.
- [ ] Test disocclusion, fast motion, thin geometry, near-plane cases, multiple
  views, dynamic/Spline instances, origin changes, unavailable/delayed results
  and teardown. Check rendered output and bounded history/feedback memory.
- [ ] Measure CPU visits and preparation time, GPU cost and visibility latency
  on occluded and low-occlusion controls before enabling an automatic policy.

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
  resource replacement, failure and shutdown. Promote defaults only after their
  acceptance budgets pass; correctness or Debug receipts do not prove speedup.
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
