# Engine Termination Lifecycle Plan

Summary: Add a UE-shaped pre-exit, asynchronous DObject destruction drain, module teardown, and final render-resource barrier before stopping the rendering thread.

Last reviewed: 2026-07-29

Status: Active
Completed:

## Current Status

Durin already has most low-level mechanisms required for deterministic
termination:

- `DObject` exposes `BeginDestroy`, `IsReadyForFinishDestroy`, and
  `FinishDestroy`;
- garbage collection retains objects whose asynchronous cleanup is incomplete
  and reports `DeferredDestroyObjectCount`;
- `FRenderCommandFence` can delimit accepted rendering work;
- the render command pipe has `Running`, `Draining`, and `Stopped` admission
  states;
- `FinalizeRenderingThreadBeforeRHIExit` atomically closes admission behind a
  final render-resource and RHI audit;
- `FRenderResource` reports destruction while initialized and the final engine
  exit path asserts that resource, command, deferred-cleanup, and RHI-delete
  queues are empty.

The missing layer is the process-wide owner protocol above those mechanisms.
`FEngineLoop::Exit()` currently:

1. shuts down Mona and drains the CPU thread pool;
2. removes and garbage-marks `GEngine`;
3. shuts down the asset manager;
4. calls `CollectGarbage()` once;
5. flushes rendering commands;
6. unloads modules;
7. flushes again, closes render-command admission, audits, and stops rendering.

One garbage-collection pass cannot complete a UE-shaped asynchronous
destruction sequence. A resource-owning object may enqueue release work in
`BeginDestroy`, return false from `IsReadyForFinishDestroy`, and become ready
only after the following render flush. No second purge currently invokes
`FinishDestroy`. Module code can therefore unload beneath deferred objects, and
the final RenderCore audit can discover live resources only after ordinary
producers have already lost their cleanup opportunity.

The immediate StaticMesh shutdown error is evidence of a missing asset-owned
release path, not a reason to make the engine enumerate StaticMesh buffers
directly. The
[Static Mesh Render-Data Lifecycle](StaticMeshRenderDataLifecycle.md) plan owns
that asset-specific correction. This plan owns the process-wide guarantee that
all such cleanup begins and drains while the rendering thread and defining
modules remain available.

Stage 0 is complete from baseline commit
`4bf2f6414a0dd42e5e12e649da0f52e2200db02b`. The initial working set is:

