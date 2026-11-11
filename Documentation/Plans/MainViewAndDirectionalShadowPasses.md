# Main View and Directional Shadow Pass Preparation Plan

Summary: Give view demand, mesh pass membership and draw readiness distinct owners, starting with main-view geometry and directional shadow cascades.

Last reviewed: 2026-11-11

Status: Completed
Completed: 2026-11-11

## Current Status

Both stages passed on 2026-11-11. The existing
[shadow preparation optimization plan](ShadowPreparationOptimization.md) owns
collection cost, cache reuse and cascade LOD optimization. This plan owns demand
and admission semantics. Reuse its command-local caches and qualified fixtures.
If the new flow changes a measured workload, record the changed inputs before
reusing performance evidence.

### Stage 1 implementation and evidence

The tested revision is based on `5b8706fe3`, with Git tree
`2e05e6a56922b0d06af2227e48a6cc6cf5b66b7e` for `Engine/` including tests.
The preset was `Win64-Debug-DurinEditor`, using threaded Vulkan on a GeForce GTX
1060 6GB. The Renderer-private requirements builder runs on the fitted view after
version and wireframe capability validation. Its deferred and shadow purpose
decisions feed resource preparation and graph planning. The base route always
collects main geometry. Main-view and per-cascade admission finalize existing
command-local buckets in place. Logical preparation counts, primitive records
and resolved indices survive filtering; RDG consumes the finalized buckets.
This avoids maintaining a second set of draw arrays.

| Acceptance case | Observed result |
| --- | --- |
| Ordinary Unlit | A visible receiver drew with zero shadow selections, scene traversals, cascades, target bytes and successful shadow draws. The same scene with an explicit diagnostic prepared a valid map and issued two shadow draws. Unlit output pixels matched. |
| Consumers and disabled light | Lit/Solid produced the qualified map. Lit/Wireframe used the forward pass, drew geometry and traversed casters once. Deferred qualification remained available on Unlit. Disabling light shadow casting left the receiver image available with zero caster traversals and shadow draws. |
| Local admission and recovery | ID 10001 participated in the receiver and at least two cascades. Targeted pending removed exactly its draws from one cascade. Every other admitted ID list and the GBuffer successful draw count matched the ready frame. Receiver-only pending preserved every cascade. The CPU qualification withheld opaque, masked and translucent batches of the same receiver component together. Clearing the fault restored the original lists and successful shadow count. |
| Camera-external caster | In both captures only ID 1 drew in the camera. Adding ID 2 outside the camera added it to the shadow lists. Successful shadow draws changed from 1 to 2 for SingleMap and 2 to 4 across ThreeCascades. At 257 脳 257, respectively 1,494 and 1,581 receiver-image pixels changed by more than two RGB levels. |
| Strict and interactive readiness | Under one shadow fault the strict request reported Pending while interactive rendering kept its receiver. Unlit strict preparation completed under the same fault. Clearing it let the original lit handle reach Ready. The multi-batch fixture's terminal shadow resource failure returned RendererResourcesUnavailable from deterministic preparation. |
| Mixed geometry and indirect indexing | Directional qualification produced 109 capture entries and passed the masked, motion, cascade and recording-parity comparisons. Multi-batch and spline pixel checks passed. The stable custom-data fixture compared 64 direct draws with one indirect draw. In the scene route fixture a withheld middle component left 64 compatible peers, indirect drawing executed and its output matched the direct path byte for byte. |

The DevTool receipts under `Build/.agent-state/logs/` are
`20261111-113647-980906-47068-RendererSceneContractTests.log` (135 cases),
`20261111-113833-752399-46800-VolumetricCloudSceneVulkanTests.log` (scene routes),
`20261111-113712-660953-45728-ctest.log` (directional qualification filtered to
`ValidatesShadowBehaviorAndSubTexelMotion`) and
`20261111-113408-019268-8940-StaticMeshRenderPreparationVulkanTests.log`
(multi-batch, spline and custom-data grouping). The retained images and
`baseline-metrics.json` are under
`Build/AgentValidation/MainViewAndDirectionalShadowPasses/Stage1/DirectionalShadowQ0Baseline/`.
The baseline fixture owns shader-mount startup and pumps strict preparation before
reading each target. This keeps asynchronous cold creation out of the pixel comparisons.

