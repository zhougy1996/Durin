# Cooked Mesh Render Readiness and Async Loading Plan

Summary: Guarantee first-consumer render-resource initialization for cooked meshes, then move cooked StaticMesh and SkeletalMesh payload residency and CPU construction off the GameThread.

Last reviewed: 2026-08-30

Status: Archived
Completed: 2026-08-30

## Current Status

All stages are complete. Cold cooked StaticMesh, SplineMesh, and SkeletalMesh
consumers now start or join bounded asynchronous residency without a warm-up
getter. CPU publication is generation-qualified and GameThread-only; GPU
initialization remains render-thread work, and a queued-resource proxy
converges without a second component mutation. Explicit blocking and retry
operations share the same detached codecs and publication rules.

Historical Stage 0 regressions were captured before the implementation and are
preserved by the first-consumer package and Vulkan tests. The prior 4 MiB cold
BulkData baseline recorded 9.95 ms metadata load and 19.77 ms first access;
these machine-local numbers motivated removing the whole interval from ordinary
component registration rather than freezing a wall-clock threshold. The final
manager exposes cumulative read-ready, worker decode/build (including cooked
collision), and GameThread completion time plus current/peak retained bytes.
The selected production limits are eight active and eight pending requests,
256 MiB for each active/pending/completion byte domain, sixteen I/O polls, and
four GameThread completions per pump. Deterministic qualification saturates the
same 8-way count/byte ratio at a scaled allocation, proves the four-completion
bound, and retains the one-valid-oversized-request policy without allocating a
256 MiB test payload. One diagnostic run recorded 72,863 us controlled read
readiness, 13 us worker work, and 36 us GameThread completion work; the read
number deliberately includes test barriers and is not a performance threshold.

Concurrent qualification combines StaticMesh and SkeletalMesh requests at the
configured boundary while superseding a generation, cancelling an unloaded
owner, retiring a package resource, destroying an object, publishing surviving
results, and shutting down. Final diagnostics prove zero live flights, pending
requests, mailbox results, and retained bytes. The skeletal bounded-domain run
also exposed and repaired the last integration caller that expected
`GetPayloadData()` to load implicitly; it now opts into the explicit blocking
boundary.

Final focused validation passed `CookedMeshLoadingTests` (2/2),
`StaticMeshTests` (75/75), `SplineTests` (40/40), `SkeletalAssetTests` (36/36),
`StaticMeshRenderPreparationVulkanTests` (2/2),
`SkeletalMeshRenderResourcesVulkanTests` (1/1), and
`CoreConcurrencyTests` (142/142). Bounded `asset-workflow`, `static-mesh`, and
`skeletal-mesh` domain selections pass. Lasting contracts live in
`AssetDataLifecycle.md` and `RenderResourceLifecycle.md`; this plan is execution
evidence only.

## Goal

Make every cooked mesh converge from an unloaded CPU payload to renderable GPU
resources when its first real consumer is a component, without requiring a
warm-up getter or a second render-state mutation. After correctness is proven,
ordinary runtime requests must not perform package I/O, payload decompression,
CPU render-data construction, or cooked collision construction on the
GameThread.

## Scope

- Cooked `DStaticMesh` payload, optional cooked collision, CPU RenderData, GPU
  resources, and StaticMesh/SplineMesh component publication.
- Cooked `DSkeletalMesh` payload, CPU RenderData, GPU resources, pose-dependent
  component publication, and parity with the StaticMesh request contract.
- Explicit blocking compatibility for loading screens, tools, focused tests,
  and callers that require immediate CPU availability.
- Bounded asynchronous package reads, worker-side decoding and candidate
  construction, GameThread publication, component invalidation, cancellation,
  stale-result rejection, diagnostics, and shutdown.
- Deterministic correctness, failure, lifecycle, concurrency, and Vulkan
  render-resource coverage plus measured GameThread-stall qualification.

## Non-Goals

- Mesh streaming, partial LOD residency, eviction, sparse buffers, or runtime
  CPU-data discard policy.
