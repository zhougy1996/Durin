# Render Resource Lifecycle

Summary: Define RenderCore resource state, deferred C++ cleanup, producer teardown, and shutdown auditing.

Modules: RenderCore, Engine, MonaImGui

Last reviewed: 2026-09-03

`FRenderResource` owns registry membership and the rendering-thread
initialization, update, and release state machine. This contract covers generic
RenderCore lifetime; asset-specific publication and RHI command execution stay
with their owning systems.

## Command Admission Boundary

RenderCore command admission is observable as `Stopped`, `Running`, or
`Draining`. Normal initialization moves it to `Running`. `TryEnqueue` reports
whether work was accepted; the compatibility enqueue entry point turns
post-close submission into an immediate actionable check. Producers stop
submitting before final shutdown begins.

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

Released concrete C++ storage is transferred to
`FDeferredRenderResourceCleanup`. Its ordered rendering-thread flush destroys
storage only after every earlier command that could use the non-owning pointer
has finished. Producers must stop publication, enqueue release, and preserve
their storage until that cleanup boundary; releasing an RHI reference alone
does not authorize immediate C++ destruction.

Texture assets apply this rule to their stable `FTextureReference` and concrete
`FTextureResource` ownership. Their publication, replacement, invalidation, and
asset diagnostics are defined by [Texture System](TextureSystem.md).

## Cooked mesh readiness

StaticMesh and SkeletalMesh CPU residency is independent of render-resource
readiness. `EnsureRenderDataLoadedBlocking()` waits for CPU residency only;
`FCookedMeshBlockingResult::Succeeded()` reports that CPU result. It does not
request GPU initialization. Call `InitResources()` explicitly, then query
`GetRenderResourceStatus()` to distinguish queued, ready, and failed resources.
`HasPendingRenderResourceInitialization()` is a nonblocking convenience query
that returns true only while GPU initialization is queued. False does not imply
readiness: CPU loading, absent, failed, and released resources also return false.
An explicit `EnsureRenderDataLoadedBlocking()` call also retries a previous CPU
failure/cancellation. An explicit `InitResources()` call also retries failed GPU
initialization using resident CPU data without performing package I/O. Neither
operation repeats completed work or duplicates an in-flight initialization.

`RequestRenderDataAndResources()` retains its nonblocking combined contract and
does not retry CPU failures/cancellations or failed GPU initialization. Components
and per-frame editor previews use this entry point to avoid repeated failed work.
Its current cooked CPU candidate is published on GameThread and queues resource
initialization immediately. A CPU-only blocking call can finish previously
requested combined work, and registered rendering consumers may still request
GPU initialization when CPU publication recreates their render state. The asset
reports the queued state before the rendering thread executes that command and
advances its resource revision again when initialization reaches Ready or Failed.

A StaticMesh, SplineMesh, or SkeletalMesh SceneProxy may be created from valid
CPU data while GPU initialization is queued. Renderer preparation still checks
the selected LOD's actual resources and rejects or falls back until they are
ready. Because CPU publication recreates registered component render state, the
queued-to-ready transition needs no warm-up getter, second asset assignment, or
external render-state mutation.

Initialization failure is sticky for the current asset/resource generation and
never leaves a partially initialized LOD accepted by the renderer.
Calling `InitResources()` explicitly transitions a failed resource back to queued
using the same validated CPU data. Replacement, unload, and destruction first
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
[Runtime Lifecycle](../Core/RuntimeLifecycle.md). Module-owned callable and DLL
retirement rules are defined by
[Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md).

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
[RHI Command Execution](RHICommandExecution.md).

## Related Documentation

- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Texture System](TextureSystem.md)
- [Renderer Resource Recovery](RendererResourceRecovery.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
