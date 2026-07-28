# Static Mesh Render-Data Lifecycle Plan

Summary: Adopt UE-style unique StaticMesh render-data ownership with explicit resource initialization, render-state recreation, release fences, and deferred retirement.

Last reviewed: 2026-07-29

Status: Active
Completed:

## Current Status

`DStaticMesh` currently owns one `std::unique_ptr<FStaticMeshRenderData>`, but
`SetRenderData` replaces that pointer immediately while scene-proxy replacement
is only queued. Existing proxies retain raw `FStaticMeshRenderData*`, so the old
C++ storage can disappear before already accepted render commands stop using
it. Once the nested buffers have initialized, ordinary replacement and asset
destruction also skip their required render-thread release.

The verified evidence and UE comparison are recorded in
[StaticMesh Render-Data Lifetime](../Investigations/StaticMeshRenderDataLifetime.md).
The selected correction preserves unique ownership instead of introducing a
counted render-data handle:

- `DStaticMesh` uniquely owns its current render data;
- detached builders uniquely own unpublished candidates;
- `DStaticMesh` uniquely owns pending retirement records after replacement or
  `BeginDestroy`;
- render-state recreation removes every raw-pointer consumer before release;
- release commands precede a fence;
- old C++ storage is destroyed only after that fence completes.

The active
[Static Mesh LOD Resources Refactor](StaticMeshLODResourcesRefactor.md) has
completed its named-buffer stage and is about to add vertex factories. This
lifecycle plan is its prerequisite because each additional vertex factory is
another registered child whose release must precede aggregate destruction.

Implementation has not started. Stage 0 first freezes the unsafe schedules and
the complete consumer inventory.

## Goal

Give StaticMesh render data the same high-level shape used by Unreal Engine:
one asset-selected render-data aggregate, explicit `InitResources` and
`ReleaseResources`, scoped render-state recreation around replacement, and a
fence that prevents final storage destruction until rendering-thread cleanup is
complete.

The completed design must provide:

- one unique owner for every concrete `FStaticMeshRenderData` at every point in
  its lifecycle;
- detached candidate build and validation before live publication;
- ordered removal of every proxy or accepted raw-pointer consumer;
- buffer-before-vertex-factory initialization and reverse release ordering;
- asynchronous asset destruction through `BeginDestroy`,
  `IsReadyForFinishDestroy`, and `FinishDestroy`;
- safe replacement, reimport, rollback, unload, GC, and shutdown;
- failed replacement retention of the last successfully published mesh;
- no DMSH, DDC, cook, material-slot, bounds, or rendered-output change.

## Scope

- Add asset-level `InitResources`, `ReleaseResources`, initialized-state, and
  resource-state diagnostics to `DStaticMesh`.
- Introduce `FPendingStaticMeshRenderDataRetirement`, which uniquely owns one
  old aggregate plus its release fence until destruction becomes legal.
- Allow a `DStaticMesh` to retain multiple pending retirements so rapid
  replacements remain ordered without transferring concrete ownership to
  proxies.
- Add a scoped `FStaticMeshRenderStateRecreateContext`, modeled on UE's
  `FStaticMeshComponentRecreateRenderStateContext`, that detaches and recreates
  every repository consumer of a mesh revision.
- Keep renderer-side proxy access non-owning and valid only between render-state
  creation and destruction commands.
- Move resource initialization out of per-frame renderer preparation and into
  the asset/candidate lifecycle.
- Initialize and validate replacement candidates before detaching the current
  successful revision.
- Route import, DDC load, cooked load, reimport, rollback, transient/debug
  creation, unload, and GC through the same lifecycle primitives.
- Add release-fence and final-shutdown diagnostics.
- Publish the implemented contract under Runtime documentation.

## Non-Goals

- Adding `FStaticMeshReference`, `FStaticMeshRenderDataHandle`,
  `FRHIStaticMeshReference`, or shared ownership of concrete render data.
- Retargeting a stable mesh identity from one aggregate to another.
- Reproducing Unreal Engine's complete `UStreamableRenderAsset`, async
  compilation, Nanite, ray tracing, PSO precache, distance-field, or LOD
  streaming systems.