- Changing cooked payload, chunk, collision, package, or `.dbulk` formats.
- Making authored-package DDC recovery or editor import/reimport asynchronous.
- Moving RHI resource creation away from the rendering thread or waiting for
  GPU readiness on the GameThread.
- Generalizing the work into a family-neutral asset compilation framework.
  Cooked residency is runtime loading, not derived-data compilation.
- Changing Skeleton compatibility, animation evaluation, mesh deformation, or
  collision query semantics.

## Selected Design and Invariants

### Public access contract

- `GetRenderData()` becomes a side-effect-free snapshot query by the end of
  the migration. It never starts I/O, waits, decodes, publishes, or retries.
- A clearly named `EnsureRenderDataAndResourcesBlocking(...)` operation owns
  the synchronous sequence: materialize CPU data, publish it, then idempotently
  queue GPU initialization. Its name and result make GameThread blocking and
  failure explicit.
- `RequestRenderDataAndResources()` is the normal runtime operation. It is
  GameThread-only, idempotent, non-waiting, and returns one coherent status
  snapshot. Repeated component/proxy requests coalesce onto the current asset
  generation rather than launching duplicate I/O or worker work.
- A request result distinguishes CPU payload phase from GPU resource phase.
  CPU phases cover `Unloaded`, `IoQueued`, `Reading`, `Decoding`, `CpuReady`,
  `Failed`, and `Cancelled`; GPU phases continue to distinguish unavailable,
  queued, ready, and failed. A non-zero generation/revision accompanies both
  snapshots so consumers can reject stale observations.
- Failure is sticky for the current cooked payload generation. Per-frame
  component or proxy requests do not retry corrupt or missing payloads.
  Explicit retry or a new asset generation is required.

### Correctness ordering

- CPU RenderData publication happens-before the transition to GPU
  `InitializationQueued`; the render command captures only the published
  candidate and its matching resource revision.
- A valid CPU candidate may back a SceneProxy while GPU initialization is
  queued. Renderers continue to reject unready LODs per frame and begin drawing
  after the same resources report ready; components do not require a second
  proxy recreation solely for the queued-to-ready transition.
- A component returns no proxy while CPU data is unavailable. Successful
  GameThread publication invalidates registered StaticMesh, SplineMesh, or
  SkeletalMesh consumers so a previously absent proxy is recreated.
- Cooked collision and StaticMesh RenderData are decoded into one detached
  result and publish as one current-generation transaction. Physics-state
  recreation observes the same successful publication boundary.
- Existing authored/editor paths with already resident RenderData retain their
  current immediate CPU behavior and use the same idempotent GPU queue helper.

### Asynchronous ownership and thread boundaries

- Engine owns one cooked-mesh runtime load manager with family-typed request
  and result values. It is separate from `FAssetCompilingManager`: no cooked
  residency request emits asset-compilation success or acquires a Developer
  module dependency.
- The manager admits package reads only after satisfying explicit request-count
  and estimated-byte limits. It retains the package-resource requests, worker
  task handles, completion mailbox, cancellation sources, attribution, and
  diagnostics until each request is terminal.
- Package I/O uses the existing asynchronous package-resource boundary.
  GameThread pumping polls completion and only consumes an already-ready read;
  it never calls a blocking `Wait()` for an unfinished request. CPU workers do
  not occupy Core worker slots waiting for file I/O.
- Workers receive owned immutable payload bytes plus detached material-slot,
  collision-policy, target, Skeleton-compatibility, and generation values.
  They never resolve or mutate a `DObject`, component, package, BodySetup,
  render resource, or live Skeleton.
- Workers validate and decode payloads and construct move-only CPU candidates.
  Only the GameThread may publish candidates, collision geometry, diagnostic
  state, resource revisions, component invalidation, and render commands.
- Large move-only results use a manager-owned bounded completion mailbox rather
  than the bounded `GameThreadDeferred` payload channel. The Engine frame pump
  drains a bounded number of completions and performs current-generation
  publication before normal component render-state reconciliation.

### Identity, cancellation, and lifetime

- Each request captures a weak object handle, object-handle generation,
  monotonically increasing mesh-load generation, runtime target, cooked field
  metadata identity, material/collision or Skeleton compatibility snapshot,
  and expected resource revision.
