# Asynchronous Texture2D Build and Readiness Plan

Summary: Move editor Texture2D source decoding and platform-data construction off the main thread while preserving transactional edits, stable render resources, and explicit readiness diagnostics.

Last reviewed: 2026-08-10

Status: Completed
Completed: 2026-08-10

## Current Status

The synchronous Texture2D pipeline is complete and documented in
[Texture System](../Runtime/Rendering/TextureSystem.md). Imports, source
provenance, usage-aware mip generation, BC compression, DDC persistence,
cooked loading, render-thread upload, material fallback, and Texture Editor
diagnostics are already production foundations rather than work remaining in
this plan.

Editor DDC misses, direct reimports, retries, and build-setting changes now
submit immutable requests to the Engine-owned bounded Texture2D coordinator.
Workers perform decode, mip generation, BC compression, validation, and DDC
persistence; the GameThread publishes only the newest matching generation.
The Texture Editor exposes readiness, timing, memory, cancellation, and wait
controls while consumers retain committed last-good or fallback content.

Transactional setting proposals, including Undo and Redo, keep their reflected
value and history cursor unpublished until asynchronous validation succeeds.
Cancellation, failure, supersession, unload, document close, and shutdown do
not publish stale candidates. The landed contract and characterization now live
in [Texture System](../Runtime/Rendering/TextureSystem.md).

## Completion Evidence

- `Win64-Debug-DurinEditor` characterization measured the full Color, Normal,
  and Data/Mask quality matrix at 1K and 4K. The 4K high-quality cases block for
  187-197 seconds synchronously. The maximum 16K size was qualified with exact
  decoded, mip-chain, and block-layout allocation bounds rather than a
  multi-hour wall-clock matrix; the runtime document labels that distinction.
- Admission is frozen at two workers, a 1 GiB estimated in-flight budget, a
  four-interactive-request burst limit, eight scanlines between mip/alpha
  checks, and 64 compression blocks between cancellation checks.
- Deterministic native tests cover payload equivalence, bounded admission,
  starvation prevention, exact-once cancellation/shutdown, phase-qualified
  decode failure, stale/superseded edits, cancelled Undo history, unload, and
  last-good behavior.
- The focused Texture, Editor Property, Material, Thumbnail, and Editor
  Rendering targets pass. The required full `all` build and a 120-tick hidden
  `DurinEditor` rendering/lifecycle smoke also pass on the selected profile.

## Goal

Make source decode, mip construction, BC compression, and derived-data writes
non-blocking for ordinary editor Texture2D loads and edits. A build result must
commit atomically on the main thread only when it still matches the asset,
source, settings, and request that produced it. Materials and editor consumers
must retain the last successful texture or the appropriate fallback throughout
queued, running, failed, cancelled, unload, and shutdown states.

## Scope

- Editor `DTexture2D` DDC-miss rebuilds, reimports, retries, and build-setting
  changes.
- Immutable build request and result values that can cross worker threads
  without accessing reflected objects.
- Bounded scheduling, cancellation, stale-result rejection, shutdown, and
  peak-memory accounting for CPU texture builds.
- Explicit queued, decoding, building, persisting, upload-pending, ready,
  failed, and cancelled diagnostics with request identity.
- Main-thread commit of source provenance, platform data, reflected settings,
  Dirty state, dependency notification, and render-resource revision.
- Texture Editor, preview, thumbnail, and material behavior while a request is
  pending or fails.
- Focused deterministic tests plus an editor responsiveness and lifecycle gate.

## Non-Goals

- Changing cooked-runtime loading, TXPL/DBLK schemas, platform formats, mip
  filters, compression algorithms, or DDC key semantics.
- Texture streaming, sparse residency, partial mip upload, or eviction policy.
  Those require measured resident-memory and workload budgets first.
- `DTextureCube`, Texture2DArray, Texture3D, or TextureCubeArray builds.
- Runtime-generated or writable texture assets.
- A general asset-build framework before a second asset type proves the same
  scheduling and commit contract.
- Moving RHI creation or upload away from the rendering thread.

## Design Decisions and Invariants

### Ownership and thread boundaries

- An Engine-owned Texture2D build coordinator owns scheduling, request state,
  cancellation tokens, concurrency limits, and shutdown. `DTexture2D` owns only
  the latest logical request identity and committed asset state.
- Workers receive value snapshots: encoded source bytes or a resolved read-only
  source path plus verified fingerprint, complete candidate build settings,
  target identity, DDC key inputs, and request identity. They never read or
  mutate `DObject`, reflection, package, editor widget, material, or render
  resource state.
- Workers may decode, build, validate, and atomically persist content-addressed
  DDC data. Result application, Dirty state, transactions, dependency
  notification, and render-resource queuing occur only on the main thread.
- Render-resource creation and upload retain the existing rendering-thread
  revision contract. CPU request identity and render revision remain distinct
  and are both reported in diagnostics.

### Request identity, cancellation, and commit

- Every request captures the asset package identity, source-content identity,
  complete build settings, target profile, and a monotonically increasing
  per-asset generation.
