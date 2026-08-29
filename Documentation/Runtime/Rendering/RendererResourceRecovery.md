# Renderer Resource Recovery

Summary: Define complete-or-null Renderer resource publication, generation-scoped retries, fallback retention, and device invalidation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-08-29

## Complete-Or-Null Construction

Nullable RHI creation is a complete-or-null boundary. Vulkan buffer, texture,
shader, graphics-pipeline, sampler, and vertex-declaration factories publish a
reference only after every required native handle and allocation exists.
Expected creation failure returns null without failing the RHI executor; device
loss, command replay, submission, presentation, and invariant failures remain
terminal.

`RHICreateGraphicsPipelineState` is a creation-only factory. Its `DebugName`
labels diagnostics and captures but does not select, reuse, or retain a PSO.
Every successful request returns a distinct complete pipeline. Renderer slots
and explicit Renderer-owned payloads hold logical ownership; recorded draws may
retain transient references until replay completes.

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
`FShaderMapBase` instances. Material and vertex-factory/mesh combinations keep
their existing private identities.

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
Engine render data. Renderer shader slots use its program identity as key and
recreate typed forward, GBuffer, and masked-shadow shader maps from its complete
compiled fragment set. Shader reload may refresh the shared fixed vertex stage;
device invalidation discards the combined RHI shaders and PSOs. Both reconstruct
lazily on the rendering thread without rereading graph state or recompiling the
material program.

Frame-transient targets use the Renderer-private provider described by
[Renderer Frame Preparation and Render Graph Execution](RendererFramePreparation.md).
Each physical texture has the same generation-scoped failure suppression and
retry semantics; complete typed bundles are returned only after every texture
resolves. Device or manual invalidation and retained-byte eviction make later
construction eligible without moving shaders, PSOs, samplers, or committed
view history into transient ownership.

The graph frame executor derives one immutable requirements value after logical
preparation and persistent-resource resolution, then acquires every requested
frame-transient bundle before the first consuming pass. The pool partitions
descriptions into bounded typed semantic groups. A failed multi-texture bundle
releases newly created siblings but retains the failed generation-aware slot,
so a same-generation frame does not partially publish or repeatedly retry it.
Pass execution receives the resolved bundle and never performs target lookup,
creation, or recovery policy itself.

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