- Component reassignment does not cancel shared mesh loading; asset unload,
  destruction, package-resource retirement, runtime shutdown, or a newer mesh
  generation cancels or supersedes it. Cancellation is advisory; the complete
  identity comparison at GameThread publication is the correctness boundary.
- Stale, cancelled, failed, or destroyed requests never mutate an object,
  publish collision, enqueue RHI work, invalidate a component, or overwrite a
  newer diagnostic.
- Shutdown closes request admission, cancels package requests and worker work,
  drains terminal results without normal frame budgets, rejects publication to
  destroyed objects, and reaches quiescence before package resources, the Core
  task system, render-command admission, or Engine code are retired.
- `BeginDestroy()` prevents new requests for the asset and participates in the
  manager's selected-object finish/cancel boundary. RenderData destruction
  still follows the existing render-command fence and deferred resource
  lifetime contract.

### Blocking compatibility after migration

- The blocking API and non-blocking API share the same codecs, validation,
  candidate publication, diagnostics, and GPU queue helper; there is no second
  payload implementation.
- Once the manager exists, blocking calls join or submit the asset's current
  request and explicitly finish that one object while pumping required
  GameThread completions. They do not start a duplicate synchronous decode.
- Blocking is prohibited from ordinary `CreateSceneProxy()`, component tick,
  renderer preparation, and implicit getters. Loading screens and tools must
  opt in at a visible call site.

## Implementation Stages

### Stage 0: Lock correctness contract and reproduce the first-consumer failure

- [x] Add a cooked StaticMesh fixture whose loaded asset still has no resident
  CPU RenderData and whose first data consumer is
  `DStaticMeshComponent::CreateSceneProxy()`; do not warm it through
  `GetRenderData()`.
- [x] Prove the current failure by recording the initial unavailable state, the
  created proxy with unready LOD0, and the failure to reach GPU-ready after the
  render queue drains.
- [x] Add equivalent first-consumer characterization for SplineMesh and
  SkeletalMesh, including SplineMesh's queued-resource proxy rejection.
- [x] Freeze the blocking API result, non-blocking request/status snapshot,
  CPU/GPU phase vocabulary, retry semantics, and generation rules in public
  headers or contract tests before implementation callers spread.
- [x] Measure cold read, decode/build, collision, and GameThread publication
  time plus payload/candidate/result bytes for representative and maximum-
  bounded fixtures; select initial request-count, byte-budget, and per-frame
  completion limits from that evidence.

#### Acceptance Gate

- Tests fail for the actual first-consumer ordering without a test-only state
  mutation, and the selected API/state contract has explicit thread, failure,
  identity, retry, memory, and ordering semantics.

### Stage 1: Fix synchronous first-consumer correctness

Dependencies: Stage 0 contract and failing coverage; asynchronous foundations
are not a dependency.

- [x] Implement the shared blocking sequence so cooked CPU data is loaded and
  validated before GPU initialization is queued; make repeated calls
  idempotent for unavailable, queued, ready, and failed states.
- [x] Migrate StaticMeshComponent, SplineMeshComponent, and
  SkeletalMeshComponent SceneProxy creation to the shared sequence instead of
  independently ordering `InitResources()` and `GetRenderData()`.
- [x] Permit StaticMesh and SplineMesh proxies with valid CPU data while GPU
  resources are queued; retain renderer-side per-LOD readiness rejection until
  initialization completes.
- [x] Make decode and GPU initialization failures observable and sticky rather
  than silently returning a null pointer and retrying through future getters.
- [x] Update cooked tests so no test relies on a warm-up getter, and prove each
  first consumer reaches `Ready` after render commands drain.
- [x] Audit direct StaticMesh/SkeletalMesh `InitResources()` callers and either
  migrate them or make the operation itself enforce its CPU-data prerequisite.

#### Acceptance Gate

- A cold cooked StaticMesh, SplineMesh, and SkeletalMesh each create or
  recreate the expected proxy and reach renderable GPU resources on their first
  consumer path. Corrupt payloads terminate in one diagnosed failure without
  retry storms, and authored/editor behavior remains unchanged.

