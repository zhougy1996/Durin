# Load Preparation

Summary: Define ownership, readiness and invalidation for material programs, exact-view preparation and component PSO precaching.

Modules: Engine, RenderCore, Renderer, DurinEd, LevelEditor

Last reviewed: 2026-11-18

## Object and program readiness

Asset completion means object readiness, not completed Shader/PSO work. Cooked
materials start `CacheResourceShadersForRendering` in PostLoad; consumers inspect
`IsCompiling`, `IsComplete`, compile status and diagnostics. Program getters do
not submit work or wait. The shader compiler manager owns asynchronous decode,
admission and publication; stale owner/content/configuration results cannot publish.
Full program validation happens at admission, not again for each draw or cascade.
See [material program lifecycle](../Materials/MaterialSystem.md).

Mesh components request residency on registration. Cooked textures use their
normal asynchronous cache/upload path and stable texture reference; offline
callers use `FinishCachePlatformData`. See the
[texture cache contract](../../Assets/Families/TextureSystem.md#derived-platform-data).

MapChange neither scans component PSOs nor renders a candidate offscreen view.
Components own proxy readiness, and the committed viewport owns view preparation.
Startup keeps its generation-scoped output/presentation gates. Shader or PSO
failure remains with its resource owner rather than changing asset-load success.

## Scene resource readiness

`FRenderResourcePreparation` is a caller-owned request handle for one actual
scene/view/output/options configuration. Pump `IRendererModule::PrepareViewResources`
on the render thread while resource descriptions need resolution. Preserve that
configuration through use; create a new request for changed coverage.

Preparation runs the normal logical and resource phases, stopping before graph
authoring, temporal-history mutation, drawing or readback. Each call builds its
own snapshot; borrowed frame/proxy values never escape to workers. Ready means
all selected resources and native PSOs completed, not coverage of future routes.

| Unfinished work | Owner and progress rule |
| --- | --- |
| CPU Shader compilation | `FShaderMapPreparation` and compiler requests; unchanged dependencies suppress the slot factory until progress or invalidation. |
| Unsubmitted PSO descriptions | The resource/material owner submits through the shared cache, preserving accepted identities without synthetic capacity rejection. |
| Accepted native PSO compilation | Shared `FPipelineState` handles; polling reads completion without resubmission or scene traversal. |
| Terminal error, cancellation or stale generation | No automatic retry of that handle; retain diagnostics and create a new request when appropriate. |

Native Shaders are created lazily by `FShaderMapResource`. Preparation uses
`PipelineStateCache::Precache*PipelineState`; required draw acquisition uses
`GetAndOrCreate*PipelineState` with a command list. Renderer payloads retain cached
`FGraphicsPipelineStateRef` / `FComputePipelineStateRef` before native completion.
Drawing records the handle, boosts pending demand and attaches its execution
dependency. The RHI executor gates replay at closed command scopes. There is no
general frame quota or render-thread compile wait. See
[RHI command execution](../RHI/RHICommandExecution.md) and
[resource recovery](RendererResourceRecovery.md).

Preparation batches collect shared PSO handles, including ready cache hits,
without waiting or an artificial handle-count admission cap. Completed resource
descriptions are not rerun just because a
native PSO is Pending. CPU/resource prerequisites can still require another pump.

The request retains PSO handles only. ShaderMaps, renderer slots and recorded RHI
commands own their respective code, payloads and native resources. Copies alias
one request; independent requests share compilation. Canceling or dropping an
observer releases its references without canceling another owner's compilation.
No service registry retains view requests or scans them each frame.

A scene-resource lifetime token is observed weakly. Scene release, preparation
configuration changes and renderer invalidation retire that token; Shader reload
also invalidates the captured generation. Every query or transition observes
existing native outcomes and invalidation before changing state. Failed,
Canceled and Stale are terminal. A completed failure is preserved even if the
caller cancels before its first progress query. Late or duplicate completion
cannot revive a terminal request or discard already accepted PSO handles.

Authored Shader demand uses asynchronous `FShaderCompilingManager` requests.
`InitializeShaderMapFromTypes` never compiles inline or waits. Explicit synchronous
compiler/map entrypoints are for startup/offline callers and reject render-thread
use. `FPreparedShaderMapScope` restricts capture to completed outputs; resource
resolution can opt into asynchronous demand. Pending slots retain typed compiler
dependencies only until publication, reset, failure or invalidation. Published
ShaderMaps retain their own code; the shader-build output cache owns completed
compiler products. See [shader cache and delivery](../Shaders/ShaderCache.md).

## Activation and uncovered work

`FSceneViewport::RequestResourcePreparation` enables background preparation.
The viewport pumps it and draws independently, so already ready content remains
visible while other resources are Pending. Ready stops background preparation;
an explicit request or Stale observation starts a new request. Failure remains
observable without hiding unrelated scene content.

Screenshots, thumbnails and deterministic tests own the stricter policy: retain
the request and postpone drawing/readback until Ready. Handle Failed, Canceled
and Stale explicitly. Component gates use `IsReadyForProxyCreation`, not overall
`IsPSOPrecaching`, because unrelated optional PSOs must not delay capture.
There is no global strict mode in `FSceneViewRenderRequest`. Once an image
renderer has submitted valid drawing and readback, native PSO dependencies belong
to that submission; a Pending observation must not restart capture. Missing CPU
resources or an absent output can still defer submission.

## Material and component PSO requests

### Renderer preparation configuration

`GetSceneRenderPreparationConfig(FeatureLevel)` returns the Renderer-owned
`FSceneRenderPreparationConfig`: scene FeatureLevel, supported routes, captured
device capabilities and configuration version. FeatureLevel selects material
programs; it does not force a render path. Collectors and view planning consume
the same snapshot.

`SetPreparationSettings` publishes on GameThread and applies through ordered
render commands. Equal settings preserve the version. Changed settings make old
observers non-current and retire obsolete collection/view preparation and view
history. Newly admitted requests wait for their matching render-thread snapshot.
Configuration identity governs coverage; equal native PSOs can still share the
device cache across policy changes. HitProxy cache identity includes the version.

### Material requests

`DMaterialInterface::PrecachePSOs` snapshots the accepted program, effective
properties, FeatureLevel, Vertex Factory data, primitive participation and
Renderer configuration. Rejection preserves the caller's output array. Engine's
`PSOPrecache.h` owns initializer/parameter values; `PSOPrecacheMaterial.h` owns
collectors and material request contracts. Renderer selects applicable passes,
variants and required coverage using the same shader selection and initializer
builders as drawing. No Scene, live vertex buffer or draw submission is required.

`FMaterialPSORequestManager` shares work by those immutable inputs. Each admission
creates an independent observer; copies alias it. Cancellation withdraws that
observer only. Material ShaderMap, pipeline, parent-chain changes or destruction
invalidate matching material observers; numeric parameter changes preserve them.
Shader/device invalidation and Renderer shutdown retire matching work. Failed
diagnostics remain available, but `IsCurrent` independently prevents reuse.

Collectors append owned `FPSOPrecacheData` descriptions with required/optional
classification and provenance. `PreparePSOResources` is a nonblocking readiness
hook; `CollectPSOInitializers` only appends prepared descriptions. Registration
is keyed by shading path and stable collector index, completed at module startup;
removal requires quiescent users. Local/Spline fixed vertex-fetch layouts share
the actual drawing layout provider and require no native vertex declaration.

Required work is prepared and submitted before optional work. Required coverage
must be declared before proxy readiness is known; an explicitly empty set is
ready. Optional compilation or failure cannot revoke completed required coverage.
At most eight requests collect required coverage concurrently; completed required
collection releases its slot immediately. This is a concurrency limit, not a
per-frame budget. Accepted requests are observed rather than recreated. Invalid descriptions and
terminal admission/compilation errors do not retry each frame.

Material/component observer counts and pipeline cache identities have no
artificial admission quotas. The device queue retains ordinary backpressure
and serialized native compilation. Completed collection
releases program/initializer/native observations on the resource-owning thread;
its weak sharing index does not keep abandoned work alive. A successful precache
records preparation, not permanent native residency. Drawing still uses the
shared cache and recorded execution dependencies after native cache eviction.

`PSOPrecacheValidation` is optional observation. Collection registers descriptions
before submission, including the complete unsubmitted suffix. Canceling an unsubmitted
observer withdraws its coverage. Shared native outcome observations outlive their
observer without retaining native resources. `Check` is read-only; `RecordUse`
counts actual submitted draws once per full identity. Invalidation clears capture
identities. Validation never controls readiness or adds a scheduling budget.

Draw preparation, command ownership, pass registration and cache invalidation
follow [renderer frame preparation](RendererFramePreparation.md), not a second
precache execution path.

## Automatic component collection and proxy publication

`DPrimitiveComponent` owns automatic requests, controlled by
`IsComponentPSOPrecachingEnabled`. Registration/PostLoad and structural
mesh/material/participation changes collect a fresh set. Disabling automatic
requests drains those owners without disabling explicit material requests or
normal drawing preparation. Custom scenes without a PSO Renderer keep their
existing publication contract.

Components snapshot their Scene's FeatureLevel and material/Vertex Factory facts.
Renderer owns Forward/Hybrid/shadow/GBuffer/translucency/HitProxy eligibility and
raster variants. Main-pass active raster/depth variants and both shadow winding
variants are required; other warming variants and HitProxy are optional. Excluded
components emit no requests. Ordinary transforms and instance numeric data do not
recreate the PSO set. Global, GPU-culling and ErrorMaterial fallback PSOs are outside
component coverage; exact-view preparation covers the selected drawing route.

Default `DelayUntilPSOPrecached` withholds a proxy until every required request is
ready. Failed/canceled/stale required work never authorizes publication. Optional
failure remains diagnostic. `AlwaysCreate` allows drawing during preparation.
Submission readiness is checked for the entire collection before any prefix can
authorize a proxy; lower-level capacity pressure remains with its accepted owner.

`FPSOPrecacheComponentData` owns the observer set and JobSet identity. Reset,
replacement, unregister and destruction withdraw it. Weak subscriptions notify
only affected owners; render-thread notifications enter a coalesced GameThread
mailbox. Every callback rechecks the JobSet, so an old completion cannot publish a
replacement proxy or retain a destroyed component. There is no per-frame scan of
all registered components. Engine/world ticks pump unsubmitted work and owner
notifications, including paused/editor worlds. Stale owners request current work.

`CheckPSOPrecachingAndBoostPriority` promotes shared work monotonically. Visible
components use Highest, hidden components High; optional warming keeps its
collector priority. The native queue selects the most urgent queued task with
FIFO ties and never preempts an executing driver call.

## Related contracts

- [Shader cache and cooked delivery](../Shaders/ShaderCache.md)
- [Material program lifecycle](../Materials/MaterialSystem.md)
- [Renderer frame preparation](RendererFramePreparation.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
