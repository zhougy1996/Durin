# Minimal GBuffer and Geometry Pass Plan

Summary: Freeze and implement the smallest measured geometry-buffer contract for every supported opaque and masked primitive family before deferred lighting begins.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

M1 completed on 2026-08-15. Scene lighting now targets scene-linear
`RGBA16_FLOAT`, one display transform owns SDR output, and the existing scene
target set costs 24 bytes per pixel. At 1920x1080 one retained extent is
49,766,400 bytes before any GBuffer attachment is added. Production readback,
cross-view isolation, native-window Present/resize/toggle behavior, full native
coverage, full build, hidden-window editor smoke, and RTX 3090 display budgets
pass.

M2 completed on 2026-08-15 and remains qualification-only geometry transport.
Stage 0 is complete: source and
shader inspection confirms that StaticMesh, SplineMesh, SkeletalMesh, and
Terrain already converge on world position, geometric normal, tangent
handedness, four UVs, vertex color, and one versioned material binding before
the current forward fragment evaluation. The selected contract adds four
attachments totaling 16 bytes per pixel and reconstructs world position from
the existing D32 depth. Its formats, consumers, tolerances, flags, memory
limits, fixture, and RTX 3090 gates are frozen below. No opaque draw has yet
been rerouted; CPU reference sweeps pass the frozen octahedral-normal and
analytic view-relative depth-reconstruction tolerances. Stage 1 now has the
frozen semantic render-target layout and a 128 MiB byte-bounded, size-keyed,
complete-or-null attachment owner wired into `FSceneRenderer` lifecycle
without recording a production pass. CPU layout/accounting tests and Vulkan
image-failure, same-generation suppression, manual-retry, and alternating-
extent isolation tests pass. The shared shader transport is now factored into
one surface
module consumed by both the unchanged forward fragment entry point and a
four-target geometry fragment entry point. Reflection covers Local, Spline,
Skeletal, and Terrain vertex domains; focused Vulkan forward material,
skeletal, and terrain tests preserve existing output behavior. Typed renderer
pipeline payloads now own exact vertex/material parameter metadata and the
four-attachment layout for all four domains. Injected image, shader-module,
and graphics-pipeline failures prove same-generation suppression, ordered
manual recovery, shader reload, device-generation recreation, and explicit
release/recreation. Stage 1 is complete without recording a production draw;
Stage 2 is complete behind the opt-in qualification route. StaticMesh,
SplineMesh, SkeletalMesh, and Terrain reuse their prepared LOD, section,
material, deformation, palette, patch, and batch inputs to publish the frozen
four-attachment contract before the unchanged forward pass. Production views
remain disabled by default. Vulkan fixtures prove exact forward-output parity,
real attachment publication, masked participation, spline and skeletal
deformation, Terrain automatic/fixed LOD and large-coordinate behavior, plus
translucent and unlit exclusion. Per-family attempts, successful writes,
rejects, skips, attachment bytes, and per-view availability are exposed in the
existing view diagnostics. Focused Vulkan targets, `fast-all`, and the full
build pass. Stage 3 is complete: a shared CPU/Slang decoder owns octahedral
normal, packed channel, R11G11B10 emissive, flag, and analytic view-position
reconstruction semantics. Nine channel/depth/reconstruction debug modes and a
temporary decoded-material-input A/B mode write HDR Scene Color before the
unchanged display transform. Vulkan readback covers all four color attachments
for StaticMesh, SplineMesh, SkeletalMesh, and Terrain; D32 is sampled through
the debug pass because the current RHI deliberately defers depth/stencil
transfer copies. Background, valid flags, base material fields, opacity,
emissive, unit normals, masked participation, and per-family publication pass.
The analytic-versus-inverse reference heatmap stays on the within-tolerance
side, while the CPU perspective/orthographic sweeps meet the frozen positional
gate. Main, auxiliary, camera-preview, thumbnail, offscreen, Present, resize,
and alternating-extent fixtures pass without stale reuse. Stage 4 is complete.
The validation-enabled RTX 3090 fixture uses driver 591.86, Vulkan 1.4.325,
`Win64-Debug-DurinEditor`, 30 warm-up frames, and 120 measured 1920x1080
frames. The geometry pass measures `78,096 ns` median and `79,136 ns` p95,
well below the `350,000/500,000 ns` gates; attachment and peak retained bytes
are both `33,177,600` for its single extent. Focused Renderer/Engine/RHI/Vulkan
owners, the 55-target `fast-all` profile, the complete 72-target ordinary
native aggregate, full build, native-window Present/resize, resource reload,
and an 8-second hidden-editor startup/shutdown smoke pass. The first aggregate
run had one transient parallel `VulkanRHIIntegrationTests` process crash; its
58 tests passed in isolation and the unchanged aggregate passed on immediate
rerun. The lasting
[Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md) now owns encoding,
reconstruction, resource, failure, memory, diagnostics, and the M3 input seam.
Production forward rendering remains unchanged. M3 is activated through
[Deferred Directional Lighting](DeferredDirectionalLighting.md), whose Stage 0
must freeze parity and cost contracts before implementation.