### Stage 2: Extract worker-safe cooked mesh products and manager foundations

Dependencies: Stage 1.

- [x] Split StaticMesh render/collision decoding and SkeletalMesh payload/
  RenderData construction into pure functions that consume owned immutable
  inputs and return detached move-only products with structured failures.
- [x] Preserve byte-for-byte and structure-for-structure equivalence between
  the blocking baseline and worker-safe codecs across valid, truncated,
  incompatible, oversized, and compressed payloads.
- [x] Add the Engine-owned cooked-mesh load manager with bounded admission,
  asynchronous package reads, Core task scheduling, cancellation, task
  attribution, a bounded completion mailbox, and coherent diagnostics.
- [x] Implement weak-object plus generation-qualified GameThread publication,
  selected-object finish/cancel, package-retirement handling, startup unwind,
  and shutdown drain ordering.
- [x] Add deterministic manager tests with controlled I/O/task barriers for
  coalescing, limits, cancellation, supersession, exactly-once terminal state,
  stale-result rejection, and shutdown; do not use timing sleeps.

#### Acceptance Gate

- Detached codecs match the synchronous baseline, the manager never accesses
  managed objects from workers, request/byte limits are enforced, and every
  admitted request reaches exactly one terminal outcome across cancellation,
  stale publication, package retirement, and shutdown.

### Stage 3: Migrate cooked StaticMesh to the non-blocking request path

Dependencies: Stage 2.

- [x] Start or join `RequestRenderDataAndResources()` from StaticMeshComponent
  and SplineMeshComponent assignment/registration, with CreateSceneProxy as an
  idempotent fallback trigger rather than a blocking load site.
- [x] Publish current StaticMesh RenderData and optional collision atomically on
  GameThread, queue GPU initialization immediately, then invalidate registered
  StaticMesh/SplineMesh render and physics state through the existing recreate
  boundary or a measured replacement.
- [x] Remove cooked I/O and decoding side effects from StaticMesh
  `GetRenderData()` and migrate every caller that depended on implicit loading
  to either request, observe, or explicitly block.
- [x] Preserve a CPU-ready/GPU-queued proxy and renderer readiness behavior;
  prove the queued-to-ready transition requires no second component mutation.
- [x] Integrate retry, diagnostics, unload, destruction, package retirement,
  runtime reinitialization, and blocking compatibility through the manager's
  one request identity.

#### Acceptance Gate

- Cold cooked StaticMesh and SplineMesh component registration perform no
  blocking package read, wait, payload decode, collision build, or RenderData
  construction on GameThread. Successful completion recreates missing proxies
  and reaches rendering/collision readiness; failure and unload leave no stale
  proxy, physics state, task, completion, or render command.

### Stage 4: Migrate cooked SkeletalMesh and qualify family parity

Dependencies: Stage 3 and its manager/lifecycle evidence.

- [x] Add SkeletalMesh typed requests that snapshot required Skeleton
  compatibility and mesh metadata without retaining worker access to live
  Skeleton or component state.
- [x] Trigger non-blocking requests from SkeletalMeshComponent assignment/
  registration and use pose validity plus CPU readiness independently when
  deciding whether a proxy can be created.
- [x] Remove cooked I/O and build side effects from SkeletalMesh
  `GetRenderData()` and `GetPayloadData()` callers that participate in runtime
  rendering; retain explicit blocking only where immediate CPU access is a
  documented requirement.
- [x] Publish current SkeletalMesh candidates on GameThread, queue GPU
  resources, and invalidate registered skeletal consumers without losing or
  publishing a pose for the wrong mesh generation.
- [x] Cover Skeleton mismatch, pose arrival before/after CPU readiness,
  reassignment, unload, cancellation, and resource initialization failure.

#### Acceptance Gate

- Cold cooked SkeletalMesh rendering is non-blocking on GameThread, the newest
  compatible mesh/pose pair alone can publish a proxy, and StaticMesh and
  SkeletalMesh share identical request, cancellation, diagnostic, finish, and
  shutdown semantics where their payload domains overlap.

### Stage 5: Qualify performance, lifecycle, and lasting contracts

