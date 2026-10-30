# Load Preparation

Summary: Prepare cooked material programs and exact scene Shader/PSO resources before covered drawing, with bounded ownership and explicit readiness.

Modules: Engine, RenderCore, Renderer, LevelEditor

Last reviewed: 2026-10-30

## Object and program readiness

Async asset completion retains its existing object-readiness meaning. Materials
start preparation through `CacheResourceShadersForRendering`; callers observe
`IsCompiling`, `IsComplete`, the existing compile status and owner diagnostic.
There is no separate public program-preparation handle or terminal state machine.
Explicit offline waits and cancellation use the existing shader compiler manager.
The internal request snapshots cooked bulk content revision, configuration,
quality, feature level and backend. Parameter-only publications do not invalidate
decoding. A same-sized inline replacement or admitted write changes the bulk
revision and prevents stale publication. Package I/O and family decode never
dereference live objects; the compiler queue publishes current results on the
game thread through existing declaration, collection, properties and proxy checks.

Unload, retired storage, changed configuration or replaced content prevents
pending publication. Failure stays on the material rather than failing asset
loading. At most 16 read/decode continuations are admitted; each releases its
admission on exit, including cancellation and failure. Each request retains the
existing 8 MiB encoded family limit. Decoded products and package-task storage
have additional allocation cost; these limits are not a process RSS bound.

