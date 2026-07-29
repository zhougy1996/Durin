# Static Mesh Render-Data Lifecycle Plan

Summary: Adopt UE-style unique StaticMesh render-data ownership with explicit resource initialization, component render-state recreation, and one destruction release fence.

Last reviewed: 2026-07-29

Status: Active
Completed:

## Current Status

`DStaticMesh` now owns one `std::unique_ptr<FStaticMeshRenderData>` and exposes
no public render-data setter or publication primitive. Initial CPU data is
installed privately in the uninitialized state. Live replacement initializes a
detached candidate behind a targeted fence, removes component render state,
publishes once, then releases and fences the locally owned old data before
components recreate their proxies.

The verified evidence and UE comparison are recorded in
[StaticMesh Render-Data Lifetime](../Investigations/StaticMeshRenderDataLifetime.md).
The selected correction preserves unique ownership instead of introducing a
counted render-data handle:

- `DStaticMesh` uniquely owns its current render data;
- detached builders uniquely own unpublished candidates;
- synchronous replacement temporarily owns the old render data in a local
  `std::unique_ptr`;
- component render-state recreation removes every raw-pointer consumer before
  replacement releases the old render data;
- release commands precede a fence;
- old C++ storage is destroyed only after that fence completes.

The active
[Static Mesh LOD Resources Refactor](StaticMeshLODResourcesRefactor.md) has
completed its named-buffer stage and is about to add vertex factories. This
lifecycle plan is its prerequisite because each additional vertex factory is
another registered child whose release must precede render-data destruction.

Stage 0 is complete from baseline `9cfee542`. The deterministic contract suite
now reproduces the current immediate-destruction violation without executing
undefined behavior, pauses the selected replacement/destruction protocol at
draw, detach, release, fence, and render-data-delete boundaries, and covers rapid
replacement, registered/unregistered/garbage/reassigned consumers, thumbnails,
auxiliary scenes, GC-style readiness, and no-RHI teardown. The existing
StaticMesh, material-preview, thumbnail, and Vulkan rendered-output baselines
remain green.

The Stage 0 scheduler remains useful as proof of the unsafe baseline and FIFO
ordering boundaries, but its asset-owned multiple-retirement model is
superseded by this review. Stage 1 converts editor consumers to components, and
Stage 3 replaces the model with serialized local-old ownership.

Stage 1 is complete from baseline `1115c0dd`. Worlds now supply the stable
renderer-scene endpoint retained by registered scene components; main and
preview scenes share that registration contract; MaterialPreview, material
thumbnails, and TextureCube thumbnails install only component-owned render
state; and the scoped StaticMesh recreate context uses generation-checked
component handles without an asset consumer registry.

Stage 2 is complete from baseline `c379c1f7`. `DStaticMesh` now explicitly
initializes and releases its current render data, component proxy creation
queues initialization before proxy addition, resource initialization rolls
back completely on partial failure, and object destruction uses one
non-blocking release fence only when that mesh actually submitted resource
work. The obsolete renderer-wide `PrepareSceneResources` hook has been removed.

Stage 3 is complete from baseline `7f9b6c3a`. DDC, cooked load, source rebuild,
transient/debug construction, reimport, and bundle rollback all use the same
private publication primitive. `ExchangeImportedState` accepts a detached
import candidate, never swaps asset release fences or resource states, and
leaves only fully released CPU data on the candidate object for symmetric
rollback.

The post-Stage 3 API review also narrowed the runtime control surface while
preserving UE's explicit resource lifecycle shape. `GetRenderData()` is a
const, side-effect-free inspection; `InitResources()` remains the explicit
initialization operation used before component proxy creation; resource
release, aggregate state, and the destruction fence are asset-private.
Scene proxies retain only const render-data borrows.

The cross-plan boundary with
[Engine Termination Lifecycle](EngineTerminationLifecycle.md) is now pinned:
engine shutdown first detaches non-DObject process/subsystem consumers, then
the shutdown coordinator owns all GC/render-flush rounds. Worlds expose their
own renderer scene, and registered components retain that stable endpoint
instead of consulting the global `GEngine` pointer. `DStaticMesh` owns only its
current render data and release fence; process exit phase and a separate
StaticMesh admission state are not asset APIs.