Dependencies: Stages 3 and 4.

- [x] Compare Stage 0 cold-load measurements against the non-blocking path and
  show that ordinary component registration contains no I/O/decode-scale
  GameThread interval; report bounded publication cost separately.
- [x] Stress concurrent StaticMesh/SkeletalMesh loads at the selected request
  and byte budgets while reassigning components, unloading packages, retiring
  resources, destroying objects, and shutting down Engine services.
- [x] Validate renderer fallback/rejection, resource failure, retry, device
  lifecycle, collision publication, and component recreation across every
  CPU/GPU phase.
- [x] Move the implemented residency, thread, status, failure, and shutdown
  contract into the authoritative asset/rendering documentation and leave this
  plan as execution evidence rather than a competing specification.
- [x] Run focused feature and Vulkan integration targets first, then the
  bounded domain selections justified by shared Engine/task/package lifecycle
  changes. Run broader validation only if the implementation crosses shared
  infrastructure or focused evidence is insufficient.

#### Acceptance Gate

- The measured ordinary path is non-blocking with bounded queue, memory, and
  GameThread publication work; lifecycle stress reaches zero live requests,
  results, package reads, render commands, and resources; focused and required
  cross-domain validation passes; lasting documentation owns the final
  contract.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| First-consumer correctness | Cold cooked assets are not warmed by getters; StaticMesh, SplineMesh, and SkeletalMesh converge from unavailable CPU data to GPU-ready resources after their first component request. |
| Codec equivalence | Blocking and worker-safe paths produce equivalent RenderData, material/section mapping, bounds, collision, Skeleton compatibility, and failure classifications. |
| Non-blocking GameThread | Instrumented ordinary requests never issue unfinished waits, package reads, decompression, payload decode, collision construction, or mesh RenderData construction on GameThread. |
| Admission and memory | Deterministic tests prove request-count, estimated-byte, completion, and per-frame publication bounds, including one valid oversized request policy without queue deadlock. |
| Publication | Only a live object with matching object/load/resource generations and metadata snapshots can publish CPU data, collision, diagnostics, component invalidation, and GPU work. |
| Consumers | Static, spline, and skeletal proxies behave correctly while CPU unavailable, CPU ready/GPU queued, ready, failed, cancelled, reassigned, or destroyed. |
| Lifecycle | Package retirement, unload, replacement, cancellation, Engine shutdown, render-command drain, and RHI shutdown leave no callback, task, mailbox result, non-owning RenderData pointer, or initialized resource behind. |
| Failures and retry | I/O, schema, decompression, validation, collision, Skeleton compatibility, CPU publication, and GPU initialization failures are phase-qualified, sticky per generation, and explicitly retryable. |
| Focused targets | `StaticMeshTests`, `SplineTests`, `SkeletalAssetTests`, `StaticMeshRenderPreparationVulkanTests`, and `SkeletalMeshRenderResourcesVulkanTests`, plus package/task/lifecycle targets actually changed by the implementation. |

## Definition of Done

- First-use cooked StaticMesh, SplineMesh, and SkeletalMesh rendering cannot be
  stranded by CPU-load/GPU-init ordering.
- Ordinary runtime component and SceneProxy paths perform no synchronous cooked
  mesh I/O or CPU construction on GameThread.
- Public getter, non-blocking request, explicit blocking, status, failure, and
  retry semantics are distinct and documented.
- Work is bounded, coalesced, generation-safe, cancelable, observable, and
  fully drained before dependent package, task, render, and Engine lifetimes.
- CPU-ready/GPU-queued proxies converge without polling mutations or a second
  external render-state change.
- The authoritative runtime documents describe the landed contract and all
  required focused, integration, lifecycle, and performance gates pass.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Package Bulk Data](../../../Runtime/Assets/BulkData.md)
- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Task System](../../../Runtime/Core/TaskSystem.md)
- [Render Resource Lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshRenderStateRecreateContext.cpp`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/SplineMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageResource.h`
- `Engine/Source/Runtime/Engine/Private/Asset/BulkData.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/PackageResource.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SplineMeshComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAnimationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportVulkanTests.cpp`
