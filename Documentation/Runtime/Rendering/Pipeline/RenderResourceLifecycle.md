# Render Resource Lifecycle

Summary: Define RenderCore resource state, deferred C++ cleanup, producer teardown, and shutdown auditing.

Modules: RenderCore, Engine, Renderer, MonaImGui

Last reviewed: 2026-11-12

`FRenderResource` owns registry membership and the rendering-thread
initialization, update, and release state machine. This contract covers generic
RenderCore lifetime; asset-specific publication and RHI command execution stay
with their owning systems.

## Command Admission Boundary

RenderCore command admission is observable as `Stopped`, `Running`, or
`Draining`. Normal initialization moves it to `Running`. Render commands
require running admission; submission after closure is a contract failure.
Producers stop submitting before final shutdown begins.

## Resource State

Producer-side begin operations enqueue lifecycle work. Render-thread operations
assert affinity and reject invalid double initialization or release.
Game-thread owners queue transitions through
`FRenderResource::BeginInit_GameThread()`,
`BeginUpdateRHI_GameThread()`, and `BeginRelease_GameThread()`. These entry
points assert the caller thread and preserve command ordering; actual
`InitResource`, `UpdateRHI`, and `ReleaseResource` work remains
rendering-thread only.

## Deferred C++ Cleanup

GPU completion and allocation reuse follow the owning
[RHI sync-point contract](../RHI/RHICommandExecution.md). Retaining or releasing a
`FRHIGPUSyncPointRef` affects metadata lifetime only; it never cancels executable
work or proves that an allocation's other queue uses have retired. Canceled
detached work may permit retirement, but cannot make its output ready.

Released concrete C++ storage is transferred to
`FDeferredRenderResourceCleanup`. Its ordered rendering-thread flush destroys
storage only after every earlier command that could use the non-owning pointer
has finished. Producers must stop publication, enqueue release, and preserve
their storage until that cleanup boundary; releasing an RHI reference alone
does not authorize immediate C++ destruction.

Texture assets apply this rule to their stable `FTextureReference` and concrete
`FTextureResource` ownership. Their publication, replacement, invalidation, and
asset diagnostics are defined by [Texture System](../../Assets/Families/TextureSystem.md).
Concrete resources and stable references retire through deferred cleanup;
render commands do not retain the texture UObject, and asset destruction does
not join GPU initialization, including when the Task system has already stopped.

Saved package reload builds each candidate texture against its own unpublished
`FTextureReference`. The coordinator completes selected compilation and CPU cache
work through existing interfaces. With an active RHI, a generic render-command
fence waits for pending uploads before resource validation and object publication;
rejection therefore leaves the live texture and its resource intact. There is no
texture-specific reload completion interface.
Headless CPU validation accepts the prepared platform product but is not GPU
qualification evidence. After reference commit, cloud and material consumers
refresh their native texture-reference caches, and displaced resources follow the
ordinary deferred cleanup boundary.

## Source-owned draw resources

`CreateRenderResource<T>()` pairs shared source ownership with
`DeleteRenderResource`. While admission is running, deletion queues `ReleaseResource` when initialized
followed by deferred cleanup, without reading render-thread state on the releasing
thread. After admission closes, only uninitialized storage can be deleted directly. Last initialized
owner release requires running command admission. Producers therefore release
these owners before admission closes. `VisitInitializedRenderResources` is a
render-thread-only registry visitor; callbacks must not change registry membership.
Bulk release collects matching resources before releasing them. Deferred cleanup
removes storage from its queue under the queue lock, then destroys it outside that
lock: source destructors may release child resources and reenter cleanup.

Material proxies own `FUniformExpressionCache` and its real `FUniformBuffer`.
Standalone material representations own an expression cache too. Scene owns live
Collection buffers; material caches own compiled-program defaults. Primitive scene
information owns primitive buffers. Vertex-factory input bindings own deformation
buffers. HitProxy request storage owns its temporary pipelines and Collection
buffers. Cached `FMeshDrawCommand` values borrow these resources directly and
store non-owning `FMeshDrawShaderBindings`. Cache and visible storage retain only
command memory; weak source/pipeline references reject expired sources.

Renderer device-resource release retires preparations and clears command metadata,
then releases registered primitive owners, material proxies and uniform resources.
This filtered traversal leaves independently owned asset resources untouched.
`ReleaseResource` unregisters surviving source objects; they initialize lazily on
next use, preserving the shutdown requirement of an empty registry.

### Required lifetime boundaries

- Collection, preparation and resolved draw handles belong to one render frame.
  Source removal invalidates preparation. CPU fact retention does not authorize
  rendering against removed sources or replaying earlier Uniform contents.