- `Documentation/Plans/EngineTerminationLifecycle.md`;
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`;
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`;
- `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`;
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectLifecycle.h`;
- `Engine/Source/Runtime/Core/Public/CoreGlobals.h`;
- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`;
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`.

The first scheduler-controlled regression now records the synthetic shutdown
destruction checkpoints before release submission, while its fence is pending,
before readiness, before `FinishDestroy`, and at physical destruction. It
proves that the current one-pass exit collection leaves the object deferred and
that later collection passes do not repeat `BeginDestroy`.

The current exit ownership graph is pinned as follows:

| Current operation | State owned | Required coordinator boundary |
| --- | --- | --- |
| `MonaShutdown` | UI frame production, windows, Mona renderer viewports | producer quiescence and consumer detach |
| `ShutdownEngineThreadPool(true)` | accepted CPU work and its result publication | producer quiescence |
| remove and garbage-mark `GEngine` | engine, world, actor, component, viewport, and scene object hierarchy | consumer detach then DObject drain |
| `ShutdownAssetManager` | loaded packages, asset registry/cache references, asset publication | producer quiescence and root release |
| `CollectGarbage` | virtual DObject destruction and physical object storage | repeated shutdown object drain |
| `UnloadModulesAtShutdown` | reverse-order module callbacks and module-static owners | only after zero deferred objects |
| render flush and finalization | accepted commands, render-resource cleanup, RHI deletes, command admission | module drain then atomic final closure |
| `ShutdownApplicationCore` | platform application state | after rendering and RHI stop |

Static loading traces pin normal editor module order as `RenderCore`,
`VulkanRHI`, `MonaImGui`, `Renderer`, `MainFrame`, `LevelEditor`,
`MaterialEditor`, then `TextureEditor`. `UnloadModulesAtShutdown` sorts by
descending load index, so normal shutdown callbacks run in the exact reverse
order. The final render command is inserted while admission is `Running`,
changes admission to `Draining` under the command-pipe mutex, audits the
resource registry and cleanup queue, submits and drains RHI deletion, and only
then permits `ShutdownRenderingThread` to mark admission `Stopped`.

The owner inventory and selected release channels are:

| Ownership class | Current owners | Selected quiesce/release owner |
| --- | --- | --- |
| permanent/intrinsic DObject | reflected types and intrinsic packages | survivor audit; these must never acquire render affinity |
| rooted DObject | `GEngine`, loaded packages, preview/thumbnail lights, temporary editor roots | engine, asset, preview/thumbnail, and editor owners remove their own roots before the DObject drain |
| DObject render resource | texture references/resources and StaticMesh buffer aggregates | the concrete DObject `BeginDestroy` lifecycle |
| scene/proxy | engine worlds, renderer scenes, component proxies, preview and thumbnail scenes | engine/editor scene owner queues detach before asset release |
| subsystem/module-static | Mona viewports, renderer state, editor caches and preview services | pre-exit callback or owning `ShutdownModule` |
| deferred cleanup | `FDeferredRenderResourceCleanup` records | RenderCore flush after the originating owner releases |
| RHI deferred delete | RHI resources released by render commands | final RHI submit before `RHIExit` |

Exit-time producers are Mona/application ticks and viewport changes; engine
world/component registration and scene mutation; asset load/save and texture
resource builds; asynchronous import/build result publication; thumbnail and
preview scheduling/upload; renderer lazy StaticMesh initialization; accepted
thread-pool tasks; and module callbacks. Stage 1 must give each producer an
owner-specific rejection or drain callback before any roots or modules are
released.

Focused validation passed for
`FCoreDObjectReflectionTests.ShutdownDestructionRequiresPostFenceGarbageCollectionPasses`,
and the complete `all` target built successfully on
`Win64-Debug-DurinEditor-Tests`.

The reproducible baseline command is:

```powershell
.\DevTool.bat run --project Sandbox\Sandbox.dproject --args `
  --hidden-window --exit-after-ticks=3 --termination-lifecycle-baseline
```

The bounded-tick option requests `RequestEngineExit()` from the ordinary main
loop, and the baseline option snapshots state at entry to `FEngineLoop::Exit`
before the first shutdown owner runs. The 2026-07-29 sample contained 93
`DObject` instances: 8 rooted and 73 permanent. The rooted set was the four
intrinsic packages (`CoreDObject`, `Engine`, `AssetCore`, and `DurinEd`),
`EditorEngine`, and the three loaded project packages (`NewLevel`,
`Mesh_Teapot`, and `TEXCUBE_PureSky_512x512`). The permanent set consisted only
of reflected `DClass`, `DStruct`, and `DEnum` objects; none appeared in the
render-resource owner diagnostics.

At the same boundary the render registry held 8 initialized resources and no
pending deferred cleanup: one `FTextureReference` and one
`FTextureCubeResource`, both owned by
`/Game/Textures/TEXCUBE_PureSky_512x512`, plus the six StaticMesh buffers
`FPositionVertexBuffer`,
`FStaticMeshVertexBuffer::FTangentsVertexBuffer`,
`FStaticMeshVertexBuffer::FTexcoordVertexBuffer`, `FColorVertexBuffer`,
`FStaticMeshVertexBuffer`, and `FRawStaticIndexBuffer`. The six mesh resources
still report an unspecified diagnostic owner. During the existing single GC
pass, those six buffers were destroyed while RHI-initialized; the process then
failed with Windows status `0xC0000005` before the final RenderCore audit and
the normal `"Durin Engine exited."` marker. This is the frozen failure baseline,
not a successful smoke test.

Stage 0 handoff:

- baseline commit:
  `4bf2f6414a0dd42e5e12e649da0f52e2200db02b`;
- continuation evidence commit:
  `d7ce8805`;
- working set: this plan, `Launch.cpp`, `LaunchEngineLoop.cpp`,
  `BuildAndRun.md`, the CoreDObject lifecycle implementation/API, the
  RenderCore finalization path, module manager, and the scheduler regression in
  `ReflectionTypeTests.cpp`;
