# Deferred Contact Visibility Integration Plan

Summary: Move directional contact shadows into deferred Lit-opaque visibility, consume GBuffer receiver data, and retire post-scene HDR subtraction plus its permanent intermediate targets.

Last reviewed: 2026-08-16

Status: Archived
Completed: 2026-08-16

## Current Status

Completed on 2026-08-16. Directional contact shadowing now produces an
on-demand `R8_UNORM` visibility mask from standard-Lit GBuffer flags,
geometric normal, D32, current-view matrices, and the selected directional
light. It records after GBuffer/GTAO preparation and before deferred lighting.
Deferred lighting multiplies only the post-cascade directional term; retained
forward records afterward. Missing or failed optional resources bind the white
default texture and continue the same deferred route.

Production `DirectionalDirect`, `ContactColor`, their MRT outputs, Scene Color
subtraction, and the permanent 12-byte-per-pixel overhead are removed. Scene
targets now cost 12 bytes per pixel and 24,883,200 bytes at 1920x1080; the
cache ceiling is 96 MiB. The validation-enabled RTX 3090 production fixture
measured 406,784 ns median and 695,392 ns p95 total, 70,502,400 active bytes,
120,834,048 bytes including cascaded shadow storage, and a 256 MiB combined
retained ceiling. Enabled contact adds one 2,073,600-byte mask at 1920x1080;
disabled or receiver-empty views add no pass or target request.

The fixed trace baseline remains selected: 16 midpoint samples, 0.01 start,
0.20 maximum world distance, 0.012 thickness, and 48 pixels. GBuffer receiver
classification adds standard-Lit rejection plus a geometric-normal grazing
fade from 0.02 to 0.20. The existing contribution diagnostic remains the one
published view. Separate hit-distance, rejection, and budget views were not
added: their values are deterministic shader inputs/classification and are
covered by reflection, layout, image, Unlit, constrained-view, resize,
failure/retry, and Vulkan qualification tests without expanding the public
view-settings ABI.

Validation passed for renderer and full builds, focused layout and reflection
contracts, contact target failure/retry/invalidation/release, full
EditorGridVulkan integration, DirectionalShadowBaselineVulkan qualification,
GBuffer/production RTX 3090 qualification, HDR display qualification,
`fast-all`, documentation validation, all-plan validation, Vulkan validation,
and editor smoke.

The following paragraphs preserve the superseded implementation baseline used
for migration comparison.

The existing opt-in implementation works but is not a qualified production
tier. It runs after complete scene composition, marches 16 fixed midpoint
samples through final `SceneDepth`, reads the selected directional contribution
from `DirectionalDirect`, and subtracts the occluded portion into the
`RGBA16_FLOAT` `ContactColor` ping-pong target. Focused Vulkan coverage proves
bounded near-field darkening, byte-identical Unlit-view output, one enabled
pass, and zero pass failures. Contact shadows remain disabled by default;
user-scene motion review and target-GPU evidence remain open.

That ownership predates the production hybrid renderer. Lit solid
opaque/masked surfaces now have one GBuffer plus deferred-lighting owner, while
Unlit opaque/masked and translucent surfaces compose later through retained
forward. A contact pass after retained forward can therefore observe depth and
HDR that no longer describe the deferred receiver stored in
`DirectionalDirect`. It also ignores available GBuffer validity and normal
data, while the scene-target cache permanently allocates both
`R11G11B10_FLOAT` `DirectionalDirect` and `RGBA16_FLOAT` `ContactColor` even
when the feature is disabled.

This revision supersedes qualification of that post-scene subtraction design.
The selected architecture computes scalar contact visibility from the current
view's GBuffer and depth before deferred lighting, applies it only to the
selected directional term inside deferred lighting, then records retained
forward. Disabled or unavailable visibility is factor one. After parity is
established, production `DirectionalDirect`, post-scene contact composition,
and the permanent `ContactColor` dependency are removed.

Single-layer screen-space limitations remain explicit: off-screen casters,
hidden depth layers, and authored geometry gaps cannot be recovered by this
technique. The plan improves ownership, silhouette rejection, scale behavior,
memory, and bandwidth without claiming otherwise.

## Goal

Make directional contact shadowing a bounded, optional deferred visibility
term for standard-Lit opaque/masked receivers. Use current-view GBuffer data to
reduce invalid receiver and depth-discontinuity hits, preserve the directional
and cascade-shadow contracts, remove post-scene HDR subtraction, and eliminate
its permanent intermediates while meeting explicit image, motion, failure,
memory, and RTX 3090 performance gates.

## Scope

- Produce scalar contact visibility for valid standard-Lit GBuffer receivers
  from current-view depth, normal, matrices, and selected light direction.
