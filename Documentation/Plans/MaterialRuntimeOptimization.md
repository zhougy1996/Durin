# Material Runtime Optimization Plan

Summary: Measure material publication and Collection rendering, prebind parameter layout indices, and retain mesh commands across Collection value updates.

Last reviewed: 2026-10-16

Status: Active
Completed:

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
Cook schema changes. Stage 2 is next; the final all build remains outstanding.

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

- [ ] Implement immutable Collection allocation reuse and binding refresh.
- [ ] Pass Collection output/caching regressions and affected tests.
- [ ] Run all build, document contracts, validate documents and commit.

### Stage 3: Follow-up optimization backlog

Deferred scope: cache indexing/eviction and memory budgets; Primitive/Instance
Custom Data; usage-selected shader stages; Uniform Expression/Preshader products;
cross-package shader libraries; proactive PSO preparation. Select each only after
Stages 0-2 measurements identify a workload and a separate acceptance budget.
No implementation of these follow-ups is authorized by this plan's first slice.