- key decision: automation requests ordinary exit after a bounded tick count,
  and the optional entry snapshot is read-only; asset-specific buffer release
  remains outside `FEngineLoop`;
- open question carried into Stage 1: each producer needs an idempotent
  quiescence callback before any root release, while the unspecified StaticMesh
  diagnostic owner should be made concrete by its owning lifecycle plan;
- validation: scheduler regression passed, all 46 CoreDObject reflection tests
  passed, full `all` build passed, changed-document validation passed, and the
  expected failing runtime baseline above was captured.

Stage 1 is complete on top of continuation commit `7a17255f`. Core now owns
`EEngineExitPhase`, the testable `FEngineExitCoordinator`, and the
`FOnEnginePreExit` registration API. `FEngineLoop::Exit` advances every phase
at its selected boundary and rejects re-entry; the coordinator publishes the
new phase before synchronously invoking pre-exit callbacks in registration
order. Registration is rejected once quiescence begins.

Process exit phase is deliberately not a leaf-level admission API. The engine
thread pool, asset manager, renderer, and thumbnail schedulers own explicit
local admission state and close it from their pre-exit callbacks. Accepted CPU
tasks drain through the thread pool; thumbnail schedulers cancel their queued
and in-flight publication. DObject-owned producers do not query pre-exit:
component registration follows the object's pending-kill state, and `DTexture`
closes render-resource build admission in `BeginDestroy`, invalidates its
revision, and transfers resource storage to deferred cleanup. Texture cleanup
is no longer first submitted from its C++ destructor. Downstream scene,
resource, preview, and render-command APIs remain available for teardown.

Focused coordinator/quiescence validation passes 3 tests. The complete Texture
and Thumbnail targets pass 69/69 and 45/45, and all 27 package asset tests pass.
A full `all` build succeeds on `Win64-Debug-DurinEditor-Tests`. The bounded
hidden-window regression logs owner-local quiescence for the thread pool, asset
manager, and renderer before advancing through `DetachingRenderConsumers`,
`DrainingObjects`, `UnloadingModules`, and `ClosingRenderAdmission` in order.
Texture resources now release through `BeginDestroy`; the process still
terminates with the frozen `0xC0000005` StaticMesh baseline before
`RenderingStopped`, as expected until the object-drain and StaticMesh
resource-release stages are implemented.

Stage 1 handoff:

- baseline commit: `7a17255f`;
- working set: this plan, Core exit coordination and thread-pool admission,
  AssetCore manager admission, Renderer scene admission, thumbnail producer
  shutdown, and Engine DObject teardown paths;
- key decision: the process exit phase is orchestration and diagnostics only;
  producers own local admission, while DObject cleanup is closed by
  `BeginDestroy` or an explicit object unload operation;
- open question carried into Stage 2: the shutdown-only object drain must
  alternate GC and render flushes without adding a global phase dependency to
  resource-owning DObject subclasses;
- validation: focused admission tests, all Texture/Thumbnail/package tests,
  full `all` build, and the expected failing runtime baseline above.

## Goal

Make normal engine termination a deterministic, diagnosable protocol in which:

- high-level producers stop before object and render-resource teardown begins;
- scene and proxy consumers detach before asset-owned resources release;
- unreachable resource-owning `DObject` instances receive `BeginDestroy` while
  render-command admission remains open;
- garbage collection and rendering work are alternately drained until every
  asynchronous object destruction is ready and finalized;
- module-owned non-`DObject` resources release before module code unloads;
- no module unloads while a deferred object may still dispatch its virtual
  cleanup through that module;
- final render admission closes only after ordinary cleanup has converged;
- rendering-thread and RHI shutdown occur only after final audits pass.

The terminating process does not need to destroy every permanent or rooted
`DObject` before stopping rendering. Any surviving object must have no
initialized render resource, no pending render work or retirement, and no
post-render-shutdown destructor behavior that touches RenderCore or RHI.

## Scope

- Add an explicit game-thread engine-exit coordinator and observable exit
  phases.
- Add a UE-like pre-exit notification while modules, `DObject`, RenderCore, and
  RHI are still operational.
