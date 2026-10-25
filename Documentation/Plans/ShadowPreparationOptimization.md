# Shadow Preparation Optimization Plan

Summary: Reduce repeated StaticMesh collection, resource resolution and command-hit work across directional shadow cascades while preserving dynamic validation and resource lifetime.

Last reviewed: 2026-10-20

Status: Active
Completed:

## Current Status

Baseline measurement is complete at `6ccf6abdc`. Temporary instrumentation was
removed, eight source files were restored with matching original hashes, and
the working tree was clean. The restored build passed all four directional
shadow qualification cases; 97 baseline images and their metrics file remained
byte-identical. Stage 1 is complete: ordinary StaticMesh now reuses stable Section
resource preparation within a command and across cascades. Dependency comparison,
matched Release CPU diagnostics, final regression and source cleanup passed.

Stages 0 and 1 are complete. Stage 2 is partially implemented; Stage 3 remains
pending. The new profile still puts logical collection/preparation ahead
of resources; narrow the next measurements before implementing another stage.
The Stage 1 follow-up simplifies redundant resource keys/owners and resource
policy parameters. The logical-stage reprofile is complete and selects collector
admission/snapshot insertion as the next logical target, with repeated material
currentness checks as a separate resource target. Temporary measurement code has
been removed and final regression passed. The Stage 2 follow-up separates
Collector.Add checks, moves and insertion, and replaces the command-local
validation cache's ordered lookup with hash lookup. Current-value comparisons,
invalidation and per-submission checks remain intact. Broader Batch
materialization and resource-currentness changes remain pending.
The post-hash reprofile now separates receiver and shadow costs: current-value
and transform checks remain the largest admission subranges, but resource
currentness/command-hit work and projected-size calculation rank ahead as next
optimization targets. No additional runtime optimization is implemented by this
measurement follow-up.
Formal timing qualification still requires an exclusive quiet lane; the recorded
CPU comparisons are diagnostic evidence, not a GPU/FPS acceptance claim.

The existing implementation contract remains
[retained StaticMesh batch inputs](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md).
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

## Resource Dependency Contract (2026-10-20)

The resource context is local to each invocation of
`ResolveSceneRenderResources_RenderThread`, including a new context on every
prepared-pipeline retry. Receiver and cascade resource calls borrow it serially;
it never becomes a persistent renderer cache or worker input. A standalone
directional-shadow resource call creates its own context for its cascade loop.
Hybrid retained/GBuffer feature preparation remains on the existing path.

Only exact ordinary, non-instanced StaticMesh capture opts in. Carry that
renderer-owned eligibility through collected/prepared primitive facts; published
geometry or a local binding alone must not opt custom/dynamic providers in.
No Engine public API change is required. Renderer-private preparation layouts
and resource-call signatures change; inspect all Engine/Sandbox/RoadWeaver
consumers and validate the affected preparation/scene/dynamic/reload targets.

The entry key is the immutable description owner plus an explicit resource policy
distinguishing sampler-only deferred receivers, forward and shadow preparation.
Before lookup, the current geometry/binding must match the immutable geometry
owner, with valid declaration/streams and the exact local binding type. This
coherence check makes the additional binding/declaration keys redundant. Description
identity already owns geometry/LOD Section, material/program, vertex-factory
layout, raster/depth/topology and winding policy. Entries retain description
and material owners and a resolved pipeline reference, rather than
borrowing raw cache-slot payloads. Context reuse also checks renderer identity,
the complete coordinator Shader/Device/Manual generation, renderer release
epoch and surface-material resource revision; a change clears the context before
lookup. The latter covers sampler creation/replacement, failed creation, bounded
eviction and independent release, which need not change coordinator generations.
Only successful results enter
the context, so pending/failed shader, pipeline or sampler work retries normally.

`EnsureSectionResources_RenderThread` resolves factory/shader map/pipeline and
ensures material samplers. It does not resolve live textures, collections,
environment, primitive Uniforms or pass bindings. Those remain in the existing
Uniform/binding paths, including texture-reference/sampler currentness,
collection schema/payload checks and per-view refresh. Per-draw resource readiness,
binding/declaration checks and success/failure accounting remain in place.
Transforms, custom data, LOD selection, sorting, visibility and shadow bias
remain outside cached resource facts. Immutable publication and registered-factory
contracts still apply; native mutation requires replacement publication.

Comparison configuration: existing `Win64-Release-DurinEditor` preset, with
Debug correctness/diagnostic coverage retained. Use alternating reference/shared
frames in the same process: 30 warm-up pairs and 120 measured pairs, three rounds,
five-box and 128-overlap fixtures, SingleMap and ThreeCascades. Before Stage 1,
the labels exercise the same original resource path (A/A). Record all raw samples
under `Build/StableSectionResources/` and retain system-noise observations.

Predeclared CPU comparison guardrails: require at least a 5% median resource
preparation reduction in each 128-caster three-cascade round; the median of
per-round resource P95 ratios must not exceed 1.05. Small-fixture and total CPU
preparation median/P95 regressions must stay within the larger of 5% or 0.020 ms.
An A/A median difference above 3% or P95 difference above 10% requires investigation
or a repeated quiet sample before treating the affected measurement as acceptance.
GPU timing remains diagnostic without an exclusive lane; no FPS claim is permitted.