- Adding background source import or derived-data construction.
- Making asynchronous replacement publication a prerequisite; current
  synchronous reimport/build APIs may wait on a targeted candidate-init fence.
- Changing payload schemas, builder versions, DDC keys, source provenance,
  cooked packages, or reflected asset fields.
- Redesigning material slots, component overrides, transforms, or render-pass
  ownership.
- Letting renderer consumers retain `DStaticMesh`.

## Design Decisions and Invariants

### UE Ownership Shape

The selected design follows these public Unreal Engine boundaries:

- `UStaticMesh` uniquely owns `FStaticMeshRenderData` and exposes
  `InitResources`, `ReleaseResources`, initialized-state observation, a release
  fence, and `IsReadyForFinishDestroy`.
- `FStaticMeshRenderData` owns the complete renderable aggregate and explicitly
  initializes/releases its children.
- `FStaticMeshComponentRecreateRenderStateContext` destroys render state for
  components using the mesh and recreates it after the asset operation.
- `FStaticMeshSceneProxy` has explicit render-thread resource creation and
  destruction callbacks.
- `BeginInitResource`, `BeginReleaseResource`, and `FRenderCommandFence`
  preserve game-thread/render-thread ordering without sharing ownership of the
  concrete aggregate.

Durin adopts those responsibilities rather than their complete implementation.
Its command pipe is FIFO and its `DObject` lifecycle already supports
`BeginDestroy`, deferred `IsReadyForFinishDestroy`, and `FinishDestroy`, so the
required foundation exists.

### Exactly One Owner per Aggregate

One concrete `FStaticMeshRenderData` moves through these ownership states:

```text
detached builder/candidate
        |
        v
DStaticMesh::RenderData
        |
        v
FPendingStaticMeshRenderDataRetirement::RenderData
        |
        v
destroyed after release fence
```

Each arrow is a `std::unique_ptr` move. There is never more than one owner and
no proxy, scene, preview, thumbnail, or render command acquires ownership.

`DStaticMesh` may own:

- at most one current `RenderData`;
- at most one detached replacement candidate while a synchronous publication
  request is in progress;
- zero or more `PendingRenderDataRetirements`.

Pending retirements are asset-owned lifecycle records, not additional current
versions.

### Raw Proxy Pointers Are Bounded Borrows

`FStaticMeshSceneProxy`, `FTextureCubePreviewSceneProxy`, and other render-state
objects may retain a raw `FStaticMeshRenderData*` only under this contract:

- the pointer is captured when render state is created;
- all commands using that proxy execute before its removal command;
- every removal command is queued before the pointed-to aggregate's resource
  release commands;
- the release fence is queued after every release command;
- aggregate C++ storage remains uniquely owned until the fence completes.

The FIFO command order is:

```text
earlier draw/use
-> remove old proxy/render state
-> release old vertex factories
-> release old index/vertex buffers
-> release fence
-> destroy old aggregate storage
```

Keeping a raw pointer is therefore acceptable only for registered render-state
consumers. Synchronous game-thread/editor inspection through `GetRenderData()`
must not retain the returned pointer across a replacement call.

### Render-State Recreate Context

`FStaticMeshRenderStateRecreateContext` is the Durin counterpart to UE's
component context.

On construction it:

- snapshots every live `DStaticMeshComponent` using the mesh with generation
  checked object handles;
- snapshots registered preview, thumbnail, and auxiliary-scene consumers;
- requests destruction/removal of their old render state;
- preserves only the producer information required to recreate those consumers.

On destruction it:

- rebuilds component material/proxy snapshots from the new current render data;
- requests recreation of registered preview and auxiliary render states;
- refreshes bounds or other current component state where required.

Every long-lived raw-pointer consumer must either participate in this context
or prove that its command completes synchronously before publication. A
component scan alone is insufficient because preview and thumbnail scenes are
not components.

### Current Resource Initialization

`DStaticMesh::InitResources` is the sole high-level initializer for current
render data.

