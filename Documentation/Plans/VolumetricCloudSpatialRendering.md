# Volumetric Cloud Spatial Rendering Plan

Summary: Implement deterministic depth-aware volumetric-cloud spatial rendering and composition through public RHI contracts.

Last reviewed: 2026-08-21

Status: Completed
Completed: 2026-08-21

## Current Status

P1 activated on 2026-08-21 after Volume Texture Foundation, Persistent View
State Foundation, and Compute Renderer Integration completed. Stage 0 is
complete: the first implementation is a flat world-Z slab rendered at full
spatial resolution into scene-linear radiance/transmittance, with a
compute-first route, an equivalent fragment fallback, and disabled/no-cloud as
the terminal fallback.

The current Renderer already owns sampled 3D textures, synchronous compute,
transactional resource slots, explicit texture transitions, scene-linear
`RGBA16_FLOAT` targets, reconstructed opaque depth, and sorted translucency.
Stage 1 completed on 2026-08-21. The renderer-private contract now publishes
validated immutable parameters and required/optional texture bindings,
saturating target bytes, overflow-safe 8x8 dispatch sizing,
compute/fragment/disabled routing with exact reasons, finite height-slab
intersection, route/sample/byte counters, and a deterministic CPU spatial
reference. The CPU reference covers empty/solid/structured density,
below/inside/above cameras, opaque-distance clipping independent of device-depth
encoding, fitted-view exclusion, and optional-white weather equivalence.

`VolumetricCloud.slang` compiles matched fragment/compute spatial integration
with the frozen Base/Detail/Weather/Depth/Sampler/Uniform ABI and an explicit
`rgba16f` compute output. The exact 64-cubed Base, 32-cubed Detail, and 64-squared
Weather fixed-seed fixtures upload, transition, sample, restore, and read back
through public Vulkan RHI in inline and threaded modes. Validation passes 74/74
`EditorRenderingTests`, 36/36 `RenderShaderContractTests`, and 63/63
`VulkanRHIIntegrationTests`.

Stages 2 and 3 completed on 2026-08-21. `FVolumetricCloudRenderer` now owns
complete-or-last-known-good compute, fragment, fallback-weather, composite, and
extent-target payloads. The compute route performs one public 8x8 dispatch with
explicit input/output restoration; the fragment route writes the same fitted
`RGBA16_FLOAT` target; and a scene-linear ping-pong draw implements the frozen
radiance/transmittance algebra without a copy or load pass. Real Vulkan readback proves
compute/fragment agreement within 2/1024 per half channel, fitted-view
isolation, exact composite algebra, compute failure fallback, target recovery,
and the disabled route.

The hybrid retained-forward phase is split into retained opaque/masked, cloud,
and sorted-translucency passes. With no P2 scene snapshot the default prepared
input remains disabled, preserving all existing view output; per-view counters
publish route/reason, samples, bytes, dispatch/draw, and composite structure.
Validation passes 75/75 `EditorRenderingTests`, 7/7
`EditorGridVulkanTests`, and 1/1 `VolumetricCloudVulkanTests`; the isolated
cloud route/composite target passes in both inline and threaded RHI execution.
The 36/36 shader-contract and 63/63 Vulkan
RHI integration targets, `fast-all`, the complete Debug Editor build, and a
120-tick hidden-window Debug Editor startup/render/clean-shutdown smoke also
pass on the available RTX 3090 host. Stage 4 is complete. Lasting contracts are published in
`Documentation/Runtime/Rendering/VolumetricCloudSpatialRendering.md`.

On 2026-08-21 the user explicitly replaced the unavailable GTX 1060 6GB /
Vulkan 1.3.280 qualification identity with the available RTX 3090 / Vulkan
1.4.325 identity. This is a recorded post-observation adapter rebaseline, not
an original frozen result. It changes only adapter/API identity: the extents,
executors, 30/120 frame counts, pixel tolerances, 64 MiB ceiling, 12/16 ms
compute gates, and 150% fragment/compute median gate remain unchanged.

`VolumetricCloudQualificationTests` now owns the frozen three-extent,
compute/fragment, 30-warm-up/120-measured-frame matrix. It reads the Vulkan
physical-device identity, emits one machine-readable record per route/extent,
and enforces the timing gates only for the exact named adapter/API while always
enforcing route structure, half-float parity, target bytes, and lifecycle. The
2026-08-21 RTX 3090 Vulkan 1.4.325 measurements used for the approved rebaseline
are:

| Executor / extent | Compute median / p95 | Fragment median / p95 | Retained bytes / parity |
| --- | ---: | ---: | ---: |
| Inline, 1280x720 | 306,368 / 722,496 ns | 314,272 / 317,056 ns | 22,118,400 / pass |
| Inline, 1919x1079 fitted | 442,912 / 845,184 ns | 451,616 / 849,472 ns | 49,694,424 / pass |
| Inline, 1920x1080 | 632,608 / 1,041,408 ns | 629,792 / 1,100,416 ns | 49,766,400 / pass |
| Threaded, 1280x720 | 306,592 / 308,416 ns | 314,176 / 317,376 ns | 22,118,400 / pass |
| Threaded, 1919x1079 fitted | 452,960 / 908,480 ns | 456,864 / 885,472 ns | 49,694,424 / pass |
| Threaded, 1920x1080 | 629,376 / 1,011,936 ns | 618,944 / 1,032,384 ns | 49,766,400 / pass |

Every compute sample records one dispatch, zero producer draws, and zero copies;
forced fragment records zero dispatches, one draw, and zero copies. Raw records
remain in the DurinDevTool/CTest logs. Both reruns reported `status=named_gate`;
compute stayed below the unchanged 12/16 ms gates, fragment stayed below 150%
of compute median, parity passed, and retained targets stayed below 64 MiB.

The final Stage 4 host pass includes both qualification executors, 1/1
`VolumetricCloudVulkanTests`, 75/75 `EditorRenderingTests`, 36/36
`RenderShaderContractTests`, 63/63 `VulkanRHIIntegrationTests`, `fast-all`, the
default native aggregate, the full Debug Editor build, and a validation-enabled
120-tick hidden-window startup/render/clean-shutdown smoke.

The post-commit Definition-of-Done audit added
`VolumetricCloudSceneVulkanTests`. A development-only prepared-view seam now
injects the already-frozen P1 input without creating a reflected P2 object or
changing the production default. On both executors, the test proves exact
disabled/invalid-input output, enabled compute and forced-fragment SceneRenderer
counters, final SRGBA8 parity within 1/255, cloud-visible offscreen output,
window-backed Present, resize, and clean release.

P1 completed on 2026-08-21 after the approved RTX 3090 identity rebaseline and
named-gate rerun. All Stage 0-4 pixel, structure, timing, memory, recovery,
aggregate, build, runtime, and documentation gates pass. The frozen renderer
handoff contains only values and generic texture bindings; the active
`VolumetricCloudSceneContract` P2 plan owns reflected components and scene
publication without changing P1 GPU ownership or quality policy.

## Goal

Render one deterministic global volumetric-cloud layer that samples generic
base/detail volume textures and an optional weather texture, clips its ray
interval against opaque depth, and composites between opaque lighting and
sorted translucency. The implementation must use public RHI operations, select
compute normally when eligible, retain an explicit fragment fallback, preserve
the exact disabled/no-cloud path, and publish enough stable spatial contracts
for the P2 scene/component plan.

## Scope

- A renderer-private immutable cloud input with base/detail 3D texture and
  optional weather 2D texture bindings.
- One flat, height-bounded world-Z cloud slab with bounded horizontal trace
  distance and camera-below/inside/above behavior.
- Full-resolution deterministic spatial ray marching with base/detail density,
  coverage, erosion, wind offset, extinction, and a minimal one-directional-
  light form term.
- Scene-linear cloud radiance plus transmittance output, opaque-depth clipping,
  and composition before sorted scene translucency.
- Compute-first execution, equivalent fragment fallback, complete disabled
  fallback, explicit transitions, recoverable resource publication, counters,
  timing hooks, and deterministic capture/readback tests.
- Forward/special-view and hybrid-deferred paths, fitted offscreen targets, and
  window-backed Present without changing post-process or editor assistance.

## Non-Goals

- Reflected cloud actors/components, scene-proxy registration, serialized cloud
  properties, or generic Details editing; P2 owns those contracts.
- Temporal jitter, low-resolution reconstruction, history, dynamic resolution,
  or quality tiers; P3 owns them.
- Production phase functions, multi-scattering, self-shadow integration, ambient
  modeling, or receiver cloud shadows; P4 owns them.
- Specialized cloud/volume editor UI, import/generation workflows, presets, or
  debug panels; P5 owns them.
- Multiple layers, spherical-planet shells, local volumes, general volumetric
  media, a render graph, indirect dispatch, or asynchronous compute.

## Design Decisions and Invariants

### Coordinate and density model

- P1 selects one flat slab bounded by world Z. The frozen qualification layer
  spans Z = 1,500 to 3,500 world units, with a 100,000-unit maximum horizontal
  trace distance. Cameras below, inside, and above the slab are explicit test
  regimes. Spherical curvature is deferred until a named world-scale use case
  demonstrates that the flat-domain error is unacceptable.