- A newer request, unload, destruction, package replacement, or editor shutdown
  cancels older work cooperatively. Cancellation is an optimization; generation
  and snapshot comparison at main-thread commit are the correctness boundary.
- A stale or cancelled result may populate the content-addressed DDC when its
  bytes are valid, but it cannot change the asset, package, diagnostics for a
  newer generation, or stable texture reference.
- Failure never discards the last successfully committed platform data or GPU
  texture. An asset with no successful resource continues to resolve the
  existing role-specific fallback.

### Transactional editor changes

- A build-setting proposal does not publish its reflected value before its
  asynchronous candidate succeeds. The coordinator retains the old value and
  the complete proposed value set while work is pending.
- Successful completion applies one normal reflected transaction on the main
  thread, installs the matching platform data without rebuilding it again, and
  produces one undo record and one Dirty transition.
- Failed or cancelled proposals leave reflected values, package bytes, Dirty
  state, undo history, platform data, and the stable texture target unchanged.
- Undo and redo submit the restored complete setting set through the same
  asynchronous path. The editor serializes proposals per texture document;
  requesting another change supersedes the pending proposal rather than
  creating an out-of-order transaction chain.

### Scheduling and resource bounds

- Texture builds use a bounded worker limit separate from rendering-thread
  upload. The coordinator exposes queued/running counts and estimated source,
  intermediate, and result bytes.
- Admission uses a configurable in-flight byte budget based on conservative
  decoded and mip-chain estimates. Requests that exceed the concurrent budget
  wait in FIFO order; a single valid texture larger than the budget may run
  alone so the queue cannot deadlock.
- Interactive reimport and setting changes may precede background load misses,
  but priority cannot starve an already admitted job. Exact priority and
  default budget values are frozen from Stage 0 measurements rather than
  embedded in this plan.
- Shutdown stops admission, cancels queued and running work, drains main-thread
  completions, and destroys the coordinator before the task system it uses.

### Readiness and diagnostics

- Readiness reports phase, request generation, source/build identity, queued
  time, worker time, estimated/actual bytes, and the latest matching failure.
- `Ready` means platform data committed and the matching render upload
  completed. `UploadPending` remains distinct from CPU build completion.
- Consumers observe committed state only. Pending candidate bytes are private
  to the coordinator until commit and are not exposed to previews, thumbnails,
  materials, save, or cook.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned here |
| --- | --- | --- |
| CPU build | Pure decode and mip/build helpers produce detached source and platform values. | Calls are synchronous and have no request/cancellation protocol. |
| Derived data | Versioned content-addressed keys, validated payloads, and atomic persistence. | DDC miss rebuild and persistence block the main thread. |
| Asset commit | Candidate edits commit atomically and reject invalid settings. | Validation constructs the full candidate synchronously before property publication. |
| GPU publication | Stable references, build revisions, stale-upload rejection, and last-good fallback are implemented. | CPU build identity and upload readiness are not presented as one coherent lifecycle. |
| Editor | Texture Editor shows build/upload failures and independent previews. | It cannot show queue/progress/cancellation or keep edits responsive during compression. |
| Lifecycle | Render work has ordered release and shutdown diagnostics. | Worker jobs do not yet have unload, replacement, and shutdown ownership. |

## Implementation Stages

### Stage 0: Characterize cost and freeze the asynchronous contract

- [x] Add test-only timing and peak-byte instrumentation around decode, mip
  generation, compression, DDC persistence, and main-thread commit without
  changing execution behavior.
- [x] Capture representative 1K and 4K Color, Normal, and Data/Mask builds at
  Low, Normal, and High quality on the selected Agent Build Profile, plus exact
  maximum-supported decoded/intermediate/result allocation bounds.
- [x] Record main-thread stall, worker CPU time, decoded/intermediate/result
  bytes, and concurrent-build peak memory.
- [x] Select the default worker limit, in-flight byte budget, priority ordering,
  cancellation checkpoints, and the maximum interval between checkpoints.
- [x] Freeze the request/result value types, phase model, main-thread completion
  pump, and coordinator shutdown order in code-facing documentation or tests.

#### Acceptance Gate

- The baseline identifies at least one reproducible editor-blocking build,
  accounts for the dominant memory allocations, and yields explicit scheduling
  constants and lifecycle invariants that tests can assert.

### Stage 1: Add the bounded Texture2D build coordinator

Dependencies: Stage 0.

- [x] Extract decode, build, validation, and DDC persistence into a worker-safe
  request/result path with no reflected-object access.
- [x] Implement bounded FIFO admission, interactive priority, in-flight byte
  accounting, cooperative cancellation, and a main-thread completion queue.
- [x] Add per-asset generation checks and immutable source/settings/target
  comparison before completion can commit.
- [x] Implement stop-admission, cancel, drain, and destruction ordering for
  normal shutdown and failed startup unwind.
- [x] Add deterministic coordinator tests using controlled barriers rather
  than timing-dependent sleeps.

#### Acceptance Gate

- Tests prove concurrency and byte limits, priority without starvation,
  cancellation, stale-result rejection, exactly-once completion, and shutdown
  with queued and running jobs; ThreadSanitizer-supported configurations report
  no coordinator data race when available.

