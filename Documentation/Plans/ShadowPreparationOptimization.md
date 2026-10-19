# Shadow Preparation Optimization Plan

Summary: Reduce repeated StaticMesh collection, resource resolution and command-hit work across directional shadow cascades while preserving dynamic validation and resource lifetime.

Last reviewed: 2026-10-19

Status: Active
Completed:

## Current Status

Baseline measurement is complete at `6ccf6abdc`. Temporary instrumentation was
removed, eight source files were restored with matching original hashes, and
the working tree was clean. The restored build passed all four directional
shadow qualification cases; 97 baseline images and their metrics file remained
byte-identical. No optimization in this plan has been implemented.

Current stage: Stage 0, baseline and dependency contract. Stage 1 is the first
implementation priority. Later stages require a fresh bottleneck check after
Stage 1; record any resulting scope or priority change before continuing.

The existing implementation contract remains
[retained StaticMesh batch inputs](../Runtime/Rendering/StaticMeshBatchPublication.md).
The previous work already reuses stable Section descriptions and command-local
admission/transform facts. This plan first extends reuse into the subsequent
resource-preparation path.

## Goal

Resolve shared, stable StaticMesh resource inputs once per preparation command
and relevant policy, then construct compact instance/view results without
repeating expensive material/pipeline resolution or weakening current checks.
Demonstrate correctness and measurable CPU preparation improvements in matched
small and high-overlap scenes, including an optimized build.

### Scope and selected decisions

- Initially support ordinary, non-instanced local `FStaticMeshSceneProxy` inputs
  admitted by the existing retained-input contract. Preserve dynamic fallbacks
  for SplineMesh, render-instance arrays, derived providers and custom bindings.
- Start with a command-local resource context shared by the receiver and shadow
  cascades. Reuse the existing persistent shader/pipeline caches on a miss;
  do not introduce another global pipeline cache or cross-frame instance cache.
- Share immutable material-binding owners and resolved pipeline facts, not the
  complete `FResolvedMeshDrawRecord`, which also carries instance/view bindings.
- Identity must account for the retained descriptor owner, binding/layout/vertex
  declaration, material/program, pass/raster/depth/winding policy, and relevant
  shader/device/manual generation. Audit dependencies before finalizing the key.
- Keep view Uniforms/pass bindings, primitive indices/Uniforms, shadow bias,
  live texture/collection snapshots and resource readiness at their required
  dynamic boundaries. Do not accept provider-supplied validation-success flags.
- Keep strong owners in resolved and in-flight snapshots. New lookups must not
  retain stale negative results or bypass existing retirement/device-reset rules.
- GPUScene, indirect submission, shadow-map content caching, PCSS and filter
  changes are outside this plan. Additional parallelism is not the first step.

## Baseline and Evidence

The existing qualification fixtures use five opaque boxes and 128 high-overlap
ordinary opaque StaticMesh primitives, a shared material, automatic LOD,
1920x1080, Medium filtering, disabled contact shadows and persistent commands.
SingleMap and ThreeCascades each use 30 warm-up frames and 120 measured frames.
Three broad runs and three separate refined runs were preserved. Receiver and
shadow counters were separated; the following values cover three shadow
cascades only, excluding receiver preparation.

Values are milliseconds, the median of three per-run medians/P95s from the
refined probe cohort. Parent scopes include their children and must not be
added to them; independent scope medians do not sum to an enclosing median.

| CPU stage, 128 primitives | Median | P95 |
| --- | ---: | ---: |
| Caster discovery and membership | 0.4755 | 0.6673 |
| Collection and admission, inclusive | 10.2614 | 11.7056 |
| Section and transform resolution | 2.3236 | 2.9377 |
| Primitive/draw assembly | 1.3510 | 1.6835 |
| Sorting/finalization | 0.4707 | 0.6112 |
| Finish/result transfer | 0.9919 | 1.3734 |
| Logical preparation, inclusive | 16.2511 | 18.5472 |
| Subsequent resource preparation, inclusive | 10.8439 | 12.3039 |