## Frozen Stage 0 Contract

### Consumers and transport

| Value | Source | Selected transport | Required consumer |
| --- | --- | --- | --- |
| World position | Existing D32 plus projection parameters | Analytically reconstruct view-relative position at pixel center; no position attachment | M3 directional/local/shadow evaluation; M5 screen-space position |
| Shading normal | Shared tangent frame plus constant/texture normal decode | Octahedral UNORM8 pair | M3 BRDF/environment; later AO edge handling |
| Geometric normal | Pre-normal-map oriented visible-side normal | Independent octahedral UNORM8 pair | M5 receiver offset, grazing confidence, edge and same-surface rejection |
| Base color, metallic | Material constants, textures, and vertex color | RGB UNORM8 plus metallic UNORM8 | M3 direct/environment BRDF |
| Roughness, ambient occlusion | Material constants and textures | Two UNORM8 channels | M3 BRDF and indirect/environment attenuation |
| Emissive | Material constant plus texture, finite non-negative HDR | `R11G11B10_FLOAT` | M3 HDR emissive composition |
| Effective opacity | Base alpha, opacity texture, and vertex alpha | UNORM8 | Preserve Scene Color alpha for opaque/masked parity |
| Material class | Eligible lit opaque/masked pass | Surface flag bit 0 (`StandardLit`); zero is background/invalid | Decode validity and diagnostics; no runtime branch among unsupported classes |
| Mask decision | Existing material mask and static threshold | Discard before any attachment write | Opaque/masked coverage parity |
| Primitive identity | Prepared-view sort/counter identity | Not stored | Diagnostics stay draw/counter-owned; no M3-M5 pixel consumer justifies bytes |

Unlit, translucent, special forward, sky, and editor assistance do not publish
records. Terrain's dithered LOD coverage and masked material rejection happen
before publication. Spline and skeletal deformation continue to produce the
same local intermediates consumed by the shared vertex output; Terrain
synthesizes the equivalent world position, normal, tangent, UV, and color.

### Attachment layout

| Attachment | Format | Channels | Bytes/pixel |
| --- | --- | --- | --- |
| `GBufferMaterial` | `RGBA8_UNORM` | base color RGB, metallic A | 4 |
| `GBufferNormals` | `RGBA8_UNORM` | shading octahedral RG, geometric octahedral BA | 4 |
| `GBufferSurface` | `RGBA8_UNORM` | roughness R, ambient occlusion G, effective opacity B, flags/255 A | 4 |
| `GBufferEmissive` | `R11G11B10_FLOAT` | finite non-negative scene-linear emissive RGB | 4 |

All attachments clear to zero. Zero flags plus background depth means invalid
GBuffer data. Valid standard-lit pixels write flag value `1`; unused bits must
remain zero. The geometry render pass clears once, stores every attachment,
and leaves color attachments graphics-shader-readable. D32 is cleared/filled
with the same reversed-Z contract and remains shader-readable after the pass.

Octahedral encode/decode must stay within `1.0 degree` angular error for finite
unit normals after UNORM8 quantization. Base color, metallic, roughness, AO,
and effective opacity allow at most `1/510` absolute quantization error.
Emissive permits at most `1.0%` relative error for values in `[2^-14, 64]` and
an absolute error of `2^-14` below that range. Pixel-center D32 reconstruction
uses the frozen analytic reversed-Z perspective/orthographic equations rather
than a cancellation-prone inverse view-projection multiply. It must be finite
and stay within `max(0.002, 3e-5 * distance-to-view)` world
units of the forward interpolant for the frozen perspective, orthographic,
constrained-aspect, reversed-Z, large-coordinate, and grazing fixtures.

