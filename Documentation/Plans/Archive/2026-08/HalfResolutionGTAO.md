# Half-Resolution GTAO Plan

Summary: Make half-resolution GTAO the production default through deterministic 2x2 representative selection and full-resolution depth/normal-aware resolve while retaining full-resolution GTAO as the quality reference.

Last reviewed: 2026-08-18

Status: Archived
Completed: 2026-08-18

## Current Status

Stages 0 through 3 are implemented. Production defaults to half-resolution
selector-directed GTAO, reduced bilateral filtering, and a full-resolution
edge-aware resolve; full resolution remains selectable as the reference. The
qualified raw interval uses a selector prepass because public RHI cannot read
an attachment being written by the same draw and repeated per-tap 2x2 scans
failed the relative performance gate.

RTX 3090 threaded qualification measured full feature `681,808/697,248 ns`,
half feature `397,568/400,032 ns`, and resolve `116,416/117,600 ns` median/p95.
Half median is `58.3%` of full and passes the `65%` rollout gate. Focused
contract, shader, Vulkan lifecycle/reload, threaded and inline qualification,
full build, documentation validation, and orderly hidden-editor smoke are
clean. On 2026-08-18 the `Win64-Debug-DurinEditor` ordinary native aggregate
passed all 74 targets, including `ViewportTests`, and the incremental full
`all` build remained clean. The previous unrelated viewport-picking blocker no
longer reproduces; Stage 4 and the plan are complete.

## Goal

Reduce the normal production cost of GTAO without changing its indirect-only
composition ownership, deterministic non-temporal behavior, optional failure
semantics, or established grounding character. Half-resolution GTAO must
preserve silhouettes, thin geometry, viewport isolation, odd extents, and
foreground/background ownership through a full-resolution depth/normal-aware
resolve, and must demonstrate a material GPU-time reduction under the existing
qualification protocol before becoming the default.

## Scope

- A selectable half-resolution GTAO quality tier that uses the existing
  three-slice/four-step horizon algorithm rather than a separate SSAO kernel.
- Deterministic mapping between full-resolution view rectangles and reduced AO
  rectangles for nonzero origins, odd origins, odd extents, and constrained
  sub-viewports.
- Deterministic selection of one representative standard-Lit full-resolution
  sample for each covered 2x2 block, with an explicit invalid/background value.
- Half-resolution raw visibility, selector, scratch, and bilateral filtering
  resources with exact active and retained byte accounting.
- A full-resolution `R8_UNORM` resolved visibility target produced with
  depth-, geometric-normal-, validity-, and viewport-aware reconstruction.
- Existing Raw, Confidence, Filtered, and FinalFactor diagnostics adapted to
  distinguish native half-resolution data from resolved production data.
- Full-resolution GTAO retained as an opt-in quality/reference route and as the
  source of deterministic comparison images.
- Focused image, lifecycle, command-recording, memory, and GPU timing evidence
  sufficient to select the production default.

## Non-Goals

- Adding a separate random-kernel SSAO implementation or silently renaming a
  lower-sample approximation as GTAO.
- Temporal accumulation, motion vectors, camera jitter, reprojection,
  checkerboard history, disocclusion history, or cross-frame convergence.
- Changing the three-slice/four-step horizon formula, world radius, falloff,
  thickness, geometric-normal ownership, or indirect-light composition in the
  same rollout.
- Bent normals, directional AO, multi-bounce compensation, GTSO, ray tracing,
  signed-distance fields, or off-screen occluders.
- Applying AO to direct lighting, shadows, emissive, Unlit, translucent,
  SkyBox/background, display mapping, or editor assistance.
- Introducing a render graph, transient allocator, asynchronous compute, or a
  Vulkan-specific Renderer path.
- Treating reduced memory alone as success; the selected path must meet both
  image and GPU-time gates.

## Design Decisions and Invariants

### Quality selection and rollout

- The existing enable setting continues to own `Off` versus `On`. An immutable
  per-view quality selection distinguishes `HalfResolution` and
  `FullResolution`; it is not inferred from viewport size or backend type.
- `FullResolution` preserves the current two-target algorithm and is the
  qualification reference. `HalfResolution` becomes the editor and production
  default only after every acceptance gate passes. Until then it is selectable
  through a development/qualification route.
- Wireframe, Unlit, isolated forward-reference, and migration-fallback views
  continue to allocate and sample no GTAO at either quality.
- A failed half-resolution resource, raw, filter, or resolve operation disables
  GTAO for only that view and binds factor one. It never falls back silently to
  full-resolution GTAO, because an optional feature failure must not introduce
  an unbounded cost spike.