### Stage 0 Frozen Inventory and Handoff

The source inventory classifies StaticMesh render-data access as follows:

| Access class | Sites | Frozen rule |
| --- | --- | --- |
| Detached build ownership | import, DDC/cooked decode, transient/debug builders | A detached `std::unique_ptr` may be inspected only by the synchronous builder until publication transfers ownership. |
| Synchronous inspection | asset details, cook/payload code, bounds/slot queries, level-editor hit tests, tests | `GetRenderData()` results cannot survive a call that can publish or destroy mesh data. |
| Installed render state | `FStaticMeshSceneProxy` created by `DStaticMeshComponent`, material thumbnails, and MaterialPreview; `FTextureCubePreviewSceneProxy` created by TextureCube thumbnails | These are the only production stored raw pointers. Stage 1 converts every editor path to component-owned render state so replacement needs no asset-side consumer registry. |
| Queued render-thread borrow | renderer traversal/draw through an installed proxy; Vulkan capture tests with retained assets | The borrow is bounded by proxy/asset retention and FIFO commands; no queued production command independently owns render data. |

The selected correction for the frozen consumer inventory is:

- `DWorld` exposes its renderer scene; `DPrimitiveComponent` captures that scene
  from its owning actor's world at registration and uses the same endpoint for
  proxy removal and recreation.
- a reusable `FPreviewScene` owns a preview `DWorld`, level, renderer scene, and
  preview actors/components. It is a general editor facility intended for
  mesh, material, texture, particle, and later effect previews.
- material thumbnails and MaterialPreview use ordinary
  `DStaticMeshComponent` instances inside an `FPreviewScene`.
- TextureCube thumbnails use a transient preview component that owns the
  specialized proxy inside the preview world but still participates in the
  same component render-state lifecycle.
- `FStaticMeshRenderStateRecreateContext` scans component objects using the mesh,
  matching UE's component context. `DStaticMesh` stores no consumer callbacks,
  scene list, or consumer registry.

The Stage 0 working set is:

- `Documentation/Plans/StaticMeshRenderDataLifecycle.md`;
- `Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshRenderDataLifetimeContractTests.cpp`;
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`;
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialTestSupport.h`;
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`.

The renderer change only restores the complete test/module lifecycle for a
no-RHI preview owner: `StartupModule` opens scene admission but skips default
GPU texture creation when no RHI exists; the preview harness now pairs
Startup/Release/Shutdown instead of relying on the old default-open state.

Stage 0 validation:

- `StaticMeshTests`: 50/50 passed, covering canonical DMSH, DDC, material-slot,
  and bounds contracts;
- `FStaticMeshRenderDataLifetimeContractTests.*`: 6/6 passed;
- material and TextureCube thumbnail consumers: 7/7 passed;
- `FMaterialTests.*`: 35/35 passed, including MaterialPreview lifecycle;
- `StaticModelImportVulkanTests`: 1/1 passed rendered-output baseline.

## Goal

Give StaticMesh render data the same high-level shape used by Unreal Engine:
one asset-selected `FStaticMeshRenderData`, explicit `InitResources` and
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
- Add one asset-owned release fence used only by asynchronous object
  destruction.
- Add a scoped `FStaticMeshRenderStateRecreateContext`, modeled on UE's
  `FStaticMeshComponentRecreateRenderStateContext`, that detaches and recreates
  every component using a mesh.
- Add a reusable preview-scene/world owner and convert editor preview and
  thumbnail StaticMesh proxy producers to component-owned render state inside
  that world.
- Keep renderer-side proxy access non-owning and valid only between render-state
  creation and destruction commands.
- Move resource initialization out of per-frame renderer preparation and into
  the asset/candidate lifecycle.
- Initialize and validate replacement candidates before detaching the current
  successful render data.
- Route import, DDC load, cooked load, reimport, rollback, transient/debug
  creation, unload, and GC through the same lifecycle primitives.
- Add release-fence and final-shutdown diagnostics.
- Publish the implemented contract under Runtime documentation.

## Non-Goals

- Adding `FStaticMeshReference`, `FStaticMeshRenderDataHandle`,
  `FRHIStaticMeshReference`, or shared ownership of concrete render data.
- Retargeting a stable mesh identity from one render-data object to another.
- Reproducing Unreal Engine's complete `UStreamableRenderAsset`, async
  compilation, Nanite, ray tracing, PSO precache, distance-field, or LOD
  streaming systems.
- Adding background source import or derived-data construction.
- Adding a StaticMesh-specific admission state, consumer registry, consumer
  callbacks, pending-retirement queue, or lifecycle pump.
- Making asynchronous replacement publication a prerequisite; current
  synchronous reimport/build APIs may wait on a targeted candidate-init fence.
- Changing payload schemas, builder versions, DDC keys, source provenance,
  cooked packages, or reflected asset fields.
- Redesigning material slots, component overrides, transforms, or render-pass
  ownership.
- Letting renderer consumers retain `DStaticMesh`.
- Supporting overlapping live replacement publications; synchronous callers
  serialize replacement and retire their local old render data before returning.

## Design Decisions and Invariants

### UE Ownership Shape

The selected design follows these public Unreal Engine boundaries:

- `UStaticMesh` uniquely owns `FStaticMeshRenderData` and exposes
  `InitResources`, `ReleaseResources`, initialized-state observation, a release
  fence, and `IsReadyForFinishDestroy`.
- `FStaticMeshRenderData` owns the complete renderable data and explicitly
  initializes/releases its children.
- `FStaticMeshComponentRecreateRenderStateContext` destroys render state for
  components using the mesh and recreates it after the asset operation.
- `FStaticMeshSceneProxy` has explicit render-thread resource creation and
  destruction callbacks.
- `BeginInitResource`, `BeginReleaseResource`, and `FRenderCommandFence`
  preserve game-thread/render-thread ordering without sharing ownership of the
  concrete render-data object.

Durin adopts those responsibilities rather than their complete implementation.
Its command pipe is FIFO and its `DObject` lifecycle already supports
`BeginDestroy`, deferred `IsReadyForFinishDestroy`, and `FinishDestroy`, so the
required foundation exists.

### Exactly One Owner per Render-Data Object

One concrete `FStaticMeshRenderData` moves through these ownership states:

```text
detached builder/candidate
        |
        v
DStaticMesh::RenderData
        |
        +-- object destruction: retained until DStaticMesh::ReleaseResourcesFence
        |
        +-- synchronous replacement: local old unique_ptr until a targeted fence
        v
