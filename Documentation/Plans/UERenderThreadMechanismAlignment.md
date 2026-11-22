# UE Render Thread Mechanism Alignment Plan

Summary: Qualify rendering CPU critical-path improvements and separate asynchronous draw recording, native translation, and ordered submission.

Last reviewed: 2026-11-23
Status: Active
Completed:

## Current Status

Stage 0 needs an exclusive timing lane, windowed attribution, and frozen budgets.
Stages 1–3 have functional delivery. The cache default, RT preparation cost, and
occlusion overhead still need performance acceptance. Stage 4 is the remaining
implementation stage. Stage 5 owns combined qualification and final acceptance.

Implemented contracts:

- [Frame preparation and scene cache ownership](../Runtime/Rendering/Pipeline/RendererFramePreparation.md),
  [retained mesh publication](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md),
  and [directional-shadow caster preparation](../Runtime/Rendering/Features/DirectionalShadows.md#fitting-and-caster-preparation).
- [Persistent view occlusion history](../Runtime/Rendering/Scene/PersistentViewState.md#main-view-occlusion-history)
  and [GPUScene storage](../Runtime/Rendering/Scene/GPUScene.md).
- [CPU task dependencies and wait policy](../Runtime/Core/TaskSystem.md#dependencies-waiting-and-cpu-policy),
  [RDG execution](../Runtime/Rendering/Pipeline/RenderGraph.md), and
  [RHI command lists, submission and completion](../Runtime/Rendering/RHI/RHICommandExecution.md).

## Dependencies and scope

- [UE-Aligned GPU Scene](UEAlignedGPUScene.md) owns persistent scene data,
  command identity, grouping, and its qualification gates.
- [Shadow Preparation Optimization](ShadowPreparationOptimization.md) owns
  receiver/cascade reuse and projection. Use its fixtures and shared results.
- [RHI Creation Contract Tightening](RHICreationContractTightening.md) owns
  PSO readiness and terminal submission outcomes.
- Preserve direct, CPU-instanced, indirect, and serial/inline routes.
  GPU async compute, multiple physical GPU queues, Nanite, bindless rendering,
  shadow-map content caching, and presentation policy are outside this plan.
- Reconfirm local UE version and symbols before Stage 4. The working reference
  was `D:/Programs/UE_5.8`, UE 5.8.2 CL 56702186. Compare
  `FParallelMeshDrawCommandPass::DispatchDraw`, `FParallelCommandListSet::Dispatch`,
  the RHI Dispatch/Translate/Submit pipes, and the Vulkan and Metal
  `RHIGetParallelCommandContext` implementations.

## Implementation Stages

### Stage 0: Establish attribution, UE contracts and acceptance budgets

Depends on the recording prerequisite `345a2fc30`. Use the
[rendering baseline protocol](../Development/Build/RenderingPerformanceBaseline.md).

- [ ] Freeze workloads and camera sequences. Use the five-box and high-overlap
  shadow controls, and add a small no-shadow control. Use disjoint A/B views
  exceeding the old 512-entry working set, delayed revisits, unique
  sections and materials, repeated instances with instancing on/off,
  dynamic/Spline updates, and high/low occlusion with motion and cuts.
- [ ] Record quiet Release timing with Tracy off and separate diagnostic traces.
  Add windowed end-to-end attribution for GT/RT active CPU and waits, worker
  scheduling/joins, RHI pressure, native translation/submission, GPU
  retirement, acquire and Present. The existing offscreen receipt supplies
  attribution for one fixture.
- [ ] Capture frame median/p95 and A/A noise alongside cache churn,
  primitive/section visits, task occupancy and joins, allocation and upload
  bytes, native draws, translation time, and peak/retained memory.
- [ ] Freeze workload-specific latency, p95, and memory budgets. The primary
  lane is Windows Vulkan on RTX 3090 (recorded driver 616.64, device API
  1.4.351, loader 1.4.357). Cover threaded and inline Vulkan correctness,
  and native Metal correctness when its backend code or execution contracts
  change. Keep raw samples retrievable.

### Stage 1: Make preparation-cache retention stable across views

- [ ] Qualify the shipped default against Stage 0 timing and memory budgets in
  Stage 5. Compare the maintained disjoint-view cache control and candidate with
  matching timing settings, and run the memory census separately.

### Stage 2: Build a dependency-driven preparation pipeline

- [ ] Qualify RT active CPU and critical-path cost against Stage 0 budgets.
  Include the three-cascade contention fixture and the proxy-transform
  follow-up. Keep existing GPUScene joins and the shared pool while measuring
  worker pickup, RT on-CPU time, and slow-frame frequency. Measure mixed
  editor/background workloads before considering foreground-worker reservation.

### Stage 3: Reject occluded work conservatively

- [ ] Qualify the current previous-render feedback policy on the frozen
  high/low occlusion controls in an exclusive GPU window. Include readback wait
  (`Renderer.Occlusion.WaitForFeedback`), CPU visits and preparation, GPU cost,
  visibility latency, and the frustum-only pixel/draw-ID oracle. Previous
  timing receipts predate this wait policy.

The GBuffer feedback dispatch and result publication follow
[the persistent-view contract](../Runtime/Rendering/Scene/PersistentViewState.md#main-view-occlusion-history)
and [RHI readback completion](../Runtime/Rendering/RHI/RHICommandExecution.md#flush-and-synchronous-operations).

### Stage 4: Separate asynchronous recording, native translation and submission

Depends on Stage 2 immutable inputs and Stage 0 RHI attribution. Use the Stage 3
visibility integration cases before closing this stage.

- [ ] Define asynchronous recording results and optional statistics publication.
  Carry owned inputs and completion prerequisites from Renderer to RDG/RHI so
  eligible passes can hand off recording without an immediate RT join.
  Preserve synchronous observations where callers require them.
- [ ] Preserve terminal failure and cancellation, shutdown completion, deferred
  cleanup, and independent-CPU wait rules.
- [ ] Add capability-gated Vulkan secondary command buffers with worker-owned
  command pools, descriptors and temporary allocations. Keep render-pass
  inheritance, attachment operations, and execution order on the parent path.
- [ ] Separate translation dependencies from ordered native submission.
  Preserve graphics/compute ownership, PSO readiness, fence/serial semantics,
  resource and command-storage lifetime, frame limits, and backpressure.
- [ ] Audit Metal and retain serial execution until independent encoder state
  and native qualification are available. Keep serial/inline fallbacks for
  unsupported capabilities and small workloads.
- [ ] Test out-of-order completion, draw order, translucent continuation,
  timing/capture scopes, indirect fallback, failed PSOs, cancellation, queue
  pressure, retirement, and shutdown. Demonstrate recording/translation overlap
  and reduced critical-path wait in traces. Qualify changed backends and apply
  the frozen budgets before promoting automatic policies.

### Stage 5: Integrate, qualify and publish lasting contracts

Depends on Stages 0–4 and their applicable receipts.

- [ ] Compare final and baseline workloads with matching fixtures, cache state,
  instrumentation, and alternating run order. Report RT active CPU and waits,
  RHI/GPU costs, median/p95, A/A noise, and memory.
- [ ] Exercise combined multi-view histories, dynamic content and cascades,
  cache pressure with asynchronous submission, replacement, failure, and
  shutdown. Close the cache-default and occlusion performance gates.
- [ ] Validate owners and consumers across `Durin.dworkspace`. Require one final
  workspace `all` build covering the final command-list/context layout and
  build graph. Reuse an applicable build through documentation-only changes.
- [ ] Complete changed-backend correctness, including native Metal where its
  code or contracts change. Keep unavailable required lanes open.
- [ ] Publish changed contracts in their Runtime owners and record exact
  revisions, selections, settings, artifacts, and remaining limits here.
  Complete the plan once all required gates have applicable evidence.

## Validation records

Reuse correctness only where relevant inputs and environment still match the
[test workflow](../Agents/Testing.md). Historical timing locators below were
recorded in the plan at `de335e8bf`. The dated artifacts are absent in this
checkout. Retrieve them or rerun the pinned inputs before using their numbers
for a gate.

### Build and correctness

| Tested inputs | Passed coverage | Reuse limit |
| --- | --- | --- |
| `345a2fc30` main-pass recording prerequisite, Win64-Debug-DurinEditor | Workspace `all`, 155 RHI tests, 177 renderer contracts, 256 Core concurrency tests, two forced-parallel Vulkan pixel/culling regressions | Prerequisite only. |
| Stage 1 at `aa9c239ad` plus cache changes, Win64-Release-DurinEditor, RTX 3090 Vulkan | Disjoint-view cache qualification, four renderer contracts and three focused Vulkan cases for invalidation, retained state and resource replacement | Functional cache evidence. The same run's timings used the cache census on a shared host. |
| Stage 2 at `2934ba6e3` plus tests, then `e2ef02cee`, Win64-Release-DurinEditor, RTX 3090 Vulkan | `SchedulerShutdownDrainsAdmittedMeshPreparation` and `QualifiesIndependentMultiBatchGeometryAndFactory` cover accepted work and source teardown. The later run passed 28 static-mesh cases, 178 renderer scene contracts and directional-shadow behavior regression. | Functional and lifetime evidence. |
| Stage 3 previous-render wait, follow-up based on `067f2c9c5`, Win64-Release-DurinEditor, RTX 3090 Vulkan | 163 RHI command-list contracts, 180 renderer scene contracts, 28 Vulkan static-mesh cases, 15 Sandbox tests and 2 RoadWeaver viewport tests. Pending-copy, frame-gap, first-frame pixel and draw-ID regression passed. | The Release workspace build hit MSVC `C1083` on three generated ContentBrowserAssetDataSource object paths. Metal and exclusive timing remain open. |
| `249ffd425` render-thread teardown, Win64-Debug-DurinEditor | Workspace `all`, 651 focused native tests, bounded editor startup/exit with threaded and inline RHI, and RoadWeaver viewport and material qualification target builds | Teardown/build evidence for this revision. Stage 5 requires coverage of the final execution changes. |

The `249ffd425` test selections were `RenderContractTests`,
`RendererSceneContractTests`, `MaterialRuntimeTests`, `MaterialCookTests`,
`EditorRenderingTests`, `StaticMeshThumbnailTests`, and `SandboxGameplayTests`.
The startup case was `BoundedTickExitUsesNormalApplicationShutdown` in
`LaunchStartupCharacterizationTests`. The workspace build log is
`Build/.agent-state/logs/20261122-235443-334756-28316-cmake.log`, with the
two startup logs under the same directory at `20261122-235505-910487-23928-ctest.log`
and `20261122-235524-643972-34980-ctest.log`.

### Performance controls and diagnostics

On 2026-11-23, windowed Sandbox `/Game/Levels/GrayboxStage15` reproduced the
reported difference at the saved viewport camera on RTX 3090 Vulkan. The
`0b7130de1` stock Debug/Tracy pair measured on/off frame-interval medians
of 9.469/6.252 ms. An intermediate capture with early dispatch and polling
showed 3.625 ms median from result publication to RT return. After making the
wait follow publication, that span was 0.040 ms, and the wait itself went
from 5.663/8.437 to 2.221/4.581 ms p50/p95 on waiting frames. Neither
steady-state trace saw a native fence wait inside the readback fallback.
The final uncapped toggle pair measured 16.798/21.402 ms on and
14.036/17.296 ms off for frame interval p50/p95. These were Debug diagnostics
with uncontrolled focus and host load. Repeat in a fixed-focus exclusive
Release lane for end-to-end acceptance. Traces and sampling/analysis scripts
are under `Build/Diagnostics/OcclusionGraybox/`. Use `before-{on,off}.tracy`,
`isolated-after-on.tracy`, and `final-{on,off}.tracy` from this checkout.

The 2026-11-19 baseline and cache runs used `Win64-Release-DurinEditor`,
MSVC 14.44.35207, RTX 3090, Tracy off, and Vulkan validation off. The offscreen
and cache comparison runs used threaded RHI. Retain work with
`DURIN_TEST_KEEP_WORK=1` and run
`.DevTool.bat test StaticMeshRenderPreparationQualificationTests --mode qualification --preset Win64-Release-DurinEditor --test-jobs 1 --report`.

| Control | Reproduction and recorded result |
| --- | --- |
| Mixed geometry, `ffe2f169a` plus sample retention | 64 primitives and 256 draws, three runs of 30 warm-up and 120 measured samples. Preparation call median 255.55–259.05 µs, p95 293.40–327.60 µs. Historical reports and samples under `Build/Qualification/UERenderThreadMechanismAlignment/20261119/`. |
| Offscreen A/B attribution, probe revision `2956bdd8f` | `RecordsDisjointViewCacheChurnBaseline`, `DURIN_TEST_DISJOINT_VIEW_ATTRIBUTION=0` for timing and `1` for instrumentation. Alternating RT medians 16.30–17.22 ms, p95 17.88–18.15 ms. Draw-recording join 4.49–4.62 ms in the detailed lane. Reports under `20261119/frame-attribution/` in the same root. |
| Maintained cache control and Stage 1 candidate | Use the same case with `DURIN_TEST_DISJOINT_VIEW_MEMORY=0` for timing and `1` for census. Control rebuilt 1,024 descriptions per alternating frame. Candidate reused all 1,024 across 360 alternating and six revisit frames. Accounted retention peaked at 2,299,904 bytes vs. 1,162,240 for the control. Timing-only control A/A p95 ranged 19.74–36.23 ms. Reports under `20261119/cache-retention/` and `20261119/stage-1/`. |
| Stage 2 three-cascade attribution | Use `DirectionalShadowBaselineVulkanTests` with the five-box, 128 and 512-object controls. The retained probes use `DURIN_PREP_SAMPLE_FILE` and the receipt's `timing.patch`. At `6d377fc2a`, combined C2 queue residence added 129.1 µs on paired slow frames. At `ad3a93461` there was spare worker capacity. At `760de3322`, total RT preparation p50/p95 was 1364/1898 µs at 512 objects. Historical samples and exclusions are in `20261121/stage-2/` and `20261122/stage-2-c2-queue/`, `20261122/stage-2-c2-occupancy/`, and `20261122/stage-2-default-collection/` in the same root. |
| Stage 3 high/low occlusion, `f4c9972a63` and indexed follow-up based on `23ad86143` | `ProfilesOccludedAndExposedMainViews`, Win64 Release Vulkan on RTX 3090 (616.64). Two 1080p scenes of 512 quads, 30 warm-up and 120 measured frames per block. Use `DURIN_OCCLUSION_CSV` and `DURIN_OCCLUSION_REVERSE=1` for reverse order. Indexed follow-up CPU medians off/on were 1324/174 µs high and 1328/1416 µs low, with concurrent browser GPU activity. Historical samples in `Build/Qualification/Stage3-20261122/stage3-samples.zip` and `Build/Qualification/Stage3-20261122-Bounds/stage3-samples.zip`. |