- Density coordinates are stable world-space positions normalized by explicit
  base/detail scales and translated by a deterministic wind offset. Base density
  establishes low-frequency shape; detail density erodes occupied base density;
  optional weather controls coverage. Missing weather binds white. Missing or
  invalid required volume inputs disables the cloud for that view.
- Deterministic fixtures use generated 64x64x64 base `R8_UNORM`, 32x32x32 detail
  `R8_UNORM`, and 64x64 weather `R8_UNORM` data with fixed seeds and no runtime
  randomness. P1 consumes generic textures and does not add cloud semantics to
  `DVolumeTexture` or TextureBuild.

### Spatial integration and output

- The full-resolution reference uses at most 32 primary samples with an
  interval-derived step length. Empty samples do no lighting work; occupied
  samples use at most four directional form samples. Blue-noise jitter,
  temporal interleaving, and reconstruction are prohibited in P1 reference
  captures.
- Rays intersect the slab analytically and then clamp their far endpoint to
  world position reconstructed from the current opaque depth using the existing
  view matrices and depth convention. Pixels with no positive interval write
  zero radiance and unit transmittance.
- The cloud target is `RGBA16_FLOAT`: RGB stores scene-linear premultiplied
  radiance and A stores transmittance in [0, 1]. Composition is exactly
  `SceneColor = CloudRadiance + CloudTransmittance * SceneColor`; outside the
  fitted view the target is zero radiance/unit transmittance.
- P1's fixed light term exists only to make spatial density readable: one
  normalized prepared directional-light direction/color when available, or a
  frozen neutral fallback. Its numeric response is not the authored production
  lighting contract and may be replaced only by P4.

### Route, pass order, and lifetime

- The production route is compute when the reflected payload, sampled/storage
  views, dispatch limits, inputs, and extent are valid. An equivalent fullscreen
  fragment route is selected when compute is unavailable. Disabled/no inputs or
  total resource failure performs no cloud pass and leaves Scene Color
  bit-identical to the existing path.
- Compute dispatch uses 8x8 groups outside graphics render passes. Density and
  depth transition to compute-readable, cloud output transitions to compute
  writable and then graphics-readable, and inputs return to their declared
  downstream states through public RHI transitions. No copy pass or device-idle
  wait is allowed.
- The current retained-forward phase is split only into retained opaque/masked
  and sorted translucent phases. Cloud composition occurs between them in both
  special-forward and hybrid-deferred paths. Sky/bootstrap and opaque lighting
  precede clouds; post-process, editor assistance, and Present remain after
  translucency.
- Renderer resource slots publish complete payloads and retain last-known-good
  state across shader/device generations. Targets are extent-keyed and bounded
  to 64 MiB total across fragment, compute, and composite targets. Resize/reload/retry cannot
  expose partial resources or stale fitted-view pixels.

### Frozen qualification gates

- Qualification adapter: NVIDIA GeForce RTX 3090, Vulkan 1.4.325, Win64
  Debug DurinEditor. Extents are 1280x720, 1920x1080, and a 1919x1079 target
  containing a 1601x901 fitted viewport at (137, 89), through threaded and inline
  RHI executors.
- Reference captures cover camera below/inside/above, empty/solid/structured
  density, opaque foreground clipping, translucent foreground ordering, no
  cloud, invalid required input, and compute/fragment routes. Compute and
  fragment cloud targets must differ by no more than 2/1024 per half-float
  channel and final 8-bit output by no more than 1/255 per channel; no-cloud
  output is byte-identical.
- The P1 spatial reference is deliberately a correctness baseline rather than
  the shipped temporal quality tier. After 30 warm-up and 120 measured frames,
  compute must record one dispatch/no cloud draw/no copy and stay within 12 ms
  median and 16 ms p95 at 1920x1080 on the named adapter. Fragment must remain
  functional and within 150% of compute median. Retained cloud targets stay at
  or below 64 MiB, excluding generic input assets owned elsewhere.

## Current Foundations and Gaps