### Memory, timing, and reference fixture

- GBuffer cost is exactly 16 bytes per pixel: `33,177,600` bytes at
  1920x1080. Together with M1 scene targets and one SDR output, one active
  route is `91,238,400` bytes.
- The GBuffer size cache retains the current extent and evicts oldest other
  extents above `128 MiB`; four 1920x1080 GBuffer extents fit and five do not.
  Combined frozen scene-target plus GBuffer cache ceilings are `320 MiB`.
- RTX 3090 qualification uses driver 591.86, Vulkan 1.4.325,
  `Win64-Debug-DurinEditor`, validation enabled, 30 warm-up frames, and 120
  measured 1920x1080 frames. The geometry pass must not exceed `350,000 ns`
  median or `500,000 ns` p95.
- The timing/image fixture tiles opaque and masked StaticMesh, SplineMesh,
  SkeletalMesh, and Terrain across one full output with one-cover overdraw.
  It includes constant and textured materials, authored normal perturbation,
  mirrored/two-sided geometry, emissive above one, masked edges, perspective
  and orthographic views, and one large-coordinate Terrain view.
- Debug and A/B comparisons use exact integer/flag checks, the numeric
  tolerances above, zero-background checks, and deterministic image hashes for
  each primitive/material/view case. Intentional contract changes require
  explicit rebaseline review.

## Goal

Publish and implement one minimal, evidence-backed GBuffer that deterministically
encodes every currently supported opaque and masked StaticMesh, SplineMesh,
SkeletalMesh, and Terrain draw. Preserve the forward renderer as the production
lighting owner during this milestone, provide explicit debug/reconstruction
evidence, and establish qualified inputs for M3 without introducing a second
material or lighting model.

## Scope

- Inventory every material and geometric field required by M3 directional,
  environment, emissive, shadow, and later GTAO/contact consumers.
- Compare stored and reconstructed world position, geometric normal, shading
  normal, tangent handedness, base color, metallic, roughness, ambient
  occlusion, emissive, material class, effective opacity, and primitive
  identity candidates.
- Select attachment formats, channel packing, clear values, load/store actions,
  layouts, transitions, byte accounting, and cache retention.
- Add an opaque/masked geometry pass for StaticMesh, SplineMesh, SkeletalMesh,
  and Terrain using shared material decode and deformation inputs.
- Add GBuffer debug views, readback, deterministic fixtures, and temporary
  forward-versus-geometry A/B evidence.
- Cover per-view ownership, resize, alternating extents, resource failure,
  retry, shader/device invalidation, reload, and shutdown.
- Freeze RTX 3090 1920x1080 bandwidth, memory, and geometry-pass GPU gates.

## Non-Goals

- Deferred directional, environment, emissive, local-light, or shadow
  evaluation; those belong to M3 and M4.
- Routing translucent, particle, water, hair, unlit, or editor-assistance
  surfaces through the GBuffer.
- GTAO, decals, clustered/tiled lighting, revised contact shadows, or a render
  graph.
- Expanding supported light counts or changing BRDF, material authoring,
  deformation, LOD, culling, or shadow-selection behavior.
- Keeping an unrestricted second generic opaque renderer after the later M4
  rollout gate.

## Design Decisions and Invariants

### Data contract

- Every stored bit has a named consumer and a measured stored-versus-
  reconstructed comparison. Unused attachment channels are not a design
  invitation.
- Geometric-surface identity remains independent of authored normal maps. M5
  may consume the selected stable geometric normal or equivalent signal.
- Material decode, tangent-space normal application, PBR parameters, emissive,
  and alpha-mask decisions reuse shared shader facilities. The geometry pass
  changes transport, not material semantics.
- Opaque and masked draws are eligible. Mask rejection occurs before GBuffer
  publication; alpha-blended surfaces remain forward.
- World-position storage is not assumed. Stage 0 compares depth reconstruction
  against explicit position storage over perspective, orthographic,
  constrained-aspect, large-coordinate, and reversed-Z fixtures.

### Pass and ownership

- `FSceneRenderer` owns pass ordering. Renderer-owned size-keyed resources own
  GBuffer allocation and never alias another view's current extent.
- The production forward lighting pass remains authoritative in M2. The new
  geometry pass may run only in qualification/A-B modes until M3 consumes it.
