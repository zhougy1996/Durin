# Material Runtime Optimization Plan

Summary: Optimize material runtime binding, renderer cache retention, and Primitive/Instance numeric variation.

Last reviewed: 2026-10-16

Status: Completed
Completed: 2026-10-16

## Current Status

Stage 4 is complete. Its complete 12-case Mesh Vulkan target passed, including
the custom-data case after the existing animation cases in one process; the
new case also passed alone. The fixture respects the shared asset compiler's
terminal lifecycle. All 12 targets in `@domain=material` passed, including the
two new CPU tests and the extended source-free Cook test. WorldTests passed
162 cases, RendererSceneContractTests 61, and RendererResourceReloadVulkanTests
both cases. The workspace all-build passed across Engine, Sandbox and RoadWeaver.
The production base is `c2aa9cc838453474dbcc6bfa428139671e7eeba1`, plus this
stage's changes, using Win64-Debug-DurinEditor with MSVC 14.44.35207 and Tracy.
The chosen design follows UE's
[Custom Primitive Data](https://dev.epicgames.com/documentation/unreal-engine/storing-custom-data-in-unreal-engine-materials-per-primitive?lang=en-US)
and [ISM custom-data ownership](https://dev.epicgames.com/documentation/unreal-engine/instanced-static-mesh-component-in-unreal-engine),
with Durin-specific fixed bounds and immutable snapshots.

The real Vulkan 64-instance workload uses distinct colors and per-instance WPO,
plus primitive WPO and masked opacity. Byte-for-byte Forward, all five GBuffer
attachments and masked-shadow depth match the dynamic-material reference and
direct custom-data route. At equal output, the dynamic reference submits 64
successful GBuffer draws with 64 material-uniform groups; custom data admits one
64-candidate GPU group and one indirect draw with one material-uniform group.
Changing one instance's mask preserves one indirect draw and changes depth and
shadow pixels; deleting an instance leaves 63 direct draws under the unchanged
64-candidate minimum. Reordering preserves output and stable IDs. Primitive data
updates change WPO output while retaining the accepted material program.

Each primitive/instance GPU record grows from 256 to 512 bytes. The existing
4 MiB generated-buffer cap charges that increased stride; no GPU-memory or
frame-time speedup is claimed. Instance updates copy/validate bounded snapshots;
incremental GPU-scene updates, serialized component defaults and instanced
physics remain outside the executed runtime scope. These tradeoffs and the
implemented API are documented in [the runtime contract](../Runtime/Rendering/MaterialCustomData.md).

Stage 4 receipts are under `Build/MaterialRuntimeOptimization/`:
`stage4-mesh.{xml,log}`, `stage4-custom-gpu.{xml,log}`,
`stage4-custom-cpu.{xml,log}`, `stage4-material.{xml,log}`,
`stage4-WorldTests.{xml,log}`, `stage4-RendererSceneContractTests.{xml,log}`,
`stage4-RendererResourceReloadVulkanTests.{xml,log}`, and `stage4-all-build.log`.
Reproduce with the named targets and the custom-data case in
StaticMeshRenderPreparationVulkanTests; no opt-in timing variable is needed.
Affected discovery expands to most workspace tests because the Engine API is
shared. The bounded material/mesh/lifecycle selections and required all-build
cover this change; unrelated application-hosted and qualification suites were
not implicitly enabled.

Stage 3's final candidate passed all 18 slot/invalidation cases (including the
opt-in workload), all 11 StaticMeshRenderPreparationVulkanTests cases and both
RendererResourceReloadVulkanTests cases. The workspace all-build also passed
across Engine, Sandbox and RoadWeaver. The Stage 3 reference is production revision
`a1973ceb612c4523b9ded7bfcf7b396d9661918c` plus only the new benchmark harness; the candidate is that revision
plus this stage's implementation. The unchanged Win64-Debug-DurinEditor profile
uses MSVC 14.44.35207, Debug and Tracy. Release timing acceptance was explicitly
deferred before runtime edits because this is the configured diagnostic lane;
no latency/noise threshold or end-to-end frame improvement is claimed.

Pooled 90-sample observations per scenario (microseconds per sweep):

| Keys | Access | Reference median/p95 | Candidate median/p95 | Mean recreations, reference/candidate |
| --- | --- | --- | --- | --- |
| 256 | cyclic | 156.00 / 181.00 | 91.50 / 101.30 | 0 / 0 |
| 256 | hot | 124.00 / 157.20 | 92.50 / 164.00 | 2.07 / 2.07 |
| 512 | cyclic | 437.15 / 573.00 | 188.35 / 226.10 | 0 / 0 |
| 512 | hot | 261.40 / 309.50 | 185.60 / 259.90 | 6.27 / 6.27 |
| 1024 | cyclic | 23876.30 / 26254.50 | 1904.05 / 3190.50 | 1024 / 1024 |
| 1024 | hot | 3269.40 / 4866.80 | 526.10 / 920.10 | 115.80 / 103 |
| 4096 | cyclic | 96376.55 / 102409.20 | 7217.50 / 9022.00 | 4096 / 4096 |
| 4096 | hot | 14156.25 / 15400.60 | 2122.10 / 2498.50 | 467.60 / 410 |

At capacity, evictions equal recreations; below capacity measured evictions are
zero. Cyclic overflow still thrashes under LRU. Peak accounted slot storage at
512 resident entries increases from 368640 to 401408 bytes (360 to 392 KiB);
inline integer payload weight remains 2048 bytes. At 256 entries the respective
charges are 184320 and 204800 bytes. Candidate medians improve in all eight
diagnostic groups; the 256-key hot p95 is slower, so no universal tail-latency
claim is supported. Hash collisions and byte/count limits are correctness gates,
not probabilistic timing assertions. The byte gate covers charged cache storage,
not the transitive memory held by live consumers.

Evidence is retained under `Build/MaterialRuntimeOptimization/`:
`stage3-reference.csv`, `stage3-candidate.csv`, `stage3-comparison.json`,
`stage3-compare.ps1`, matching logs and XML, `stage3-slot-tests.xml`,
`stage3-mesh.xml`, `stage3-reload.xml`, and `stage3-all-build.log`. The first list-based candidate's raw
CSV and comparison are retained as `stage3-list-candidate.csv` and
`stage3-list-comparison.json`. Reproduce with the opt-in case and settings in
[the measurement protocol](../Development/Build/RenderingPerformanceBaseline.md#binding-update-and-submission-workload).
Affected analysis expands to application-hosted and unrelated renderer consumers;
the selected cache/invalidation, mesh output and reload gates cover this private
cache change, with all-build covering workspace integration.

Stage 0 passed. Reference production revision is
`ed82411474a27b3af248f71355fb9bc355337d5f` plus this stage's measurement harness
and telemetry exposure only. Win64-Debug-DurinEditor uses MSVC 14.44.35207 and
Tracy; all timings are diagnostic, not Release acceptance. Sandboxed MSVC failed
with documented C1902; the same build passed outside the sandbox.

CPU selection `FMaterialQualificationTests.ProxyPublicationAndResolveBaseline`
passed; three-run median observations for 32 proxies are approximately 5.1,
14.2 and 52.4 ms at 8/32/128 parameters. The real Vulkan Collection fixture
passed with opt-in sampling, including its Forward/GBuffer/masked-shadow output
checks. Commands are documented in the measurement protocol. Retained evidence:
`Build/MaterialRuntimeOptimization/reference-cpu.{csv,log,xml}` and
`Build/MaterialRuntimeOptimization/reference-collection.csv`,
`reference-collection-test.log`, `reference-collection.xml`.
Raw samples remain diagnostic; structural correctness is the hard gate.

Stage 0 is committed as `3cb4ff78d`. Stage 1 passed 23 focused Proxy and
representation tests, including mixed numeric/resource byte equivalence and
program/parameter/type invalidation. The candidate CPU workload also passed;
raw samples, log and XML are retained as `candidate-cpu.*` beside the reference.
The new fixture explicitly initializes RenderCore before its first rendering
thread lifetime. MaterialRuntimeTests now declares its dynamically loaded
TextureCompressor dependency so clean focused builds deploy it. No authored or
Cook schema changes. Stage 1 is committed as `d6e699fc9`; its new regression also
passed alone. Stage 2's focused Vulkan test and all 11 mesh preparation/animation
tests passed, including texture/material/transform invalidation, resource
replacement, time and Collection output. Final validation passed all 12 targets
in `@domain=material`, all 11 mesh preparation/animation cases, both
RendererResourceReloadVulkanTests cases, and the workspace `all` build across
Engine, Sandbox and RoadWeaver. The final Collection regression additionally
covers mixed contracts, missing-world defaults, incompatible schema rejection,
and different retained defaults for one Collection ID without repeated rebinding.
Stages 0-4 are complete; Stages 5-8 remain deferred.

On the NVIDIA GeForce GTX 1060 6GB diagnostic lane, pooled 360-sample Collection
RenderView CPU median/p95 changed from 2.189/3.913 ms to 2.315/4.268 ms in the final
standalone comparison after the mixed/default-Collection isolation fixes. Earlier
exploratory candidates measured 1.807/2.134 ms; no Collection latency improvement
is established, and the final diagnostic observation is slower. Every warm
sample changed from 0 hits/2 misses/1 surface preparation/1 binding batch to
2 hits/0 misses/0 surface preparations/0 binding batches. The 128-parameter CPU
median/p95 changed from 52.431/70.972 ms to 46.925/58.236 ms. These are observations
from Debug/Tracy, not accepted Release speedup claims. Raw candidate Collection
CSV/log/XML and `comparison.json` remain under `Build/MaterialRuntimeOptimization`.
The Collection candidate is `d6e699fc9` plus this Stage 2 commit's source changes.
Final reports are `stage2-renderer.xml`, `resource-reload.xml`,
`material-regressions.xml`, `stage1-standalone.xml`, and `all-build.log` in the
same evidence directory. Broad affected selection expands to unrelated Engine
consumers; bounded material-domain plus renderer/GPU selection was used instead,
with the required all build covering shared API consumers. No timing budget
was frozen; Release timing qualification remains a future measurement task.

## Goal

Reduce repeated material binding work while preserving immutable publication,
typed validation, counted resource lifetime, exact world isolation, and current
precise shader/pipeline identity contracts. Execute Stages 0-4 in this plan.

## Selected Decisions

- Preserve GUID-facing authoring APIs and compiled layout v4. Prebind indices in
  each render proxy against its accepted immutable program; reuse them for value
  publications with the same GUID/type sequence. Indexed writes must check both
  field identity and type. No asset or cooked schema change is needed.
- Cache immutable Collection uniform allocations by retained snapshot identity
  (including layout/default fallback identity). Never update storage referenced
  by already prepared commands in place. Reuse command structure and unchanged
  material/primitive resources; rebuild material-set bindings when Collection
  allocation changes. Validate every required schema before considering a hit.
- Retain existing geometry eligibility, generation invalidation, texture checks,
  budgets, shader maps and PSOs. Do not broaden caching to Spline in this slice.
- Follow the dependency and lifetime principles in UE's
  [Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine):
  retained commands require precise invalidation and resource ownership. The
  indexed LRU policy is a measured Durin decision, not a claim about UE's policy.
- Stage 2 inspection found uniform groups currently merge equal local payloads
  even when their programs reference different Collections. Refine groups by the
  exact ordered Collection contracts/defaults before caching or binding them;
  retain deduplication for collection-free programs. Add a mixed-material output
  regression after the frozen benchmark workload, without changing its inputs.
- Follow [build](../Agents/BuildAndRun.md), [test](../Agents/Testing.md), and
  [measurement authority](../Development/Build/RenderingPerformanceBaseline.md).
  Raw CSV/log/XML belongs under ignored Build or native test Work directories.

## Implementation Stages

### Stage 0: Establish reproducible baselines

Scope: add a CPU proxy publication/resolve workload with 8/32/128 parameters and
32 proxies; extend the real Collection fixture with an opt-in renderer workload.
Record 30 warm-ups, 120 measured samples, three runs, raw nanosecond samples,
median/p95, configuration and counters. Capture a reference before runtime edits.
Timing remains diagnostic unless the documented Release/quiet-lane prerequisites
are independently satisfied; correctness and structural counters are hard gates.

- [x] Add and execute the CPU baseline and retain reference samples.
- [x] Add and execute Collection reference sampling when Vulkan is available.
- [x] Record exact reference revision, settings, raw paths and authority.

### Stage 1: Prebind parameter layout indices

Depends on Stage 0. Scope: Engine representation builder and render proxy only,
plus focused tests and the runtime contract. Build a binding index once for a new
program or parameter GUID/type sequence; subsequent value changes use checked
indexed writes and no repeated linear parameter/field searches.

Validation: mixed numeric/texture values match GUID builder output; stale,
coalesced and malformed publications preserve behavior; program, GUID order and
type changes cannot reuse stale indices. An unchanged schema builds one index
across repeated value publications. Rerun the identical CPU workload and compare
raw median/p95 without promoting Debug results to a Release claim.

- [x] Implement checked prebinding and structural counters.
- [x] Pass focused proxy/representation regressions and candidate sampling.

### Stage 2: Cache Collection-backed persistent draw commands

Depends on Stage 1. Scope: Renderer mesh uniform/binding preparation, existing
Collection GPU fixture and runtime documentation. Resolve current Collection
snapshots before command reuse. Retain immutable allocations across unchanged
frames; value changes rebuild only dependent bindings while reusing commands.

Validation: warm unchanged frames hit the persistent command cache and perform
no material surface preparation; value-only changes still hit commands, refresh
bindings and change output; repeated frames reuse those bindings. Alternate
worlds, including equal version numbers, without leaking values. Preserve
Forward, GBuffer and masked-shadow output; incompatible schemas reject before
reuse. Cover absent-world defaults, texture/generation invalidation and resource
retirement through focused and affected regressions. Execute the Collection
candidate workload and a workspace all build for the shared Engine API changes.

- [x] Implement immutable Collection allocation reuse and binding refresh.
- [x] Pass Collection output/caching regressions and affected tests.
- [x] Run all build, document contracts, validate documents and commit.

### Stage 3: Improve cache lookup and retention

Scope: the private renderer resource-slot cache and its StaticMesh, GBuffer and
sampler consumers. Replace linear lookup with a hash index using exact equality,
and insertion-order eviction with constant-time recency tracking. Preserve slot
indices for diagnostics, failure memoization, generation dependencies and copied
resource ownership. Bound both entry count and accounted resident slot bytes;
accounting includes inline key/payload, charged index/recency nodes and hash
buckets, excluding diagnostic strings, transitive shared shader/RHI allocations,
allocator overhead and driver memory. Keep persistent draw
and Collection allocation caches unchanged in this stage.

Validation: freeze a backend-free workload before runtime edits, with a 512-slot
capacity and 256/512/1024/4096-key working sets. Measure hot access mixed with cold
admissions and cyclic over-capacity access, three runs, five warm-ups and 30 raw
samples per scenario. Report median/p95, evictions, recreation requests and
retained bytes. Debug/Tracy results are diagnostic only: no Release latency gate
is selected or claimed in this stage. Structural gates require exact collision
handling, bounded count/bytes (including lowered budgets), hot-key retention,
generation/failure behavior and prepared resources surviving eviction/reset.
Run focused slot tests and renderer invalidation/Vulkan regressions, then all
build for renderer consumer integration. Release qualification remains deferred
until a quiet Release/Tracy-off lane is available; do not infer it from Debug.

- [x] Capture the unchanged implementation's scale workload.
- [x] Implement indexed recency retention, accounting and consumer migration.
- [x] Pass correctness and renderer integration gates; compare raw samples.
- [x] Document accounting limits, validate documents and commit.

The first indexed candidate used `std::list::splice` for recency. Its Debug
integer-key hot sweeps regressed at 256/512 keys despite faster over-capacity
access. The final implementation uses links embedded in stable heap-owned slots,
preserving the same LRU decisions and exact index equality.

### Stage 4: Add Primitive and Instance numeric data

Scope: bounded typed numeric channels in primitive/instance publication and MIR
reads, integrated with existing instancing/GPU culling rather than new material
owners per object. Validation: distinct per-instance pixels, WPO/pass parity,
correct removal/reordering, reduced binding fragmentation and actual draw-count
comparison against dynamic instances at equal output.

Selected scope: 32 finite float channels each for Primitive and Instance data,
zero defaults, typed Float/Float2/Float3/Float4 graph reads with checked constant
indices, immutable game-to-render publication, and runtime StaticMesh instance
snapshots with stable nonzero IDs and local transforms. Empty instance lists draw
nothing; a null list retains ordinary single-mesh behavior. ID-based updates
survive removal/reordering. Numeric changes create no material variants. Reuse
Local geometry GPU grouping/culling; direct/Spline/shadow paths read the same
primitive data. Instance lists are Local StaticMesh only. Physics instancing,
asset serialization, editor instance painting and automatic WPO bounds expansion
are outside this runtime slice; callers must provide conservative WPO bounds.

Validation: reject invalid indices/types, nonfinite data, singular/non-affine
transforms and duplicate IDs atomically. Check MIR identity and Cook compatibility.
Real Vulkan gates cover distinct pixels, WPO and Forward/GBuffer/masked-shadow
parity, updates after warm caching, reorder/removal, binding fragmentation and
actual draw counts versus equal-output dynamic material instances. Run material
and renderer regressions, workspace all-build and document validation. Retain
reports under Build; no frame-time speedup claim is required.

- [x] Implement bounded data and ordered scene/component publication.
- [x] Implement typed graph/MIR reads and direct/instanced shader transport.
- [x] Pass CPU, Cook and real Vulkan output/grouping regressions.
- [x] Complete all-build, update contracts, validate documents and commit.

Stage 4's first Vulkan run passed direct pixel/pass parity but exposed that GPU
plan discovery visits only Opaque, despite existing Masked GPU pipeline and
submission support. Include compatible Masked draws in the same bounded grouping
plan, with the custom-data mask and shadow fixture as its acceptance gate.
Translucent sorting and Spline grouping eligibility remain unchanged.

The variable-WPO MID reference also exposes inconsistent dynamic-uniform
classification between material vertex and fragment shaders. Classify the
optional vertex Material uniform consistently, but keep its population in
material composition rather than geometry binding. Preserve strict shader-layout
conflict rejection. The full Vulkan case and material regressions own this gate.

### Stage 5: Select and share shader-stage products (deferred)

Scope: explicit geometry/pass usages, editor/Game stage sets and stage-level
dependency identities. Validation: all declared uses render and Cook source-free;
missing uses fail admission rather than compile in Game. Count stage artifacts,
cold/warm compiler work and retained bytes; prove pixel-only changes reuse
compatible vertex artifacts without weakening atomic required-set admission.

### Stage 6: Extract uniform expressions (deferred)

Scope: frequency analysis and a bounded numeric preshader product, initially for
pure material-parameter arithmetic. Validation: CPU/GPU value parity with explicit
float tolerances and finite behavior, correct parameter/World/view invalidation,
no illegal derivative or stage movement, and measured shader work versus CPU
evaluation/upload cost on simple and complex UV graphs.

### Stage 7: Share cooked shader code across packages (deferred)

Scope: content-addressed shared code, package references, preload/chunk ownership
and deterministic patch order. Validation: equal code occurs once in a selected
library, all material families load without sources, missing/corrupt chunks fail
deterministically, and package size/read amplification/cold-load latency are
measured together.

### Stage 8: Prepare PSOs before first visibility (deferred)

Scope: collect upcoming component/material/factory/state combinations during
loading and submit through the existing asynchronous pipeline cache. Validation:
compare first-use incomplete requests and replay wait spans, cold/warm loading
costs, memory and image parity; preserve cancellation, target and device-generation
rules. Required combinations must be selected from actual scene usage.

Stages 5-8 remain sequencing guidance outside the authorized Stage 0-4 slice.
Each requires workload selection and an explicit measurement budget before implementation.