- Define producer quiescence for editor/preview work, CPU tasks, asset loading,
  scene mutation, resource initialization, and module callbacks.
- Add a bounded shutdown-only drain for deferred `DObject` destruction.
- Separate `DObject`-owned resource cleanup from module/global resource
  cleanup.
- Order scene detach, object release, module shutdown, command admission close,
  rendering-thread stop, and `RHIExit`.
- Add progress diagnostics for objects, fences, resources, commands, cleanup
  owners, RHI deletes, modules, and the active exit phase.
- Add deterministic tests for asynchronous cleanup and late submission.
- Update Runtime lifecycle documentation after the implementation is stable.

## Non-Goals

- Requiring `GDObjectArray` to be empty before rendering-thread shutdown.
- Adding a common base class or marker interface for every `DObject` that owns
  render resources.
- Making RenderCore inspect `DObject` subclasses or release asset resources on
  their behalf.
- Adding type-specific StaticMesh, Texture, Material, thumbnail, or viewport
  cleanup to `FEngineLoop`.
- Replacing per-owner `BeginDestroy`, subsystem shutdown, or module
  `ShutdownModule` implementations with a global resource sweep.
- Silently clearing the render-resource registry or pending queues to make the
  final audit pass.
- Allowing normal runtime GC to block on a global `FlushRenderingCommands`.
- Supporting restart of the engine, rendering thread, RHI, or module graph in
  the same process.
- Redesigning garbage-collection reachability, clustering, root semantics, or
  object allocation.
- Treating crash or forced-process termination as an orderly cleanup path.

## Design Decisions and Invariants

### One Coordinator, Ordered Exit Phases

`FEngineLoop::Exit()` remains the single process-level owner. It advances an
observable, monotonic state:

```text
Running
-> QuiescingProducers
-> DetachingRenderConsumers
-> DrainingObjects
-> UnloadingModules
-> ClosingRenderAdmission
-> RenderingStopped
-> Complete
```

Only the game thread advances phases. Re-entry is rejected. Diagnostics always
include the current phase.

RenderCore command admission remains `Running` through producer quiescence,
scene detach, object drain, and module shutdown because those phases must still
submit release work. Admission transitions atomically to `Draining` only inside
`FinalizeRenderingThreadBeforeRHIExit`, after all ordinary owners report
completion.

### UE-Shaped Pre-Exit Notification

Add a process-wide `OnEnginePreExit` multicast notification in Core. It fires
once at the beginning of `QuiescingProducers`, before roots are removed, modules
are unloaded, or render-command admission changes.

The notification is for subsystem and module owners to:

- reject new asynchronous requests;
- cancel or drain accepted producer work;
- stop late result publication;
- release roots, caches, preview sessions, and retained scenes;
- begin cleanup of non-`DObject` resources whose owner will not participate in
  garbage collection.

It is not a per-asset release broadcast. Resource-owning `DObject` subclasses
remain responsible for their own virtual destruction lifecycle.

Callbacks execute synchronously on the game thread in registration order.
Registration after `QuiescingProducers` begins is rejected. A callback may
enqueue teardown commands but may not start new user-visible work.

### Two Cleanup Channels

Termination recognizes two ownership channels:

```text
DObject-owned resources
    -> GC Conditional BeginDestroy
    -> virtual BeginDestroy
    -> release commands and fence
    -> IsReadyForFinishDestroy
    -> FinishDestroy

module/subsystem-owned resources
    -> OnEnginePreExit or ShutdownModule
    -> release commands
    -> module teardown flush
```

The engine coordinates these channels but does not know their concrete resource
types.

Every resource-owning `DObject` must begin cleanup in `BeginDestroy`, retain
concrete C++ storage until its fence completes, report readiness without
blocking normal GC, and ensure `FinishDestroy` cannot enqueue first-time release
work.

Every non-`DObject` owner must stop producers and release resources from
pre-exit or module shutdown while admission remains open.

### Consumer Detach Precedes Asset Release

Worlds, play sessions, viewports, thumbnails, previews, and renderer scenes
queue destruction of their proxies and snapshots before asset owners queue
resource release.

