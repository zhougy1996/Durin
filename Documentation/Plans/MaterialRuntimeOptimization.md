# Material Runtime Optimization Plan

Summary: Measure material publication and Collection rendering, prebind parameter layout indices, and retain mesh commands across Collection value updates.

Last reviewed: 2026-10-16

Status: Completed
Completed: 2026-10-16

## Current Status

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
Stages 0-2 are complete; Stages 3-8 remain deferred follow-up guidance.

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
shader/pipeline identities. Execute the first three stages in this plan.

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

### Stage 3: Improve cache lookup and retention (deferred)

Scope: indexed renderer slot lookup, measured eviction policy and byte budgets.
Validation: compare 256/512/1024/4096-entry working sets, hot hits and cyclic
over-capacity access; report median/p95, evictions, recreation requests and retained
bytes. Preserve generation invalidation and live-resource ownership. Select a
Release/noise budget before implementation; changing FIFO to LRU alone is not a
completion condition.

### Stage 4: Add Primitive and Instance numeric data (deferred)

Scope: bounded typed numeric channels in primitive/instance publication and MIR
reads, integrated with existing instancing/GPU culling rather than new material
owners per object. Validation: distinct per-instance pixels, WPO/pass parity,
correct removal/reordering, reduced binding fragmentation and actual draw-count
comparison against dynamic instances at equal output.

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

Stages 3-8 are sequencing guidance for separate follow-up plans, not acceptance
gates or implementation scope for the executed Stage 0-2 slice. Each requires
workload selection and an explicit measurement budget before implementation.
