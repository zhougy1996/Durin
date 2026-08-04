# StaticMesh Render-Data Lifetime

**Status:** Resolved; implementation validated
**Last reviewed:** 2026-08-04

## Scope And Verdict

This investigation records the baseline lifetime violation and the selected
UE-shaped correction. The implementation is now landed: `DStaticMesh` retains
unique ownership through candidate initialization, component render-state
recreation, reverse child-resource release, targeted fences, and deferred
object destruction.

The appropriate correction is UE-style unique ownership, not a
`FTextureReference`-style indirection or a counted render-data handle:

- a detached builder uniquely owns an unpublished candidate;
- `DStaticMesh` uniquely owns the published aggregate;
- after detaching all render-state consumers, `DStaticMesh` moves the old
  aggregate into a uniquely owned pending-retirement record;
- resource-release commands and then a render-command fence are queued;
- only fence completion permits destruction of the old C++ storage.

The implementation is tracked by
[Static Mesh Render-Data Lifecycle](../Plans/StaticMeshRenderDataLifecycle.md).

The detailed findings below are historical baseline evidence. They describe
the unsafe pre-implementation schedules and remain useful for explaining why
the ownership and ordering rules are required.

## Resolution And Validation

- `DStaticMesh` owns one current render-data aggregate and one destruction
  release fence; detached candidates and synchronous displaced data retain
  exactly one unique owner.
- `FStaticMeshRenderStateRecreateContext` removes registered component proxies
  before replacement release and recreates them only against current data.
- `FStaticMeshRenderData` initializes LOD buffers before vertex factories and
  releases vertex factories before buffers.
- DDC, cooked load, source rebuild, reimport, rollback, package unload, GC,
  and no-RHI destruction use the same asset lifecycle primitives.
- The current-head full `all` build, complete native suite, focused StaticMesh
  recreate tests, Vulkan lifecycle test, and hidden-window editor startup/
  shutdown smoke all pass.

The parallel validation run also exposed a temporary scene-snapshot directory
collision across test processes. Scene import staging now includes the process
ID in its temporary directory identity, so concurrent reimport tests cannot
delete one another's captured source files.

## Verified Findings

### P1 — Replacement destroys storage before asynchronous proxy retirement

`DStaticMesh::SetRenderData` assigns a new `std::unique_ptr` directly and only
then calls `NotifyLoadedComponents`. Assignment destroys the previous
`FStaticMeshRenderData` immediately on the calling thread.

`FStaticMeshSceneProxy` and `FTextureCubePreviewSceneProxy` both retain a raw
`FStaticMeshRenderData*`. `FScene::AddOrReplacePrimitive` and
`FScene::RemovePrimitive` enqueue their actual proxy-map mutations through
`ENQUEUE_RENDER_COMMAND`, so notifying a component does not synchronously
invalidate the old proxy.

The resulting accepted schedule is:

```text
old proxy captures old RenderData*
-> game thread replaces unique_ptr and destroys old storage
-> old proxy removal/replacement command executes later
```

**Impact:** this is a source-verified use-after-free-capable ordering. A
deterministic runtime reproduction has not yet been added, so this investigation
does not claim an observed crash.

**Required direction:** isolate old consumers with a scoped render-state
recreate context, queue every detach before release, and retain unique ownership
of old storage through a fence placed after those commands.

### P1 — Initialized child resources have no asset-owned release path

`DStaticMesh::~DStaticMesh` is defaulted. `SetRenderData` and
`ExchangeImportedState` can also destroy or transfer an aggregate without an
asset-level `ReleaseResources` operation.

Meanwhile, `FRendererModule::PrepareSceneResources` discovers mesh pointers by
walking scene proxies and lazily calls `FStaticMeshRenderData::InitResources`.
Selected tests manually release mesh data, but the production asset lifecycle
does not provide the corresponding high-level release contract.

**Impact:** once child buffers are registered or initialized, replacement,
unload, garbage collection, and shutdown can bypass their required
render-thread release ordering. Adding per-LOD vertex factories would increase
the number of affected registered children.

**Required direction:** make `DStaticMesh::InitResources` and
`DStaticMesh::ReleaseResources` the high-level boundary. The aggregate must
initialize buffers before dependent vertex factories and release them in
reverse dependency order.

### P1 — `ExchangeImportedState` swaps live render identities

`DStaticMesh::ExchangeImportedState` swaps both imported metadata and the live
`RenderData` pointers, then asynchronously notifies components for both assets.
Any already queued or installed proxy remains associated with the old raw
address until its later scene command runs.

**Impact:** package/reimport commit and rollback can cross asset identities while
render-thread consumers still refer to the pre-swap aggregates. A release fence
attached only to one side would not be sufficient if ownership continues to
swap.