The initial Release build and all A/A qualification runs passed. The balanced
128-caster three-cascade median ratios were 1.0003/1.0002/1.0197, with a subsequent
confirmation ratio of 1.0046. Small-fixture/tail variability was investigated
with that additional unchanged-path run; all raw runs remain available. Tail
noise still prevents an exclusive-lane performance claim. Compare matching
reference/shared CPU cohorts and report P95 variation explicitly rather than
discarding outliers. Baseline artifacts are `release-aa-pilot`,
`release-aa-balanced-{1,2,3}` and `release-aa-confirm` under the evidence directory.

## Stage 1 Evidence (2026-10-20)

Compared the implementation on `3e9c43a5b` (the renderer resource source matches
`715844869`; the earlier runtime baseline was `6ccf6abdc`)
against the original resource path selected by a temporary control in the same
binary. Each of three Release rounds used the configuration and samples above,
balanced AB/BA order, five-box and 128-caster scenes, SingleMap/ThreeCascades.
The reference bypassed only the new context; both paths used identical logical
preparation, Uniforms, bindings and persistent command caches.

128-caster, three-cascade CPU times, reference/shared:

| Round | Resource median | Resource P95 | Logical + resource median | Logical + resource P95 |
| --- | --- | --- | --- | --- |
| 1 | 0.6836 / 0.4569 ms | 0.8833 / 0.6649 ms | 1.6951 / 1.4701 ms | 2.1674 / 1.9645 ms |
| 2 | 0.6685 / 0.4443 ms | 1.0507 / 0.7037 ms | 1.6584 / 1.4380 ms | 2.2831 / 2.0861 ms |
| 3 | 0.6777 / 0.4521 ms | 1.0861 / 0.7377 ms | 1.6980 / 1.4733 ms | 2.3854 / 2.5035 ms |

Resource medians fell 33.2-33.5%; the median resource P95 ratio was 0.6792.
Combined medians fell 13.2-13.3%. The third combined P95 rose 4.95%, within
the predeclared guardrail; logical P95 also varied, including a 6.49% rise in
round 1. All small-fixture and combined median/P95 comparisons met their
guardrails. Small three-cascade resource medians fell 11.1-15.2%; these
microsecond-scale observations do not establish frame-rate improvement.

In every warmed 128-caster three-cascade sample, shadow material/pipeline
preparation calls fell from 384 to 1, with 383 context hits. Receiver resource
calls fell from 128 to 1, with 127 hits. These count actual helper work; the
resource timer also includes the unchanged Uniform/binding paths. A Debug
diagnostic round measured 11.1201 / 6.0704 ms resource median and
27.6212 / 22.3376 ms combined median. Debug/Tracy and Vulkan validation inflate
these costs; Release is the selected optimized comparison.

After reuse, Release logical preparation remains about 0.99-1.02 ms against
0.44-0.46 ms resources; Debug measured 15.87 ms against 6.07 ms. The earlier
collector/projection/admission split remains useful attribution evidence, but
these new totals do not prove a narrower bottleneck inside the remaining
resource or logical ranges. Stage 2 should first remeasure command-hit probing
and materialization separately from collection; Stage 3 LOD work remains pending.

A temporary isolated dependency test compared complete resolved resource records
for opaque/masked and mirrored inputs, repeated cascades, material/LOD changes,
receiver policies, dynamic fallback, invalid/restored bindings, failed-key retry,
stale Shader/Device/Manual context generations, independent sampler/renderer
release, renderer identity and owner lifetime. It passed. Independent sampler
creation/reset was exercised; bounded-eviction invalidation was audited against
the slot-cache eviction counter, rather than forcing unsupported sampler states.

All four qualification cases passed in each comparison round. Captures matched
the prior 97 baseline images plus metadata byte-for-byte. Timing is diagnostic:
GTX 1060 6GB, ordinary desktop contention/implicit layers, no exclusive quiet GPU
lane. No GPU/FPS or whole-frame improvement is claimed.