- Scene Color stays HDR and display mapping remains unchanged. GBuffer debug
  views are scene-domain diagnostics mapped through the existing display path.
- Transitions are explicit and use existing graphics/transfer ownership. M2
  does not require a render graph, transient allocator, async compute, or a
  second queue.

### Failure and rollout

- GBuffer payloads publish complete-or-null. A missing required attachment,
  shader, pipeline, or primitive input disables the qualification path for
  that view and cannot reuse stale data.
- A failure in M2 never breaks the production forward output. Counters and
  diagnostics distinguish unavailable, rejected, skipped, and successful
  geometry views.
- Cache policy is byte-based. Stage 0 freezes both per-extent bytes and peak
  retained bytes before implementation.

## Current Foundations and Gaps

| Area | Existing foundation | M2 gap |
| --- | --- | --- |
| Scene targets | HDR color, depth, directional direct, contact color, explicit layouts, 24 B/px accounting | No geometry attachments, semantic layout, or retained-byte gate |
| Materials | Versioned PBR representation and shared opaque/masked/translucent shader paths | No pass-boundary packing or decode parity proof |
| Geometry | StaticMesh, SplineMesh, SkeletalMesh, and Terrain production draws | No one geometry-pass contract spanning all four families |
| Position/normal | Reversed-Z depth and tangent-space shading normals | No qualified reconstruction tolerance or stable geometric-surface signal |
| Lifecycle | Transactional Renderer resource slots and size-keyed caches | No GBuffer payload failure/retry/resize evidence |
| Diagnostics | View counters, Vulkan readback, image fixtures, GPU timestamps | No attachment debug modes, reconstruction heatmaps, or M2 timing seam |

## Implementation Stages

### Stage 0: Freeze consumers, layout candidates, and budgets

- [x] Inventory each M3-M5 consumer and map it to exact source material,
      primitive, view, depth, or identity data.
- [x] Inventory current StaticMesh, SplineMesh, SkeletalMesh, and Terrain vertex
      outputs, deformation paths, material bindings, mask behavior, and
      available stable primitive identifiers.
- [x] Compare depth-derived position against stored position and compare normal
      encodings over perspective, orthographic, constrained-aspect,
      large-coordinate, mirrored-tangent, grazing, and authored-normal fixtures.
- [x] Select one attachment/channel layout with explicit formats, clear values,
      alpha/material-class rules, layouts, transitions, and debug meanings.
- [x] Freeze numeric per-pixel bytes, one-extent bytes, maximum retained bytes,
      and RTX 3090 1920x1080 median/p95 geometry-pass budgets before production
      implementation.
- [x] Freeze deterministic tolerances and reference captures for all supported
      primitive/material families, masked edges, reconstruction, view classes,
      and forward A/B output.

#### Acceptance Gate

- Every stored or reconstructed field has a named consumer, encoding,
  tolerance, and failure rule; one layout and numeric memory/GPU budget are
  selected with no unresolved position, normal, identity, or packing choice.

### Stage 1: Establish resources, layouts, and shared shader transport

- [x] Add semantic GBuffer render-target layouts and exact format/load/store/
      final-state tests without changing the production scene pass.
- [x] Add byte-accounted, size-keyed complete-or-null GBuffer resources with
      bounded retention and current-view isolation.
- [x] Factor shared material decode, tangent-space normal application, mask
      rejection, and geometric-normal preparation for forward and geometry
      writers without changing selected shading equations.
- [x] Add typed geometry-pass shader parameters and pipeline payloads for each
      required primitive vertex factory and material class.
- [x] Inject attachment, shader, and pipeline failures; prove same-generation
      suppression, manual retry, reload, device invalidation, and release.

#### Acceptance Gate

- Layouts, byte accounting, shared shader transport, and transactional payloads
  pass focused CPU/RHI tests while production rendering remains unchanged.

### Stage 2: Encode every opaque and masked primitive family

- [x] Record the geometry pass for visible opaque and masked StaticMesh draws,
      preserving LOD, section, material, culling, and mask behavior.
- [x] Add SplineMesh deformation parity without a separate material or packing
      path.
- [x] Add SkeletalMesh palette/deformation parity with coherent pose and bounds
      snapshots.