- Full-resolution and half-resolution resource payloads are keyed separately.
  Alternating qualities, extents, or views cannot reuse an incompatible target.

### Reduced extent and viewport mapping

- The reduced target extent is ceil-divided from the owning full target:
  `HalfWidth = (FullWidth + 1) / 2` and
  `HalfHeight = (FullHeight + 1) / 2`. Zero extents remain ineligible.
- A full view rectangle `[Origin, Origin + Extent)` maps to the reduced
  half-open rectangle
  `[floor(Origin / 2), ceil((Origin + Extent) / 2))` independently on each
  axis. Raw and filter viewport/scissor state is restricted to that rectangle.
- Shader mapping uses integer pixel coordinates; normalized interpolation is
  not an ownership rule. Every full pixel maps to its containing 2x2 block,
  and every reduced texel records which in-view full pixel represents it.
- Partial blocks at odd view origins and right/bottom edges consider only full
  pixels inside the fitted view. Sampling, filtering, and resolving may not
  read a neighboring letterbox region or another view merely because both
  occupy the same 2x2 block in the owning target.
- World radius remains `0.75`. The `96` full-resolution pixel projected-radius
  cap becomes at most `48` reduced texels while horizon lookup coordinates map
  back to the selected full-resolution GBuffer positions. The reduction must
  not double the world-space AO radius.

### Representative selection and raw output

- Each reduced texel examines its covered in-view 2x2 full-resolution pixels.
  It selects the nearest valid standard-Lit surface in reconstructed view
  depth; equal-depth ties use a fixed row-major order. If no candidate is
  valid, the texel publishes visibility one and an invalid selector.
- Foreground selection is deliberate: mixed foreground/background blocks must
  not let background visibility darken a foreground silhouette. The later
  resolve may under-occlude an unmatched background pixel, but it may not
  borrow an unrelated foreground factor.
- The raw interval first writes a compact selector/validity attachment, then
  writes half-resolution `R8_UNORM` visibility while sampling that selector.
  Public RHI cannot read an attachment being written by the same draw, and
  rescanning every 2x2 block for every horizon tap fails the rollout gate.
  Stage 0 must freeze a portable
  renderable and sampleable encoding, preferring `R8_UNORM` values for the four
  row-major selectors plus invalid. If the exact format is not supported by
  public RHI capability checks, Stage 0 selects another portable compact format
  before implementation; backend escape hatches are forbidden.
- Horizon reconstruction, geometric-normal use, same-surface rejection,
  distance falloff, finite checks, and deterministic fixed-noise semantics
  remain those of the full-resolution reference. Noise is derived from stable
  full/reduced coordinates and never from frame order.

### Half-resolution filtering

- The existing separable radius-two bilateral pair runs over reduced texels.
  It retains five fixed Gaussian taps per direction and the existing geometric-
  normal and reconstructed-depth thresholds.
- The selector maps each center and tap back to its chosen full-resolution
  GBuffer sample. Invalid selectors, out-of-view texels, non-standard-Lit
  surfaces, depth failures, and normal failures receive zero neighbor weight;
  the valid center always contributes.
- Filter radius remains two reduced texels. Any visible broadening relative to
  the reference is owned by the half-resolution quality tier and must remain
  inside the frozen discontinuity tolerance; it must not be hidden by changing
  the world radius or bilateral thresholds.
- Ping-pong remains raw A -> horizontal B -> filtered A. The selector is
  immutable across both filter passes.

### Full-resolution edge-aware resolve

- Half-resolution GTAO is never sampled directly by deferred lighting. A
  dedicated full-resolution resolve writes one `R8_UNORM` visibility value for
  every pixel in the fitted view, after filtering and before deferred lighting.
  Deferred lighting therefore retains its existing exact full-pixel load and
  factor-one fallback contract.
- For a valid standard-Lit destination pixel, the resolve considers the four
  spatially nearest reduced texels. Each candidate selector identifies the
  representative full-resolution depth and geometric normal. Candidate weight
  combines fixed bilinear spatial weight with the existing depth and normal
  acceptance rules; invalid, background, out-of-view, depth-discontinuous, and
  normal-discontinuous candidates contribute zero.
- Weights are normalized only over accepted candidates. If no candidate is
  accepted, visibility is one. This conservative white fallback is preferred
  over an AO halo or foreground/background leak.
- Background and non-standard-Lit destination pixels resolve to one. Resolve
  writes only the fitted full-resolution viewport/scissor and cannot modify
  another view or letterbox region.
