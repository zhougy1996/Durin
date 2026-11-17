# UE Render Thread Mechanism Alignment Plan

Summary: Reduce rendering CPU critical paths through persistent preparation caches, dependency-driven preparation, conservative visibility and UE-style asynchronous command dispatch and parallel native translation.

Last reviewed: 2026-11-17

Status: Active
Completed:

## Current Status

This plan consolidates the four remaining findings from the local UE comparison:
serial preparation and delayed task replenishment, preparation-cache churn across
views, missing occlusion rejection, and single-threaded native RHI translation.
Implementation has not started. Stage 0 is the next action.

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

The local comparison reference is UE 5.8.0, CL 53629095, branch
`++UE5+Release-5.8`, installed at `E:/Programs/Epic Games/UE_5.8`.
Reconfirm its version and record exact source symbols when each stage starts;
other plans' UE 5.8.2/5.8.3 references are separate evidence.

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

## Implementation Stages

### Stage 0: Establish attribution, UE contracts and acceptance budgets

Depends on: prerequisite `345a2fc30`; no additional runtime optimization.

Outcome: a reproducible baseline and a fixed contract/ownership inventory for
the four remaining mechanisms.

- [ ] Reconcile current source and overlapping plans; map every proposed change
  to its owner, consumers and exact local UE symbols. Record deliberate
  adaptations, including current synchronous recording observations.
- [ ] Capture quiet Release, Tracy-off timing baselines with separate diagnostic
  traces. Separate GT/RT active CPU, scheduling/join time, RHI backpressure,
  translation/submission, GPU completion and Present/acquire waits.
- [ ] Freeze workloads for small scenes; large unique sections/materials;
  repeated instances with dynamic instancing on/off; two disjoint views each
  exceeding the old 512-entry working set; camera revisits; dynamic/Spline
  providers; shadow-heavy views; and occluded interior/exterior camera paths.
- [ ] Record cache hits/misses/evictions, visited primitives/sections, task
  occupancy and joins, allocation/upload bytes, native draws, translation time
  and retained/peak memory alongside frame median/p95.
- [ ] Fix workload-specific latency, p95 and memory acceptance budgets before
  changing runtime policy, using A/A noise and the qualified device. Name the
  primary Vulkan device and required backend correctness lanes.
- [ ] Resolve the exact UE-referenced occlusion route and conservative history
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