destroyed
```

Each arrow is a `std::unique_ptr` move. There is never more than one owner and
no proxy, scene, preview, thumbnail, or render command acquires ownership.

`DStaticMesh` owns at most one current `RenderData`. A synchronous publication
operation owns at most one detached candidate and one local old render data. It
does not return until the old render data's targeted release fence completes, so
no retired versions accumulate on the asset.

### No Asset Admission or Consumer Registry

StaticMesh adds no lifecycle state parallel to `DObject`. Once `BeginDestroy`
has started, build, publication, and initialization calls violate the existing
object lifecycle contract; they do not consult a separate asset admission
flag. If background mesh compilation is added later, its task owner must cancel
or finish work before object cleanup rather than adding a generic gate to
`DStaticMesh`.

`DStaticMesh` also stores no components, scenes, callbacks, or consumer
registrations. Component references keep an ordinary live mesh reachable;
replacement uses a scoped component scan; package/world teardown unregisters
components; and engine shutdown detaches process-owned scenes before the
DObject drain.

### World and Preview-Scene Ownership

Renderer scene selection is a world responsibility rather than a StaticMesh or
component special case.

- `DWorld` exposes the `IScene` used by components registered through actors in
  that world.
- `DEngine` installs the main renderer scene on the active main world.
- a reusable editor `FPreviewScene` owns one renderer scene plus a preview
  `DWorld` and level; its actors and components use ordinary registration.
- `EWorldType::Preview` distinguishes preview behavior from Editor,
  PlayInEditor, and Game policy.
- the preview world may tick when a preview requires simulation. Static mesh
  and thumbnail previews may leave play/tick disabled; particle and effect
  previews can opt into the same world lifecycle later.
- teardown runs component unregister/end-play first, releases the renderer
  scene second, and releases or garbage-marks the preview world hierarchy last.

Components capture their world's scene when registered and retain that
non-owning endpoint until unregister completes. Changing a component to another
world therefore requires unregister followed by registration; there is no
public arbitrary `SetRenderScene` escape hatch.

This refactor applies the world-scene lookup consistently to primitive,
directional-light, and sky components. A preview world must not partially use
its own primitive scene while its lights still mutate `GEngine->GetMainScene()`.

### Raw Proxy Pointers Are Bounded Borrows

`FStaticMeshSceneProxy`, `FTextureCubePreviewSceneProxy`, and other component
render-state objects may retain a raw `FStaticMeshRenderData*` only under this
contract:

- the pointer is captured when render state is created;
- all commands using that proxy execute before its removal command;
- every removal command is queued before the pointed-to render data's resource
  release commands;
- the release fence is queued after every release command;
- render-data C++ storage remains uniquely owned until the fence completes.

The FIFO command order is:

```text
earlier draw/use
-> remove old proxy/render state
-> release old vertex factories
-> release old index/vertex buffers
-> release fence
-> destroy old render-data storage
```

Keeping a raw pointer is therefore acceptable only inside component-owned
render state. Synchronous game-thread/editor inspection through
`GetRenderData()` must not retain the returned pointer across a replacement
call.

### Render-State Recreate Context

`FStaticMeshRenderStateRecreateContext` is the Durin counterpart to UE's
component context.

On construction it:

- snapshots every live `DStaticMeshComponent` using the mesh with generation
  checked object handles;
- requests destruction/removal of their old render state through each
  component's retained scene endpoint.

On destruction it:

- rebuilds component material/proxy snapshots from the new current render data;
- refreshes bounds or other current component state where required.

The context applies only to live synchronous replacement. Asset destruction
does not recreate render state: ordinary reachability keeps a mesh alive while
a component references it, package/world and preview-scene teardown unregister
components, and the engine shutdown coordinator detaches remaining
process-owned scenes before the DObject drain. No context destructor may
recreate a proxy for an object already in `BeginDestroy`.

Every long-lived raw-pointer consumer must be component-owned. Direct editor
proxy installation is removed instead of being accommodated with callbacks or
an asset-side registry.

### Current Resource Initialization

`DStaticMesh::InitResources` is the sole high-level initializer for current
render data.

- It assigns owner diagnostics to every child.
- It queues LOD buffer initialization before per-LOD vertex factories.
- The initialization command records whether the current render data is ready
  or failed.
- It is idempotent for the current render data.

The renderer has no scene-wide StaticMesh initialization hook. Draw code only
validates readiness and renders a proxy whose component already requested asset
initialization before proxy addition.

Initial load or transient creation with no active RHI may retain CPU render
data in an uninitialized state. The first render-state creation requests
initialization before it queues proxy creation.

### Replacement Candidate Publication

Replacement retains the last successful live render data until the candidate is
known to be renderable:

1. build and CPU-validate a detached candidate;
2. queue candidate resource initialization using non-owning pointers while the
   publication operation retains the candidate's unique ownership;
3. queue a candidate-init fence;
4. for current synchronous reimport/build APIs, wait for that targeted fence;
5. if initialization fails, release the candidate, wait for its targeted fence,
   destroy it, and keep the current asset/proxies unchanged;
6. construct `FStaticMeshRenderStateRecreateContext` for the current mesh;
7. move the old current render data into a local `std::unique_ptr`;
8. install the initialized candidate as `DStaticMesh::RenderData`;
9. queue reverse-order release for the local old render data, begin a targeted
   fence after those commands, and wait for that fence;
10. destroy the local old render data;
11. destroy the recreate context, which queues new component proxy creation
   after candidate initialization.

This targeted wait is not a full renderer flush. Synchronous publication is
intentionally serialized and cannot accumulate old render-data objects. A future
asynchronous publication design may introduce a dedicated compilation result
owner, but it must not turn `DStaticMesh` into a general retired-version queue.

### Asset Destruction

`DStaticMesh::BeginDestroy`:

1. relies on the existing `DObject` destruction state as the mutation boundary;
2. queues reverse-order release for the current render data;
3. begins its single `ReleaseResourcesFence`;
4. calls `Super::BeginDestroy`.

`DStaticMesh::IsReadyForFinishDestroy` returns true only when:

- its single release fence is complete;
- the superclass is ready.

`FinishDestroy` verifies the fence, verifies that every nested resource is
unregistered, and clears current render data. GC therefore defers destruction
without blocking `BeginDestroy`. During orderly engine shutdown,
process/subsystem scene owners and components detach before the DObject drain;
`DStaticMesh` does not discover or manage its consumers.

### Resource and RHI Release

For one render-data object, initialization is:

```text
LOD vertex/index buffers
-> LOD vertex factories
-> ready
```

Release is:

```text
LOD vertex factories
-> LOD index/vertex buffers
-> FRenderResource registry empty for this render data
-> release fence
-> render-data C++ destruction
```

Final RHI object destruction may remain in the RHI deferred-delete queue after
the render data's RHI references are dropped. Engine shutdown drains that queue
after RenderCore resource and StaticMesh resource audits pass.

Repeated init/release is idempotent. Partial initialization failure releases
all successfully initialized children before the candidate's targeted fence
can complete.

### Import, Reimport, and Exchange

Detached import candidates must not initialize against or inherit the live
asset's identity before bundle commit.

`ExchangeImportedState` no longer uses a raw swap of active `RenderData`,
initialization flags, or release fences between assets. The import workflow
instead:

1. builds detached authored state plus detached render data;
2. commits source, material-slot, manifest, and cooked metadata to the intended
   asset;
3. invokes the asset-qualified replacement publication path;
4. performs a symmetric replacement request when bundle rollback restores old
   authored state.

The destination receives the initialized candidate. The displaced data is
released first and only then moved to the candidate object as CPU rollback
state. Owner diagnostics are rebound on initialization, while asset destruction
fences never move.

### No StaticMesh Reference or Counted Handle

StaticMesh does not need texture-style RHI indirection:

- a texture is one bindable RHI identity that can retarget without rebuilding
  every consumer;
- mesh render data contains buffers, sections, bounds, slot layout, and vertex
  factories, so structural replacement recreates proxy state;
- unique current/local-old ownership plus explicit fences already protects
  component proxy borrows.

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
- primitive scene IDs and the scoped recreate context provide component
  render-state recreation without a second asset-side notification scan.
- `DWorld`, `DLevel`, actor/component registration, and `IScene` already provide
  the pieces needed for a reusable preview world; ownership is not yet joined.

### Gaps

- the removed public `SetRenderData` destroyed old storage before queued
  proxies stopped using it.
- `DStaticMesh` has no initialization state, release fence, or destruction
  readiness override.
- production replacement and destruction do not release initialized nested
  `FRenderResource` objects.
- initialization is renderer-discovered and repeated during scene preparation.
- editor preview and thumbnail paths bypass components and install StaticMesh
  raw-pointer proxies directly.
- `DEngine` owns `MainScene` separately from `DWorld`, and primitive, light, and
  sky components discover it through `GEngine`; this prevents an independent
  preview world from using ordinary component registration.
- `ExchangeImportedState` swaps live render ownership between package
  identities.
- selected tests manually call `ReleaseResources`, masking missing asset
  ownership.
- no scheduler-controlled test pauses replacement or destruction at detach,
  release, fence, and final-delete boundaries.

## Implementation Stages

### Stage 0: Freeze Consumers and Unsafe Schedules

- [x] Record baseline commit `9cfee542` and the initial working set.
- [x] Inventory every stored, captured, returned, and immediate-use
  `FStaticMeshRenderData*` and classify it as detached build access,
  synchronous inspection, registered render state, or queued render-thread
  borrow.
- [x] Identify the detach/recreate entry point for components, material
  thumbnails, TextureCube previews, and every auxiliary scene.
- [x] Add scheduler-controlled tests paused before old-proxy removal, resource
  release, fence completion, and render-data destruction.
- [x] Reproduce replacement while an old draw/proxy remains queued and asset GC
  after buffers initialize.
- [x] Cover registered, unregistered, garbage-marked, preview, thumbnail,
  rapid-replacement, and no-RHI cases.
- [x] Pin DMSH, DDC, material-slot, bounds, and rendered-output behavior.
- [x] Record diagnostics sufficient to prove exact command and destruction
  order without timing sleeps.

Dependencies: none.

#### Acceptance Gate

- Every long-lived consumer has a selected detach/recreate path; unsafe
  replacement and unload schedules fail deterministically against the baseline;
  and compatibility baselines remain unchanged.

### Stage 1: Normalize Component-Owned Render State

- [x] Add `EWorldType::Preview` and a world renderer-scene endpoint with an
  explicit owner-before-world teardown contract.
- [x] Make primitive, directional-light, and sky components obtain their scene
  from their owning world, retain it while registered, and use it for every
  add, update, and remove operation.
- [x] Add a reusable `FPreviewScene` that owns a preview world, level, renderer
  scene, and ordinary preview actors/components, with optional play/tick.
- [x] Add `FStaticMeshRenderStateRecreateContext` with generation-checked
  `DStaticMeshComponent` handles; construction detaches and destruction
  recreates.
- [x] Convert material thumbnails and MaterialPreview to ordinary
  `DStaticMeshComponent` instances in `FPreviewScene`.
- [x] Convert TextureCube thumbnail geometry to a transient component that owns
  its specialized proxy inside `FPreviewScene`.
- [x] Remove direct long-lived editor installation of proxies containing
  `FStaticMeshRenderData*`.
- [x] Keep proxy render-data fields non-owning and document their
  component-render-state bounded-borrow contract.
- [x] Add focused component and preview tests for registered, unregistered,
  reassigned, garbage-marked, reused-generation, auxiliary-scene, and
  no-`GEngine` detach cases.

Dependencies: Stage 0.

#### Acceptance Gate

- Every long-lived StaticMesh proxy is owned by component render state; main and
  preview components obtain stable scene endpoints through their worlds;
  preview teardown is ordered; and `DStaticMesh` contains no consumer registry,
  callback list, world, or scene discovery logic.

#### Stage 1 Implementation Handoff

- Baseline: `1115c0dd`.
- Runtime working set: `DWorld`, `DEngine`, `DSceneComponent`, primitive,
  directional-light, and sky components, plus
  `FStaticMeshRenderStateRecreateContext`.
- Editor working set: reusable `FPreviewScene`, ordinary StaticMesh preview
  components, the specialized `DTextureCubePreviewComponent`, MaterialPreview,
  and the rendered-thumbnail preview-scene pool.
- Key decisions: the scene remains owned by the host (`DEngine` or
  `FPreviewScene`) and is non-owningly exposed by its world; components capture
  it at registration; scene changes re-register only components that were
  already registered; preview teardown unregisters the level before releasing
  scene storage; and recreate-context destruction revalidates both mesh and
  component generations plus current assignment.
- Open question carried to Stage 2: initial resource initialization must precede
  component proxy-add commands without restoring renderer-side per-frame
  initialization.
- Validation: `MaterialTests` 45/45, `StaticMeshTests` 53/53,
  `ThumbnailTests` 45/45, `WorldTests` 35/35, `SkyBoxTests` 9/9, and
  `StaticModelImportVulkanTests` 1/1 passed; the full `all` target built
  successfully for `Win64-Debug-DurinEditor-Tests`.

### Stage 2: Add Explicit Asset Resource Lifecycle

- [x] Add `DStaticMesh::InitResources`, `ReleaseResources`,
  `AreRenderingResourcesInitialized`, resource state, and diagnostic
  accessors.
- [x] Add one asset-owned `ReleaseResourcesFence` used only by
  `BeginDestroy`/`IsReadyForFinishDestroy`/`FinishDestroy`.
- [x] Implement reverse-order release, partial-init rollback, and idempotent
  init/release.
- [x] Remove per-frame initialization ownership from
  `FRendererModule::PrepareSceneResources`.
- [x] Ensure initialization commands precede every component proxy-add command.
- [x] Add focused tests for initial init, repeated init, partial failure,
  non-blocking GC deferral, final resource-registry cleanup, and no-RHI
  destruction.

Dependencies: Stage 1.

#### Acceptance Gate

- The asset has exactly one current render-data object and one destruction release
  fence; GC defers without blocking; and every initialized nested resource
  unregisters before render-data destruction.

#### Stage 2 Implementation Handoff

- Baseline: `c379c1f7`.
- Runtime working set: `DStaticMesh`, `FStaticMeshRenderData`,
  `DStaticMeshComponent`, `DTextureCubePreviewComponent`, renderer interfaces,
  engine and thumbnail render calls, and StaticMesh proxy lifetime diagnostics.
- Key decisions: there is no StaticMesh lifecycle revision; state belongs to
  the one current render-data object. A component requests initialization
  synchronously on the game thread before its scene queues proxy addition.
  Initialization is idempotent, validates every LOD, and rolls back all earlier
  LOD resources if a later LOD fails. Release walks LODs and nested buffers in
  reverse order.
- Destruction decision: `BeginDestroy` queues release and begins the one asset
  fence when initialization or release work was submitted. A mesh that never
  submitted render work, including no-RHI use, keeps the fence in its default
  completed state and destroys without consulting global engine or command-pipe
  phases.
- Removed fallback: `IRendererModule::PrepareSceneResources` and all callers
  were deleted; draw paths only consume ready resources and skip unavailable
  data.
- Open question carried to Stage 3: candidate initialization and synchronous
  replacement must bind commands to the exact candidate/current render-data
  pointer while their unique owners retain storage through targeted fences.
- Validation: `StaticMeshTests` 54/54, `MaterialTests` 45/45,
  `ThumbnailTests` 45/45, `StaticModelImportVulkanTests` 1/1,
  `SkyBoxVulkanIntegrationTests` 1/1, and
  `TextureCookIntegrationTests` 1/1 passed; the full `all` target built
  successfully for `Win64-Debug-DurinEditor-Tests`.

### Stage 3: Publish Candidates and Integrate Asset Workflows

- [x] Remove public `SetRenderData`; keep detached candidate publication private
  to `DStaticMesh` with no public builder/test lifecycle helper.
- [x] Keep public render-data inspection const and side-effect-free; expose no
  public release, aggregate resource-state, or destruction-fence controls.
- [x] Initialize replacement candidates before live commit and use a targeted
  candidate-init fence for current synchronous APIs.
- [x] Keep the last successful asset/proxies unchanged when candidate
  initialization fails; release and destroy the failed detached candidate after
  its targeted fence.
- [x] Serialize synchronous replacement: detach components, move current data
  to a local unique owner, install the candidate, release/fence/delete the local
  old render data, then recreate components.
- [x] Route DDC hit, source rebuild, cooked load, debug/transient creation,
  reimport, undo/redo, and rollback through the same lifecycle.
- [x] Refactor `ExchangeImportedState` and static-model bundle commit/rollback
  so active resource state and release fences never move between assets.
- [x] Remove manual test-only mesh `ReleaseResources` compensation.
- [x] Validate serialized consecutive replacement, failed replacement, unload,
  package reload, ordinary GC, and engine exit.
- [x] Audit current StaticMesh resources by owner, state, and fence
  status at final shutdown.

Dependencies: Stage 2.

#### Acceptance Gate

- Every asset workflow uses one publication primitive; failed candidates retain
  the last successful mesh; package-qualified lifecycle state never swaps
  between assets; and shutdown leaves no initialized mesh resource.

#### Stage 3 Implementation Handoff

- Baseline: `7f9b6c3a`.
- Runtime working set: `DStaticMesh`, its private candidate initialization and
  retirement helpers, and `FStaticMeshRenderStateRecreateContext`.
- Editor working set: static-model bundle mutation/rollback and retained
  preview-asset teardown.
- Key decisions: no candidate publication method is public. Initial load only
  installs CPU data; first component registration initializes it. Replacement
  alone performs the synchronous candidate-init wait. Recreate-context
  destruction performs the one component update, so there is no overlapping
  `NotifyLoadedComponents` scan. Following UE's semantic split,
  `InitResources()` is explicit and `GetRenderData()` performs no initialization;
  unlike UE's broad engine-internal API, Durin exposes only const render-data
  inspection and keeps release/state/fence control private.
- Rollback decision: `ExchangeImportedState` transfers a ready candidate into
  the destination, releases the displaced data behind a local fence, and leaves
  that released CPU data on the candidate object. A symmetric call restores it;
  neither asset's resource state nor destruction fence is transferred.
- Validation: `StaticMeshTests` 54/54, `MaterialTests` 45/45,
  `TextureTests` 69/69, `ThumbnailTests` 45/45,
  `StaticModelImportVulkanTests` 1/1,
  `SkyBoxVulkanIntegrationTests` 1/1, and
  `TextureCookIntegrationTests` 1/1 passed. The Vulkan lifecycle case covers
  consecutive publication, symmetric rollback, candidate-init failure,
  last-successful retention, resource-count stability, GC, package
  reload/unload, and clean RHI exit. The full `all` target built successfully
  for `Win64-Debug-DurinEditor-Tests`, and the plan validator passed.

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

Dependencies: Stage 3, Stage 3 of
`EngineTerminationLifecycle.md`, and the implementation stages of
`StaticMeshLODResourcesRefactor.md`.

#### Acceptance Gate

- Focused and full validation passes; payload and rendered baselines remain
  unchanged; replacement uses only targeted fences; final RenderCore/RHI audits
  are clean; and runtime documentation is authoritative.

## Validation Matrix

| Boundary | Required validation |
| --- | --- |
| Unique ownership | Detached candidate, current asset data, local old data during synchronous replacement, final destruction |
| DObject lifecycle | BeginDestroy, non-blocking readiness polling, FinishDestroy, repeated GC |
| Command order | Draw/use, detach, reverse release, fence, render-data destruction |
| Candidate publication | Init fence, success, partial failure, last-successful retention |
| Buffer lifecycle | Complete/partial init, retry, reverse release, registry removal |
| Components | Registered, unregistered, reassigned, garbage-marked, reused generation, main and preview worlds |
| Preview world | Scene ownership, optional tick, component-before-scene teardown, no main-scene mutation |
| Editor previews | Material thumbnail, TextureCube thumbnail, MaterialPreview through preview-world components |
| Import | DDC hit, source rebuild, transient/debug, reimport, bundle commit/rollback |
| Asset lifecycle | Serialized replacement, package unload/reload, no RHI, shutdown |
| Compatibility | DMSH fixture, DDC key, cook payload, slots, bounds, rendered output |
| Performance | Targeted fence duration, proxy churn, no duplicate per-component geometry |

All configure, build, test, and runtime actions follow
`Documentation/Development/Build/BuildAndRun.md`; this plan does not duplicate
profile-specific commands.

## Definition of Done

- `DStaticMesh` uniquely owns current `FStaticMeshRenderData` and explicitly
  initializes/releases its resources.
- Detached candidates, current data, and the synchronous local old render data
  preserve exactly one owner during build, replacement, failure, and
  destruction.
- No StaticMesh reference, counted render-data handle, or proxy-side concrete
  ownership is introduced.
- Every long-lived raw-pointer consumer is component-owned and participates in
  render-state detach/recreate.
- Main and preview scenes share the same world/component registration contract;
  particle and effect previews require no new scene-lifetime model.
- Replacement order is draw → detach → reverse release → fence → delete, and
  new resource init precedes new proxy creation.
- GC uses `IsReadyForFinishDestroy` rather than blocking `BeginDestroy`.
- Failed replacement retains the last successful published mesh.
- Vertex factories release before LOD buffers and every nested
  `FRenderResource` unregisters before render-data destruction.
- Import, rollback, unload, GC, and shutdown require no manual test-only
  release.
- DMSH, DDC, cook, package, slot, bounds, and rendered-output contracts remain
  compatible.
- Final StaticMesh resource, RenderCore registry, deferred cleanup, command-pipe,
  and RHI deletion audits are empty.
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

- [Engine Termination Lifecycle](EngineTerminationLifecycle.md)
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
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
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