`DetachingRenderConsumers` closes and detaches process/subsystem consumers such
as Mona viewports, preview services, and other non-DObject owners before object
drain begins. It does not enumerate component proxies or replace object-local
destruction. `DEngine::BeginDestroy` queues release of its owned scenes, and
resource-owning DObject subclasses detach their registered consumers before
they queue resource release. Those teardown paths use stable scene/consumer
detach endpoints rather than consulting the global `GEngine` pointer.

The coordinator, not a DObject subclass, owns the render flush after the first
shutdown GC pass. `DEngine::BeginDestroy` and asset `BeginDestroy` therefore
queue their local detach/release work without performing a global flush.

The global order is:

```text
stop producer publication
-> queue scene/proxy detach
-> queue asset resource release
-> release fence
-> finish object destruction
```

Asset implementations must still detach their registered consumers during
ordinary runtime replacement or GC. The global scene drain is a shutdown
barrier, not a substitute for correct per-asset lifetime. A StaticMesh
destruction path may perform an idempotent detach of any residual registered
consumer, but it must not recreate render state after object teardown begins.

### Shutdown DObject Drain

Add a game-thread-only `DrainDObjectDestructionForShutdown` operation. One drain
round performs:

```text
CollectGarbage
-> observe swept and deferred counts
-> FlushRenderingCommands
-> CollectGarbage again
```

Additional rounds run only while deferred objects remain. Each round records:

- objects entering `BeginDestroy`;
- objects completing `FinishDestroy`;
- deferred object count and representative object identity/class;
- pending render fences and resource retirements;
- initialized render resources, pending render commands, and cleanup owners;
- elapsed time and whether the round made progress.

Normal termination uses a bounded deadline plus a no-progress threshold. A
timeout or repeated no-progress state is fatal and reports the blocking owners;
it does not skip `IsReadyForFinishDestroy`, force-delete their memory, or
continue to unload modules.

The drain ends when there are no pending `BeginDestroyed` objects awaiting
`FinishDestroy`. Permanent or still-rooted objects may remain only under the
survivor contract below.

Shutdown root-release operations, including AssetManager shutdown, do not call
`CollectGarbage` internally. The coordinator is the sole owner of shutdown GC
and render flush alternation, so it observes the first `BeginDestroy` transition
and every later readiness pass. Ordinary runtime package unload may retain its
existing local GC behavior.

This drain covers objects already participating in GC. Pending replacement
retirements owned by a still-live/rooted asset are a separate owner-local queue;
Stage 3 pumps and audits those queues before final render closure. A zero
deferred-object count does not by itself prove zero asset retirements.

### Surviving DObject Contract

Rendering-thread shutdown does not require physical destruction of every
`DObject`. Before admission closes, every surviving object must satisfy:

- it owns no initialized `FRenderResource`;
- it has no pending resource initialization, update, release, or retirement;
- no scene proxy or render command borrows its storage;
- its later `FinishDestroy` and C++ destructor do not require RenderCore, a
  renderer module, or RHI.

Stage 0 inventories permanent/rooted objects and selects an explicit pre-exit
release owner for any survivor with render-facing state. Absence from the GC
candidate set never exempts a resource from final audit.

### Module Unload Boundary

No module unload begins while deferred object destruction remains. Module
pre-exit callbacks first remove roots and external references; the DObject drain
then completes virtual destruction while module code is loaded.

Reverse-order `ShutdownModule` may release module-static and subsystem-owned
resources. Render-command admission remains open throughout module unload.
After all module shutdown callbacks return, the engine flushes accepted render
commands and verifies that module-owned resource/cleanup diagnostics are empty.

No object whose virtual destructor or `FinishDestroy` resides in an unloaded
module may remain pending.

### Final Render Closure Is Validation, Not Recovery

`FinalizeRenderingThreadBeforeRHIExit` remains the atomic transition that:

1. queues the final RenderCore/RHI audit behind every accepted command;
2. changes admission from `Running` to `Draining`;
3. rejects later submissions;
4. drains accepted commands and deferred RHI deletion;
5. permits rendering-thread stop only when the audit passes.

By this phase, ordinary cleanup must already have converged. The final command
must not enumerate assets, invoke GC, unload modules, or synthesize missing
Release calls.