- Raw and Filtered diagnostics identify their native half-resolution domain;
  FinalFactor and production output use the resolved full-resolution factor.
  Confidence distinguishes invalid destination, accepted candidate count,
  rejected depth, rejected normal, and white-fallback cases.

### Ordering, ownership, and failure

Half-resolution production records:

```text
directional shadow -> GBuffer/D32
                   -> representative selector -> half raw visibility
                   -> half bilateral horizontal/vertical
                   -> full-resolution edge-aware AO resolve
                   -> sky/clear bootstrap
                   -> deferred direct
                      + environment * material AO * resolved GTAO
                      + emissive
                   -> retained forward -> contact -> display
                   -> editor assistance
```

- GTAO remains an optional Renderer-owned pre-lighting factor. The resolve does
  not darken Scene Color and no other consumer acquires AO ownership.
- Shader, pipeline, target, selector, uniform, render, filter, or resolve
  failure increments a specific bounded diagnostic and binds the white
  fallback for the affected view. Partial current-frame output and a successful
  prior view are never sampled.
- Resource publication remains complete-or-null against device, shader, and
  manual generations. Resize, quality changes, reload, invalidation, retry,
  cache eviction, recorded-command lifetime, and shutdown preserve the current
  coordinator contract.

### Memory and performance qualification

- The selected target set is half-resolution raw, scratch, and selector plus a
  full-resolution resolved `R8_UNORM` target. With a one-byte selector this is
  `3 * ceil(W/2) * ceil(H/2) + W * H` bytes.
- At 1920x1080 the active set is exactly `3,628,800` bytes, versus the current
  `4,147,200` bytes. The cache retains the existing `32 MiB` ceiling; exact
  fit/eviction counts for even, odd, and mixed-quality entries are frozen and
  tested in Stage 0 rather than inferred from the old full-resolution pair.
- Timing intervals separately cover half raw, half bilateral pair, full resolve,
  deferred composition increment, complete GTAO feature, and the existing
  shadow-through-display frame. Full-resolution reference timings are captured
  in the same warmed process and sample window.
- Rollout requires controlled median and p95 evidence on the named target
  adapter. Stage 0 freezes absolute gates after reproducing the existing
  protocol and before optimizing the candidate. At minimum, half-resolution
  feature median must be no more than `65%` of the same-run full-resolution
  feature median, full resolve may not erase the raw/filter saving, and p95 may
  not regress above the existing full-resolution p95 gate.
- RenderDoc event timing is supporting diagnostic evidence only. Qualification
  continues to use public RHI GPU timing queries, fixed extent/content, warm-up,
  sample count, adapter, driver, and build-profile reporting.

## Current Foundations and Gaps

| Area | Existing foundation | Half-resolution gap |
| --- | --- | --- |
| Algorithm | Deterministic three-slice/four-step full-resolution horizon pass | No reduced-coordinate, representative-selection, or mixed-depth-block contract |
| Stabilization | Full-resolution separable radius-two bilateral filter | Filter assumes AO and GBuffer pixel coordinates are identical |
| Composition | Deferred lighting loads one full-resolution filtered R8 factor | No full-resolution edge-aware resolve exists |
| Resources | Two size-keyed full-resolution R8 targets, 32 MiB cache, exact counters | No quality-keyed half targets, selector, resolved target, or mixed-quality accounting |
| Diagnostics | Raw, Confidence, Filtered, FinalFactor, capture and timing seams | Native-half versus resolved output and resolve rejection are not observable |
| Lifecycle | Complete-or-null creation, retry, reload, invalidation, eviction, recorded lifetime | Quality switching and four-resource transactional publication are unproven |
| Qualification | CPU references, image fixtures, GPU timestamps, RTX 3090 gates | No half/full parity fixtures, odd-origin mapping matrix, or relative rollout gate |

## Implementation Stages

### Stage 0: Freeze reduced mapping, quality, and rollout gates

- [x] Reproduce full-resolution enabled/disabled GPU timing under the published
  protocol and record adapter, driver, profile, viewport extent, warm-up,
  sample count, median, and p95 for every existing interval.
- [x] Freeze the half-target and half-view integer mapping for zero, minimum,
  even, odd, nonzero-origin, constrained-aspect, and target-edge rectangles,
  including proof that partial 2x2 blocks never inspect outside the fitted view.
- [x] Validate the compact selector attachment through public RHI format/layout
  support and freeze its encoding, clear value, shader decode, capture format,
  and exact target-byte formula.
- [x] Freeze half/full/off immutable view settings and editor UI behavior,
  default rollout sequencing, counters, debug names, and failure reasons.