Raw data and receipts: ignored `Build/StableSectionResources/`, including
`release-ab-{1,2,3}/`, `debug-ab-1/`, `comparison.json`, `dependencies.xml` and
`optimized-captures/`. Per-round `source.txt` and `qualification.log` identify the
exact retained run. The first unretained A/B qualification run was excluded from
timing evidence; the saver now requires CSVs from the run named in its test log.
Temporary comparison tests, controls and counters were removed before final
regression. The implemented contract lives in
[command-local Section resources](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md#command-local-section-resources).

Final validation used `3e9c43a5b` plus this implementation and the independent
fixture fix `1dceb941b`. Final relevant source hashes and capture comparisons
are in `final-{debug,release}/`; later edits change documentation only.
`test affected --explain` expanded to the Renderer module's broad test list.
The changed APIs/layouts are Renderer-private; all Engine/Sandbox/RoadWeaver
consumers were inspected. Bounded consumer coverage was:

| Win64-Debug-DurinEditor target | Result |
| --- | --- |
| StaticMeshRenderPreparationVulkanTests | 15 passed |
| RendererSceneContractTests | 63 passed |
| SplineTests | 69 passed |
| RendererResourceReloadVulkanTests | 2 passed together and 2 passed in isolation |
| EditorRenderingTests | 91 passed; existing opt-in `LookupAndRetentionBaseline` skipped |

The resource-reload target initially failed before Section preparation because
render command fences now require the task system. Its two existing fixtures
had not migrated that startup order. The separate fixture-only commit starts
the scheduler before rendering and retains it through render/RHI shutdown;
no shadow implementation behavior was changed for this failure.

After temporary source restoration, directional shadow qualification passed
all four cases in both Debug and Release. Both configurations again matched
97 images plus metadata byte-for-byte. The two temporarily edited test sources
were restored with matching original SHA-256 hashes; reference switches,
runtime counters, dependency-test code, generators and backup test sources were
removed. Remaining raw logs, CSVs, hashes and summaries are ignored artifacts.
No task dispatch/worker threshold changed, so the conditional >256-candidate
worker-path gate does not apply to this serial resource-preparation change.
Changed-document and all-plan validation passed; later documentation edits were
revalidated before handoff.

## Section Simplification and Logical Reprofile (2026-10-20)

This follow-up is based on `2bf9e3f03`. The key is now a typed description pointer
and `EStaticMeshSectionResourcePolicy`; the entry no longer repeats the binding
owner. The description retains it through the immutable geometry record. Current
binding/geometry/declaration/streams/type checks, generation/release/sampler
invalidation, failure retry and downstream dynamic checks are unchanged. The
policy enum replaces the helper's two booleans and numeric policy values.

Win64 sizes measured by the temporary audit: key 32 -> 16 bytes; entry payload
152 -> 136 bytes. This saves 32 bytes of key/value payload per entry, excluding
allocator, node and bucket overhead. It does not establish a whole-frame memory
or latency improvement.

An isolated old/new comparison passed for receiver policies, repeated cascades,
opaque/masked/mirrored inputs, material and LOD owner replacement, binding
mismatch/restoration, dynamic fallback, failed-key retry, stale context
generations, sampler/renderer release and indirect owner lifetime. A distinct
geometry record/binding remained alive through the new entry's description
after direct owners and the reference entry were released, then expired when
the new entry was cleared.

Three Release A/B rounds used the same five-box/128-overlap and
SingleMap/ThreeCascades fixtures, balanced AB/BA order, 60 warm-up frames and 240
measured frames (120 per mode). The reference used the previous four-part key
and extra binding owner; both modes shared the comparison wrapper and unused
empty comparison map. 128-caster three-cascade resource medians were:

| Round | Previous key/owner | Simplified key/owner |
| --- | ---: | ---: |
| 1 | 0.45850 ms | 0.45330 ms |
| 2 | 0.45005 ms | 0.44755 ms |
| 3 | 0.45355 ms | 0.44695 ms |

The 0.6-1.5% median differences are too small for a firm speedup claim here.
Tail guardrails were not uniformly met: round 3 SingleMap combined P95 rose
from 0.5976 to 0.7054 ms, while its resource P95 fell. The retained confirmation
run reversed that SingleMap difference (0.7463 / 0.6959 ms), but its five-box
three-cascade combined P95 varied from 0.1597 to 0.2050 ms. Logical preparation
was unchanged by the key simplification. Keep all cohorts and label these
observations diagnostic; do not accept a formal timing gate or attribute those
tails to the resource change without an exclusive quiet comparison. Retain the
change for the verified simpler identity/ownership contract and smaller payload.

Logical measurements used the simplified path, with three independent Release
broad-probe rounds and three independent refined-probe rounds. Each alternated
coarse and detailed probes in the same process with the same 60/240 frame counts.
A Debug broad-probe round cross-checked the ordering. The table below reports
the median of refined per-run medians/P95s, in milliseconds, for **receiver plus
three cascades** (four calls / 512 admitted batches), rather than shadow-only
telemetry. Parent scopes include their children; medians are not additive.

| Range | Probe cohort | Median | P95 |
| --- | --- | ---: | ---: |
| Collection, inclusive | Coarse | 0.75965 | 1.1074 |
| Publication/input capture, inside collection | Coarse | 0.05690 | 0.0934 |
| Projected size and LOD facts, inside collection | Coarse | 0.20440 | 0.2752 |
| Section/transform resolution | Coarse | 0.24830 | 0.3620 |
| Primitive/draw assembly | Coarse | 0.15305 | 0.2222 |
| Sort/finalization | Coarse | 0.05480 | 0.0796 |
| Finish/result transfer | Coarse | 0.00020 | 0.0004 |
| Provider plus admission, inside collection | Refined detail | 0.38885 | 0.6228 |
| Collector.Add: admission and snapshot insertion, inside provider | Refined detail | 0.30850 | 0.4766 |
| Captured batch/fact transfer, inside collection | Refined detail | 0.12810 | 0.1746 |
| Transform cache lookup, inside resolution | Refined detail | 0.08590 | 0.1126 |
| Stable command hit probing, resource stage | Coarse | 0.47180 | 0.6873 |
| Material resource currentness, inside hit probe | Refined detail | 0.24010 | 0.3063 |

Broad detail measured Batch construction at 0.0552 ms, projected-size evaluation
at 0.19865 ms and LOD selection at 0.03635 ms. Its many nested clocks increased
shadow logical median by 31.3% against coarse probes; refined probes reduced
that observed difference to 11.4%. These detailed values identify ranges, not
uninstrumented production budgets. They do not prove allocator/copy attribution.
The Debug broad comparison agreed on the ordering: admission 5.85 ms versus
Batch construction 1.14 ms, projection 2.28 ms and LOD selection 0.134 ms.
All observed preparation task waits were zero; this 128-candidate fixture uses
the inline path. Do not infer >256-candidate worker scaling from these samples.

`Collector.Add` includes value/coherence validation, by-value Batch moves,
insertion into its batch vector and outcome accounting. This timing does not
isolate pure validation or allocator cost; Batch field population is the smaller
separately measured range. Do not remove checks or assume allocation dominates
from this inclusive result.

Next implementation priority is superseded by the
[post-hash shadow-only profile](#post-hash-shadow-only-profile-and-next-priorities):
material/pass resource currentness first, orthographic projected-size preparation
second, then a broader primitive snapshot/materialization change if remeasurement
still supports it. Preserve all current-value and dynamic-provider checks.

Raw evidence is in ignored `Build/SectionResourceSimplification/`: A/B and
confirmation directories, `profile-release-{1,2,3}`, `refined-release-{1,2,3}`,
`profile-debug-1`, `report.json`, `dependencies.xml`, per-run source paths/logs and
capture comparisons. All retained successful qualification runs matched the
same 97 images plus metadata. Early probe runs with mixed stale build layouts
were excluded: restoring temporary inputs had preserved old timestamps, leaving
incompatible Context object code. Refreshing dependency timestamps fixed the
measurement build; final restoration also refreshes timestamps and checks hashes.
No production workaround or weakened assertion was added for that failure.

Final validation on `2bf9e3f03` plus the three-file simplification passed
StaticMeshRenderPreparationVulkanTests (15), RendererSceneContractTests (63),
SplineTests (69), RendererResourceReloadVulkanTests (2) and EditorRenderingTests
(91 passed, existing opt-in lookup benchmark skipped): 240 passed and one
skipped in total. `test affected --explain` selected broad Renderer-module
coverage; bounded targets cover the owning implementation and dynamic,
lifecycle, scene and editor consumers. The helper signature is private and
there is no public Engine API/layout or build-graph change requiring `all`.

After removing the comparison/audit/profiling code, both Debug and Release
DirectionalShadowBaselineVulkanTests passed all four cases and matched the 97
baseline images plus metadata byte-for-byte. Original Engine/collection sources
and both temporarily edited test fixtures matched their backup SHA-256 hashes.
Final relevant hashes are `final-source-hashes.json`; receipts are the `final-*`
XMLs and `final-{debug,release}/` logs/capture comparisons. Temporary reference
maps, direct-owner audit, probes, generators, headers and source backups were
removed. Only the source simplification and owning documentation remain in the
commit. Changed-document and all-plan validation passed; later documentation
edits were revalidated before handoff.

## Stage 2 Collector.Add measurement and lookup change

Against `ef1060769`, temporary scopes separate admission from argument movement,
the second by-value move into `Admit`, vector insertion and outcome accounting.
Insertion includes allocation and the move into vector storage; it is not a
standalone allocator measurement. The 128-overlap ThreeCascades fixture submits
512 batches across receiver plus three cascades. Its insertion counters observed
512 growing-capacity insertions and zero spare-capacity insertions. Shadow
logical/resource telemetry below excludes the receiver; do not add those ranges
to the four-view Add timings.

Three Release rounds and one Debug round alternate coarse/fine probes within
one process, after 60 warm-up frames with 240 measured frames (120 per mode).
Instrumented median milliseconds for 128-overlap ThreeCascades:

| Range | Release 1 | Release 2 | Release 3 | Debug |
| --- | ---: | ---: | ---: | ---: |
| Admission checks, inclusive | 0.81205 | 0.43265 | 0.42350 | 3.46105 |
| Current bounds/custom-data path, within admission | 0.24230 | 0.12940 | 0.12850 | 0.87340 |
| Current transform path, within admission | 0.26970 | 0.14225 | 0.13960 | 1.63830 |
| Move into Add | 0.04120 | 0.02315 | 0.02220 | 0.52810 |
| Move into Admit | 0.04495 | 0.02410 | 0.02310 | 0.48020 |
| Snapshot insertion | 0.10435 | 0.05390 | 0.05375 | 0.85745 |

Fine timers perturb the short scopes: Release Add medians are 0.639/0.358/0.339
ms with coarse probes versus 1.334/0.720/0.701 ms with fine probes. The first
round also ran substantially slower overall; it remains in the evidence. Treat
the detailed costs as attribution diagnostics, not uninstrumented absolute
costs. Both configurations rank admission above moves/storage, with current-value
and transform cache paths the largest subranges. These paths include table
lookup and exact comparisons; the profile does not isolate lookup alone.

Selected bounded change: replace the private ordered entry table in
`FMeshBatchValidationCache` with a hash table, keeping the existing ID pair,
current-value comparisons, invalid-state transitions and transform rebuild rules.
The table is serial, command-local, never iterated and never erases entries.
Node references survive rehash. No Batch API, move count, vector or publication
contract changes are needed. All per-submission checks remain dynamic. The
controlled lookup comparison, rather than the fine timer alone, supports this
choice. More invasive compact snapshots and shared validation boundaries remain
separate Stage 2 work.

Six Release A/B rounds alternate tree/hash paths in ABBA order with the same
60/240 warm-up/sample settings and five/128-object, SingleMap/ThreeCascades
fixtures. Only the coarse outer Add probe is enabled. For 128 ThreeCascades,
Add median/P95 milliseconds and shadow logical-plus-resource ratios are:

| Round | Tree Add median/P95 | Hash Add median/P95 | Combined median ratio | Combined P95 ratio |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0.55355 / 5.69710 | 0.48055 / 5.33500 | 0.8946 | 1.9432 |
| 2 | 0.54715 / 2.45480 | 0.47325 / 2.55750 | 0.9532 | 0.6320 |
| 3 | 0.54980 / 0.68400 | 0.45480 / 0.55800 | 0.9134 | 0.7931 |
| 4 | 0.55330 / 1.46360 | 0.41270 / 1.25390 | 0.9043 | 1.1927 |
| 5 | 0.50885 / 1.39100 | 0.32875 / 1.14820 | 0.8600 | 0.9139 |
| 6 | 0.72675 / 1.89940 | 0.60610 / 1.42650 | 1.0680 | 0.9989 |

Every large three-cascade Add median decreases; the median of round ratios is
0.8306 (P95 ratio median 0.8411). SingleMap Add regresses 13.0% in the first
round and improves 10.7-13.6% in the other five; small-scene changes are only
microseconds. Debug large three-cascade Add changes 6.39430 to 5.32815 ms,
with combined preparation 23.66780 to 22.55085 ms.

Three subsequent A/A rounds run the hash path under both labels. Large
three-cascade Add median ratios are 0.9420/0.9700/1.0895, while combined median
ratios are 0.9163/0.8709/0.9985. This confirms meaningful host noise in aggregate
preparation. All six A/B cohorts are retained; tail/aggregate guardrails are
not uniformly satisfied, and the A/A investigation does not turn them into
passes. Retain the bounded lookup change for its consistent targeted Add benefit
and unchanged validation; leave formal aggregate timing acceptance outstanding
until a quiet lane is available. Do not claim a whole-frame or GPU speedup.

A temporary tree/hash audit agrees on 4,096 receiver/shadow admission outcomes,
counters and snapshots, with changed/invalid/restored transforms, bounds and
custom data; IDs/LOD/purpose, binding/publication and dynamic geometry failures;
duplicate submissions; and deliberately colliding keys. Matrix facts and rebuild
counts agree. An additional 4,096 inserted keys force rehash while a retained
transform reference remains valid and at the same address.

Raw evidence lives in ignored `Build/CollectorAddOptimization/`: the
`profile-*`, `hash-ab-*`, `hash-aa-*` CSVs, per-round summaries/logs/source paths,
`dependencies.xml` and capture comparisons. A/A JSON retains the old tree/hash
column labels, but both columns execute hash lookup. Every successful profiling
and comparison qualification matches 97 reference images plus metadata.
Temporary source restoration uses fresh modification times to rebuild all
changed layouts. Final validation on `ef1060769` plus the one-header change
passed an incremental Debug `build all` and StaticMesh preparation (15), renderer
scene contracts (63), Spline (69), resource reload (2), and editor rendering
(91 passed, one existing opt-in benchmark skipped): 240 passed and one skip.
`test affected --explain` selects Engine-wide module coverage; symbol searches
across Engine, Sandbox and RoadWeaver identify the cache implementation and
Renderer owner, with no additional direct cache consumers. The focused tests
cover its admission/rendering/lifecycle users; the public layout change also
receives the broader compile/link check.

After removing temporary probes and the tree/hash audit, final Debug and Release
shadow qualification each passed all four cases and matched the same 97 images
plus metadata byte-for-byte. The four probe-edited sources match their original
hashes. `final-source-hashes.json`, `source-restoration.json`, `validation-summary.json`,
`final-all-build.log`, `final-*.xml` and `final-{debug,release}/` retain final-state
receipts. Temporary tests, generators, comparison headers/switches and source
backups were removed; raw data and analysis/receipt scripts remain under `Build/`.
Changed-document and all-plan validation passed. Aggregate timing acceptance,
broader Stage 2 materialization/currentness work and Stage 3 remain open.

## Post-hash shadow-only profile and next priorities

The follow-up measures `e6a7a395a`, including hash-based admission lookup, without
changing production behavior. Seven temporarily edited files are restored from
byte-identical backups with fresh modification times. Unlike earlier combined
Add measurements, receiver and shadow scopes have independent lanes: first-use
inversion in the receiver is not attributed to cascades.

The existing directional-shadow qualification supplies five-box and 128-overlap
opaque StaticMesh fixtures, SingleMap and ThreeCascades. Three Release rounds
and one Debug round use 90 warm-up and 360 measured frames per fixture, with 120
samples each for disabled probes, broad scopes and fine scopes. The repeating
order is `0,1,2,2,1,0`. The first Release smoke run validates probe assertions but
has no retained raw CSV and is excluded from the numerical summary. Subsequent
runs retain raw samples. Timing remains diagnostic without an exclusive lane.

128-overlap ThreeCascades, shadow only, disabled-probe median/P95 milliseconds:

| Round | Logical preparation | Resource preparation | Logical + resources |
| --- | ---: | ---: | ---: |
| 1 | 0.95870 / 1.81950 | 0.48330 / 0.83580 | 1.48585 / 2.57840 |
| 2 | 1.02345 / 1.80350 | 0.47765 / 0.90700 | 1.51570 / 2.63370 |
| 3 | 0.90820 / 1.75120 | 0.46385 / 0.81180 | 1.45090 / 2.53510 |

Discovery/membership medians are 0.03200-0.03335 ms. Sorting is nested inside
logical preparation, 0.02665-0.02820 ms. Five-box ThreeCascades uses ten cascade
memberships: logical medians range 0.08215-0.14810 ms and resources 0.03295-0.06830
ms across rounds. These host-sensitive absolute ranges are not a before/after
speedup claim. The 128-candidate fixture uses inline preparation; worker scaling
above the task threshold is not measured.

Broad shadow scopes, median milliseconds (nested rows must not be summed):

| Scope | Release 1 | Release 2 | Release 3 |
| --- | ---: | ---: | ---: |
| Collection, inclusive | 0.47750 | 0.62855 | 0.45690 |
| Publication/input capture, within collection | 0.02640 | 0.03640 | 0.02310 |
| View facts/projection/LOD, within collection | 0.16990 | 0.21980 | 0.17020 |
| Primitive/provider/transfer loop, within collection | 0.26965 | 0.36410 | 0.25265 |
| Resolve collected facts | 0.16075 | 0.20660 | 0.14525 |
| Assemble prepared records | 0.12260 | 0.16015 | 0.11325 |
| Finalize/sort prepared records | 0.04125 | 0.05820 | 0.04040 |
| Finish view | 0.05025 | 0.06805 | 0.04670 |
| Stable Section resources, within resource preparation | 0.04920 | 0.05750 | 0.05250 |
| Uniform groups/command hits, within resource preparation | 0.37285 | 0.46310 | 0.37185 |
| Bindings, within resource preparation | 0.03485 | 0.04755 | 0.03375 |

Fine shadow scopes, median milliseconds. They attribute work within the broad
ranges; the transform-cache row includes both admission and render resolution,
while admission-transform is only the admission subset.

| Scope | Release 1 | Release 2 | Release 3 |
| --- | ---: | ---: | ---: |
| Collector.Add, inclusive | 0.29490 | 0.30325 | 0.26125 |
| Admission checks, inclusive | 0.15940 | 0.16845 | 0.14905 |
| Current bounds/custom-data cache path | 0.04870 | 0.04920 | 0.04060 |
| Admission transform check | 0.06510 | 0.06820 | 0.05900 |
| All transform-cache accesses | 0.08230 | 0.08885 | 0.07065 |
| Projected AABB screen size | 0.17950 | 0.19420 | 0.16625 |
| LOD selection | 0.02945 | 0.02940 | 0.02845 |
| Cached material resource currentness | 0.19250 | 0.18695 | 0.18145 |
| Command lookup + primitive-state check | 0.15850 | 0.15505 | 0.15370 |
| Collected snapshot/fact transfer | 0.11840 | 0.12465 | 0.10050 |
| Collector snapshot insertion | 0.04400 | 0.04960 | 0.03555 |

Every fine sample asserts the following high-overlap per-command behavior:

| Work | Receiver | Three shadow cascades |
| --- | ---: | ---: |
| Current-value cache accesses | 128 | 384 |
| Current-value rebuilds | 128 | 0 |
| Transform-cache accesses | 256 | 768 |
| Transform rebuilds/inversions | 128 | 0 |

Thus cascades repeat exact comparisons/lookups, not inversions or full numeric
revalidation in these fixtures. Receiver frustum culling is disabled, so all
shadow members have already been captured by the receiver. Shadow-only casters
absent from receiver collection can still require a first transform/value build;
the proposed reuse must preserve that lazy first-use path.
Per-frame current values plus the admission transform have median shares of
68.1-68.8% of measured admission and median costs of 0.10085-0.11545 ms in the
initial three fine cohorts. Projection is 0.166-0.194 ms, resource currentness 0.181-0.193 ms, and
command lookup/primitive-state comparison 0.154-0.159 ms. Uniform/command-hit
preparation is the largest resource subrange; stable Section resource reuse is
already comparatively cheap. Whole collection remains the largest broad logical
range, with costs also spread across provider calls and snapshot transfer.
Do not describe current-value/transform checks as the single dominant shadow
preparation bottleneck after this split.

Fine instrumentation increases logical medians to 1.23220-1.51935 ms versus
0.90820-1.02345 ms with disabled probes. Broad scopes also show host variation,
including the second round at 1.22040 ms. Do not subtract independently sampled
medians to estimate pure check, move or allocator costs. Debug agrees on the
broader ordering: disabled logical/resources medians are 15.31035/6.22035 ms;
fine current values/admission transform 0.12305/0.30220 ms, projection 2.19745 ms,
resource currentness 1.64825 ms and snapshot/fact transfer 2.10640 ms.

A second set of three Release rounds further separates command lookup from
primitive-state comparison, using the same three modes and sample settings.
The original command-hit row above includes both; the refined scopes are:

| Fine shadow scope | Release 1 | Release 2 | Release 3 |
| --- | ---: | ---: | ---: |
| Lookup/readiness/cacheability | 0.06040 | 0.06205 | 0.08790 |
| Capture/compare cached primitive state | 0.10435 | 0.10605 | 0.17790 |
| Material resource currentness | 0.18060 | 0.19590 | 0.23870 |
| Projected screen size | 0.17005 | 0.17240 | 0.22235 |

The third refined round runs slower broadly: disabled logical/resources medians
are 1.49480/0.72425 ms versus 0.90240/0.45460 and 0.91150/0.47280 ms in rounds 1/2.
It is retained, and no code regression or speedup is inferred from that change.
The refined Debug run also passes the same hit/build assertions: lookup is
0.80440 ms, primitive capture/comparison 0.63580 ms, material currentness
1.54270 ms and projection 2.10515 ms. Debug and Release assign different relative
costs to copying/equality versus lookup; prioritize the optimized configuration.
Primitive equality/capture is a significant additional current-state cost in Release,
larger than the lookup portion; do not label the complete old row as map lookup.
It compares the cached command's input, matrices, bounds center and custom data,
separately from Collector admission. Matrix inversion is still not repeated.
Thus repeated current-state comparisons remain worthwhile follow-up work, even
though material currentness and projection are larger localized ranges. The
first recommendation below balances measured cost with a small, auditable reuse
boundary; a unified primitive snapshot has wider API and lifetime implications.

Next implementation priorities:

1. **Reuse material/pass resource-currentness work in the command-hit scan.**
   Start with a memo local to one `PrepareUniforms_RenderThread` invocation,
   keyed by the strong cached material owner and required pass, rather than a
   bare material/group ID. The high-overlap scan performs 384 shadow currentness
   checks despite shared material inputs. Audit distinct owners before choosing
   a key; do not assume all published or cached surfaces in one group are equal.
   Preserve texture-reference/fallback/sampler checks on the first access,
   shader/device/manual generations, resource revisions, collection validation,
   per-view pass refresh and retry/failure behavior. Prove the scan's mutation
   boundary; invalidate if preparation can change its dependencies. Keep each
   draw's readiness, input and current primitive-state comparison. Expand reuse
   across cascades only after explicit dependency/lifetime proof. Measure calls
   per unique owner/pass and matched total CPU preparation, not just lookup time.
2. **Specialize orthographic cascade screen-size preparation.** Validate view
   constants once per view and evaluate an AABB center/half-extent projection
   for constant-w orthographic matrices. Preserve projected AABB diameter,
   viewport normalization, depth/near-plane epsilon and diagnostic status;
   compare general-path results and LOD decisions at thresholds and fall back
   where floating-point boundary equivalence is uncertain. Keep the perspective
   path. The fixture has one LOD, but dropping projection solely for that reason
   would also change diagnostic behavior and needs separate justification.
3. **Then consider renderer-owned primitive snapshots and lighter view records.**
   This may reduce copies and repeated comparisons across admission, resolution
   and command-hit lookup. Scope it to ordinary StaticMesh and preserve current
   scene-state capture, invalid/restored inputs and the full dynamic-provider
   path. Do not add another cross-frame success cache or prioritize a global
   Batch container rewrite from these inclusive scopes.

These are Durin proposals informed by Epic's public documentation/API, not a
local UE source audit. UE's
[mesh drawing pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
separates retained batches/commands from view and primitive data, groups bindings
by update frequency, and requires invalidation of every cached dependency.
Its manual shadow-pass discussion does not imply AddToScene command caching for
all shadow views. The
[screen-radius API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/ComputeBoundsScreenRadiusSquared)
is a cheaper LOD-metric reference; replacing Durin's AABB diameter with a sphere
would change authored thresholds and is outside the proposed specialization.

Evidence is under ignored `Build/PostHashShadowProfile/`: per-fixture CSVs,
`profile-release-{1,2,3}`, `profile-debug-1`, `refined-release-{1,2,3}`,
`refined-debug-1`, `report.json`, XML/logs and source
paths. All eight measured qualification runs pass their existing assertions,
new cache hit/build assertions and the 97-image-plus-metadata comparison.
After both profiling passes, all seven source files match `e6a7a395a` byte for
byte, and normal Debug/Release builds each pass all four qualification cases.
Both final runs match 97 reference images and metadata exactly. The final
unprobed Release run (the original 30 warm-up/120 measured frames) reports
128-overlap three-cascade logical median/P95 0.85800/1.41890 ms, discovery
0.03240/0.05140 ms, static/Spline preparation 0.81110/1.34450 ms and resource
preparation 0.44500/0.66270 ms. Five boxes report logical 0.07390/0.10550 ms and
resources 0.03140/0.04440 ms. The latter fixture has ten cascade memberships.
These normal-build values calibrate aggregate cost; the detailed attribution
comes from the separate probe cohorts, without claiming identical timing scope.

`final-source-hashes.json`, `restoration.json`, `validation-summary.json`,
`final-shadow-{debug,release}.xml` and `final-{debug,release}/` retain the final
receipts. Temporary tests/probes, source generators and all seven source backups
have been removed; raw samples, logs and analysis/receipt scripts remain ignored
under `Build/`. Engine source/tests have no final diff, so `e6a7a395a`'s passing
incremental all build and 240-test/one-existing-skip regression remain applicable
under the result-reuse policy. This follow-up adds only plan evidence/priorities;
it does not require repeating unrelated module tests or broad builds.
Changed-document and all-plan validation pass. Stage 2 implementation tasks and
formal quiet-lane performance acceptance remain open.

## Implementation Stages

### Stage 0: Baseline and dependency contract

Outcome: a reviewable cache/validation contract and reproducible comparison setup.

- [x] Record the current baseline, nested-stage boundaries, raw evidence and
  measurement limitations; confirm source restoration and shadow correctness.
- [x] Audit `PrepareShadowResources_RenderThread`, `EnsureSectionResources_RenderThread`,
  `PrepareUniforms_RenderThread` and the retained description/capture owners.
  Document every cache-key dependency and which checks remain dynamic.
- [x] Define command-local context ownership, API consumers and fallback/failure
  behavior before implementation. Identify shared API/layout changes across
  Engine, Sandbox and RoadWeaver using `Durin.dworkspace`.
- [x] Select an existing optimized configuration and establish matched baseline
  variance. Record numerical median/P95 guardrails before evaluating changes;
  do not choose thresholds after seeing the optimized result.

Completion: dependencies and lifetime are explicit, baseline artifacts are
retrievable, and comparison settings/guardrails are recorded in this plan.

### Stage 1: Reuse stable Section resource preparation

Depends on Stage 0. Outcome: ordinary StaticMesh resolves unique stable Section
resources once per command/policy instead of once per instance/cascade.

- [x] Add the command-local context and immutable resource facts; use retained
  identities and strong owners, with existing generation-aware caches on misses.
- [x] Share it across receiver/cascade resource preparation. Keep per-instance
  and per-view records separate; retain dynamic-provider fallbacks.
- [x] Preserve failure outcomes and dynamic texture/collection/readiness checks
  at the audited boundaries. Cover shader/device/manual invalidation and retirement.
- [x] Compare cold/warm prepared outputs and pixels with the reference path;
  measure unique resource resolutions, material/pipeline cost and total preparation.
- [x] Reprofile and record the remaining bottlenecks before choosing Stage 2/3 scope.
- [x] Simplify redundant resource keys/owners and policy parameters; compare
  correctness/performance, refine logical-stage measurements and remove probes.

Completion: classification/rendering/lifetime match the reference, unique stable
resource resolution replaces repeated work, and matched measurements satisfy
the Stage 0 guardrails without moving cost into another phase.

### Stage 2: Reduce command-hit validation and batch materialization

Depends on Stage 1 evidence. Outcome: shared dependencies are checked at the
appropriate primitive/material boundary and views reference compact frame facts.

- [x] Reprofile optimized preparation with separate receiver/shadow scopes and
  cache hit/build counts; record UE-informed priorities and remove probes.
- [x] Split Collector.Add checks/moves/insertion and optimize command-local entry
  lookup while retaining exact current-value checks; remove temporary probes.
- [ ] Separate current primitive-state validation from shared material/resource
  validation; reuse each within a command without losing current-value checks.
- [ ] Reduce repeated complete Batch/Record construction, owner copies and small
  containers where measured; use immutable frame instance facts plus view references.
- [ ] Keep purpose masks, IDs, LOD selection, binding coherence, duplicate/dynamic
  geometry checks and changed/invalid/restored input behavior correct.
- [ ] Verify same-frame/next-frame updates, collections/textures, custom data,
  transforms, bounds, mirrored winding, proxy removal and dynamic fallbacks.
- [x] Re-measure collection, hit probes, result transfer and aggregate preparation;
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