Before final closure:

```text
deferred DObject destruction == 0
pending asset retirements == 0
initialized render resources == 0
pending render-resource cleanup == 0
```

After final closure:

```text
pending render commands == 0
pending RHI deletes == 0
render admission == Draining
```

Only then may `ShutdownRenderingThread` transition admission to `Stopped`, and
only then may `RHIExit` execute.

### Failure Policy

- Late high-level work after pre-exit begins is rejected with owner and phase.
- Release commands remain accepted until final admission closure.
- Enqueue after admission closure is an actionable lifecycle failure.
- A stalled fence or deferred object blocks orderly termination and reports its
  identity; it is never treated as success.
- Live RenderCore or RHI state at the final audit is fatal.
- Test and no-RHI configurations use the same ownership states but may complete
  a never-initialized resource without constructing a render fence.
- Abnormal process termination may bypass orderly cleanup but must not weaken
  normal-exit assertions.

## Current Foundations and Gaps

### Foundations

- `FEngineLoop::Exit` is already the single owner of subsystem, object, module,
  render-thread, RHI, and application shutdown ordering.
- `ShutdownEngineThreadPool(true)` already drains accepted CPU work before
  object destruction.
- `DObject` garbage collection already calls `BeginDestroy`, polls
  `IsReadyForFinishDestroy`, invokes `FinishDestroy`, and reports deferred
  destruction.
- `FRenderCommandFence` and `FlushRenderingCommands` provide FIFO completion
  boundaries.
- scenes expose explicit asynchronous `Release`.
- RenderCore already diagnoses initialized-resource destruction and performs a
  final registry/cleanup/RHI audit.
- render-command admission already closes atomically behind a final command.
- the completed Render Resource Lifetime plan established deferred concrete
  resource and RHI deletion contracts.

### Gaps

- there is no process-wide pre-exit notification or high-level exit phase;
- `CollectGarbage` runs only once before module unload;
- deferred objects receive no guaranteed post-fence purge during exit;
- module unload is not gated on completion of asynchronous `DObject`
  destruction;
- rooted/permanent render-resource owners have no exit-time inventory;
- producer rejection reports RenderCore admission state but not the higher-level
  exit phase;
- shutdown diagnostics do not identify the `DObject` or fence blocking purge;
- StaticMesh initializes nested resources from renderer traversal but has no
  asset-owned destruction protocol;
- no deterministic test pauses object cleanup between release submission,
  fence completion, readiness, final destruction, module unload, and render
  admission closure.

## Implementation Stages

### Stage 0: Freeze The Exit Graph And Blocking Owners

Dependencies: none.

- [x] Record baseline commit
  `4bf2f6414a0dd42e5e12e649da0f52e2200db02b` and the initial working set.
- [x] Trace every current `FEngineLoop::Exit` operation and record which
  subsystem, object, module, RenderCore, RHI, or application state it owns.
- [x] Inventory rooted/permanent `DObject` instances and every initialized
  `FRenderResource` owner at normal editor shutdown.
- [x] Classify render-facing owners as `DObject`, subsystem, module-static,
  scene/proxy, deferred cleanup, or RHI deferred-delete ownership.
- [x] Identify every producer that can enqueue object, scene, render-resource,
  or RHI work during exit.
- [x] Add scheduler-controlled tests that pause a synthetic `DObject` before
  release, at its fence, before readiness, and before `FinishDestroy`.
- [x] Capture baseline hidden-window exit diagnostics, including the currently
  unreleased StaticMesh resources.
- [x] Pin module unload order and the final command-admission/audit behavior.

#### Acceptance Gate

- Every exit-time producer and resource owner has one selected quiesce/release
  callback; tests reproduce the one-pass-GC deferred-destruction gap and no
  owner depends on an unspecified destructor order.

### Stage 1: Add Pre-Exit And Producer Quiescence

Dependencies: Stage 0.

- [x] Add the monotonic engine-exit phase and game-thread assertions.
- [x] Add the one-shot `OnEnginePreExit` multicast notification in Core.
- [x] Broadcast pre-exit before removing `GEngine`, shutting down the asset
  manager, collecting garbage, or unloading modules.