- [x] Add Terrain patch/LOD/batching parity and large-coordinate coverage.
- [x] Keep translucent, special forward, unlit, and editor-assistance ownership
      unchanged and prove they do not publish GBuffer records.
- [x] Publish per-family attempts, successful writes, rejects, attachment bytes,
      and view-local diagnostic counters.

#### Acceptance Gate

- Every supported opaque/masked primitive family writes the selected contract
  deterministically; exclusions remain forward and no production-lit image or
  feature owner changes.

### Stage 3: Qualify reconstruction, debug views, and A/B evidence

- [x] Add channel debug views and reconstruction-error heatmaps through the
      existing HDR/display pipeline.
- [x] Read back every attachment and verify clear/background, opaque, masked,
      geometric normal, shading normal, material, emissive, AO, identity, and
      alpha rules.
- [x] Compare reconstructed versus source values against Stage 0 tolerances for
      every primitive family and selected difficult view/geometry fixture.
- [x] Add temporary forward-versus-GBuffer A/B fixtures that decode the stored
      material inputs without performing deferred lighting.
- [x] Prove main, auxiliary, preview, thumbnail, Present/offscreen, resize, and
      alternating-size isolation with no stale attachment reuse.

#### Acceptance Gate

- Debug/readback/A-B evidence meets every frozen tolerance across primitive,
  material, view, and lifecycle matrices; no attachment contains unexplained
  data or unowned padding semantics.

### Stage 4: Qualify cost and publish the M3 input contract

- [x] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the required
      ordinary native aggregate, and the full build through root workflows.
- [x] Capture the frozen RTX 3090 geometry-pass timing matrix and record
      adapter, driver, profile, warm-up/sample count, median, p95, attachment
      bytes, and peak retained bytes.
- [x] Run validation-enabled native-window and hidden-editor resize/reload/
      shutdown smoke coverage.
- [x] Publish the lasting GBuffer encoding, reconstruction, ownership, memory,
      failure, and diagnostic contracts under Runtime Rendering.
- [x] Re-review M3 against the qualified GBuffer and activate
      `DeferredDirectionalLighting.md` only after this plan's exit gate passes.

#### Acceptance Gate

- Image, reconstruction, primitive-family, lifecycle, focused/aggregate test,
  full-build, runtime, documentation, memory, and RTX 3090 GPU gates pass; the
  stable M3 input contract is published and no M2 qualification route has
  become an accidental second production renderer.

## Validation Matrix

| Contract | Required stages | Validation outcome |
| --- | --- | --- |
| Attachment formats and packing | 0-4 | Exact layout tests and Vulkan readback match the selected semantic channels |
| Position and normal reconstruction | 0, 3-4 | Difficult view/geometry fixtures stay within frozen numeric tolerances |
| Primitive families | 0, 2-4 | StaticMesh, SplineMesh, SkeletalMesh, and Terrain opaque/masked writes pass |
| Material parity | 0, 1-4 | Shared decode, normal, PBR inputs, emissive, AO, mask, and alpha rules agree |
| Excluded surfaces | 2-4 | Translucent, special forward, unlit, and editor assistance do not enter the GBuffer |
| View isolation | 1, 3-4 | Main/auxiliary/preview/thumbnail/Present/offscreen and alternating extents remain independent |
| Recovery | 1-4 | Failure, retry, reload, device invalidation, resize, and shutdown never consume stale attachments |
| Memory and GPU | 0, 4 | Per-extent/retained bytes and RTX 3090 median/p95 meet frozen numeric gates |

## Definition of Done

- All Stage 0-4 gates pass and the lasting contract is published.
- One minimal GBuffer deterministically represents all supported opaque and
  masked primitive families within frozen reconstruction tolerances.
- Stored bits, reconstructed values, resource states, byte costs, and failure
  behavior are explicit and tested.
- Production forward lighting remains unchanged during M2; temporary A/B
  routes are bounded qualification tools.
- M3 activation is evidence-backed and the hybrid-deferred roadmap records M2
  completion.

## Deferred Follow-ups

- Deferred directional/environment/emissive evaluation belongs to M3.
- Existing local lights, forward translucency composition, and opaque-owner
  rollout belong to M4.
- GTAO and any normal-aware contact-shadow revision belong to M5 or a later
  evidence-backed plan.
- Decals, scalable light culling, render graphs, transient allocation, and
  asynchronous compute remain separately gated.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
