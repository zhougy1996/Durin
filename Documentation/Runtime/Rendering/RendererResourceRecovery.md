# Renderer Resource Recovery

Summary: Define complete-or-null Renderer resource publication, generation-scoped retries, fallback retention, and device invalidation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-09-08

## Complete-Or-Null Construction

Nullable RHI creation is a complete-or-null boundary. Vulkan buffer, texture,
shader, graphics-pipeline, sampler, and vertex-declaration factories publish a
reference only after every required native handle and allocation exists.
Expected creation failure returns null without failing the RHI executor; device
loss, command replay, submission, presentation, and invariant failures remain
terminal.

`RHICreateGraphicsPipelineState` returns a complete pipeline or null; it does
not promise a distinct object on every call. Vulkan reuses a strongly retained
pipeline for an equal normalized graphics-pipeline key. `DebugName` labels
diagnostics and captures and is excluded from that key. Changing the name does
not require a new pipeline, while changing semantic pipeline state can select
a different pipeline even with the same name. Compute pipelines follow the
same Vulkan cache policy. Renderer slots and explicit Renderer-owned payloads
hold logical ownership; the backend cache and recorded commands can also retain
references. Cache eviction selects only entries with no external references.

Texture assets use the owned update protocol in [Texture System](TextureSystem.md).
An explicit `UpdateResource()` retries installed immutable input. Availability
remains true after failed replacement when a prior successful allocation exists;
resource release never clears the latest completed update error. These assets
have no render-request generation, and thumbnail readiness is checked separately
from last-successful fallback availability.

## Transactional Resource Slots

Fixed Renderer resources, static-mesh shader and pipeline identities, editor
assistance, shared fullscreen geometry, and Texture Editor previews use
`TRenderResourceCreationSlot`. A slot constructs a complete candidate in local
ownership and publishes only after every binding, RHI resource, and pipeline
succeeds. Callers observe the prior complete payload, a newly committed payload,
or no payload; partially initialized aggregates are never visible.

Fixed non-Material shader compilation and typed lookup are centralized in
RenderCore's [Global Shader](GlobalShaders.md) map. Each bounded exact shader
set has its own transactional slot and strong `FGlobalShaderSetRef` lifetime.
Renderer feature payloads retain typed `TShaderMapRef` values and the exact set
used by their PSO; they do not allocate, cast, or own private global
`FShaderMapBase` instances. Material and vertex-factory/mesh combinations use
`TMaterialShaderRef` over an exact `FMaterialShaderMap`; consumers likewise do
not initialize an ordinary map, perform a raw lookup, or cast a shader.

`FSimpleElementRenderer` follows the same contract. Its line and sprite Global
Shader sets are independently demandable, so one unavailable class skips only
dependent batches. Output/depth/blend/shader-class pipeline keys retain the
exact typed shader refs used to create them. Persistent vertex and index upload
buffers grow to bounded power-of-two capacities; a failed allocation publishes
no partial batch, reports once for the current device generation, and becomes
eligible again on the next frame. Device invalidation releases pipelines,
declarations, atlas resources, and upload buffers before lazy reconstruction;
shutdown performs the same ordered release.

Each owner tracks independent shader, device, and manual generations. A failed
attempt records its generation, error category, context, identity, diagnostic,
retry dependencies, and fallback state. Repeated lookup in the same relevant
generation neither calls the factory nor logs the same failure again. A later
relevant generation permits one new lazy attempt.

Same-device shader or manual refresh may retain a complete last-known-good
payload as stale-ready. Device-generation changes always discard dependent RHI
payloads before replacement, so fallback never crosses a device generation.
This seam coordinates reconstruction; it does not recover a lost Vulkan device
or a failed RHI executor.

Compiled materials retain one immutable accepted `FMaterialCompilerResult` in
Engine render data. The Renderer adapter verifies its identity, target, pass
contract, and generated entry point before combining accepted fragments with
fixed mesh stages in one exact typed candidate. RenderCore validates reflection,
bindings, set identity, and merged layout. Shader reload may refresh a fixed
mesh stage; device invalidation discards combined RHI shaders and PSOs. Both
reconstruct lazily without rereading graph state or recompiling generated
Material IR. Failed same-device candidates retain the compatible complete map
and pipeline; device-generation changes permit no RHI fallback.