**Required direction:** exchange detached imported/build state, not active
render ownership. Each asset must publish its candidate through the same
detach, retirement, and recreate transaction used by ordinary replacement.

### P2 — Consumer discovery is incomplete as a lifetime boundary

`NotifyLoadedComponents` can find loaded `DStaticMeshComponent` users, but
StaticMesh render data is also borrowed by preview/thumbnail paths such as
`FTextureCubePreviewSceneProxy`. A component-only recreation helper would leave
those consumers outside the lifetime proof.

**Impact:** an apparently safe component rebuild could still release an
aggregate borrowed by a registered non-component proxy.

**Required direction:** inventory every persistent raw-pointer consumer and
register it with one StaticMesh render-state recreation boundary. Detached
builder inspection may remain outside that boundary only while the candidate
has not been published.

## Confirmed Supporting Mechanisms

- Durin's render-command pipe preserves FIFO ordering, allowing detach commands,
  reverse-order resource releases, and a fence to form one retirement sequence.
- Durin already provides `FRenderCommandFence` with `BeginFence`,
  completion observation, and `Wait`.
- `DObject` already exposes `BeginDestroy`, `IsReadyForFinishDestroy`, and
  `FinishDestroy`; garbage collection polls readiness before final destruction.
  StaticMesh can therefore retire asynchronously during normal GC rather than
  blocking every destruction.
- `BeginCleanupRenderResource` demonstrates deferred unique ownership for an
  individual `FRenderResource`, but `FStaticMeshRenderData` is an aggregate
  rather than one `FRenderResource`. Its owner must coordinate all children and
  retain the aggregate through the final fence.

## UE Reference And Interpretation

The relevant public UE contracts are:

- [`UStaticMesh`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UStaticMesh)
  uniquely owns its render data and exposes explicit resource lifecycle state;
- [`FStaticMeshRenderData`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshRenderData)
  owns and initializes/releases the renderable aggregate;
- [`FStaticMeshComponentRecreateRenderStateContext`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshComponentRecreateRend-)
  removes component render state for a mesh and recreates it when the context
  ends;
- [`FRenderCommandFence`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FRenderCommandFence)
  represents completion of previously queued rendering work;
- [`UStaticMesh::IsReadyForFinishDestroy`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/UStaticMesh/IsReadyForFinishDestroy)
  participates in deferred object destruction.

The design lesson is not that every replacement must flush the render thread.
It is that unique ownership must outlive all non-owning render-thread uses.
Normal GC can poll a release fence. A synchronous editor/build API may wait on
a targeted candidate-initialization fence when it must return a render-ready
asset, without turning every retirement into a global render-thread flush.

## Alternatives Considered

### Counted render-data handle

A counted immutable revision handle would close the immediate storage lifetime
hole, but it would distribute ownership into proxies and commands, obscure who
must release registered children, and permit aggregate destruction on an
arbitrary last-owner thread. Durin does not currently need this complexity for
StaticMesh streaming or asynchronous compilation, so it is not selected.

### Stable `FStaticMeshReference` indirection

Texture references solve a different problem: materials need a stable binding
identity whose target texture may change independently. A StaticMesh proxy
depends on aggregate layout, sections, bounds, buffers, and vertex factories;
retargeting one stable pointer does not recreate those dependent structures.
It is therefore not a replacement for render-state recreation.

### Full render-thread flush on every replacement

A full flush would make deletion safe but unnecessarily stalls unrelated
rendering work. It remains a possible shutdown fallback or diagnostic, not the
normal ownership protocol.

## Validation Gaps And Acceptance Evidence

Implementation should add evidence for all of these schedules:

1. replace a rendered mesh while old draw and proxy-removal commands are queued;
2. perform rapid consecutive replacements with multiple pending retirements;
3. fail candidate initialization and preserve the last published mesh;
4. reimport and rollback without swapping live render ownership;
5. destroy through GC and prove `FinishDestroy` waits for release completion;
6. cover component, preview, thumbnail, transient/debug, unload, and shutdown
   consumers;
7. verify buffer-before-factory initialization and reverse release order.

Diagnostics should fail when aggregate storage reaches destruction with
initialized children, an incomplete retirement fence, or a registered consumer
outside the recreation boundary.

## Relevant Implementation

- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`:
  `DStaticMesh::~DStaticMesh`, `SetRenderData`, `NotifyLoadedComponents`, and
  `ExchangeImportedState`;
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`:
  current `RenderData` ownership and public replacement API;
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`:
  raw StaticMesh render-data borrows;
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`:
  asynchronous proxy add, replacement, and removal;
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`:
  proxy-driven lazy resource initialization;
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h` and
  `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`:
  fence and deferred-destruction foundations.