- It assigns owner/revision diagnostics to every child.
- It queues LOD buffer initialization before per-LOD vertex factories.
- It queues a completion fence after initialization.
- It records whether initialization has been requested and whether the current
  revision is ready or failed.
- It is idempotent for one current revision.

`FRendererModule::PrepareSceneResources` no longer initializes mesh resources
opportunistically every frame. It only validates readiness and renders a proxy
whose asset lifecycle has already requested initialization.

Initial load or transient creation with no active RHI may retain CPU render
data in an uninitialized state. The first render-state creation requests
initialization before it queues proxy creation.

### Replacement Candidate Publication

Replacement retains the last successful live revision until the candidate is
known to be renderable:

1. build and CPU-validate a detached candidate;
2. queue candidate resource initialization using non-owning pointers while the
   publication operation retains the candidate's unique ownership;
3. queue a candidate-init fence;
4. for current synchronous reimport/build APIs, wait for that targeted fence;
5. if initialization fails, release and retire the candidate and keep the
   current asset/proxies unchanged;
6. construct `FStaticMeshRenderStateRecreateContext` for the current mesh;
7. move the old current aggregate into a pending retirement and queue its
   release/fence after context construction has queued detach work;
8. move the initialized candidate into `DStaticMesh::RenderData`;
9. destroy the recreate context, which queues new proxy creation after
   candidate initialization.

This targeted wait is not a full renderer flush. A later asynchronous
publication design may poll candidate completion and perform steps 6–9 in a
game-thread finalize task, without changing ownership or proxy ordering.

### Pending Retirement

`FPendingStaticMeshRenderDataRetirement` contains:

- `std::unique_ptr<FStaticMeshRenderData> RenderData`;
- owner package and render revision diagnostics;
- one `FRenderCommandFence ReleaseFence`;
- lifecycle state distinguishing release queued, fence pending, and ready to
  destroy.

Creating a retirement:

1. requires all consumer detach commands already to be queued;
2. queues reverse-order `ReleaseResources`;
3. begins the fence after release commands;
4. stores the unique old aggregate until the fence completes.

Completed retirements may be pruned during later asset operations, GC
readiness checks, or an explicit lifecycle pump. Destruction after fence
completion may occur on the game thread because every nested
`FRenderResource` is already unregistered and its RHI references were released
on the rendering thread.

No retirement destructor waits. Destroying an incomplete retirement is a
lifecycle error.

### Asset Destruction

`DStaticMesh::BeginDestroy`:

1. prevents new builds, initialization, or render-state recreation;
2. requests detach/removal of any remaining registered consumers;
3. moves current render data into a pending retirement;
4. queues resource release and begins its fence;
5. calls `Super::BeginDestroy`.

`DStaticMesh::IsReadyForFinishDestroy` returns true only when:

- every pending retirement fence is complete;
- no current render data remains;
- no initialization/publication request can still target the asset;
- the superclass is ready.

`FinishDestroy` verifies and clears completed retirements before physical
destruction. GC therefore defers destruction without blocking its initiating
game-thread call.

### Resource and RHI Release

For one aggregate, initialization is:

```text
LOD vertex/index buffers
-> LOD vertex factories
-> ready
```

Release is:

```text
LOD vertex factories
-> LOD index/vertex buffers
-> FRenderResource registry empty for this revision
-> release fence
-> aggregate C++ destruction
```

Final RHI object destruction may remain in the RHI deferred-delete queue after
the aggregate's RHI references are dropped. Engine shutdown drains that queue
after RenderCore resource and aggregate-retirement audits pass.

Repeated init/release is idempotent. Partial initialization failure releases
all successfully initialized children before the candidate's retirement fence
can complete.

### Import, Reimport, and Exchange

Detached import candidates must not initialize against or inherit the live
asset's owner/revision identity before bundle commit.

`ExchangeImportedState` no longer swaps active `RenderData`, initialization
flags, release fences, or pending retirements between assets. The import
workflow instead:

1. builds detached authored state plus detached render data;
2. commits source, material-slot, manifest, and cooked metadata to the intended
   asset;
3. invokes the asset-qualified replacement publication path;
4. performs a symmetric replacement request when bundle rollback restores old
   authored state.

Owner diagnostics and fences therefore remain attached to the correct package.

### No StaticMesh Reference or Counted Handle

StaticMesh does not need texture-style RHI indirection:

- a texture is one bindable RHI identity that can retarget without rebuilding
  every consumer;
- a mesh revision contains buffers, sections, bounds, slot layout, and vertex
  factories, so structural replacement recreates proxy state;
- unique current/pending-retirement ownership plus explicit fences already
  protects raw-pointer consumers.

A counted handle becomes eligible only if Stage 0 proves that a required
long-lived consumer cannot participate in render-state detach/recreate or
cannot be bounded by a release fence. Such a finding requires a recorded plan
revision; it is not the default design.

## Current Foundations and Gaps

### Foundations

- `DObject` already implements the UE-shaped
  `BeginDestroy`/`IsReadyForFinishDestroy`/`FinishDestroy` phases and GC reports
  deferred destruction.
- `FRenderCommandFence` already inserts a FIFO completion point and supports
  non-blocking completion checks or targeted waits.
- `FRenderResource` already provides render-thread init/release affinity,
  initialized-resource registration, diagnostics, and deferred cleanup.
- The render command pipe preserves accepted command order and audits pending
  work before shutdown.
- `FStaticMeshRenderData` already aggregates LOD resources, sections, material
  slots, and bounds and exposes render-thread-only init/release entry points.
- The LOD-resource plan already specifies buffer-before-factory initialization
  and factory-before-buffer release.
- `DStaticMesh::NotifyLoadedComponents` and primitive scene IDs provide a
  starting point for component render-state recreation.

### Gaps

- `SetRenderData` destroys old storage before queued proxies stop using it.
- `DStaticMesh` has no initialization state, release fence, pending retirement,
  or destruction readiness override.
- production replacement and destruction do not release initialized nested
  `FRenderResource` objects.
- initialization is renderer-discovered and repeated during scene preparation.
- the loaded-component scan does not cover preview, thumbnail, and auxiliary
  scene consumers.
- `ExchangeImportedState` swaps live render ownership between package
  identities.
- selected tests manually call `ReleaseResources`, masking missing asset
  ownership.
- no scheduler-controlled test pauses replacement or destruction at detach,
  release, fence, and final-delete boundaries.

## Implementation Stages

### Stage 0: Freeze Consumers and Unsafe Schedules

- [ ] Record baseline commit
  `b9deab4cbedf2dea93e56e8f11f6773216d93ea1` and the initial working set.
- [ ] Inventory every stored, captured, returned, and immediate-use
  `FStaticMeshRenderData*` and classify it as detached build access,
  synchronous inspection, registered render state, or queued render-thread
  borrow.
- [ ] Identify the detach/recreate entry point for components, material
  thumbnails, TextureCube previews, and every auxiliary scene.
- [ ] Add scheduler-controlled tests paused before old-proxy removal, resource
  release, fence completion, and aggregate destruction.
- [ ] Reproduce replacement while an old draw/proxy remains queued and asset GC
  after buffers initialize.
- [ ] Cover registered, unregistered, garbage-marked, preview, thumbnail,
  rapid-replacement, and no-RHI cases.
- [ ] Pin DMSH, DDC, material-slot, bounds, and rendered-output behavior.
- [ ] Record diagnostics sufficient to prove exact command and destruction
  order without timing sleeps.

Dependencies: none.

#### Acceptance Gate

- Every long-lived consumer has a selected detach/recreate path; unsafe
  replacement and unload schedules fail deterministically against the baseline;
  and compatibility baselines remain unchanged.

### Stage 1: Add Explicit Asset Resource and Retirement Lifecycles

- [ ] Add `DStaticMesh::InitResources`, `ReleaseResources`,
  `AreRenderingResourcesInitialized`, resource revision/state, and diagnostic
  accessors.
