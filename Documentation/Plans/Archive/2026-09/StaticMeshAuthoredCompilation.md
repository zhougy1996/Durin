# StaticMesh Authored Compilation Plan

Summary: Add typed cancellable StaticMesh build outcomes and Engine-owned asynchronous authored compilation with guarded publication.

Last reviewed: 2026-09-07

Status: Archived
Completed: 2026-09-07

## Current Status

Stages 0–4 are complete. Engine owns bounded, cancellable authored compilation;
PostLoad and interactive import/reimport schedule work, explicit callers finish
only selected meshes, and cook captures detached target products. Publication
qualifies current owner/provider facts and installs complete render/collision
and provenance before one consumer refresh. Cooked residency remains separate.

Final validation passed all 36 affected routine targets across Stage 3–4,
including 107/107 StaticMesh tests and 26 authored-compilation cases. The full
MacOS-arm64-Debug-DurinEditor `all` build passed in 13.07 s. Lifecycle contracts
and bounded diagnostic snapshots are published in their owning domains. The
[qualification record](#stage-4-qualification) separates capture, worker,
publication, allocation sampling and reservation accounting.

No macOS application smoke or GPU qualification ran, following repository
policy. Win64 Game cooked payloads were validated on the macOS host; native
Win64 execution was not run. CPU publication timings use native fixtures without
GPU initialization and do not impose a wall-clock bound on editor resource
preparation or component scans. These host limitations do not leave a required
acceptance gate open.

## Goal

Make authored render/collision construction cancellable and schedulable without
blocking ordinary editor PostLoad or interactive rebuild. Publish only current,
complete candidates on GameThread and preserve last-known-good assets on failure.

## Selected Design

- Engine owns a typed StaticMesh compiling manager registered with
  FAssetCompilingManager for DStaticMesh. Reuse the existing aggregate, Core task
  system, bounded modular-feature invocation, and cache helpers; do not create
  a generic import-job framework or merge authored work into cooked loading.
- Providers remain synchronous value-only recipes inside bounded feature calls.
  Add typed Succeeded/Failed/Cancelled build outcomes and borrowed execution
  controls for cancellation and metrics. No provider-owned callback, task, or
  object dependency survives the invocation. Update feature version and callers
  together when changing the provider ABI.
- Capture immutable source, normalization, material reconciliation, collision
  settings, target, and provider facts before worker dispatch. Material object
  bindings remain owner-thread state; workers receive stable value metadata.
- A worker builds a complete render/collision candidate. Publication must not
  trigger an expensive collision rebuild on GameThread. The owner thread
  rechecks object lifetime, request serial, input identity, material/settings
  freshness, and dependencies before one validated application boundary.
- Manager request serials enforce latest-wins. Accepted source/settings/material
  changes invalidate incompatible outstanding work. Failed validation must not
  cancel valid existing work. Do not add a second persisted authored generation
  or repurpose cooked/GPU/collision revision counters.
- Accepted requests deliver exactly one GameThread terminal result: Succeeded,
  Failed, Cancelled, or Superseded. Rejection before acceptance is synchronous.
  Cancellation prevents application even if a provider finishes late; a valid
  cache write already in progress may complete without changing the live asset.
  Cache persistence failure remains an operation diagnostic, not build failure.
- Bound active/pending work, decoded input and output bytes, retained terminals,
  and completion work per pump. Admission uses validated source estimates and
  includes collision allocations. Freeze policy and oversized-request behavior
  in Stage 0 using measured fixtures.
- PostLoad schedules authored work; getters and scene preparation remain free
  of blocking builds. Explicit synchronous callers join/finish the relevant
  request through the same candidate logic. Cook captures detached products
  without dirtying or replacing the authored asset. Scene import keeps its
  synchronous detached all-or-nothing publication contract.
- Standalone editor import/reimport may prepare physical inputs synchronously,
  then await typed build completion. Apply before save; a save failure leaves
  the valid applied state dirty for retry. Do not invent rollback machinery
  for workflows that do not have an external compensating transaction.

## Implementation Stages

### Stage 0: Freeze API, publication, and budget contracts

Depends on the source residency plan's completion.

- [x] Audit all synchronous callers, source/settings/material mutation paths,
  collision candidate construction, component refresh, save, cook, and editor
  lifetime ownership. Record which callers schedule and which explicitly finish.
- [x] Define request/result/control APIs and candidate ownership including both
  render and collision; enumerate every freshness qualifier and invalidation
  site, including edits during an initial PostLoad build.
- [x] Select measured count/byte/pump budgets and oversized-request rejection or
  explicit blocking policy; specify provider retirement and shutdown ordering.
- [x] Define component/thumbnail/Inspector behavior while data is pending and
  the exact callback/save/close behavior for standalone import adapters.

Completion: API, publication ordering, caller map, and measurable bounds are
recorded with no unresolved scheduling decision.

### Stage 1: Type outcomes and make detached recipes cancellable

Depends on Stage 0.

- [x] Replace bool-only render/collision outcomes with typed results and bounded
  diagnostics; update Engine, StaticMeshBuild, standalone/Scene import, and cook.
- [x] Add cancellation checks before expensive work and within long geometry,
  acceleration, and collision loops, with measured checkpoint granularity.
- [x] Construct a combined detached candidate and separate atomic owner-thread
  application from CPU recipe work. Preserve material and collision coherence.
- [x] Test cancellation before build, during render, during collision, and before
  application; missing/ambiguous provider and corrupt input are failures, while
  cache write failure preserves a usable product with a diagnostic.

Completion: synchronous paths retain behavior and expensive work is cancellable
without publishing partial results.

### Stage 2: Implement the Engine compiling manager

Depends on Stage 1.

- [x] Register the StaticMesh route and implement bounded admission, worker
  scope, completion mailbox, selected finish/cancel, and diagnostics retention.
- [x] Capture value-only inputs; track request serial and deterministic identity;
  apply current candidates only on GameThread and emit aggregate success once.
- [x] Wire mutation invalidation and object destruction/package retirement.
  Stop admission, cancel, drain callbacks/workers, and release feature calls
  before module/task shutdown; test manager reinitialization.
- [x] Add deterministic barrier-based tests for supersession, mutation during
  build, duplicate/late completion, destroy/unload, provider retirement, mixed
  aggregate compilers, fairness, and count/byte bounds. Avoid timing-only races.

Completion: every accepted request terminates once; stale work cannot mutate
assets or emit success; bounded ownership drains at shutdown.

### Stage 3: Integrate PostLoad, editor operations, and explicit barriers

Depends on Stage 2.

- [x] Switch authored PostLoad and interactive rebuild to scheduling; preserve
  last-known-good render/collision data and refresh registered consumers once
  a candidate is accepted. Keep cooked loading on its existing manager.
- [x] Adapt standalone import/reimport completion, provenance application,
  conflict controls, close/destruction behavior, dirty marking, and save errors.
- [x] Keep explicit synchronous entrypoints and Scene detached orchestration
  using shared build logic; prevent duplicate work and finish-all barriers.
- [x] Make cook wait only where required, then capture its target-qualified
  detached result; verify no authored bytes, dirty state, or residency mutation.
- [x] Test save failure, failed reimport, edits during compilation, missing
  physical source on rebuild, source-dependent DDC fallback, and CPU/GPU
  readiness separation across Inspector, preview, thumbnail, and components.

Completion: normal editor consumers do not block on authored recipes; explicit
barrier callers remain correct and runtime cooked behavior is unchanged.

### Stage 4: Qualify and publish lifecycle contracts

Depends on Stage 3.

- [x] Run affected build, import, collision, compilation, cook, component, and
  resource-lifetime coverage using the repository workflows; include shutdown
  and cancellation under load with bounded diagnostics and retained bytes.
- [x] Record GameThread capture/publication cost separately from worker cost;
  demonstrate expensive source decode/recipes stay off ordinary PostLoad and
  normal completion publication does not run collision construction.
- [x] Update asset compilation, asset lifecycle, async editor operations, and
  StaticMesh rendering contracts; supply the diagnostic snapshot API needed by
  [payload inspection](../../StaticMeshPayloadInspection.md).
- [x] Record actual target results and outstanding host limitations; complete
  only when required acceptance gates are satisfied.

Completion: functional, lifetime, budget, and consumer evidence is recorded.

## Stage 0 Decisions And Handoff

These are selected implementation contracts, not claims of implemented async
behavior. No package-format change or additional authored generation is needed.

### Caller And Mutation Audit

| Current boundary | Selected behavior |
| --- | --- |
| `DStaticMesh::PostLoad` in `StaticMeshCook.cpp` | Validate metadata and schedule canonical authored input at background priority. Never acquire geometry on GameThread. An asset without canonical input and without provenance remains an empty asset. Invalid canonical input is a synchronous rejection. |
| `BuildStaticMeshSynchronously` overloads | Explicit selected barrier: join identical current work; otherwise submit and finish this asset. Preserve bool compatibility by mapping the typed terminal result. Never finish unrelated assets. |
| `DStaticMeshFactory::FactoryCreateFromFile` and `CreateTransientStaticMeshFromFile` | Keep the object-returning synchronous factory contract through the selected barrier. The editor import dialog needs a separate completion adapter to avoid this barrier in interactive use. |
| `DStaticMeshFactory::Reimport` / `ReimportFromFiles` | Prepare physical inputs synchronously, then submit the detached candidate and complete asynchronously. |
| `ReimportStaticMesh` / `ReimportStaticMeshFromFile` | Existing explicit bool/save APIs remain selected synchronous wrappers around the same preparation and candidate logic. |
| `SceneDirectImport.cpp` | Keep detached synchronous all-or-nothing orchestration and pass its borrowed cancellation predicate into both recipes. `SceneImport.cpp` translates the plan; actual build/application calls are in `SceneDirectImport.cpp`. |
| `ContributeToCook` / `SerializeCooked` | Remove the `PostLoad` fallback that currently mutates missing authored render data. Finish only a relevant pending source mutation, then build/capture a target-qualified detached render/collision projection. Serialize that projection without publishing it or changing authored source residency/dirty state. |
| `SetCollisionSourceMode`, `SetCollisionQueryPolicy`, `RebuildCollision` | Keep explicit synchronous APIs; interactive callers use the combined async request. An initial pending build accepts valid new settings and replaces its request even before CPU render data exists. |
| `SetRenderData`, `SetImportedRenderData` | Validate the entire replacement before invalidation. Successful explicit replacement supersedes outstanding work; private manager application consumes its own request without self-invalidation. |
| `RenameMaterialSlot`, `SetImportedDefaultMaterial` | Validate first, mutate, then invalidate incompatible work. Requeue an initial pending build with updated facts so an edit cannot leave the asset permanently empty. No-op edits do not supersede work. |
| `SetBodySetup`, direct `DBodySetup` setters | Include body identity, mode, policy, primitive settings/revision in freshness. Notify the owning mesh after accepted direct body edits; ignore the manager's own validated geometry installation. |
| Reflection/transaction restoration of source, normalization, materials or body | Compare current reflected value facts again before application, even if a setter notification was bypassed. If stale, supersede and requeue from current valid canonical input; never publish the old candidate. |
| `PublishAssetImportData` / import settings and hint edits | Owner-thread provenance qualifier; validated edits supersede incompatible source operations. Source hints are provenance, not recipe input or a DDC fallback. |
| `BeginDestroy` and package unloading | Cancel by generation-safe handle before resource release. `UnloadPackage` currently garbage-collects before retiring package resources; no manager strong object reference may prevent that collection. Detached bulk handles may drain after retirement, but cannot publish. |

`NormalizedSize`, source, slots, and body are private reflected fields; there is
no public normalization setter today. `GetBodySetup()` does expose mutable body
settings. Checking only mesh setters is therefore insufficient.

### Typed Inputs, Terminal Delivery, And Publication

- `EStaticMeshBuildStatus` is `Succeeded`, `Failed`, or `Cancelled` for detached
  Engine and provider calls. `EStaticMeshCompilationStatus` adds `Superseded`.
  Never infer cancellation from diagnostic text. Missing/ambiguous/retired
  providers, invalid input and incompatible products are failures.
- `FStaticMeshBuildExecutionControl` borrows a cancellation predicate and metrics
  sink for one synchronous invocation. No borrowed state or provider callback
  escapes it. Bump `IStaticMeshBuildProvider::FeatureVersion` with all provider
  implementations, fake providers and callers. CPU PhysicsCore construction
  needs its own borrowed control propagated into hull and triangle/BVH loops;
  checking only before and after those calls is insufficient.
- A combined detached candidate owns canonical source, value-only reconciled slot
  metadata, normalized render buffers/bounds, completed ray acceleration, and
  simple/complex collision geometry plus mode/policy. Material bindings stay
  exclusively in the owner-thread application record; the existing
  `FStaticMeshReconciliationSnapshot` contains `TObjectPtr` bindings and cannot
  simply be moved into a worker request unchanged.
- Capture owner and package generation-safe handles, current source identity,
  requested source identity, normalization, ordered slot metadata and material
  handles, body identity/settings, import-data identity/state, target platform
  and profile, and provider descriptor plus registration lifetime identity.
  The current and requested source identities differ during reimport. Recheck
  all captured owner facts before publication; resolve material handles only on
  GameThread. DDC producer versions do not replace provider lifetime checks.
- Admission validates source headers, enums, bindings, provider availability and
  reservation before advancing the manager's per-object request serial. Rejected
  requests neither cancel existing work nor invoke completion. Accepted requests
  enqueue exactly one terminal callback on GameThread, including superseded
  requests. Do not invoke callbacks recursively from submission or under locks.
- Cancellation and supersession detach publication eligibility immediately, but
  a late worker and its bytes remain accounted until consumed. Do not release a
  reservation merely because the observer has received its terminal result.
  A valid cache write already underway can finish; cache persistence diagnostics
  do not turn a valid CPU product into failure.
- Worker finalization must include payload/slot/LOD validation, bounds, ray tree,
  and collision construction on both cold and cache-hit paths. Current
  `SetRenderData` copies a payload for validation, and
  `CommitRenderDataCandidate` recalculates bounds, builds ray trees and collision;
  the async application seam must bypass these already-completed CPU operations.
- Before the first live mutation, validate fresh owner facts and candidate
  compatibility and prepare any required owned body/provenance object. Then
  install source, slots/bindings, normalization, render and collision together.
  Resource preparation failure preserves the old state. No fallible recipe or
  metadata validation remains after the first mutation.
- Use one `FStaticMeshRenderStateRecreateContext` for both first publication and
  replacement; first publication currently omits this context. It refreshes
  registered StaticMesh and SplineMesh consumers. Emit aggregate success once
  only after successful application; GPU readiness remains independently tracked.
  Measure the context's global object scan and resource preparation separately
  from worker cost rather than claiming constant-time publication.

### Measured Fixture And Selected Bounds

`FStaticMeshAuthoredCompilationTests.RepresentativeCandidateBudgets` generates
nondegenerate grid triangles with missing normal/tangent/UV channels, clears
source residency, and exercises cold detached render, ray acceleration and
triangle collision. It uses an isolated DDC directory and does not persist.
The following single-run observations are macOS arm64 Debug measurements on
2026-09-07, not portable performance thresholds or peak-allocation measurements:

| Triangles | Canonical bytes | Render payload bytes | Ray retained bytes | Collision retained bytes | Decode + render | Ray | Collision |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 201 | 540 | 148 | 388 | 0.876 ms | 0.101 ms | 0.776 ms |
| 100,000 | 4,800,153 | 13,200,400 | 2,759,296 | 10,248,808 | 1,365.330 ms | 536.605 ms | 1,614.367 ms |

Select these initial configurable manager defaults:

| Resource | Limit and accounting rule |
| --- | --- |
| Running workers | 2 Core tasks; no additional pool. |
| Outstanding records | 32 total, including pending, running, mailbox and terminal-awaiting records. Superseded late workers retain their record charge. |
| Reserved bytes | 1 GiB across all accepted records; maximum 512 MiB per request. Pending input, decoded arrays, render buffers, collision/ray temporaries and products, cache buffers and mailbox candidates share the reservation. |
| Initial reservation | Checked arithmetic: `1 MiB + 64 * canonical wire bytes + 1024 * mesh count + 32768 * material slot count`. The large fixture reserves 308,292,160 bytes. The multiplier is a conservative policy allowance for absent channels, copies and collision/ray construction, not a measured peak. |
| Provider expansion | Validate decoded counts against wire metadata before allocation; enforce reservation against predicted render/acceleration/collision allocations before construction. Recheck capacities and retained bytes before mailbox publication. A provider that cannot fit fails without application. Stage 2 must instrument these allocations; an after-the-fact size check alone does not prove the bound. |
| Oversized request | Synchronous rejection before acceptance, including explicit manager-backed synchronous wrappers. No silent over-budget or inline GameThread fallback. Existing detached Scene/cook APIs retain their explicit caller-owned limits. |
| Ordinary pump | At most 2 terminal deliveries and a 2 ms soft admission deadline, further limited by aggregate quota. Once a publication begins it completes atomically; the deadline does not promise an individual commit takes less than 2 ms. |
| Fairness | FIFO within background/interactive classes, at most 4 interactive dispatches before an eligible background request. Oldest admitted request reserves its bytes before newer requests can overtake it. |
| Retained history | 128 terminal diagnostic records, at most 4 KiB text each; no source, product, callback or material binding retained in terminal history. |
| Cancellation checkpoints | Initially at most 1024 vertices/indices or 256 triangles per linear-loop checkpoint, and every BVH/hull partition. Check before bulk/cache work and after non-interruptible I/O. Stage 1 measures checkpoint counts and cancellation latency; sorting and allocation require explicit bounded handling. |

Existing format limits (1 GiB authored canonical data, 256 MiB ray acceleration,
2 million collision triangles and 256 convex input vertices) remain additional
limits, not admission reservations. Convex input above its existing limit is a
failure; do not silently substitute triangle collision or simplify geometry.
Stage 2 adds all-channel, many-section, malformed-count, convex and saturated-queue
coverage to validate worst-case accounting. Stage 4 measures capture/application
cost and peak allocation under concurrent cancellation separately.

### Consumer, Adapter, And Shutdown Decisions

Inspector remains read-only and can close while background PostLoad is pending.
It displays pending/terminal CPU diagnostics alongside the existing independent
GPU status. Preview/components retain last-known-good data; an initially empty
asset waits for success and the registered-consumer refresh. Thumbnail polling
returns pending while authored work exists and requests GPU resources after CPU
success; it must not call an authored finish barrier or report missing bounds as
failure while pending. Existing cooked loading keeps its own manager and policy.

The standalone interactive operation owns its completion state in
AssetForgeBuiltins/StaticMeshEditor, never in a provider. Prepare source bytes
and a detached provenance value first. The current `PrepareImportData` modifies
an existing import-data object before mesh application; replace that ordering.
Prevalidate/create detached owner-thread provenance before mesh application,
then publish it at the same validated application boundary. Apply before save;
save failure keeps valid applied state dirty and reports failure for retry.
The adapter does not compensate or roll back a valid applied mesh.

Disable conflicting source/settings/import/save controls while an interactive
operation is active and reject explicit closing of its importing dialog.
Inspector closing does not cancel unrelated background work. Adapter destruction
detaches UI notification before canceling its own request; terminal delivery still
occurs to the adapter state exactly once and cannot dereference a destroyed
widget. Object/package destruction prevents application and save. Cancelled,
superseded and failed builds never invoke save. Factory reimport completions map
the typed outcome to the existing factory result vocabulary without retaining a
factory object pointer. Save work is not performed under manager locks.

Register `Durin.StaticMesh` for `DStaticMesh` through the existing aggregate.
Shutdown closes admission first, signals cancellation for every accepted request,
joins the Engine task scope, consumes late results/terminals on GameThread,
releases callbacks and bounded diagnostics, then releases registration. Finish-all
is a shutdown/global barrier only. Provider feature calls remain bounded and
retirement rejects new invocations while draining existing calls; no provider
function, vtable-owned result deleter or callback survives feature release. Engine
aggregate shutdown precedes provider/module unload and Core task admission closure.
Reinitialization starts with empty queues/diagnostics and fresh serial ownership.

Stage 0 validation: the named native fixture passed (1/1). `test affected`
passed CookedMeshLoadingTests, DerivedDataCacheTests, StaticMeshTests and
StaticMeshThumbnailTests (4/4 targets). `doc validate --scope changed` passed.
No application smoke or GPU qualification was required for this contract and
CPU fixture change.

## Stage 1 Completion And Handoff

The public detached Engine/provider calls now return `FStaticMeshBuildOutcome`
with `Succeeded`, `Failed`, or `Cancelled`. Existing explicit bool workflows
retain their contextual success checks. Provider ABI is version 2; Scene passes
its borrowed cancellation callback and maps typed cancellation to its existing
canceled diagnostic category. Engine clears output on every failed/cancelled
operation, caps returned error and cache diagnostics at 4096 bytes, and checks
again after recipe/cache work so a late cancellation cannot return a usable
candidate. Cache persistence failure remains a successful product diagnostic.

Borrowed controls record checkpoint counts. Render validation, normals,
tangents, channel processing and geometry assembly check every 256 work units.
PhysicsCore adds a distinct `Cancelled` diagnostic and borrowed synchronous
cancellation for convex/triangle construction; triangle cleanup, BVH record and
bounds loops, sort comparisons and partition boundaries can unwind scratch
ownership without returning geometry. Cancellation exceptions are local to each
synchronous implementation and do not cross its public call boundary.

Validation: `FStaticMeshAuthoredCompilationTests.*` passed 3/3 tests, including
pre-build, late terminal, multiple deterministic render checkpoints, collision
cleanup/BVH checkpoints, and convex pre-build cancellation. `test affected`
expanded to all routine native targets after the PhysicsCore public-interface
change and passed 72/72 targets on macOS arm64 Debug. Characterization and GPU
qualification were excluded by the repository runner. The full `all` build
also passed using the same MacOS-arm64-Debug-DurinEditor profile. No application
smoke ran.

The continuation adds `FStaticMeshAuthoredBuildRequest` with value-only slot
metadata and a sealed `FStaticMeshAuthoredCandidate`. Source, completed render,
ray acceleration and collision products move together. Snapshot material bindings
stay on the owner thread. Application checks source identity, normalization,
ordered slot metadata/bindings, body handle/revision and collision settings before
mutating anything; cancellation is rechecked immediately before the application
boundary. Manager-level request serial, package/dependency and provider-lifetime
qualification remains Stage 2 work.

`CommitRenderDataCandidate` consumes completed CPU work for the sealed path and
updates canonical source/normalization before registered consumers refresh.
First authored publication uses the same refresh scope as replacement; the
existing cooked first-publication policy remains unchanged. A duplicate bounds
scan in resource preparation was removed. The legacy mutable
`ApplyStaticMeshBuildResult` still validates/rebuilds and remains an explicit
synchronous compatibility API, not the future manager's completion seam.
PostLoad, explicit synchronous builds, standalone import and detached Scene
orchestration now construct the sealed candidate before application.

Canonical decode checks before bulk I/O, after I/O, before array allocation and
every 256 decoded/validated elements; cancellation publishes no source residency.
Ray construction checks triangle/bounds work, node counting, sort comparisons
and partitions and rejects over-budget retained layout before scratch allocation.
Collision identity hashing streams canonical bytes in 256-element batches instead
of retaining the complete encoded geometry solely to hash it. No wire/schema or
DDC identity change is intended.

The expanded authored suite passed 8/8 cases: completed collision/ray identities
survive application with the provider unloaded, stale material/body snapshots
and pre-application cancellation preserve live state, source decode and ray
construction stop at deterministic checkpoints, missing/ambiguous providers fail,
and cache persistence failure still returns a usable product. The 100,000-triangle
combined fixture recorded `build_ns=3679786750`, `publication_ns=58125`,
`checkpoints=272814`, `maximum_checkpoint_gap_ns=1015147334` on macOS arm64 Debug.
These are single-run diagnostic timings without GPU initialization, not
cross-host thresholds or a claim of bounded cancellation latency. The continuation
passed all 28 `test affected` targets, the full `all` build, changed-document
validation and `git diff --check`. No application smoke or GPU qualification ran.

Final continuation: render and collision payload APIs now carry borrowed
cancellation through conversion, serialization and parsing, finite/index/section
validation, ordinal lookup, and collision topology/BVH reconstruction. Bounds
construction is cancellable for detached candidates; material reconciliation scans
check during matching, and section extrema share the checked index assembly loop.
Library allocation/copy and container/archive work have entry/exit checks but are
not preemptible. Temporary stack sampling located the remaining long gaps around
container packing and archive copies; the sampling instrumentation was removed.
No generic serialization framework, wire format or DDC key change was introduced.

The authored suite passed 9/9 cases, including deterministic mid-encode and
mid-decode cancellation for both payload types, unchanged caller output, restored
cache-hit behavior, and a one-shot cancellation predicate that cannot be erased
by cache fallback. The complete fixture recorded `build_ns=3698354833`,
`publication_ns=60125`, `checkpoints=331954`,
`maximum_checkpoint_gap_ns=129534834`; three cancellation samples recorded
`maximum_cancel_to_return_ns=11836084`, including scratch destruction. This
qualifies cooperative checkpoint granularity, not a hard latency guarantee for
indivisible library calls or arbitrary hosts. Worst-case reservation/expansion
accounting and concurrent cancellation remain the explicit Stage 2/4 gates.
Final validation passed all 72 routine `test affected` targets, the full `all`
build, changed-document validation and `git diff --check` on macOS arm64 Debug.
No application smoke or GPU qualification ran.

Standalone provenance transaction restructuring, target-qualified cook capture,
manager admission/lifetime and consumer scheduling remain in their original later
stages.

## Stage 4 Qualification

The authored suite contains 26 cases; the complete StaticMesh target passed
107/107 tests. The final affected run covered Stage 3–4 changes from `25f0c9e24`
and passed all 36 routine targets on macOS arm64 Debug (19.47 s build,
25.49 s tests). This includes import, Scene orchestration, collision, cooked
loading/cook, aggregate compilation, package retirement, material/StaticMesh/
spline consumers, thumbnails, texture adapters, renderer and resource lifetime.

Deterministic barriers cover two running workers, 32-record saturation,
byte/oversize rejection, supersession, provider retirement, reflected/direct
mutation, initial edits at capacity, explicit cancellation suppressing requeue,
late-byte retention, shutdown/restart and 128-entry history eviction. Added
all-channel/128-section and convex-limit fixtures verify both successful products
and failure preserving the previous mesh. Latest diagnostic selection now uses
request identity across retained workers and terminal history, so an older late
worker cannot hide a newer successful publication. Cold/warm/persistence-failure
observations remain value-only; polling neither starts compilation nor acquires
source residency. Prepared provenance validation finishes before live mutation.

The concurrent fixture holds two complete 100,000-triangle render/ray/triangle
collision candidates at the mailbox, cancels one, and verifies its reservation
is retained until actual worker exit. The other publishes with completed
collision. Worker phase hooks assert execution off GameThread; completion
callbacks assert GameThread execution. The sealed-application test also publishes
with its provider unloaded and preserves ray/collision identities, demonstrating
that application cannot reconstruct those products through the provider.

Final affected-run observations (single macOS arm64 Debug run):

| Observation | Value |
| --- | --- |
| Two-request reservation | 616,583,552 bytes |
| Sampled default malloc-zone peak in-use bytes, 1 ms interval | 339,524,256 bytes |
| Default malloc-zone in-use bytes at held mailbox | 219,395,104 bytes |
| Process peak resident set | 502,054,912 bytes |
| Owner capture, one concurrent request | 5,417 ns |
| Detached worker, same request | 3,932,148,375 ns |
| Owner publication, same request | 51,292 ns |
| Complete standalone candidate / CPU publication | 3,698,080,459 ns / 66,625 ns |
| Maximum measured cancellation checkpoint gap | 124,928,750 ns |
| Maximum cancel-to-return over three checkpoints | 11,866,166 ns |

Allocation samples include unrelated allocations in the default malloc zone
and can miss shorter peaks; resident-set high water includes the whole test
process and prior tests. These measurements qualify the conservative allocation
envelopes with concurrent fixtures, not a hard allocator quota or proof of peak
allocation for arbitrary providers. Reservation accounting itself is exact and
returns to zero after drain. Initial physical import preparation remains an
explicit synchronous step. Ordinary PostLoad only captures metadata; decoded
geometry, render/ray/collision work belongs to workers. Publication measurements
include CPU preparation and consumer refresh in native fixtures without GPU
initialization; a populated editor's global component scan/resource fence can
cost more and is not promised to fit the 2 ms soft pump deadline.

The full `all` build subsequently passed in 13.07 s on the same profile.
No application smoke, GPU qualification or native Win64 execution ran.

Long-lived contracts now live in Asset Compilation, Asset Data Lifecycle,
Async Asset Operations and StaticMesh Rendering. The read-only manager snapshot
supplies request/source/provider identities, optional render/collision DDC
observations, bounded diagnostics and separate capture/worker/publication costs
for the payload-inspection plan. Missing/evicted observations have request ID
zero, and optional products remain unavailable until observed.

## Validation And Contract Owners

Use [build workflow](../../../Agents/BuildAndRun.md) and
[testing workflow](../../../Agents/Testing.md), resolving native targets at execution.
Contracts: [asset compilation](../../../Runtime/Assets/AssetCompilation.md),
[asset lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md),
[async editor operations](../../../Editor/Architecture/AsyncAssetOperations.md), and
[StaticMesh rendering](../../../Runtime/Rendering/StaticMeshRendering.md).

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshBuild.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshBuildProvider.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshBuildProvider.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCollision.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCompilingManager.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedMeshLoadManager.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.cpp`
- `Engine/Source/Developer/StaticMeshBuild/`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Source/Runtime/PhysicsCore/Private/Collision/CollisionGeometry.cpp`
