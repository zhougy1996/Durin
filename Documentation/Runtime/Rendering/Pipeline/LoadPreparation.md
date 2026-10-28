# Load Preparation

Summary: Prepare cooked material programs and exact scene Shader/PSO resources before covered drawing, with bounded ownership and explicit readiness.

Modules: Engine, RenderCore, Renderer, LevelEditor

Last reviewed: 2026-10-28

## Object and program readiness

Async asset completion retains its existing object-readiness meaning. A loading
owner may independently call `FMaterialProgramPreparation::Request` on the game
thread before accepted-program demand. The request snapshots the bounded cooked
bulk range/content revision and quality, feature level and backend. Parameter-only
material publications do not invalidate immutable cooked program decoding. A same-sized inline replacement or admitted write invalidates a pending
request even when its range metadata and material edit version are unchanged. Package
I/O and family decode run without dereferencing live objects. `Poll` admits only
a current result on the game thread, through the same declaration, collection,
properties and proxy publication checks as demand loading.

Program preparation reports Pending, Ready, Failed, Canceled or Stale. Ready
retains the immutable accepted program; failure does not become asset-load
failure. Cancellation withdraws one request. Copies alias that request. Separate
requests do not cancel each other. Unload, retired package storage, changed
configuration or replaced cooked content prevents pending publication. At most
16 read/decode continuations are admitted; each releases its admission on exit,
including cancellation and failure. Retaining terminal handles consumes no work
admission. Terminal results remain observable after cancellation. Each request
has the existing 8 MiB encoded family limit. Decoded products and package-task storage have additional allocation cost;
these limits are not a process RSS bound.

The Engine-owned MapChange handler requests selected Local/Spline mesh CPU/GPU residency
with at most four requests per advancement and waits for authored compilation
without joining it. Both authored and cooked operations collect visible effective
mesh-slot materials and their parent chains; cooked operations prepare at most
eight programs concurrently. `DMaterialInterface::IsCompiling` and `IsComplete`
are nonblocking program queries, independent of textures and component PSOs.
`GetUsedTextures` replaces its output with deduplicated effective textures from
the accepted program, including instance overrides. Only effective bindings,
not overridden parent textures, participate in scene texture readiness. Texture
CPU compilation and GPU publication use their existing owners; at most four
new texture loads/uploads are requested per advancement. Cooked texture CPU
loading retains the existing explicit synchronous lazy-load boundary; upload
completion is asynchronous and is never joined here. Collections
are limited to 4,096 owners. Game BeginPlay runs on the candidate before its final
product collection, so spawned initial resources participate in admission.
MapChange does not scan or wait for component PSOs. Components use their existing
delay-proxy policy and owner notifications to appear after required PSOs become
ready, including after map activation. Required PSO failures remain observable on
the component's requests and keep its proxy withheld without failing MapChange.
No candidate offscreen view is rendered. View-dependent Shader/PSO preparation
continues through the committed viewport; startup retains the generation-scoped
output and primary-window presentation gates. Later
streamed products retain their normal component/demand preparation gates.