Resource preparation is independent of MapChange. Mesh components request CPU/GPU
residency on registration and use existing render-data change notifications.
Mesh admission pressure retains only weak owner identities in the existing cooked
load manager; its ordinary pump retries without allocating extra read/decode work.
Cancel, shutdown and owner destruction retire those intents.
Cooked materials start `CacheResourceShadersForRendering` in PostLoad. Their
compilation owner retains only the internal decode request; the shader compiler's
retry queue pumps admission and publication. The 16-request decode bound defers
excess work instead of reporting a permanent failure. Program getters are pure;
offline consumers explicitly finish compilation before inspecting accepted programs.
Dynamic instances may retain declared parameter overrides while a cooked parent
program is pending. Normal publication applies those overrides when it becomes ready;
failed parents and ready-program reachability validation retain their existing rules.
Cooked textures asynchronously read/decode platform data after PostLoad, then
apply on GameThread and upload through the existing resource-update path and
stable texture reference. Offline and reload callers use the same
`FinishCachePlatformData` completion boundary; see the
[texture cache contract](../../Assets/Families/TextureSystem.md#derived-platform-data).
SkyLight and cloud proxies begin caching without blocking
registration and retain stable references for late texture publication.
Texture read/decode failures retain their detailed message
in the existing resource diagnostic.
`IsCompiling`, `IsComplete` and texture resource state queries
do not perform blocking preparation. MapChange retains no resource lists or budgets.
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
Loading preparation and normal draws share asynchronous Shader/PSO caches.
Only preparation pumps own the bounded observer scope and its exception-to-status
mapping. Normal draws handle pending components and optional features locally.

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

`PrepareViewResources` owns the budgeted preparation scope and never draws.
`RenderView` has no preparation observer or loading deadline. Normal resource
resolution and the material PSO pump share a per-render-frame admission allowance
across views: eight native Shaders, sixteen pipeline requests and a nominal 2 ms
between operations. Exhaustion leaves local attempts Pending without throwing or
failing the view. Cache hits remain usable. The allowance ends before graph
recording; explicit loading pumps retain their independent per-pump budget.
A capture owner
pumps preparation until Ready before submitting its draw and readback; GPU
recording and large readbacks cannot change preparation back to Pending.
Preparation does not permit synchronous Shader compilation or render-thread waits.

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

`FSceneViewport::RequestResourcePreparation` enables background view preparation.
The viewport pumps `PrepareViewResources` and then calls `RenderView` independently,
including while preparation is Pending. Ready resources and components continue
to render. Failed preparation remains observable without suppressing unrelated
scene content. A Ready observer stops background pumping; explicit requests and
Stale invalidation restart it. Later view/output/feature changes use normal local
resource admission until another preparation is requested. Readiness describes
the prepared configuration, rather than promising coverage of later views.

Screenshots, thumbnails, offline output and deterministic tests own their waiting
policy: retain a preparation request, pump it for the actual scene/view/output,
and defer rendering and readback until Ready. Pending yields to a later frame;
Failed, Canceled and Stale must be handled explicitly. No global strict rendering
mode or preparation field exists in `FSceneViewRenderRequest`. Capture component
gates check `IsReadyForProxyCreation`, independently of `IsPSOPrecaching`; unrelated
optional material variants may remain Pending. Exact selected feature and draw
resources must still finish view preparation before capture. Component PSO
admission continues to use `EPSOPrecacheProxyCreationStrategy` and
`PrecacheMaterialPSOs`, independently of a view preparation request.

A preparation handle can be released after the renderer slots have adopted the
ready resources. Residency and cache eviction remain bounded by their existing
owners. Holding a preparation handle extends resource residency through its
declared activation window; it does not promise universal PSO coverage.

## Material and component PSO requests

### Renderer preparation configuration

`IRendererModule::GetSceneRenderPreparationConfig(FeatureLevel)` returns a
Renderer-owned `FSceneRenderPreparationConfig` value snapshot. It combines the
Scene's immutable level with supported Forward, Hybrid and HitProxy routes,
the captured device wireframe capability, and a module-local configuration
version. FeatureLevel selects material programs; it does not force a rendering
path. Views still select their routes within the supported set.

`SetPreparationSettings` publishes policy on the GameThread and applies the
same snapshot through the ordered render-command queue. Equal settings preserve
the version. Changes immediately make old material PSO observers non-current;
render-thread application retires obsolete collection resources and view
preparations, and invalidates view histories. Requests admitted with the new
version survive that retirement and wait for its render-thread snapshot before
collection. Failed request diagnostics remain available without authorizing reuse.

Scene plans and pass collectors consume these snapshots rather than reading
capabilities independently. Unsupported view and picking routes reject admission;
HitProxy cache identity includes the configuration version. This follows the
explicit scene-configuration boundary of [UE PSO collectors](https://dev.epicgames.com/documentation/unreal-engine/pso-precaching-for-unreal-engine).
Configuration versions describe coverage, independently of existing shader,
device and manual resource generations. Equal native PSO descriptions continue
to reuse the device cache after policy changes. Device capability capture lasts
for the Renderer module lifetime and is renewed when the module is recreated.

### Material requests

`DMaterialInterface::PrecachePSOs` explicitly snapshots the accepted program and
effective material planning identity for a bounded list of Vertex Factories.
`FPSOPrecacheParams` carries primitive participation and geometry/raster facts;
Renderer owns applicable pass selection and required classification.
Components prepare and snapshot the material resource for their Scene's
FeatureLevel. `FMaterialPSOPrecacheParams::FeatureLevel` participates in request
sharing, and collection rejects a program with a different level. Material
bindings use the same level's stable proxy; draw preparation rejects mismatched
programs. Separate Scenes can therefore share material assets while retaining
different program selections. Cooked selections reuse the bounded decode reader
and its admission limit; missing variants report failure without substituting
another level.
The default overload uses the active engine's Renderer; preview/test owners may
pass an initialized `IRendererModule` explicitly. Rejection preserves the output
array and reports an error. Program loading remains a separate readiness step.

`FMaterialPSORequestManager` shares collection and compilation by material
identity, accepted target, canonical Default vertex elements, factory type and
`FPSOPrecacheParams`, and the full Renderer preparation snapshot. Explicit and
type-provided equal layouts share work within that snapshot.
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

`FPSOPrecacheData` in RenderCore's `PSOPrecache.h` describes a graphics or
compute initializer with `bRequired` and `Priority`. `FMaterialPSOPrecacheCollection`
exposes `PSOInitializers`; collectors append descriptions without submitting native
requests. Its resource owner keeps referenced shaders and declarations alive.
The request manager submits each description to the shared pipeline cache, consumes
accepted initializer payloads, and retains asynchronous observations privately.
Capacity pressure retries only the first unsubmitted description, preserving the
accepted prefix. Invalid descriptions and terminal admission errors remain failures.
A rejected, null or throwing optional submission records a failed optional slot
without retiring observations of required pipelines. Required pending/failure
still withholds the proxy; a failure confined to optional work remains diagnostic.
Renderer collection selects each description's `bRequired`; required readiness
remains unknown until collection declares required coverage and is independent
of optional native work. A successfully declared empty applicable set is Ready
without native submissions. Collectors publish
`bRequiredPSOsCollected` after all required descriptions have been appended;
subsequent pumps may only append optional descriptions. Fixed and material
Shaders for required passes resolve before optional passes, so an optional Shader
still compiling or failing cannot withhold an otherwise ready proxy. Overall
completion and failure diagnostics remain observable separately.

PSO work has one phase: Queued, Collecting, Compiling, Completed or Retired.
Completed work publishes one immutable success/error result; retirement revokes
reuse without erasing failure diagnostics. The first three phases map to public
Pending. Required readiness remains separate from all-work completion; priority
sorting uses a stable per-pump snapshot.

The Renderer permits up to eight active material collections across updates,
including their outstanding native compilation, independently of scene visibility.
Additional requests wait without expanding their pipeline resources.
`FMeshPassProcessor` implementations own eligibility, shader selection, raster
variants, required classification and draw pipeline descriptions for Forward,
ShadowDepth, GBuffer, hybrid retained, sorted translucency and HitProxy. Actual
mesh preparation and pipeline creation enter each processor through `AddMeshBatch`.
`FMeshPassProcessorBatch` borrows the accepted material, Vertex Factory and effective
state from an admitted prepared batch element, avoiding a second geometry capture.
The pass performs final material/participation/factory filtering, selects its vertex
and fragment stages and describes pipeline state. Shared `BuildMeshDrawCommands`
logic initializes the ShaderMap, resolves the selected shader bindings and builds
owned `FMeshPassPipeline` resources containing the ShaderMap, vertex binding, fragment
shader/layout, declaration and native pipeline. Geometry visibility, uniform/parameter
packet caches and command-list recording remain with their existing owners.

`FMeshPassDrawListContext` adapts command construction to the existing asynchronous
ShaderMap and pipeline caches: a shader context publishes a complete shader payload;
a draw context consumes it and creates a native pipeline through the existing
request service. A precache context consumes the same payload and appends only an
initializer, without creating a pipeline or recording a draw. Both output paths
use the same final filtering, shader selection and `FMeshPipelineDescription`.
Material ShaderMap identity, factory/layout and selected shader stages must match
the supplied payload before either path can emit output. A retained-command context
also assembles the complete existing `FMeshDrawCommand` snapshot: geometry description,
input binding, primitive state/uniform, selected draw resources and material parameter
packets. The cache owner supplies prepared packets and owns retention/eviction; the
processor removes per-view packets before publishing the immutable command, preserving
view-local binding refresh and the existing multi-route cache representation.
Filtered, pending or failed attempts leave the output payload untouched; cached payloads retain the ShaderMap
that owns their compiled fragment layout. Each context is confined to its call on
the render thread; shared draw prototypes keep no request-specific context.

Concrete processors and their creation-function registrations live with their
passes: Base Scene owns Forward, hybrid retained and sorted translucency;
Directional Shadow, GBuffer and HitProxy own their corresponding processors.
`MeshPassProcessor.h/.cpp` contains only the base, shared collection helpers and
the registration manager. `FRegisterPassProcessorCreateFunction` registers a
factory for an existing `EMeshPipelinePass`; `FPassProcessorManager` enumerates,
looks up and creates processors through that registry. Duplicate, null and
out-of-range registrations cannot replace an existing factory. Creation callbacks
execute outside the registry lock, and each collection gets independent state.
Draw lookups lazily retain an immutable prototype. Registration belongs to module
initialization; unregistering requires quiescent users and retires that prototype.
The project pass enum already distinguishes its routes, so this registry does not
introduce an additional ShadingPath index.

Engine's `IPSOCollector::CollectPSOInitializers` follows the UE append interface:
it takes the scene configuration, accepted `FMaterialResource`, Vertex Factory
data, primitive precache parameters and an output `FPSOPrecacheDataArray`, and
returns `void`. The project resource/config types retain the existing snapshot
ownership contracts. A default collection includes required and optional variants.
Each request creates its own processor instances; immutable prototypes serve
draw descriptions. Nonblocking ShaderMap readiness is exposed separately through
`GetPSOCollectionResult`, and repeated pumps preserve the appended prefix without
duplicating entries. The host invokes required collection across every processor
before optional collection and publishes required coverage at that boundary.
The aggregate material collector traverses processors without selecting pass
shaders or pipeline state itself. Fixed Shader requests are started and
retained before nonblocking ShaderMap resolution. Inapplicable routes are skipped;
failed resource creation for applicable routes remains an explicit failure. GPU-culling and global feature PSOs are outside
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

`DPrimitiveComponent::PrecachePSOs` snapshots participation through virtual
`SetupPrecachePSOParams`, then calls the virtual `CollectPSOPrecacheData`
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

`DPrimitiveComponent` owns reflected `bCastShadow` and `bRenderInMainPass`
properties. Their setters and property edits retire the old component PSO set
and recreate its proxy. Scene publication copies `FPrimitiveRenderParticipation`
into the proxy before enqueueing it. Both receiver and shadow logical collection
check that same participation value before capturing mesh/material snapshots,
including retained StaticMesh publications and dynamic SplineMesh collection.
There is no independent depth prepass participation switch until such a drawing
route exists; main-pass depth writes and shadow rendering remain separate concepts.

Static and Spline Mesh collectors deduplicate effective material slots, including
the initialized authored default material, and emit material/Vertex Factory and
geometry facts without choosing passes or required work. Their fixed layouts
need no resident mesh buffers. Fully excluded components emit no requests.
Renderer's supported configuration covers both selectable Forward and Hybrid
paths. Per-pass eligibility shares material rules with drawing: shadows exclude
translucency, GBuffer requires non-translucent Lit material and a supported
Vertex Factory, Hybrid retained Forward covers remaining non-translucent draws,
and sorted translucency covers translucent draws. Receiver routes also require
the factory's Forward shader entry, matching actual receiver collection; an
unavailable shadow shader excludes casting without rejecting valid receivers.
HitProxy is optional.
Renderer expands up to eight raster variants covering wireframe/solid, normal/reversed
depth and both transform/instance windings; unsupported wireframe is omitted.
Shadow descriptions use only their two effective winding variants: solid fill
and forward depth regardless of the receiver mode. HitProxy uses four solid-fill
winding/depth variants. Normalized duplicates are not emitted.
Spline scale sign changes additionally select the matching no-cull policy.
Both shadow winding variants remain required independently of the receiver
wireframe/depth settings. For main passes, both winding variants of the requested
wireframe and depth convention are required for proxy creation. Other raster/depth variants are optional warming
work. The renderer resolves required descriptions before optional descriptions.
Exact view preparation covers additional selected routes before deterministic
output. Ordinary transform, instance-count and instance-custom-data changes
preserve the set. An unavailable default retains the existing ErrorMaterial
fallback path; that path and global/GPU-culling PSOs are outside this component
coverage.

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