- CPU preparation and parallel recording tasks must complete before source
  mutation/replacement/release. View preparation joins its task work; RDG recording
  waits for outstanding recording tasks on success and failure before returning.
  These operations remain inside the ordered render-command scope.
- Uniform updates and draws preserve command order. Persistent buffers retain
  current contents only; view/pass data uses separate frame buffers. RHI commands
  own copied update bytes and binding payloads until replay, and native allocation
  retirement follows GPU completion. Renderer does not retain historical Uniform
  snapshots or reference-counted binding packets.
- Texture native replacement may occur without a new material publication, so
  resource resolution still checks the selected native resources. Buffer/object
  replacement invalidates bindings; numeric updates preserve logical identity.

These boundaries distinguish source ownership, CPU recording lifetime and GPU
retirement without adding another owner registry. Preparation and cache rules are
specified in [Renderer Frame Preparation](RendererFramePreparation.md).

## Cooked mesh readiness

StaticMesh CPU residency is independent of render-resource readiness.
`GetRenderData()` observes residency, `HasPendingRenderDataLoad()` observes the
asset's current update, and `GetRenderDataLoadError()` returns its last owned
failure. The asset holds one concrete pending update; that update owns reads,
detached decoding, publication and cancellation. The manager schedules updates
within active-work and retained-completion budgets.
`EnsureRenderDataLoadedBlocking()` returns `std::expected<void, FCookedMeshLoadError>`
and waits for CPU residency only. It does not request GPU initialization. Call `InitResources()` explicitly, then query
`GetRenderResourceStatus()` to distinguish queued, ready, and invalid-CPU-data resources.
`HasPendingRenderResourceInitialization()` is a nonblocking convenience query
that returns true only while GPU initialization is queued. False does not imply
readiness: CPU loading, absent, invalid-data, and released resources also return false.
An explicit `EnsureRenderDataLoadedBlocking()` call also retries a previous CPU
failure/cancellation. `InitResources()` initializes resident CPU data without
performing package I/O; native creation failure terminates through RHI. Neither
operation repeats completed work or duplicates an in-flight initialization.

`RequestRenderDataAndResources()` retains its nonblocking combined contract and
does not retry CPU failures/cancellations or invalid CPU geometry. Components
and per-frame editor previews use this entry point to avoid repeated failed work.
Its current cooked CPU candidate is published on GameThread and queues resource
initialization immediately. A CPU-only blocking call can finish previously
requested combined work, and registered rendering consumers may still request
GPU initialization when CPU publication recreates their render state. The asset
reports the queued state before the rendering thread executes that command.
Resource revision identifies publication/init/release admission, and does not
advance merely because the render command completes.

A StaticMesh or SplineMesh SceneProxy may be created from valid
CPU data while GPU initialization is queued. Renderer preparation still checks
the selected LOD's actual resources and rejects or falls back until they are
ready. Because CPU publication recreates registered component render state, the
queued-to-ready transition needs no warm-up getter, second asset assignment, or
external render-state mutation.

Invalid CPU geometry reports `InvalidData` and never exposes a partially
initialized LOD. Correct/rebuild the asset data before requesting it again.
There is no recoverable native GPU initialization failure or GPU retry state. Replacement, unload, and destruction first
invalidate component state, then enqueue release and retain displaced RenderData
until the deferred C++ cleanup boundary.

## Producer Teardown

An unloadable producer stops admission and flushes accepted render commands
while its native code and the RHI backend remain mapped. MonaImGui first flushes
previously accepted draw or upload work that can retain non-owning viewport or
backend pointers. It then destroys platform and renderer viewport data,
releases backend RHI ownership, and flushes newly queued release work before
module shutdown returns.

Process-level consumer detachment and module ordering are defined by
[Runtime Lifecycle](../../Application/RuntimeLifecycle.md). Module-owned callable and DLL
retirement rules are defined by
[Modular Features And Module Retirement](../../Core/ModularFeaturesAndModuleRetirement.md).

## Shutdown Audit

Final rendering-thread shutdown requires an empty render-resource registry and
deferred C++ cleanup queue. A live resource reports its type and, in Debug
builds, its asset owner; pending cleanup reports whether rendering-thread
release completed. Shutdown never clears unexplained entries to make the audit
pass. Debug builds reject live resources and pending cleanup at their owning
boundary, while non-Debug builds execute the same drains and shutdown calls.
Control-flow side effects never belong inside `check` or `checkf`.

Render-command admission, accepted-command drain, RHI deferred deletion, the
terminal backend marker, and thread-stop diagnostics are defined by
[RHI Command Execution](../RHI/RHICommandExecution.md).

## Related Documentation

- [Runtime Lifecycle](../../Application/RuntimeLifecycle.md)
- [RHI Command Execution](../RHI/RHICommandExecution.md)
- [Texture System](../../Assets/Families/TextureSystem.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