- Record visibility after complete GBuffer production and before deferred
  lighting; consume it before retained-forward composition.
- Multiply only the selected directional contribution by contact visibility
  after cascade attenuation. Preserve local, environment, emissive, Unlit,
  translucent, and already-shadowed contributions.
- Preserve a short bounded trace, exact point-depth reads, explicit world- and
  screen-distance limits, opt-in default, contribution diagnostics, and
  conservative factor-one fallback.
- Select a bounded receiver bias/thickness formula using receiver normal,
  light angle, depth, and pixel footprint only if captured evidence beats the
  fixed baseline.
- Allocate visibility only for an enabled or diagnostic view and bind a
  complete factor-one fallback otherwise.
- Remove production `DirectionalDirect`, post-scene contact composition, and
  permanent `ContactColor` after deferred parity is proven.
- Give GBuffer diagnostics a separately owned on-demand output instead of
  retaining `ContactColor` as unrelated scratch storage.
- Preserve current-view isolation for main, auxiliary, preview, thumbnail,
  Present, and offscreen rendering and renderer resource generations.

## Non-Goals

- Representing off-screen casters, hidden depth layers, or repairing authored
  geometry gaps.
- Changing cascade selection, bias, filtering, transitions, or diagnostics.
- Contact shadows for point, spot, translucent, Unlit, or special-forward
  surfaces.
- Variable-penumbra softness, stochastic or ray-traced shadows, or a second
  general shadow system.
- Adding motion vectors, temporal history, HZB, asynchronous compute, a render
  graph, or transient-allocation infrastructure solely for this feature.
- Enabling contact shadows by default before target-GPU and motion gates pass.
- Restoring rejected neighboring-depth plane reconstruction, adaptive 24-64
  steps, endpoint contraction, edge heuristics, or binary refinement without
  new activation evidence.

## Design Decisions and Invariants

### Deferred visibility ownership

The production Lit solid sequence becomes:

```text
GBuffer + D32
  -> optional directional contact visibility
  -> SkyBox/clear HDR bootstrap
  -> deferred lighting with cascade visibility * contact visibility
  -> retained-forward Unlit opaque/masked + sorted translucent
  -> display and editor assistance
```

Visibility is `1.0` for unoccluded or unavailable and approaches `0.0` only
for a qualified short-range hit. The pass reads or writes no Scene Color.
Only `GBufferStandardLitFlag` pixels are receivers. GBuffer normal and light
direction may reject or fade invalid backfacing and grazing cases, but contact
visibility may never brighten a cascade-shadowed result:

```text
finalDirectionalVisibility = cascadeVisibility * contactVisibility
```

### Visibility resource and fallback

The selected representation is a single-channel normalized target, preferably
`R8_UNORM`. Stage 0 must confirm Vulkan render-target and sampled-texture
support; `R16_FLOAT` is the recorded fallback if R8 fails layout or precision
tests. The texture is scoped to one extent and resource generation and is
resolved only for enabled or diagnostic views.

A disabled feature, invalid light or matrix, missing target, shader/pipeline
failure, or failed contact pass binds a complete white fallback and continues
the same deferred path. Failure may increment a counter but may not fail the
view, expose a partial target, change lighting owner, reuse another view's
visibility, or rerender through forward.

### Trace and receiver classification

Exact texel `Load`, convention-aware device-depth ordering, finite world
separation, a 16-step upper bound, 0.20-world-unit maximum extent, and 48-pixel
maximum displacement remain the comparison baseline. Linear depth filtering
and unbounded one-sided depth matches remain forbidden.

Stage 0 freezes one candidate formula for start offset, thickness, and
grazing-angle fade from near/far, orthographic/perspective, scale, and
silhouette captures. It must have explicit minima and maxima, remain within
the existing trace bounds, and not invent a receiver plane from neighboring
depth. If every artifact gate is not equal or better, fixed constants remain.

### Target and layout retirement

Once deferred lighting consumes visibility, `DirectionalDirect` has no
production consumer. Its MRT shader output, scene-target allocation,
bootstrap/retained layouts, and attachment ABI are removed only after an
intermediate parity gate. The post-scene renderer and production
`ContactColor` are then removed. Diagnostics receive a separately named,
on-demand target and cannot keep production intermediates alive.

### Ordering, thread, and lifetime

`FSceneRenderer` coordinates the feature on the render thread. Visibility is
produced and consumed within one current-view command; resource cache entries
follow device, shader, retry, release, and extent generations. Constrained
views reuse deferred lighting's absolute attachment coordinates and
viewport-local reconstruction. No view may sample prior-view depth, normal,
visibility, matrices, or extent.

## Current Foundations and Gaps