Resource preparation includes material/pipeline preparation at 5.8657 ms
(material-binding resolution 2.0352 ms; pipeline resource resolution 3.5332 ms),
stable-command hit validation/record restoration at 3.5109 ms, and binding
preparation at 1.1393 ms. Remaining Uniform processing was about 0.1110 ms.
The final sampled frame in every refined run had 384 command hits, zero misses,
384 primitive Uniform reuses and zero primitive Uniform uploads. Upload/cache
creation is therefore not the observed warmed bottleneck.

Collection includes projected-size/LOD work at 2.3222 ms, provider construction
and admission at 4.7110 ms, and capability/fact capture and transfer at 1.2181 ms.
Do not attribute unmeasured gaps to a particular allocator or copy operation
without a controlled experiment. These fixtures use the inline preparation
path below 256 candidates, with zero preparation tasks and zero task waits.
The five-box refined medians were 0.8682 ms logical and 0.5756 ms resources;
its membership popcount was 10, compared with 384 for the high-overlap fixture.

These are Win64-Debug-DurinEditor/MSVC/Tracy/Vulkan-validation diagnostic
measurements, including probe overhead, not Release/FPS or exclusive-GPU gates.
The initial broad cohort included a CPU slowdown to 31.31 ms logical; all runs
remain available. Refined logical medians were 16.2511/16.2540/15.8752 ms.
After probe removal, the 128-object check measured 15.6334 ms logical and
11.1073 ms resources; its P95s and the small fixture showed further system
variation. Do not compare probe schemas or different fixtures as an improvement.

Local evidence is under ignored `Build/ShadowReprofile/`: `report.txt`,
`summary.json`, `run-{1,2,3}/`, `detail-{1,2,3}/`, `final-restored.log`,
`capture-comparison.json`, `originals.json` and `restored.json`. Preserve or
publish the raw CSVs/logs as CI artifacts before a handoff that cannot access
this checkout; local reports are not source-controlled performance receipts.

## UE Reference and Design Boundary