- [x] Extend the deterministic CPU reference with representative selection,
  reduced bilateral filtering, and full-resolution resolve; freeze parity and
  edge tolerances for mixed-depth 2x2 blocks, silhouettes, thin plates,
  subpixel gaps, grazing surfaces, odd extents, and viewport boundaries.
- [x] Freeze absolute half raw/filter/resolve/feature/total median and p95 gates
  plus the same-run relative gate before candidate optimization results are
  accepted.

#### Acceptance Gate

- Reduced-coordinate ownership, selector format, quality selection, resolve
  behavior, image tolerances, exact bytes, failure policy, and absolute/relative
  timing gates are unambiguous and testable without backend-specific behavior.

### Stage 1: Add quality-keyed targets and deterministic representatives

- [x] Add the immutable half/full quality setting and route it through scene
  view construction, viewport presentation, counters, and qualification options
  while retaining the existing enable toggle.
- [x] Add pure ceil-div extent, full-to-half rectangle, full-pixel-to-block, and
  selector encode/decode helpers with focused even/odd/origin/edge tests.
- [x] Extend render-target layouts and the GTAO payload with transactional half
  raw/scratch/selector and full resolved targets, exact active/retained bytes,
  quality-aware cache keys, bounded eviction, and complete-or-null publication.
- [x] Make the half raw interval publish the frozen representative before
  selector-directed visibility while preserving the full-resolution reference path.
- [x] Prove allocation, replacement failure, retry, alternating quality/extent,
  reload, device invalidation, recorded references, eviction, and shutdown
  cannot expose partial or stale targets.

#### Acceptance Gate

- Half-resolution raw output and representative identity match the CPU fixtures
  for every mapping case; resource and lifecycle evidence proves the complete
  quality-keyed payload and exact byte accounting without changing production
  composition.

### Stage 2: Filter in the reduced domain

- [x] Adapt both bilateral passes to reduced coordinates and selector-directed
  full-resolution depth/geometric-normal samples while preserving the selected
  Gaussian, depth, normal, center, and finite-value rules.
- [x] Preserve viewport/scissor isolation and reject invalid selectors or
  candidates outside the fitted full view for both filter directions.
- [x] Extend raw/filtered capture seams and Confidence diagnostics with native
  reduced extent, selector validity, and accepted/rejected neighbor evidence.
- [x] Prove deterministic repeated frames, alternating origins/extents/views,
  projection changes, camera/object motion, and resource refresh produce no
  cross-view state or stale selection.
- [x] Measure half raw and bilateral intervals against the Stage 0 component
  gates before adding resolve cost.

#### Acceptance Gate

- The half-resolution filtered signal matches the frozen reduced CPU reference,
  remains deterministic and view-local, preserves discontinuities in its native
  domain, and demonstrates the expected component saving.

### Stage 3: Resolve full-resolution visibility and integrate production

- [x] Implement the dedicated full-resolution depth/normal-aware resolve with
  four neighboring candidates, normalized accepted weights, exact fitted-view
  writes, and conservative factor-one fallback.
- [x] Transition half filtered and selector targets for resolve reads, publish
  resolved output for deferred graphics reads, and retain every recorded RHI
  reference through both command executors without Vulkan escape hatches.
- [x] Bind only the resolved full-resolution target in deferred production and
  diagnostics; preserve indirect-only composition and the complete white
  fallback when any half/resolve step fails.
- [x] Prove foreground/background mixed blocks, silhouettes, disocclusions,
  thin geometry, subpixel gaps, grazing walls, letterbox boundaries, odd
  extents/origins, Present/offscreen, and all supported geometry families
  against full-resolution and CPU references.
- [x] Prove FullResolution, HalfResolution, and Off can alternate across main,
  preview, auxiliary, and thumbnail views without stale state, blank output,
  or quality leakage.

#### Acceptance Gate

- Production half-resolution GTAO has one full-resolution factor, satisfies the
  grounding and discontinuity gates, preserves every non-interference contract,
  and degrades only the affected view to factor one on injected failure.

### Stage 4: Measure, select the default, and publish the contract

- [x] Run focused Renderer/Engine/Vulkan image, layout, failure, lifecycle, and
  timing coverage through the root build/test workflows in dedicated-RHI and
  inline execution modes where recorded commands differ.
- [x] Run the ordinary native aggregate, full build, validation-enabled editor
  main/preview/offscreen matrix, resize and quality switching, shader reload,
  stable-frame loop, and orderly shutdown because the default affects every
  solid Lit editor viewport.