Preparation uses the effective `FMaterialResource` returned by
`GetMaterialResource`, following the [material renderer boundary](../Materials/MaterialSystem.md#renderer-boundary).
`HasValidGameThreadShaderMap` checks program success and ShaderMap presence,
independently of compilation progress, textures and component PSOs.
The proxy separates `GetMaterialNoFallback`
from `GetMaterialWithFallback`. The former returns null when no valid accepted
material exists; the latter explicitly permits ErrorMaterial. Compilation is
queried independently through `IsCompiling`. Missing programs while compilation
is active do not increment invalid-material diagnostics. Mesh collection uses
the non-fallback query and propagates unresolved compilation into the existing
resource preparation batch around logical collection. An accepted previous
program remains usable during edits. PSO collection snapshots that program and
its effective properties together, without selecting a rendering fallback.
Unassigned slots still select the authored default material, while invalid
material data retains the explicit ErrorMaterial path.

The Editor thumbnail pool continues polling and retiring accepted jobs during
required scene loading, but `EndFrame(false)` defers new optional admissions.
Queued requests remain available when the host and required scene are ready.

## Scene resource readiness

A caller creates `FRenderResourcePreparation`, then pumps
`IRendererModule::PrepareViewResources` on the render thread with the actual
scene, view, output texture, present policy and render options. It must preserve
the intended configuration through activation. The result covers the reachable,
resident routes selected by those inputs, not every future scene combination.

Preparation runs normal logical, view-resource, scene-resource and feature-resource
phases with their normal typed ShaderMaps and exact PSO descriptions. It returns
before graph authoring, temporal-history Begin/Commit or scene drawing. A pending
pipeline returns Pending without joining its compile task. Rendering pipeline
requests require both an owning request scope and a running task scheduler;
missing host state never falls back to direct native PSO creation. Each pump rebuilds a
local logical snapshot; no borrowed frame/proxy values are sent to workers.
Each resource stage runs once per pump. Pipeline-batch capacity exhaustion is
reported as unavailable through a nonblocking check before Pending is returned.

Authored rendering demand shares CPU ShaderMap requests with `FShaderMapPreparation`.
Missing outputs are submitted asynchronously through `FShaderCompilingManager`,
and resource attempts remain Pending until a later frame consumes the completed
output. `InitializeShaderMapFromTypes` never compiles inline or waits, on any thread.
Shader reloads retire stale requests; terminal errors and cancellation retain
their existing outcomes. Startup and tools explicitly use
`InitializeShaderMapFromTypesSync`, `TryCompileSync`, or the manager's
`CompileMountedSync` / `CompileGeneratedSync` interfaces. These interfaces reject
rendering-thread calls; consumption-only capture also rejects synchronous map
initialization. Inline submissions are rejected by both the manager and backend
on the rendering thread, where shader request waits return WouldBlock.
Appending programs to `FShaderMapPreparation` resolves only the new entries;
invalidation or retrying a prior resolution error refreshes the complete list.

The default `FPreparedShaderMapScope` remains consumption-only for thumbnail
capture. Resource resolution opts into asynchronous demand, but a nested capture
forbids new requests throughout its scope. ShaderMap resolution returns explicit
Pending, success, failure or cancellation. Resource factories return Pending to
their slots, and the existing resource/pipeline preparation batch aggregates
unresolved slots without a Shader-specific thread-local counter or a fabricated
error. Nested slots remain retryable while independent terminal errors stay
observable. Read-only preparation lookup does not submit, retire or reprioritize
requests; creation, priority promotion and submission use the request path.

The shared preparation index holds weak references, not completed compiler
outputs. Explicit `FShaderMapPreparation` observers own their declared capture
window. Rendering resource slots retain shared preparation only while an attempt
is pending, then release it on publication, failure, cancellation, reset or
generation change. Published ShaderMaps retain their own code/resources; the
ShaderBuild output LRU remains the bounded compilation-output cache. Completion
callbacks hold weak preparation references, so abandoning the last owner cancels
unneeded work without a callback/handle ownership cycle. Other owners continue
sharing the same request. Direct thumbnail captures retain preparation through
their Preparing retries and release it before readback or on cancellation.
Hit-proxy queries use their cache or copied request as the owner of their
transient preparation window.

Material mesh passes share `MeshPipelineDescription`: raster options and the
effective material identity produce value-only raster/depth/blend state, and
explicit Forward, hybrid retained, sorted translucency, shadow, GBuffer and
HitProxy routes describe their attachment and pass-specific state. Collection
does not require a Scene, View, CommandList, native Shader or resident vertex
buffer. `MakeInitializer` adds the resolved Shader pair, vertex declaration and
pipeline layout before submission to the existing pipeline service. Normal
drawing and view preparation use these same builders. Shadow slot keys use the
same dynamic-bias normalization as the descriptions. This boundary does not
enumerate potential component uses or introduce automatic component precaching;
Shader selection and vertex declaration preparation remain separate contracts.

Vertex Factory types opt in with `EVertexFactoryFlags::SupportsPSOPrecaching`.
Their `GetPSOPrecacheVertexFetchElements(EVertexInputStreamType, Elements)` callback
describes fixed Default, PositionOnly or PositionAndNormalOnly input layouts
without creating RHI resources. Local and Spline use the same packed layout
provider as Local's actual stream setup; reduced layouts preserve stream indices
and strides. These layouts do not enable new drawing or Shader routes.
`FPSOPrecacheVertexFactoryData` owns an optional explicit Default element list
for mesh-dependent factories. Other stream types still resolve through the type.
Unsupported factories/streams, missing metadata and malformed declarations return
explicit errors; empty Default layouts require `SupportsManualVertexFetch`.
The value lives in RenderCore's `PSOPrecache.h` and retains CPU elements rather
than UE's native custom declaration, so collection requires no device or GPU
buffer residency. Type registrations must outlive the collected values.
`MatchesVertexElements` compares canonical input layouts for coverage validation;
the native request and the precache path share RHI's element validation rules.

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
Renderer prunes released, canceled and failed owners on its render thread.
Failed/canceled attempts immediately release preparation-only references and
accounted bytes; keeping a diagnostic handle does not keep an active service
slot. Failed remains the observable terminal result after cancellation, reload
or scene retirement. Pending and Ready observers still become Stale on
invalidation. Scene release and
renderer Shader/device invalidation retire matching ownership as Stale, clear
retained resources and prevent reuse of that readiness result. The caller must
create a new request after invalidation.

## Activation and uncovered work

`FSceneViewport::RequestResourcePreparation` enables an explicit drawing barrier.
The viewport supplies its observer in `FSceneViewRenderRequest::Preparation` to
one `RenderView` invocation, including after Ready. That invocation prepares the
current view once and continues directly into graph construction/drawing when
ready; Pending returns without drawing. The prepared frame is local to that call,
never borrowed across frames. `PrepareViewResources` remains an explicit
prepare-only operation for callers that do not want a draw. Observation scope
does not choose prepare-only execution or suppress the rendering submission serial.
A Ready handle describes the most recent invocation; changes to
view/output/features are rechecked before drawing. Repumping Ready replaces
its retained resource window; Pending pumps retain their partial progress.
Failed preparation remains observable and does not draw a falsely ready
scene. Cooked default-level startup and the Level Editor's main and camera-preview
viewports enable this barrier. Restoring a level in the editor requests a fresh
preparation for both viewports. The editor continues submitting UI frames while
the scene's Shader and PSO preparation is pending, instead of joining the first
scene's pipeline compilation on the rendering thread. Device invalidation
restarts a stale viewport preparation. Later scene/configuration changes and
streamed mesh components additionally use the component PSO gate below. Other
uses retain the ordinary resource-phase fallback unless their owner explicitly
requests preparation. Covered viewports keep required Shader and pipeline
admissions within their preparation budget on subsequent frames. Explicit
`FRenderResourcePreparationUseScope` consumers can still count uncovered
admissions independently of Failed; a shared cache can satisfy an uncovered
pipeline admission without new native compilation.

A preparation handle can be released after the renderer slots have adopted the
ready resources. Residency and cache eviction remain bounded by their existing
owners. Holding a preparation handle extends resource residency through its
declared activation window; it does not promise universal PSO coverage.

## Material and component PSO requests

`DMaterialInterface::PrecachePSOs` explicitly snapshots the accepted program and
effective material planning identity for a bounded list of Vertex Factories.
`RequiredPasses` must be a subset of the explicitly requested `Passes`.
The default overload uses the active engine's Renderer; preview/test owners may
pass an initialized `IRendererModule` explicitly. Rejection preserves the output
array and reports an error. Program loading remains a separate readiness step.

`FMaterialPSORequestManager` shares collection and compilation by material
identity, accepted target, canonical Default vertex elements, factory type and
`FPSOPrecacheParams`. Explicit and type-provided equal layouts share work.
Each call creates an independent `FMaterialPSOPrecacheRequestID` observer; copies
alias that observer. Canceling one observer does not cancel another's native
compilation. Failed collections remain observable rather than retrying each
frame. Shader reload, device/resource invalidation and Renderer shutdown retire
matching work as Stale and release collected resources on the render thread.
Material-owned observation scopes additionally retire that material's observers on
ShaderMap, pipeline, parent-chain changes and destruction, without invalidating
other materials sharing the same work. Dynamic parameter changes preserve them.
`IsCurrent` qualifies observer reuse independently of the work outcome: failed
diagnostics remain Failed after invalidation, but cannot authorize an old owner
even when its required PSOs completed before an optional PSO failed.

PSO work has one phase: Queued, Collecting, Compiling, Completed or Retired.
Completed work publishes one immutable success/error result; retirement revokes
reuse without erasing failure diagnostics. The first three phases map to public
Pending. Required readiness remains separate from all-work completion; priority
sorting uses a stable per-pump snapshot.

The Renderer permits up to eight active material collections across updates,
including their outstanding native compilation, independently of scene visibility.
Additional requests wait without expanding their pipeline resources. Its collector uses the normal typed ShaderMaps and shared
mesh PSO descriptions for explicit Forward, ShadowDepth, GBuffer, hybrid retained,
sorted translucency and HitProxy routes. Fixed Shader requests are started and
retained before nonblocking ShaderMap resolution. Unsupported requested routes
fail instead of claiming coverage. GPU-culling and global feature PSOs are outside
this material collector. Native compilation uses the existing service queue;
no additional compile thread pool is introduced.

There is no aggregate material-request count or aggregate Shader-code admission
limit. Waiting requests share accepted immutable programs; each program remains
bounded to 64 MiB of Shader code and each collection to 64 pipeline identities.
Concurrency bounds expanded work, not total queued metadata or process RSS.
Only pending work participates in the render-thread pump. Completed observers
retain identity and progress, while terminal pipeline observations, collection
resources and accepted-program references are released on the render thread.
The sharing index holds weak work references and prunes expired keys on admission.
Admission is thread-safe; Pump, Invalidate and Close are serialized on the
resource-owning thread. Close must precede backend/module shutdown.

Lower-level limits remain independent: the native queue admits 256 unfinished
requests, 16 MiB of pending descriptions and 4,096 observers; the shared pipeline
identity cache admits 4,096 live entries and metadata remains budgeted. Collector
admission pressure preserves completed passes and retries only the unfinished
suffix. Terminal pipeline references are released even while another pass awaits
capacity. A completed precache records successful preparation, not permanent
native residency. Vulkan may evict cache-only pipelines and reuse its driver
cache on recreation; drawing still acquires a real ready pipeline through normal
render preparation. Live draw/command references remain protected from eviction.

`FPSOPrecacheComponentData::SetMaterialPSOPrecacheData` replaces one component's
observer set and advances its JobSet. `Poll` runs on GameThread and invokes the
current set's successful completion callback once; old JobSet completions cannot
complete a replacement. Reset, unregister, proxy/material changes and teardown
withdraw the component's observation without canceling
peers. Callbacks may reset, replace or destroy their owner. Required-PSO readiness
is separate from all-work completion: `IsReadyForProxyCreation` checks required
requests, while `IsPSOPrecaching` includes outstanding optional compilation.
Failed, canceled and stale required requests never authorize readiness. Component
sets retain at most 256 observers.

## Automatic component collection and proxy publication

`DPrimitiveComponent::PrecachePSOs` calls the virtual `CollectPSOPrecacheData`
with an `FComponentPSOPrecacheParamsList`, checks readiness of the entire set, then submits its
material/Vertex Factory descriptions. PostLoad queues this work for the owner
thread, registration requests it before proxy publication, and mesh/material
replacement collects a fresh set. Missing Renderer, RHI or accepted material programs
defer submission before any prefix is accepted; no partial set authorizes a proxy.
Invalid or closed admissions remain terminal and observable. Lower-level capacity
pressure stays with the accepted material collection rather than resubmitting
the component's complete set. Proxy readiness checks do not retry deferred submission.
Oversized sets fail explicitly. `GetPSOPrecacheDiagnostic` exposes submission
diagnostics; individual request handles expose collection/native failures.

Static and Spline Mesh collectors deduplicate effective material slots, including
the initialized authored default material. Their fixed layouts need no resident
mesh buffers for collection. They request compatible Forward, hybrid retained,
shadow, GBuffer, sorted translucency and HitProxy routes. Eight raster variants
cover wireframe/solid, normal/reversed depth and both transform/instance windings
(wireframe is omitted on devices without non-solid fill);
spline scale sign changes additionally select the matching no-cull policy.
Ordinary transform, instance-count and instance-custom-data changes preserve the
set. An unavailable default retains the existing ErrorMaterial fallback path;
that path and global/GPU-culling PSOs are outside this component coverage.

The default `EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached` withholds
the proxy until every required request is ready. Failed required work remains withheld
and observable; optional failures do not revoke current required readiness. `AlwaysCreate` explicitly permits ordinary rendering while
preparation continues. Custom scenes without a PSO Renderer retain their existing
publication contract. Material ShaderMap/pipeline/parent changes immediately
withdraw affected gated proxies; dynamic numeric parameters preserve them.
Shader/device stale observations are resubmitted on the next owner tick.

Engine and world ticks call `ProcessPSOPrecacheRequests`, including paused and
editor worlds. Unsubmitted material/RHI demand is retried by its owner. Accepted
requests subscribe to their shared work and material-owner scope; progress,
required readiness, cancellation and invalidation wake only the subscribed
components. There is no global invalidation serial, shader-generation polling
or all-component registry scan. Render-thread notifications enter a coalesced
mailbox and are delivered on GameThread. GameThread material changes withdraw
affected gated proxies immediately. Each delivery checks the weak job-set identity,
so reset, replacement, destruction and unregister reject old notifications.
Neither subscriptions nor worker callbacks retain live component pointers.

`CheckPSOPrecachingAndBoostPriority` exists on both the component and its request
set. `DMaterialInterface::PrecachePSOs` also accepts `EPSOPrecachePriority`.
Priority is shared by work identity, excluded from the key, and only increases.
Visible proxy requests use Highest; hidden requests use High. Material collection
selects urgent work before ordinary work; its pump forwards promotions to native
PSO observations. The RHI queue selects the most urgent queued compile, preserving
FIFO order for ties and never preempting an executing driver call. Direct native
requests start at Medium, so Low promotion requests do not demote them.

## Related contracts

- [Shader cache and cooked delivery](../Shaders/ShaderCache.md)
- [Material program lifecycle](../Materials/MaterialSystem.md)
- [Renderer frame preparation](RendererFramePreparation.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