| Area | Existing foundation | P1 gap |
| --- | --- | --- |
| Volume inputs | `DVolumeTexture` publishes cooked sampled 3D RHI resources with deterministic mips and recovery. | Define renderer-private cloud meanings and deterministic base/detail/weather fixtures without changing generic texture ownership. |
| Compute/fallback | Contact visibility proves compute-first routing, fragment fallback, explicit transitions, reload/retry, counters, and clean shutdown. | Add an `RGBA16_FLOAT` cloud payload and density/depth sampling with equivalent routes. |
| Scene order | Sky, GBuffer/deferred lighting, retained forward geometry, sorted translucency, post-process, assistance, and Present exist. | Split retained opaque/translucent execution at one narrow boundary and insert cloud render/composite. |
| Depth/view | Fitted views, forward/reversed depth reconstruction, main/auxiliary view identity, and offscreen/Present paths are qualified. | Clamp slab intervals to opaque depth and prove every view/output route. |
| Resource lifetime | Renderer coordinator and slot caches provide complete-or-null/last-known-good publication and bounded eviction. | Own cloud shader payloads/targets, byte ceilings, failure reasons, and reload/device recovery. |
| Diagnostics | Per-view statistics, timing/capture sinks, route reasons, and runtime smoke infrastructure exist. | Publish cloud route, samples, retained bytes, failure reason, timing, and deterministic capture evidence. |

## Implementation Stages

### Stage 0: Freeze spatial, composition, fallback, and budget contracts

- [x] Confirm P0 foundations and Compute Renderer Integration are complete and
  no active Renderer plan owns the same orchestration/resource surfaces.
- [x] Select the flat world-Z slab, deterministic density mapping, required and
  optional texture roles, camera regimes, and bounded sample counts.
- [x] Freeze `RGBA16_FLOAT` radiance/transmittance semantics, depth clipping,
  fitted-view clearing, and composition algebra.
- [x] Freeze compute/fragment/disabled route eligibility, public transition
  boundary, complete-or-last-known-good publication, and 64 MiB target ceiling.
- [x] Freeze the minimal retained opaque/cloud/translucency split and explicitly
  preserve special-forward, hybrid-deferred, offscreen, Present, post-process,
  and assistance ownership.
- [x] Freeze adapter, extent, capture matrix, pixel tolerances, structural
  counters, warm-up/sample count, and spatial timing gates before observing
  cloud route timings.

#### Acceptance Gate

- Scope, coordinate/output contracts, pass order, route/failure behavior,
  fixtures, tolerances, and timing/memory budgets are explicit and do not depend
  on P2-P5 features.

### Stage 1: Build deterministic spatial inputs and pure contracts

- [x] Add renderer-private route decision, finite slab intersection,
  overflow-safe dispatch sizing, and saturating target-byte/budget contracts.
- [x] Add immutable spatial parameters, texture bindings, and sample/route
  counter contracts consumed identically by CPU, compute, and fragment paths.
- [x] Add deterministic base/detail/weather volume fixture generation using
  existing generic texture upload paths; validate missing/invalid/white-weather
  behavior without reflected cloud objects.
- [x] Add CPU reference integration for empty, solid, structured, below/inside/
  above, forward/reversed depth, fitted viewport, and opaque-clipped rays.
- [x] Add shader reflection/layout tests for base/detail/weather/depth inputs,
  uniform parameters, and one `RGBA16_FLOAT` storage output.

#### Acceptance Gate

- Pure route/math/byte contracts and deterministic input/reference fixtures pass
  without modifying frame pass order or requiring editor/scene authoring.

### Stage 2: Render and recover the cloud output

- [x] Add compute and fragment cloud shaders with matched density, interval,
  extinction, minimal light, early-termination, and complete fitted-target write
  behavior.
- [x] Add `FVolumetricCloudRenderer` resource slots, canonical texture views,
  extent cache, sampler/uniform ownership, failure categories, retry/invalidation,
  timing/capture sinks, and bounded eviction.
- [x] Dispatch compute outside render passes with explicit sampled/depth/storage/
  graphics-read transitions; add the fullscreen fragment fallback without a
  copy or Vulkan-specific call.
- [x] Prove exact route selection, group/sample counters, compute/fragment target
  parity, input-state restoration, replacement failure, reload/retry, both
  executors, repeated frames, and resource release on real Vulkan.

#### Acceptance Gate

- Deterministic cloud targets match the CPU/reference tolerances through public
  RHI in both routes/executors, failure falls back explicitly, and validation is
  clean across reload, replacement, resize, and shutdown.

### Stage 3: Insert depth-aware composition into every scene route

- [x] Split retained opaque/masked execution from combined sorted translucency
  at the narrowest shared Renderer boundary without changing draw preparation or
  translucency ordering.
- [x] Render/composite clouds after sky plus opaque lighting and before sorted
  translucency in special-forward and hybrid-deferred Lit/Solid routes; preserve
  existing Unlit/Wireframe and no-cloud behavior explicitly.
- [x] Add the cloud composite shader/pass using the frozen algebra and prove
  opaque depth clipping, translucent foreground ordering, fitted-view isolation,
  offscreen output, window-backed Present, post-process, and editor assistance.