### Stage 2: Migrate DDC-miss load, retry, and reimport

Dependencies: Stage 1.

- [x] Keep validated warm DDC hits and cooked-runtime loads behaviorally
  unchanged; submit only editor source decode/build misses to the coordinator.
- [x] Migrate retry and reimport so source bytes, provenance, platform data, DDC
  status, and dependency notifications commit together on the main thread.
- [x] Preserve the last successful resource during rebuild and use fallback for
  a never-ready asset.
- [x] Cancel or supersede requests across unload, deletion, rename/move,
  package replacement, source change, and repeated reimport.
- [x] Prevent save and cook from serializing a pending candidate; expose a clear
  wait/cancel decision when the operation requires that candidate.

#### Acceptance Gate

- A cold large Texture2D load and reimport keep the editor event loop
  responsive, commit only the newest matching request, preserve last-good or
  fallback rendering, and leave no callback, job, or package mutation after
  unload and shutdown.

### Stage 3: Migrate transactional build-setting edits

Dependencies: Stage 2.

- [x] Replace synchronous candidate construction in the reflected property
  hooks with the pending-proposal contract.
- [x] Make Usage, sRGB, maximum resolution, compression quality, alpha mip mode,
  and alpha threshold changes share one complete candidate snapshot.
- [x] Commit a successful proposal as one main-thread reflected transaction
  using the worker result without a duplicate rebuild.
- [x] Make failure, cancellation, document close, superseding edits, Undo, and
  Redo preserve deterministic values, Dirty state, and history.
- [x] Add Texture Editor controls for pending state, cancellation, phase,
  generation, elapsed time, and memory diagnostics.

#### Acceptance Gate

- Editing a compression-heavy texture remains interactive; success creates one
  correct transaction and resource update, while failure or cancellation
  changes no reflected value or undo history. Undo/Redo and save/reload converge
  on the same platform bytes and settings.

### Stage 4: Qualify consumer readiness and lifecycle

Dependencies: Stage 3.

- [x] Cover Material Editor, static-mesh rendering, previews, Content Browser
  thumbnails, dependency refresh, and role-specific fallback for every phase.
- [x] Stress concurrent loads and repeated superseding edits under the selected
  byte budget while deleting, unloading, renaming, and closing documents.
- [x] Verify failure attribution and retry recovery for source read, decode,
  build, DDC write, main-thread commit, RHI creation, and upload failures.
- [x] Compare Stage 0 main-thread stall and peak-memory measurements against the
  asynchronous implementation and publish the resulting contract in Texture
  System documentation.
- [x] Run the smallest affected native targets during development, then the
  required full `all` build and normal editor rendering smoke because this is a
  user-visible editor change.

#### Acceptance Gate

- Representative cold builds spend no compression-scale interval on the main
  thread, measured peak bytes stay within the selected admission rule, all
  consumers show last-good or deterministic fallback content, lifecycle stress
  leaves no live jobs or stale commits, focused tests pass, and the full editor
  build and smoke validation succeed.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Pure build | Identical platform payload and DDC key for synchronous baseline and worker path across usages, formats, qualities, NPOT sizes, and alpha modes. |
| Scheduling | Deterministic admission, byte bounds, priority, starvation prevention, cancellation checkpoints, and exactly-once completion. |
| Commit | Main-thread-only mutation, generation/source/settings/target match, one transaction, one Dirty transition, and no stale dependency notification. |
| Consumers | Last-good or role fallback behavior for material, scene, preview, and thumbnail paths through every CPU and GPU phase. |
| Lifecycle | Reimport storms, superseding edits, unload, delete, package replacement, document close, failed startup, and normal shutdown. |
| Failures | Source, decode, build, persist, commit, create, and upload failures remain request-qualified and recoverable. |
| Performance | Stage 0 versus final main-thread stall, total worker time, queue latency, and peak estimated/actual bytes on the same inputs and profile. |

## Definition of Done

- Ordinary editor DDC-miss builds, reimports, retries, and build-setting changes
  no longer execute source decode, mip generation, or BC compression on the
  main thread.
- The coordinator has bounded, observable CPU and memory concurrency and a
  proven unload/shutdown boundary.
- Only the newest matching result can alter asset, transaction, dependency,
  render-resource, and diagnostic state.
- Pending, failed, cancelled, upload-pending, and ready states are visible and
  consistent across the Texture Editor and material consumers.
- The runtime Texture System documentation owns the landed contract and all
  required focused, full-build, editor-smoke, and measurement gates pass.

## Deferred Follow-ups

- Extend the coordinator to `DTextureCube` only after Texture2D qualification
  and a measured cube-build stall justify it.
- Generalize asset-build scheduling only after another asset type demonstrates
  compatible request, commit, and lifecycle requirements.
- Add residency accounting before proposing texture streaming or mip eviction.
- Plan array/volume textures only with a selected renderer or asset consumer.

## Related Documentation

- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [RHI Resource Views and Transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureBuild.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/Texture2DAssetThumbnail.cpp`