- [ ] Add `FPendingStaticMeshRenderDataRetirement` with unique aggregate
  ownership, release fence, and lifecycle diagnostics.
- [ ] Add asset-owned pending-retirement storage and completion pruning without
  blocking destructors.
- [ ] Implement reverse-order release, partial-init rollback, idempotent
  init/release, and fence-after-release ordering.
- [ ] Implement `BeginDestroy`, `IsReadyForFinishDestroy`, and `FinishDestroy`
  using pending retirement fences.
- [ ] Remove per-frame initialization ownership from
  `FRendererModule::PrepareSceneResources`.
- [ ] Add focused tests for initial init, repeated init, partial failure,
  replacement retirement, multiple simultaneous retirements, GC deferral, and
  final registry cleanup.

Dependencies: Stage 0.

#### Acceptance Gate

- Current and retiring aggregates always have exactly one owner; old storage
  survives until its release fence completes; GC defers without blocking; and
  every initialized nested resource unregisters before aggregate destruction.

### Stage 2: Recreate Every Render-State Consumer

- [ ] Add `FStaticMeshRenderStateRecreateContext` with component object-handle
  generation checks and registered non-component consumer adapters.
- [ ] Queue all old render-state detach commands during context construction and
  all new render-state creation commands during destruction.
- [ ] Ensure init commands for the selected current revision precede every new
  proxy-add command.
- [ ] Keep proxy render-data fields non-owning and document their bounded-borrow
  contract.
- [ ] Integrate material-thumbnail, TextureCube preview, and auxiliary-scene
  paths rather than relying only on `GDObjectArray` component scanning.
- [ ] Handle garbage-marked, concurrently unregistered, reassigned, and reused
  object slots without stranding an old proxy.
- [ ] Add command-order tests proving draw → detach → release → fence → delete
  and init → add-proxy ordering.

Dependencies: Stage 1.

#### Acceptance Gate

- No long-lived raw-pointer consumer exists outside a registered render-state
  interval; every old consumer detaches before release begins; and proxy
  recreation observes only the selected initialized aggregate.

### Stage 3: Publish Candidates and Integrate Asset Workflows

- [ ] Replace public `SetRenderData` with detached candidate publication and
  retain narrow builder/test helpers only where required.
- [ ] Initialize replacement candidates before live commit and use a targeted
  candidate-init fence for current synchronous APIs.
- [ ] Keep the last successful asset/proxies unchanged when candidate
  initialization fails; release and retire the failed detached candidate.
- [ ] Route DDC hit, source rebuild, cooked load, debug/transient creation,
  reimport, undo/redo, and rollback through the same lifecycle.
- [ ] Refactor `ExchangeImportedState` and static-model bundle commit/rollback
  so current data, release fences, and pending retirements never move between
  assets.
- [ ] Remove manual test-only mesh `ReleaseResources` compensation.
- [ ] Validate rapid consecutive replacement, failed replacement, unload,
  package reload, ordinary GC, engine exit, and command-admission close.
- [ ] Audit pending retirements by owner, revision, state, and fence status at
  final shutdown.

Dependencies: Stage 2.

#### Acceptance Gate

- Every asset workflow uses one publication primitive; failed candidates retain
  the last successful mesh; package-qualified lifecycle state never swaps
  between assets; and shutdown leaves no current or pending mesh resource.

### Stage 4: Coordinate Vertex Factories, Validate, and Document

- [ ] Resume the Static Mesh LOD Resources Refactor and apply
  buffer-before-factory init and factory-before-buffer release inside the
  asset-led lifecycle.
- [ ] Run focused Engine, CoreDObject, RenderCore, Renderer, static-mesh,
  material, thumbnail, cook, import, and Vulkan rendering validation.
- [ ] Run the complete native suite and successful full `all` build using the
  repository build workflow.
- [ ] Run editor import, display, reimport, failed replacement, undo/redo,
  package unload/reload, and normal shutdown smoke workflows.
- [ ] Measure candidate-init wait and proxy-recreation cost; no full render flush
  or per-component GPU duplication is allowed.
