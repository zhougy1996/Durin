# Renderer Resource Recovery

Summary: Define complete-or-null Renderer resource publication, generation-scoped retries, fallback retention, and device invalidation.

Modules: RenderCore, Renderer, RHI, VulkanRHI, TextureEditor

Last reviewed: 2026-08-18

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

Frame-transient targets use the Renderer-private provider described by
[Renderer Frame Preparation and Fixed Execution](RendererFramePreparation.md).
Each physical texture has the same generation-scoped failure suppression and
retry semantics; complete typed bundles are returned only after every texture
resolves. Device or manual invalidation and retained-byte eviction make later
construction eligible without moving shaders, PSOs, samplers, or committed
view history into transient ownership.

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
and manual generation counters. `FSceneRenderer` explicitly fans accepted
requests out to concrete owners. Shader and manual invalidation leave
reconstruction lazy. Device invalidation releases every dependent payload
before advancing the device generation, recreates only startup defaults, and
leaves feature resources to rebuild on demand.

The device-invalidation request is a tested internal seam, not a claim of
Vulkan device-loss recovery. Renderer shutdown closes command admission,
unregisters development commands, enqueues release, and flushes rendering work.
Texture Editor retains module ownership of its preview slot and releases it
through its own ordered shutdown.

## Related Documentation

- [Viewport Rendering](ViewportRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Renderer Frame Preparation and Fixed Execution](RendererFramePreparation.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