Frame-transient targets use the Renderer-private RDG allocator described by
[Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md).
Allocation publishes only after every retained texture or buffer resolves.
Device or manual invalidation and bounded transient-failure retries make later
construction eligible without moving shaders, PSOs, samplers, or committed
view history into transient ownership.

The graph frame executor derives one immutable requirements value after logical
preparation and persistent-resource resolution, then the single-use graph builder sends
one retained descriptor batch to `FRDGAllocator` before the first consuming
pass. The pool key contains the complete allocation-compatible descriptor and
excludes diagnostic names, observation tags, and feature identity. Texture and
buffer requests share whole-batch reservation, retry admission, pre-allocation
eviction, rollback, publication, and error reporting; only typed RHI creation
differs. Nullable texture and buffer factories also expose typed candidate
failures through `RHITryCreateTexture` and `RHITryCreateBuffer`. Vulkan preserves
memory exhaustion, resource exhaustion, and unsupported-description categories
across both inline and threaded creation boundaries. Device loss and invariant
failures remain terminal; no retry policy catches them.

A failed batch rolls back every newly materialized candidate, including a retry
of an existing empty entry, drops unreserved idle cache, and publishes no graph
allocation or extraction destination. Before creating any candidate, the whole
batch is checked for suppressed descriptors, so a late unavailable resource
cannot repeatedly allocate and roll back an earlier prefix.

Unsupported descriptors remain suppressed until device or manual generation
changes. Memory/resource exhaustion and unclassified nullable failures retry
on later demand after 100 ms exponential backoff capped at 2 seconds, without
a maximum attempt count. A pool-wide cooldown also throttles new descriptors
and other views under pressure; reuse-only batches remain eligible. Device or
manual generation changes bypass cooldown, while shader changes do not.
Before a later creation attempt, RHI collects completed retirement without
prematurely deleting in-flight resources. A new attempt always needs a newly
authored builder. Compile or preparation failure consumes the current builder; retrying the same builder returns
InvalidState without consulting the allocator. Pass execution receives counted resources and never
performs target lookup, creation, or recovery policy itself.

Qualification policy does not participate in generation state or mutate a
prepared plan. A Renderer-private scoped policy may add feature-bounded target
requirements for one test/tool submission, but those targets follow the same
pool transaction and invalidation rules as production and supported debug
views.

## Invalidation And Commands

Renderer owns these development commands:

- `renderer.reload-shaders changed` advances shader generation and lets normal
  dependency fingerprints select changed output on next demand.
- `renderer.reload-shaders all` advances shader generation and forces
  compilation for each next-demanded shader candidate.
- `renderer.retry-resources` advances manual generation for eligible failed
  resources.

Console callbacks enqueue one render command. Views submitted before that
command retain the old generation; later views observe the new one. Resource
construction remains synchronous and demand-driven on the rendering thread.
New failures and changed fingerprints produce one diagnostic, retained fallback
is identified explicitly, and successful retry reports one recovery transition.

`FRendererResourceCoordinator` owns command admission and the shader, device,
and manual generation counters. It explicitly supplies accepted generations
to the RenderCore global map while `FSceneRenderer` fans requests out to its
remaining concrete owners. Shader and manual invalidation leave
reconstruction lazy. Device invalidation releases every dependent payload
before advancing the device generation, recreates only startup defaults, and
leaves feature resources to rebuild on demand.

`FRendererModule` is the explicit cross-module request and focused-test entry
point. It forwards only while its composed `FSceneRenderer` exists; shutdown
stops the scene renderer before destroying it. Consumers composed below the
scene renderer receive the coordinator by reference. No active coordinator
pointer or process service-locator path exists.

The device-invalidation request is a tested internal seam, not a claim of
Vulkan device-loss recovery. Renderer shutdown closes command admission,
unregisters development commands, enqueues release, and flushes rendering work.
Texture Editor retains module ownership of its preview slot and releases it
through its own ordered shutdown.

Global-map device invalidation is ordered after Renderer consumers release
their pipelines and typed refs and before the new device generation is
published. Shutdown follows the same consumer-before-map order.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