- [x] Capture same-process full/half/off timing distributions and exact active/
  retained bytes using the frozen protocol; record median, p95, relative saving,
  adapter, driver, profile, extent, sample count, and whether editor assistance
  and FXAA were enabled.
- [x] Make HalfResolution the production default only if all image,
  non-interference, lifecycle, memory, absolute, and relative timing gates pass.
  Otherwise retain FullResolution or Off as policy and record the failed gate;
  mere implementation availability does not complete rollout.
- [x] Update the lasting GTAO runtime contract, viewport rendering ownership,
  and user-facing View-menu guidance; preserve the archived original plan as
  historical full-resolution evidence.

#### Acceptance Gate

- Controlled evidence supports the selected default, all required validation is
  clean, lasting documentation describes the shipped quality/mapping/resolve/
  failure contract, and no unresolved image or performance gate remains.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Extent mapping | Even, odd, minimum, nonzero-origin, constrained, and edge rectangles map by the frozen half-open integer rule | Pure Renderer tests |
| Representative selection | Nearest valid standard-Lit candidate wins deterministically; invalid/background-only blocks publish white plus invalid selector | CPU/shader parity fixture |
| Raw horizon | Half raw preserves radius, falloff, thickness, normal ownership, finite output, and deterministic noise | GTAO qualification fixture |
| Bilateral filtering | Selector-directed reduced taps retain center and reject depth, normal, viewport, and validity discontinuities | CPU/GPU filtered readback |
| Full resolve | Four-candidate edge-aware resolve preserves silhouettes and returns white when no candidate matches | Mixed-depth and thin-geometry image matrix |
| Composition | Only deferred environment lighting receives material AO times resolved GTAO | HDR/non-interference fixtures |
| Quality switching | Off, HalfResolution, and FullResolution alternate across views and extents without stale resources or policy leakage | Renderer lifecycle tests |
| Optional failure | Every half target, selector, shader, PSO, filter, or resolve failure binds factor one for only that view | Failure-injection tests |
| Memory/cache | Exact active and retained bytes, mixed-quality keys, eviction ceiling, replacement, and recorded lifetime remain bounded | Layout/resource tests |
| Diagnostics | Raw/Filtered expose native half data; Confidence exposes resolve rejection; FinalFactor shows resolved composition | Debug capture tests |
| Performance | Same-process half median meets frozen absolute gates and is at most 65% of full median; p95 and total frame meet frozen gates | Public RHI GPU timing qualification |
| Runtime | Main, preview, auxiliary, thumbnail, Present/offscreen, resize, reload, stable frames, and shutdown remain validation-clean | Editor and Vulkan runtime evidence |

## Definition of Done

- Half-resolution GTAO reuses the established horizon algorithm, filters in the
  reduced domain, resolves one edge-aware full-resolution factor, and composes
  it only into deferred environment lighting.
- Full-resolution GTAO remains a selectable quality/reference route and Off
  remains an exact factor-one route.
- Mixed-depth blocks, silhouettes, thin geometry, background, viewport edges,
  odd origins/extents, projections, and all supported views meet frozen image
  and isolation gates without temporal state.
- Every optional failure, resize, quality change, reload, invalidation, cache
  replacement, recorded command, and shutdown path is bounded and stale-free.
- Exact memory statistics and controlled GPU timings meet the frozen gates and
  support the documented production default.
- Focused, aggregate, full-build, runtime, and documentation validation pass;
  lasting contracts are updated with completion evidence.

## Deferred Follow-ups

- A low-end SSAO fallback, reduced slice/step quality tier, dynamic quality, or
  automatic idle/interacting editor policy; each requires separate evidence.
- Temporal GTAO, checkerboard rendering, motion-vector reprojection, history
  rejection, camera-cut behavior, and temporal denoising.
- Compute-shader GTAO, async compute, wave/subgroup optimization, depth pyramids,
  a render graph, or transient aliasing.
- Bent normals, multi-bounce compensation, directional AO, GTSO, off-screen
  occluders, ray tracing, or distance-field AO.
- Persistent cross-machine GPU benchmark history or runtime autotuning.

## Related Documentation

- [Ground Truth Ambient Occlusion](../../../Runtime/Rendering/GroundTruthAmbientOcclusion.md)
- [Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Original Ground Truth Ambient Occlusion Plan](GroundTruthAmbientOcclusion.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Shaders/Slang/GroundTruthAmbientOcclusion.slang`
- `Engine/Shaders/Slang/DeferredDirectionalLighting.slang`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GroundTruthAmbientOcclusionRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GroundTruthAmbientOcclusionRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
