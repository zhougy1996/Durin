# Load Preparation

Summary: Prepare cooked material programs and exact scene Shader/PSO resources before covered drawing, with bounded ownership and explicit readiness.

Modules: Engine, RenderCore, Renderer, LevelEditor

Last reviewed: 2026-10-20

## Object and program readiness

Async asset completion retains its existing object-readiness meaning. A loading
owner may independently call `FMaterialProgramPreparation::Request` on the game
thread before accepted-program demand. The request snapshots the bounded cooked
bulk range/content revision and quality, feature level, backend and render-state
revision. A same-sized inline replacement or admitted write invalidates a pending
request even when its range metadata and material edit version are unchanged. Package
I/O and family decode run without dereferencing live objects. `Poll` admits only
a current result on the game thread, through the same declaration, collection,
properties and proxy publication checks as demand loading.

Program preparation reports Pending, Ready, Failed, Canceled or Stale. Ready
retains the immutable accepted program; failure does not become asset-load
failure. Cancellation withdraws one request. Copies alias that request. Separate
requests do not cancel each other. Unload, retired package storage, changed
configuration or changed material revision prevents pending publication. At most
16 program owners are admitted, each with the existing 8 MiB encoded family
limit. Decoded products and package-task storage have additional allocation cost;
these limits are not a process RSS bound.

Cooked Game startup explicitly requests the default level’s selected Local/Spline
mesh CPU/GPU residency in batches of four before registration, then collects the default
level's effective mesh-slot materials and their parent chains, and prepares at
most eight programs concurrently. The collection is limited to 4,096 owners.
It activates the level after admission, then requests main-viewport resource
preparation. Materials introduced by gameplay or later streaming retain demand
loading. Authored startup keeps its existing material-program loading behavior.

## Scene resource readiness

A caller creates `FRenderResourcePreparation`, then pumps
`IRendererModule::PrepareViewResources` on the render thread with the actual
scene, view, output texture, present policy and render options. It must preserve
the intended configuration through activation. The result covers the reachable,
resident routes selected by those inputs, not every future scene combination.

Preparation runs normal logical, view-resource, scene-resource and feature-resource
phases with their normal typed ShaderMaps and exact PSO descriptions. It returns
before graph authoring, temporal-history Begin/Commit or scene drawing. A pending
pipeline returns Pending without joining its compile task. Each pump rebuilds a
local logical snapshot; no borrowed frame/proxy values are sent to workers.

One pump admits at most eight missing native Shaders and sixteen new pipeline
requests. A nominal 2 ms deadline is checked between those operations, allowing
one operation per pump for progress. A native call is not preemptible and cached
logical traversal is not a hard frame-time bound. Pending work is revisited by the
owner. Pipeline requests inherit the existing normal service priority; loading
owners choose pump order rather than introducing a second scheduling queue. Terminal resource errors produce Failed rather than false readiness.

The handle retains ShaderMap code/resources, successful transactional slot
payloads and shared pipeline request identities. Resident slots also keep their
bounded request identities, so normal drawing can reuse the same weak-cache
entry. Existing ready slots participate in retention. Each of at most four active
scene owners admits 4,096 retained objects and 256 MiB of accounted code/metadata.
The accounting includes explicit code bytes, payload objects and PSO keys; nested
payload allocations, driver allocations and process RSS are not measured by it.

Copies alias one preparation observer. Separate owners share normal ShaderMap and
pipeline caches. Canceling one observer does not cancel shared compilation.
Renderer prunes released/canceled owners on its render thread. Scene release and
renderer Shader/device invalidation retire matching ownership as Stale, clear
retained resources and prevent reuse of that readiness result. The caller must
create a new request after invalidation.

## Activation and uncovered work

`FSceneViewport::RequestResourcePreparation` enables an explicit drawing barrier.
The viewport pumps preparation until Ready and retains its handle through normal
drawing. Failed preparation remains observable and does not draw a falsely ready
scene. Cooked default-level startup and the Level Editor's main and camera-preview
viewports enable this barrier. Restoring a level in the editor requests a fresh
preparation for both viewports. The editor continues submitting UI frames while
the scene's Shader and PSO preparation is pending, instead of joining the first
scene's pipeline compilation on the rendering thread. Device invalidation
restarts a stale viewport preparation. Later scene/configuration changes and
streamed uses use the ordinary resource-phase fallback unless their owner
explicitly requests another preparation. Activated viewports observe newly
required Shader and pipeline admissions separately as uncovered work in the
preparation progress. Those counts are independent of Failed; a shared cache can
satisfy an uncovered pipeline admission without new native compilation.

A preparation handle can be released after the renderer slots have adopted the
ready resources. Residency and cache eviction remain bounded by their existing
owners. Holding a preparation handle extends resource residency through its
declared activation window; it does not promise universal PSO coverage.

## Related contracts

- [Shader cache and cooked delivery](ShaderCache.md)
- [Material program lifecycle](MaterialSystem.md)
- [Renderer frame preparation](RendererFramePreparation.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