| Area | Current foundation | Refactor gap |
| --- | --- | --- |
| Receiver data | GBuffer stores material flags, normals, surface data, emissive, and D32 | Contact reads only final D32 and cannot identify the deferred receiver reliably |
| Directional ownership | Deferred lighting owns cascade attenuation and directional BRDF | Contact later subtracts copied directional light from composed HDR |
| Ordering | Unified scene entry owns deferred then retained-forward | Contact remains outside it after retained-forward |
| Trace | Exact point loads and bounds prevent the worst halos | Fixed world constants are scale-sensitive; normal and pixel footprint are unused |
| Failure | Missing contact resources preserve no-contact Scene Color | Factor-one fallback is implicit rather than a deferred input |
| Targets | Scene targets provide HDR, D32, `DirectionalDirect`, and `ContactColor` | Two permanent color targets mainly serve an opt-in post-scene effect |
| Diagnostics | Contribution view and enabled/failure counters exist | Rejection, distance, budget, pass-time, and on-demand-target evidence are absent |
| Qualification | Focused darkening and Unlit tests pass | Motion, scale, silhouette, memory, and RTX 3090 gates remain open |

## Implementation Stages

### Stage 0: Freeze migration evidence and the visibility contract

- [x] Capture legacy enabled/disabled outputs for near/far and scaled contact,
      orthographic/perspective cameras, silhouettes, grazing receivers,
      defective geometry, viewport edges, mixed retained-forward surfaces,
      constrained viewports, and alternating extents.
- [x] Record legacy target bytes, pass count, worst-case query count,
      render-thread timing, and validation-enabled RTX 3090 1920x1080 GPU
      intervals.
- [x] Inventory every `DirectionalDirect`, `ContactColor`, renderer, layout,
      shader-output, capture, test, and documentation dependency with its
      replacement owner.
- [x] Confirm `R8_UNORM` target/sample support and clear/load behavior; select
      `R16_FLOAT` only if the recorded R8 gate fails.
- [x] Freeze visibility semantics, white fallback, viewport mapping, optional
      lifetime, failure counters, diagnostics, and one bounded parameter
      formula or retain the fixed constants based on captured comparisons.
- [x] Freeze numeric GPU, active/retained byte, motion, and artifact thresholds
      before changing production output.

#### Acceptance Gate

- The legacy path has reproducible image, motion, memory, pass, and target-GPU
  evidence; every removable dependency has a disposition; one format,
  parameter formula, failure contract, and numeric gate is selected.

### Stage 1: Produce GBuffer-owned contact visibility

- [x] Add a visibility shader reading current-view flags, normal, D32,
      matrices, depth convention, viewport, and light direction without Scene
      Color.
- [x] Restrict receivers to valid standard-Lit pixels and implement the frozen
      facing, offset, thickness, distance, and fade rules.
- [x] Add the single-channel layout, bindings, ABI assertions, pipeline,
      optional per-extent resource slot, and complete white fallback.
- [x] Rename or replace `FScreenSpaceContactShadowRenderer` so its API produces
      visibility rather than composed HDR color.
- [x] Preserve the contribution view and expose receiver rejection, distance,
      and step budget through deterministic shader constants and qualification
      assertions without adding production output or a new settings ABI.
- [x] Add reflection, layout, depth-convention, viewport-origin, failure,
      retry, release, resize, and cross-view tests.

#### Acceptance Gate

- Enabled views produce a bounded scalar mask only for valid deferred Lit
  receivers; disabled and failed views resolve factor one; Vulkan validation
  reports no layout, transition, binding, or lifetime errors.

### Stage 2: Apply visibility inside deferred lighting

- [x] Extend deferred inputs with current contact visibility and enabled state.
- [x] Record visibility after GBuffer and before deferred lighting inside the
      unified scene-composition workflow.
- [x] Multiply only the selected directional contribution after cascade
      evaluation and before final surface composition.
- [x] Preserve local lights, environment, emissive, opacity, GTAO, Unlit,
      translucency, SkyBox, diagnostics, display, FXAA, and assistance.
- [x] Bind factor one on contact failure and continue the same deferred view
      without partial output or route change.
- [x] Migrate focused Vulkan tests to deferred visibility ownership, including
      mixed retained-forward scenes.

#### Acceptance Gate

- Contact-enabled images attenuate exactly one deferred directional term.
  Disabled, failed, Unlit, retained-forward-only, and unrelated lighting terms
  match frozen references, with current-view-only visibility.

### Stage 3: Retire post-scene composition and intermediates

- [x] Remove `DirectionalDirect` from deferred outputs, SkyBox/bootstrap,
      special/retained-forward layouts, shaders, scene targets, descriptors,
      and captures.
- [x] Remove post-scene Scene Color and directional sampling, HDR subtraction,
      and the production `ContactColor` ping-pong path.