- [x] Publish per-view route/reason/sample/byte/pass counters and ensure failed
  cloud work cannot fail an otherwise renderable scene view.

#### Acceptance Gate

- Reference images prove opaque/cloud/translucency order and no-cloud identity
  across forward/hybrid, main/auxiliary, fitted/offscreen/Present, supported
  depth conventions, and editor-assistance output.

### Stage 4: Qualify P1 and publish the P2 handoff

- [x] Record the approved GPU timing/structure matrix on the named adapter while
  retaining the pre-observation numeric gates after the adapter rebaseline.
- [x] Run focused Renderer/Engine/RenderCore/Vulkan coverage, native aggregate,
  full build, and a validation-enabled Debug Editor runtime matrix with resize,
  route forcing, reload/retry, stable frames, Present, and clean shutdown.
- [x] Publish lasting spatial rendering, composition, fallback, recovery,
  diagnostics, output, and parameter contracts under Runtime rendering docs.
- [x] Update the volumetric-cloud roadmap with P1 evidence and activate P2 only
  after its input parameter/resource/fallback contract is fully frozen.

#### Acceptance Gate

- Pixel, ordering, transition, recovery, lifecycle, timing, memory, focused,
  aggregate, full-build, runtime, and documentation gates pass; the P2 renderer
  input handoff contains no reflected-object or implementation-specific quality
  policy.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Slab/ray math | Below, inside, above, miss, parallel, fitted, and opaque-clipped intervals are finite and bounded | Pure Renderer tests and CPU reference |
| Density inputs | Base/detail/weather sampling is deterministic; white optional weather and invalid required-input behavior are explicit | Fixture, shader, and Vulkan sampling tests |
| Compute route | One bounded 8x8 dispatch writes the whole fitted output with explicit transitions and no copy/idle | Renderer contract and Vulkan integration tests |
| Fragment fallback | Same parameters and inputs produce the frozen target/final-image tolerance when compute is unavailable | Vulkan capture/readback fixture |
| No cloud/failure | Disabled, missing input, unsupported extent, failed payload/target, and total failure preserve a renderable scene and exact no-cloud output | Route/failure injection and image tests |
| Composition | Opaque clips clouds; clouds precede sorted translucency; post-process/assistance/Present remain downstream | Engine image/order tests and runtime smoke |
| Views/outputs | Forward/hybrid, main/auxiliary, fitted/resize, offscreen/Present, and supported depth conventions remain isolated | Engine/Vulkan multi-view tests |
| Recovery/lifetime | Reload, retry, replacement failure, device invalidation, backlog, eviction, and shutdown expose complete or last-known-good state only | Resource/lifecycle tests and validation runtime |
| Budgets | Frozen GPU timings, dispatch/draw/copy/sample counters, and retained target bytes meet Stage 0 gates | Stage 4 qualification record |

## Definition of Done

- A deterministic fixed-input cloud renders through public RHI with compute as
  the normal eligible route and an equivalent fragment fallback.
- Opaque depth clips the cloud interval, scene-linear radiance/transmittance
  composition occurs before sorted translucency, and no-cloud output is exact.
- Forward/hybrid, fitted/offscreen/Present, main/auxiliary, reload/recovery, both
  executors, and clean shutdown pass the frozen validation matrix.
- Target memory and named-adapter spatial timing satisfy the predeclared gates.
- Stable P1 contracts are documented and P2 can add scene/component authoring
  without changing the renderer input, coordinate, texture-role, or fallback
  boundary.

## Deferred Follow-ups

- P2: reflected cloud scene contract, actor/component, stable active selection,
  immutable render snapshot, serialization, duplication, and generic Details.
- P3: low-resolution rendering, temporal jitter/reprojection/rejection,
  reconstruction, typed view history, and named quality tiers.
- P4: production directional scattering, self-transmittance, ambient term, and
  receiver cloud shadows.
- P5: import/generation workflow, previews, debug UI, presets, and authoring
  diagnostics; P6: cross-feature production qualification and final contracts.
- Spherical shells, multiple/local volumes, volumetric fog, render graph,
  indirect dispatch, and asynchronous compute remain evidence-gated future work.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud spatial rendering](../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Synchronous Compute Pipelines](../Runtime/Rendering/SynchronousComputePipelines.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Persistent View State](../Runtime/Rendering/PersistentViewState.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Build and run](../Agents/BuildAndRun.md)
- [Testing](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTextureRenderResource.h`
- `Engine/Shaders/Slang/ContactShadow.slang`
- `Engine/Shaders/Slang/VolumetricCloud.slang`
- `Engine/Shaders/Slang/PostProcess.slang`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