- [x] Register editor/preview, asset, streaming, task, scene, renderer, and
  module owners that can publish late work.
- [x] Reject new high-level resource builds, uploads, scene registrations, and
  async completions after producer quiescence begins.
- [x] Preserve release-command admission until final render closure.
- [x] Add tests for re-entry, registration during exit, late requests, accepted
  work drain, and callback ordering.

#### Acceptance Gate

- After pre-exit returns, no producer can create or publish new render-facing
  lifetime work; accepted teardown commands remain legal and diagnostics name
  the rejecting owner and exit phase.

### Stage 2: Drain Asynchronous DObject Destruction

Dependencies: Stage 1.

- [ ] Add shutdown-only GC drain rounds with render flushes between readiness
  polls.
- [ ] Detach process/subsystem consumers before object drain, while keeping
  DObject component, scene, and asset detach in their own unload lifecycle.
- [ ] Ensure scene/proxy detach executes before object-owned resource release
  can destroy borrowed storage; FIFO detach, release, and fence commands may be
  queued together.
- [ ] Make DObject teardown use stable scene/consumer detach endpoints rather
  than relying on the global `GEngine` pointer after shutdown begins.
- [ ] Split shutdown root release from AssetManager's ordinary local GC so the
  coordinator owns every shutdown collection pass.
- [ ] Preserve objects and their concrete resource storage while
  `IsReadyForFinishDestroy` is false.
- [ ] Add bounded timeout, no-progress detection, representative blocker
  reporting, and final deferred-count assertions.
- [ ] Ensure no-RHI and never-initialized objects complete without waiting on an
  impossible fence.
- [ ] Prohibit first-time release submission from `FinishDestroy` or a C++
  destructor.
- [ ] Remove the global render flush from `DEngine::BeginDestroy`; object
  teardown queues owned scene release and reports residual local work through
  readiness, while the coordinator performs the following flush.
- [ ] Add tests for one fence, multiple fences, chained readiness, no progress,
  timeout diagnostics, and successful second-pass destruction.

#### Acceptance Gate

- A release submitted from `BeginDestroy` completes before `FinishDestroy`;
  shutdown does not proceed to module unload with a deferred object; and no
  global flush is added to ordinary runtime GC.

### Stage 3: Integrate Resource Owners And Module Boundaries

Dependencies: Stage 2 and Stage 2 of
`StaticMeshRenderDataLifecycle.md`.

- [ ] Integrate StaticMesh current/pending-retirement ownership without adding
  type-specific handling to `FEngineLoop`.
- [ ] Verify Texture2D, TextureCube, material, thumbnail, viewport, scene, and
  renderer ownership against the two cleanup channels.
- [ ] Add explicit pre-exit release for permanent/rooted resource owners that
  cannot become GC candidates.
- [ ] Move module-owned roots and external-reference release before the object
  drain.
- [ ] Gate reverse-order `ShutdownModule` on zero deferred object destruction.
- [ ] Flush and audit module/subsystem-owned resources after module shutdown
  callbacks and before final render closure.
- [ ] Diagnose pending objects whose class or cleanup implementation belongs to
  a module selected for unload.

#### Acceptance Gate

- No module unloads beneath pending virtual cleanup; every initialized resource
  is attributable to an active owner channel; and editor shutdown reaches the
  final barrier with zero asset retirement and deferred destruction.

### Stage 4: Harden Final Render And RHI Closure

Dependencies: Stage 3.

- [ ] Move all recoverable owner cleanup before
  `FinalizeRenderingThreadBeforeRHIExit`.
- [ ] Extend the final audit with engine-exit phase, pending retirement, and
  deferred-object summaries.
- [ ] Prove that admission closes atomically behind all accepted module/resource
  teardown.
- [ ] Preserve deferred C++ cleanup before RHI deferred deletion ordering.
- [ ] Reject all late submissions after closure and verify the pending command
  count reaches zero.
- [ ] Stop the rendering thread only after audit success and call `RHIExit`
  only after the deferred-delete queue reaches zero.
- [ ] Add fault-injection tests for live resources, pending cleanup, late
  enqueue, pending RHI delete, and shutdown invoked twice.

#### Acceptance Gate