### Stage 2 implementation and evidence

The tested `Engine/` tree is `83ff255184505ba0b38b6e8416957ea943292307`.
The fitted main-view requirements resolve one frustum. CPU primitive classification
and GBuffer instance compaction consume its planes. The existing CPU collection
boundary and the default-disabled GPU grouping option are retained. Physical
submission counters now advance on successful commands. The previous subtraction
from logical successes incorrectly reported 63 direct fallbacks for a single
64-instance indirect draw. See the [GBuffer contract](../Runtime/Rendering/Pipeline/GBuffer.md#compatible-instance-culling).

One masked primitive contained 64 compatible single-section instances. Moving
32 of them outside left its aggregate bounds visible to the CPU. Each direct
command drew one instance. Indirect instance counts came from the completed
indexed argument buffer. Every grouped case issued one indirect command and
zero direct fallbacks. Final color, four GBuffer attachments and D32 matched
the corresponding direct result byte for byte.

| Frustum | Outside | Direct draws / instances | Indirect draws / instances | GPU rejections |
| --- | ---: | ---: | ---: | ---: |
| Disabled | 0 | 64 / 64 | 1 / 64 | 0 |
| Disabled | 32 | 64 / 64 | 1 / 64 | 0 |
| Enabled | 0 | 64 / 64 | 1 / 64 | 0 |
| Enabled | 32 | 64 / 64 | 1 / 32 | 32 |

GPU timestamp medians in microseconds, with frustum culling enabled:

| Outside | Direct GBuffer | Grouped GBuffer | Compaction | Grouped GBuffer + compaction (paired sum) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 21.312 | 21.248 | 9.056 | 30.112 |
| 32 | 19.840 | 19.968 | 9.504 | 29.248 |

The all-visible case isolates merging: it saved 63 submissions (98.4%) with
zero GPU rejections. In both layouts the compaction interval outweighed the
raster savings, if any. These diagnostic intervals exclude CPU preparation,
uploads and other GPU passes. The active display used the same GTX 1060 6GB
(driver 582.66). Results apply to this 192×192 workload and do not establish
a frame-time gain or a new grouping threshold.

`Win64-Debug-DurinEditor`, threaded Vulkan, passed the following:

- `RendererSceneContractTests`: 135/135, including disabled/invalid frustum
  fallback, parallel classification, group admission and translated bounds.
- `VolumetricCloudSceneVulkanTests`: direct/indirect attachment parity
  with zero, partial and full rejection, two groups, boundary contact,
  a world shift of 1e12, resize, pool reuse and direct fallback. The
  two-group all-outside case read back zero visible and 128 culled.
- `StaticMeshRenderPreparationVulkanTests` case
  `FMaterialCustomDataVulkanTests.StableInstancesMatchDynamicMaterialsAcrossPassesAndGPUGrouping`:
  the 64-instance comparison above. Set `DURIN_PROFILE_MAIN_VIEW_CULLING=1`
  to collect 12 direct/grouped pairs per enabled layout in alternating order.
  The table excludes the first two pairs of each layout and uses the other ten.
  Raw samples (`gpu-timestamps.csv`) and all three passing logs are retained in
  `Build/AgentValidation/MainViewAndDirectionalShadowPasses/Stage2/`.

The standalone Metal shader fixture was migrated to the new constant-buffer layout.
Native Metal execution requires a macOS host and was not run for this Windows acceptance.

## Goal

An ordinary unlit view spends no work on directional shadow casters. A lit view
can display ready geometry while one shadow draw is pending. A caster outside
the camera frustum can still shadow a visible receiver. Each outcome should
follow from an explicit input to preparation and be observable before RDG execution.

## Responsibilities and interface changes

The UE references below are relative to the installed UE 5.8
`Engine/Source/Runtime` directory. Proposed Durin types are private to Renderer
in Stage 1. The existing single-view request is sufficient for this slice.

| Decision | UE 5.8 reference | Durin owner and proposed interface |
| --- | --- | --- |
| Which work does the view request? | `Engine/Public/SceneView.h`, `FSceneViewFamily::EngineShowFlags` | `FSceneRenderPipeline` derives a command-local `FSceneViewRequirements` from the fitted view, render options, preparation config and qualification policy. Compute it before primitive collection. Record main geometry and directional shadow purposes using the existing production/debug/qualification/dependency vocabulary. Both prepare-only and interactive submissions use this function. |
| Which work is possible for this scene? | `Renderer/Private/ShadowSetup.cpp`, per-shadow subject gathering | `PrepareView_RenderThread` consumes the requirements. Select the directional light and validate the shadow view before gathering subjects. The existing `BuildSceneFrameFeaturePlan` consumes requirements plus prepared scene facts to finalize the graph route. Keep light eligibility, map availability and requested purpose distinguishable. |
| Which primitives and sections belong to a pass? | `Engine/Public/PrimitiveSceneProxy.h`, `GetViewRelevance`; `PrimitiveViewRelevance.h`, draw and shadow relevance; `Renderer/Private/SceneVisibility.cpp`, `SetupMeshPasses` | Engine publishes stable `FPrimitiveRenderParticipation` through the proxy. Renderer computes view-dependent membership. Main collection uses camera visibility; `PrepareDirectionalShadowCasterTable` uses independent cascade volumes. Replace boolean shadow selectors at the collector boundary with the named receiver/shadow collection purpose already used by `FMeshBatchCollector`. Apply section/material and vertex-factory eligibility for the actual mesh pass. A primitive may belong to both lists. |
| Which eligible draws can execute now? | `Renderer/Public/MeshPassProcessor.h`, `FMeshPassProcessor::AddMeshBatch` and mesh draw commands | Existing Durin pass processors build or reuse commands. Resource resolution owns ready/pending/failed outcomes. Replace the cross-view union in `RemoveUnreadyStaticMeshPrimitives` with a renderer-private operation that takes **one** prepared collection and its resolved resources and returns an admitted list of draw references/indices. Invoke once for the receiver and once per enabled cascade. Preserve stable resolved indices, resource owners and command lifetimes through graph execution. |

Use the admitted receiver list for all main-view geometry consumers, including
GBuffer, forward and translucent draws and GPU grouping when active. Keep
component sections atomic within that receiver list. Within one cascade, a
failed section withholds that primitive's sections from that cascade. Separate
cascades may admit the same primitive independently. A shared missing mesh or
material dependency can legitimately fail in several lists. Keep logical
membership available for telemetry and strict preparation checks.

## Durin contract decisions

- **Change interactive failure propagation.** Today
  `StaticMeshRenderPreparation.cpp::RemoveUnreadyStaticMeshPrimitives` unions
  logical resource failures and unresolved draws across receiver and cascades.
  The `PendingShadowPrimitive` case in
  `Engine/Tests/Native/EngineTests/Private/SceneRenderQualification.h`
  explicitly expects fewer successful GBuffer draws. Replace that expectation
  with receiver equality and a positive assertion about the missing caster.
  Conversely, receiver-only failure affects the receiver group; an independently
  ready shadow caster remains eligible. A missing whole shadow target or pipeline
  uses the existing unshadowed lighting fallback.
- **Keep exact preparation strict.** A prepare-only request covers every
  requested, eligible receiver/cascade dependency. Pending yields and terminal
  failures remain observable to the caller. Ordinary rendering can advance with
  locally admitted draws. Preserve capture waiting, budgeted retries, stale
  invalidation, and existing view-history/output commit boundaries.
  See [load preparation](../Runtime/Rendering/Pipeline/LoadPreparation.md).
- **Keep selection and geometry semantics.** Retain current light selection,
  SingleMap/ThreeCascades, conservative invalid-bounds handling, StaticMesh and
  SplineMesh eligibility, per-cascade LOD and bias, retained batch invalidation
  and dynamic fallback. Preserve existing feature dependencies for contact
  shadows and the separate volumetric-cloud shadow path.
  See [directional shadows](../Runtime/Rendering/Features/DirectionalShadows.md)
  and [retained batch publication](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md).
- **Set scope at the consumer.** Production shadow demand follows a shading
  route that samples directional shadows, including applicable forward routes.
  An explicit shadow diagnostic or qualification that reads a map also requests
  it. The default diagnostic enum value `Lit` alone does not request work.
  A missing eligible light or invalid shadow view yields zero caster collections.
  Preserve output and diagnostic pass names where tooling depends on them;
  distinguish an empty completion marker from actual shadow depth work.

## Implementation Stages

### Stage 1: Main view and directional shadow vertical slice

Starts from the current single-view pipeline. Deliver demand, membership,
admission and execution together in this stage.

- [x] Add and exercise a pure requirements builder. Cover Lit/Solid,
  Lit/Wireframe with its actual forward consumer, ordinary Unlit, explicit
  shadow diagnostics and qualification overrides. Pass its result to logical
  preparation and feature planning. Gate caster table construction, cascade mesh
  preparation, shadow-only resource requests and depth recording on effective demand.
- [x] Feed the receiver and individual cascade collections from their own
  visibility and relevance inputs. Migrate boolean selectors and all consumers
  touched by the interface change. Reuse the existing pass manager, static and
  dynamic command storage, and command-local section cache.
- [x] Produce per-collection admitted lists after resolution. Ensure RDG
  recording and indirect candidate construction consume those lists. Count
  logical members, withheld primitives and submitted draws separately for the
  receiver and each cascade. Keep prepare-only results independent of interactive
  omission. Extend the existing fault injection to target one cascade.
- [x] Run the acceptance matrix below. Record the tested revision, fixture,
  backend, selected tests, and graph/counter or readback evidence in Current
  Status. Update the [frame preparation](../Runtime/Rendering/Pipeline/RendererFramePreparation.md),
  directional shadow and load preparation contracts alongside the implementation.

| Stage 1 acceptance case | Required observation |
| --- | --- |
| Ordinary Unlit, valid light and visible casters | Main geometry draws. Caster discovery invocations, cascade mesh preparation, shadow-only resource requests and shadow depth draws are zero. Check invocations or counters even when RDG omits an unused pass. |
| Lit, explicit shadow diagnostic, no light | With an eligible light, production and diagnostic consumers each produce their expected map. With the light removed or shadow casting disabled, caster work is zero and the ordinary lit receiver still renders. |
| One primitive shared by receiver and two cascades; inject pending in one cascade | Receiver draw IDs and GBuffer success count match the ready frame. Only the targeted cascade drops that primitive. Clearing the fault restores its draw. Inject receiver-only pending separately and verify the inverse isolation plus receiver section atomicity. |
| Camera-external caster, visible receiver | Capture with and without the caster. Its ID is absent from camera draws, present in applicable cascade draws, and the receiver shadow changes in readback. Exercise SingleMap and ThreeCascades. |
| Prepare-only and interactive submissions with identical inputs | Required pending work prevents Ready for capture; interactive output contains admitted draws. Retry reaches Ready after resolution. A terminal required failure stays observable. Unrequested shadow work never blocks an ordinary Unlit capture. |
| Ready baseline and mixed geometry | Compare lit receiver and shadow readbacks against qualified directional-shadow fixtures; include masked, spline and multi-section casters, a forward receiver and a deferred receiver. Exercise the GPU-grouped receiver if enabled to catch stale indices after filtering. |

Follow the repository [build](../Agents/BuildAndRun.md) and
[test selection](../Agents/Testing.md) workflows. Validate Renderer and affected
EngineTests consumers, including the Vulkan directional-shadow baseline and the
existing interactive pending qualification. If an Engine-facing participation
signature changes, search every workspace project and validate its affected
producers and consumers. Stage 1 passes when all six rows have evidence.

### Stage 2: Apply view requirements to main-view culling

Depends on Stage 1. Carry the resolved main-view frustum policy into both CPU
classification and GBuffer indirect submission.

- [x] Make disabled frustum culling preserve eligible instances for direct and
  grouped draws. Verify membership through draw/indirect counts using an
  off-frustum instance; pixels alone cannot expose this difference.
- [x] Measure grouped submission and GPU rejection separately with culling
  enabled. Record the cost and rejected instance count before changing the CPU
  preparation boundary. Stage 2 completes when both routes obey the same policy
  and their costs can be distinguished.

Local-light selection and spatial light lists are a separate design decision.
Multi-view submission can adopt view-family ownership when a concrete caller
requires shared view settings or output.