[UE's mesh drawing pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
separates retained static batches, stable commands, visible-command data and
per-frame primitive data; cached dependencies must invalidate when changed.
[FMeshDrawCommand](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FMeshDrawCommand)
describes stable draw work, while view-related payloads belong separately.
Use these separation and dependency principles with Durin's existing owners
and resource generations. Manual shadow passes do not imply UE's complete
AddToScene command cache, and this plan does not claim source-code equivalence.

UE's [squared sphere-radius screen metric](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/ComputeBoundsScreenRadiusSquared?lang=en-US)
is a reference for cheaper LOD evaluation, not a drop-in replacement for Durin's
projected AABB diameter and authored thresholds. Stage 3 preserves those semantics.

## Implementation Stages

### Stage 0: Baseline and dependency contract

Outcome: a reviewable cache/validation contract and reproducible comparison setup.

- [x] Record the current baseline, nested-stage boundaries, raw evidence and
  measurement limitations; confirm source restoration and shadow correctness.
- [ ] Audit `PrepareShadowResources_RenderThread`, `EnsureSectionResources_RenderThread`,
  `PrepareUniforms_RenderThread` and the retained description/capture owners.
  Document every cache-key dependency and which checks remain dynamic.
- [ ] Define command-local context ownership, API consumers and fallback/failure
  behavior before implementation. Identify shared API/layout changes across
  Engine, Sandbox and RoadWeaver using `Durin.dworkspace`.
- [ ] Select an existing optimized configuration and establish matched baseline
  variance. Record numerical median/P95 guardrails before evaluating changes;
  do not choose thresholds after seeing the optimized result.

Completion: dependencies and lifetime are explicit, baseline artifacts are
retrievable, and comparison settings/guardrails are recorded in this plan.

### Stage 1: Reuse stable Section resource preparation

Depends on Stage 0. Outcome: ordinary StaticMesh resolves unique stable Section
resources once per command/policy instead of once per instance/cascade.

- [ ] Add the command-local context and immutable resource facts; use retained
  identities and strong owners, with existing generation-aware caches on misses.
- [ ] Share it across receiver/cascade resource preparation. Keep per-instance
  and per-view records separate; retain dynamic-provider fallbacks.
- [ ] Preserve failure outcomes and dynamic texture/collection/readiness checks
  at the audited boundaries. Cover shader/device/manual invalidation and retirement.
- [ ] Compare cold/warm prepared outputs and pixels with the reference path;
  measure unique resource resolutions, material/pipeline cost and total preparation.
- [ ] Reprofile and record the remaining bottlenecks before choosing Stage 2/3 scope.

Completion: classification/rendering/lifetime match the reference, unique stable
resource resolution replaces repeated work, and matched measurements satisfy
the Stage 0 guardrails without moving cost into another phase.

### Stage 2: Reduce command-hit validation and batch materialization

Depends on Stage 1 evidence. Outcome: shared dependencies are checked at the
appropriate primitive/material boundary and views reference compact frame facts.

- [ ] Separate current primitive-state validation from shared material/resource
  validation; reuse each within a command without losing current-value checks.
- [ ] Reduce repeated complete Batch/Record construction, owner copies and small
  containers where measured; use immutable frame instance facts plus view references.
- [ ] Keep purpose masks, IDs, LOD selection, binding coherence, duplicate/dynamic
  geometry checks and changed/invalid/restored input behavior correct.
- [ ] Verify same-frame/next-frame updates, collections/textures, custom data,
  transforms, bounds, mirrored winding, proxy removal and dynamic fallbacks.
- [ ] Re-measure collection, hit probes, result transfer and aggregate preparation;
  reject unproven allocation/copy attribution and regressions outside guardrails.

Completion: reference outcomes agree, dynamic checks remain effective, and
measured hit/collection work decreases without duplicate persistent caches.

### Stage 3: Specialize cascade projected-size preparation

Depends on the latest bottleneck measurement. Outcome: cheaper projection/LOD
preparation with the same observable selection and conservative fallbacks.

- [ ] Prepare view-constant validation once; specialize orthographic cascades
  using center/half extent while retaining the general projection path.
- [ ] Evaluate lazy projection for single-LOD/ForceLOD0 inputs only where its
  lack of necessity is proven; preserve diagnostic status/counters explicitly.
- [ ] Compare projected size and LOD decisions around thresholds, touching and
  crossing near planes, invalid bounds/views, aspect ratios and depth conventions.
  Use a general-path fallback where boundary equivalence is uncertain.
- [ ] Measure projection/LOD and total logical preparation; retain authored
  threshold semantics and require the recorded numerical/correctness guardrails.

Completion: selection/fallback semantics agree with the reference and the
optimization reduces measured work in the applicable views.

### Stage 4: Final acceptance and handoff

Depends on all retained implementation stages. Outcome: validated final sources,
durable contracts, retrievable evidence and no temporary comparison machinery.

- [ ] Follow [build selection](../Agents/BuildAndRun.md) and
  [test/result reuse](../Agents/Testing.md). Validate owning modules and affected
  consumers; require one incremental `all` build when public layout/ABI or
  foundational changes prevent reliable bounded consumer coverage.
- [ ] Run focused StaticMesh preparation, renderer scene contracts, Spline and
  resource-reload coverage. Include opaque/masked materials, LOD/resource changes,
  shader/device reset, dynamic inputs and in-flight snapshot lifetime.
- [ ] Run directional shadow qualification for changed preparation behavior;
  preserve reference outcomes and compare baseline images/metrics. Explain any
  intentional diagnostic-only difference rather than weakening assertions.
- [ ] Alternate reference/new paths in matched small/high-overlap fixtures with
  three rounds of warm-up and samples; report median/P95, phase boundaries and
  aggregate cost. Confirm the result in the selected optimized configuration.
- [ ] Add a larger-than-256-candidate correctness/performance case if task-path
  behavior is affected; report worker work and waits separately from wall time.
- [ ] Remove temporary tests, reference switches and detailed probes, restore the
  normal build, and validate final relevant inputs. Retain raw data under `Build/`
  or CI artifacts and record the tested revision/configuration/commands here.
- [ ] Update the owning Runtime contract, validate documentation, and commit
  isolated implementation/status changes with the exact Plan and Stage trailers.

Completion: all retained stages and acceptance checks have evidence, temporary
machinery is gone, lasting rules live in the Runtime contract, and the plan can
be marked Completed following the plan lifecycle rules.