- [x] Give diagnostics a separately named on-demand output and prove they do
      not force production allocation.
- [x] Simplify post-process ownership to Scene Color except for an explicitly
      active diagnostic result.
- [x] Remove obsolete pipelines, samplers, counters, tests, and documentation.
- [x] Prove disabled production retains neither retired target and records no
      contact pass.

#### Acceptance Gate

- Production contains no `DirectionalDirect` or production `ContactColor`
  allocation, MRT write, read, pipeline, or capture. Disabled contact adds no
  target/pass; enabled contact adds at most one single-channel target and pass.

### Stage 4: Qualify image stability, failures, and cost

- [x] Run fixed-camera comparisons across lighting, distance, scale, camera
      type, reversed-Z bounds, silhouettes, grazing angles, edges, and defective
      geometry.
- [x] Run camera/object motion sequences measuring changed pixels, mask
      instability, halos, acne, detached contact, and bound transitions.
- [x] Prove off-screen and hidden-layer cases remain bounded limitations rather
      than false coverage or unbounded darkening.
- [x] Inject target, shader, pipeline, uniform, light, matrix, and retry
      failures and prove factor-one fallback and later-view recovery.
- [x] Run focused geometry, shadow, GBuffer, GTAO, retained-forward,
      post-process, layout, shader, lifecycle, Present/offscreen, preview, and
      thumbnail tests through repository workflows.
- [x] Run native aggregates, required build, Vulkan validation, and editor
      smoke, then re-run the RTX 3090 fixture against Stage 0 gates.

#### Acceptance Gate

- Image, motion, failure, lifecycle, build, aggregate, Vulkan, memory, and RTX
  3090 gates pass without new acne, halos, retained-forward corruption,
  cross-view reuse, or concealment of real geometry defects.

### Stage 5: Publish the deferred contact contract

- [x] Update directional-shadow, deferred, GBuffer, forward, HDR/display, and
      viewport contracts with lasting ownership, ordering, fallback, format,
      memory, diagnostics, and limitations.
- [x] Record final target removal, metrics, bytes, GPU intervals, and validation
      evidence in Current Status.
- [x] Remove superseded post-scene language and align the hybrid roadmap with
      the implemented Runtime contract.
- [x] Run changed-document and all-plan validation and complete this plan only
      after every gate passes.

#### Acceptance Gate

- Runtime documentation is authoritative, no active document presents the
  retired subtraction path as production, validators pass, and final evidence
  is reproducible.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Receiver ownership | Only standard-Lit opaque/masked GBuffer pixels receive contact visibility |
| Directional composition | Visibility changes only selected directional light after cascade attenuation |
| Bounds and artifacts | Distance, scale, silhouette, grazing, edge, camera-mode, and motion fixtures meet frozen gates |
| Disabled/failure parity | Disabled and every injected failure are byte-identical to the no-contact reference |
| View isolation | Alternating views/extents and all output modes never reuse visibility inputs |
| Target retirement | Production `DirectionalDirect` and `ContactColor` allocation/write/read counts are zero |
| Diagnostics | Contribution uses current-view output; distance, rejection, and budget remain deterministic qualified constants |
| Lifecycle | Resize, reload, device generation, retry, release, and shutdown preserve fallback and release resources |
| Memory/performance | Disabled costs zero target/pass; enabled bytes and RTX 3090 intervals meet Stage 0 gates |
| Publication | Focused/aggregate tests, build, Vulkan, smoke, Runtime docs, and document validators pass |

## Definition of Done

- Directional contact shadowing is optional deferred visibility owned by valid
  standard-Lit opaque/masked GBuffer receivers.
- It reads no Scene Color, performs no post-scene HDR subtraction, and runs
  before retained-forward composition.
- Production `DirectionalDirect` and permanent `ContactColor` no longer exist;
  disabled contact allocates no visibility resource or pass.
- Enabled contact uses one bounded single-channel current-view mask and every
  optional failure degrades to explicit factor one.
- Fixed-camera, motion, silhouette, scale, failure, lifecycle, memory, and RTX
  3090 gates pass without claiming off-screen or multilayer coverage.
- Lasting behavior and limitations are published in Runtime documentation.

## Deferred Follow-ups

- Motion-vector temporal filtering after a renderer-wide velocity contract.
- HZB acceleration or half-resolution tracing after profiling proves need.
- Local-light contact shadows after separate shadow ownership and activation.
- Multi-layer or ray-traced visibility for off-screen and hidden casters.
- Variable penumbra or stochastic sampling under a qualified quality tier.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [HDR Scene Color and Display Mapping](../../../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Shaders/Slang/ContactShadow.slang`
- `Engine/Shaders/Slang/DeferredDirectionalLighting.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