- [ ] Publish lasting StaticMesh ownership, render-state recreation, release
  fence, and shutdown rules under Runtime rendering documentation.
- [ ] Resolve the linked investigation only after implementation and full
  validation land.

Dependencies: Stage 3 and the implementation stages of
`StaticMeshLODResourcesRefactor.md`.

#### Acceptance Gate

- Focused and full validation passes; payload and rendered baselines remain
  unchanged; replacement uses only targeted fences; final RenderCore/RHI audits
  are clean; and runtime documentation is authoritative.

## Validation Matrix

| Boundary | Required validation |
| --- | --- |
| Unique ownership | Detached candidate, current asset data, pending retirement, final destruction |
| DObject lifecycle | BeginDestroy, non-blocking readiness polling, FinishDestroy, repeated GC |
| Command order | Draw/use, detach, reverse release, fence, aggregate destruction |
| Candidate publication | Init fence, success, partial failure, last-successful retention |
| Buffer lifecycle | Complete/partial init, retry, reverse release, registry removal |
| Components | Registered, unregistered, reassigned, garbage-marked, reused generation |
| Non-component consumers | Material thumbnail, TextureCube preview, auxiliary scene |
| Import | DDC hit, source rebuild, transient/debug, reimport, bundle commit/rollback |
| Asset lifecycle | Rapid replacement, package unload/reload, no RHI, shutdown, admission close |
| Compatibility | DMSH fixture, DDC key, cook payload, slots, bounds, rendered output |
| Performance | Targeted fence duration, proxy churn, no duplicate per-component geometry |

All configure, build, test, and runtime actions follow
`Documentation/Development/Build/BuildAndRun.md`; this plan does not duplicate
profile-specific commands.

## Definition of Done

- `DStaticMesh` uniquely owns current `FStaticMeshRenderData` and explicitly
  initializes/releases its resources.
- Detached candidates and pending retirements preserve exactly one owner during
  build, replacement, failure, and destruction.
- No StaticMesh reference, counted render-data handle, or proxy-side concrete
  ownership is introduced.
- Every long-lived raw-pointer consumer participates in render-state
  detach/recreate.
- Replacement order is draw → detach → reverse release → fence → delete, and
  new resource init precedes new proxy creation.
- GC uses `IsReadyForFinishDestroy` rather than blocking `BeginDestroy`.
- Failed replacement retains the last successful published mesh.
- Vertex factories release before LOD buffers and every nested
  `FRenderResource` unregisters before aggregate destruction.
- Import, rollback, unload, GC, and shutdown require no manual test-only
  release.
- DMSH, DDC, cook, package, slot, bounds, and rendered-output contracts remain
  compatible.
- Final retirement, RenderCore registry, deferred cleanup, command-pipe, and RHI
  deletion audits are empty.
- Lasting lifecycle rules are published under Runtime rendering documentation.

## Deferred Follow-ups

- Non-blocking game-thread candidate publication after background build.
- LOD streaming and per-LOD residency transitions.
- CPU-data discard after upload under an explicit access policy.
- Batched component render-state recreation for large reimport sets.
- Stable numeric mesh IDs or bindless geometry tables for future GPU-driven
  rendering.
- Mesh-shader, ray-tracing, Nanite-like, skeletal, procedural, and instanced
  resource variants.

## Related Documentation

- [StaticMesh Render-Data Lifetime Investigation](../Investigations/StaticMeshRenderDataLifetime.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Static Mesh LOD Resources Refactor](StaticMeshLODResourcesRefactor.md)

Unreal Engine reference contracts:

- [UStaticMesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMesh)
- [UStaticMesh::IsReadyForFinishDestroy](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/UStaticMesh/IsReadyForFinishDestroy)
- [FStaticMeshRenderData](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshRenderData)
- [FStaticMeshComponentRecreateRenderStateContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshComponentRecreateRend-)
- [FRenderCommandFence](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRenderCommandFence)
- [BeginInitResource](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/BeginInitResource)

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshUpdateTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/StaticModelImportVulkanTests.cpp`