- Final closure is a pure validation/drain boundary; every invalid state fails
  with an actionable owner and phase; and no cleanup path needs a running
  rendering thread after `ShutdownRenderingThread`.

### Stage 5: Validate And Publish The Runtime Contract

Dependencies: Stage 4, Stage 4 of `MultithreadingV1.md`, and Stage 3 of
`StaticMeshRenderDataLifecycle.md`.

- [ ] Run focused Core, CoreDObject, Engine, RenderCore, Renderer, RHI, asset,
  StaticMesh, thumbnail, viewport, and module lifecycle tests.
- [ ] Run repeated editor startup/exit with empty, loaded, rendered,
  replacement-pending, preview, thumbnail, PIE, and project-loaded states.
- [ ] Run shutdown while accepted CPU work, render uploads, proxy removals,
  resource releases, deferred cleanup, and RHI deletes are in flight.
- [ ] Verify Debug diagnostics and non-Debug behavior on the supported runtime
  variants.
- [ ] Run the complete native suite and full `all` build through the repository
  workflow.
- [ ] Publish the stable exit phase, owner channels, object drain, module
  boundary, render closure, and failure rules in
  `Documentation/Runtime/Core/RuntimeLifecycle.md`.

#### Acceptance Gate

- Focused and full validation pass; repeated editor shutdown is clean; all
  final audits report zero pending resource-affine work; and Runtime Lifecycle
  is the authoritative lasting contract.

## Validation Matrix

| Boundary | Required validation |
| --- | --- |
| Pre-exit | One-shot broadcast, callback order, re-entry rejection, late producer rejection |
| Scene consumers | Main world, PIE, viewport, thumbnail, preview, auxiliary scenes |
| DObject lifecycle | BeginDestroy, incomplete readiness, fence completion, FinishDestroy, physical destruction |
| GC drain | One round, multiple rounds, no progress, timeout, no-RHI |
| Unique resource ownership | Current asset data, pending retirement, released survivor storage |
| CPU work | Accepted-task drain, cancellation, no late publication |
| Module lifecycle | Root release, deferred-object gate, reverse unload, module-owned resources |
| Render commands | Accepted teardown, atomic admission close, post-close rejection |
| Render resources | Initialized registry, deferred C++ cleanup, owner diagnostics |
| RHI | Deferred deletion, final submit, zero pending deletes before exit |
| Process | Repeated startup/exit, hidden window, loaded project, PIE, active previews |

## Definition of Done

- Engine termination has an explicit, observable, monotonic phase model.
- Pre-exit stops every producer before roots, objects, modules, and rendering
  infrastructure are dismantled.
- Resource-owning `DObject` instances begin asynchronous cleanup through their
  own lifecycle overrides.
- Shutdown drains deferred object destruction to completion before module
  unload.
- Surviving permanent/rooted objects satisfy the no-render-affinity contract.
- Module and subsystem resources release while command admission is open.
- Final render closure performs validation rather than type-specific recovery.
- Rendering stops before `RHIExit`, with every resource, command, cleanup, and
  deferred-delete audit clean.
- StaticMesh shutdown requires no engine-level type enumeration or manual
  test-only release.
- Full build, native tests, and repeated editor shutdown validation pass.
- Stable behavior is documented in Runtime Lifecycle and this plan is marked
  completed.

## Deferred Follow-ups

- General engine restart or renderer/RHI restart within one process.
- Incremental runtime GC scheduling improvements unrelated to shutdown.
- Parallel `BeginDestroy` or multithreaded physical object destruction.
- Per-module object-allocation provenance beyond what unload safety requires.
- Crash-path best-effort GPU cleanup.
- General asset streaming and asynchronous compilation beyond participating in
  pre-exit quiescence.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Static Mesh Render-Data Lifecycle](StaticMeshRenderDataLifecycle.md)
- [Static Mesh Render-Data Lifetime Investigation](../Investigations/StaticMeshRenderDataLifetime.md)
- [Multithreading V1](MultithreadingV1.md)
- [Render Resource Lifetime](Archive/2026-07/RenderResourceLifetime.md)

## Related Code

- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`
- `Engine/Source/Runtime/Core/Public/Delegates/Delegate.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
