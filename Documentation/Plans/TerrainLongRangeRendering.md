# Terrain Long-Range Rendering Plan

Summary: Separate Terrain content visibility from the camera far plane, migrate main-scene depth to reversed Z, conceal the Terrain distance boundary, and retain stable precision while the camera rotates or moves through large worlds.

Last reviewed: 2026-08-14

Status: Active
Completed:

## Current Status

Stages 0 through 4 are implemented. Perspective scene views share a finite-far
reversed-Z builder and carry explicit clip/depth state; main depth clears to
`0` and uses `GreaterOrEqual`, while directional shadows remain forward-Z.
Runtime Camera settings and the Level Editor View menu expose the bounded clip
and Terrain distance policy.

Terrain uses horizontal closest-AABB distance with inner, transition, and
radial-rejected classifications before resource preparation. Opaque and masked
transition coverage uses deterministic 4x4 dithering. The 48-byte instance
payload carries double-prepared patch clip and camera-relative world anchors;
the Vulkan origin and `(10000000.25, -10000000.5, 0)` fixtures produce exact
matching readbacks.

Qualification passed the full Debug `all` build, `fast-all`, the Terrain
domain, renderer/Vulkan integrations, focused viewport/projection/render tests,
Terrain Vulkan and qualification targets, and the directional-shadow Vulkan
qualification on 2026-08-14. A hardware-backed EditorGrid regression now also
covers multiple rotated reversed-Z views, coplanar Terrain stability, and
occlusion by meaningfully closer Terrain. Remaining Stage 5 work is the named
editor/cooked runtime motion capture and its image-difference report; the
implementation and lasting contracts are otherwise in place.

## Goal

Render finite Terrain to a configurable long distance without exposing an empty
far-plane cut, without patch visibility flicker during slow camera rotation,
and without introducing main-scene depth instability at the supported distance
and coordinate envelopes.

## Scope

- Perspective editor and runtime main-scene views.
- One canonical camera projection builder and explicit near/far validation.
- Reversed-Z main-scene depth clear, compare, projection, reconstruction, and
  depth-sampling contracts.
- A Terrain-specific radial render distance independent of camera yaw, with a
  bounded transition region before rejection.
- Conservative patch selection across the transition region and deterministic
  behavior for invalid settings or views.
- Terrain GPU coordinate preparation that retains sub-pixel stability at the
  frozen large-coordinate fixture.
- Renderer and Terrain diagnostics for selected, transition, and rejected
  patches.
- Focused CPU tests, Vulkan render tests, editor/runtime motion captures, and
  performance characterization.

## Non-Goals

- Infinite procedural Terrain, world partition, heightmap streaming, or virtual
  texturing.
- Increasing the existing 1025x1025 heightmap render ceiling.
- Changing Terrain geometric-error LOD thresholds, adjacency resolution,
  stitching topology, collision, or authoring coordinates.
- Reversing directional-shadow depth or changing cascade distribution and
  shadow compare sampling in this plan.
- Adding temporal anti-aliasing solely to hide the distance boundary.
- Using a larger far clip as the content-visibility policy.
- Guaranteeing that a finite Terrain asset covers the horizon when the camera
  can physically see beyond its authored XY extent.

## Design Decisions and Invariants

### Projection and content distance are separate

- `NearClip` and `FarClip` define the valid projection interval. Terrain render
  distance defines content policy and must be strictly inside `FarClip` by a
  frozen safety margin.
- Terrain distance classification is radial about `ViewLocation` and therefore
  invariant under pure camera rotation. Stage 0 freezes whether the canonical
  metric is full 3D distance or horizontal XY distance; all CPU and shader paths
  use the same selected metric.
- Patch bounds, not patch centers, determine classification. A patch is rejected
  only when its closest conservative bound distance is beyond the transition
  end. A patch intersecting the transition remains submitted.
- The ordinary view frustum still rejects patches wholly outside the visible
  screen. No Terrain submission survives merely because it is inside the radial
  render distance.

### Main-scene depth uses one explicit convention

- Perspective editor and runtime camera views migrate together to reversed Z.
  Their main-scene depth target clears to `0`, opaque/masked geometry compares
  with `Greater` or the exact equality-inclusive variant frozen in Stage 0, and
  near depth remains greater than far depth.
- The projection convention is carried explicitly in `FSceneView`; consumers
  do not infer it from arbitrary matrix elements.
- Orthographic shadow views retain their existing forward-Z contract. Main-view
  and shadow depth state may not share an ambiguous default initializer.
- This plan retains a finite projection far plane because CPU frustum extraction
  and content bounds already require one. Infinite-far projection is deferred
  unless Stage 0 proves it necessary for a frozen acceptance gate.
- CPU visibility and projected-size math remain double precision. RHI depth
  images remain the existing supported floating-point/depth format; the plan
  changes mapping and comparison rather than allocating a larger format.

### The distance boundary is concealed, not merely displaced

- The transition begins at `FadeStart` and finishes at `RenderDistance`, with
  `0 <= FadeStart < RenderDistance < FarClip` after validation.
- Stage 0 selects one bounded concealment path against a frozen image sequence:
  stable coverage dithering, atmospheric/horizon blending, or an already
  available equivalent. The selected path must not require TAA for correctness
  and may not silently turn opaque Terrain into order-dependent translucency.
- The transition value is continuous and derived from the same conservative
  distance convention used by CPU selection. Rejected patches are already
  visually indistinguishable from the selected horizon/background within the
  qualification tolerance.
- Invalid or non-finite distance settings use documented bounded defaults and
  increment diagnostics. They never disable all Terrain or expand visibility
  without a ceiling.

### Stability does not depend on frame history

- Pure camera rotation with a fixed location produces the same radial distance
  classification and transition values. Frustum membership may change only as
  geometry enters or leaves the screen.
- The first implementation uses conservative bounds and a transition band, not
  retained per-view patch hysteresis. This preserves the existing submission-
  local and deterministic Terrain contract.
- Any later temporal hysteresis requires a separate plan with explicit view
  identity, reset, multi-viewport, memory, and render-thread ownership rules.

### Large-coordinate precision is camera relative at the GPU boundary

- World, component, patch bounds, and visibility stay double precision on CPU.
- Terrain vertex submission must avoid converting the sum of a large world
  origin and large sample-space offset to float before camera-relative
  cancellation. Stage 2 freezes and implements the smallest representation that
  passes the large-coordinate fixture, such as a per-instance patch anchor plus
  patch-local sample coordinates.
- Camera-relative rendering may not change canonical height texel selection,
  UVs, normals, shared topology keys, patch identity, collision, or authored
  world bounds.
- StaticMesh and SkeletalMesh large-world migration is deferred unless the
  shared projection change exposes a regression in their required baseline.

## Current Foundations and Gaps

| Area | Foundation | Gap |
| --- | --- | --- |
| Camera projection | Editor and runtime construct finite perspective matrices; Camera exposes reflected near/far values | Duplicated builders, mismatched defaults, forward-Z depth loss, no explicit depth convention |
| CPU visibility | Double-precision six-plane frustum extraction and conservative AABB classification | Far plane doubles as a hard rotating content boundary |
| Terrain selection | Per-view 64x64 patch classification, deterministic LOD, adjacency, and counters | No radial render distance, transition band, or boundary concealment |
| GPU depth | RHI supports greater/greater-equal compare operations | Main-scene pipelines and clears assume forward Z; consumers need an audited migration |
| GPU position | CPU matrices are double and converted only at shader upload | Terrain sample-derived local XY and final float transform have no qualified large-coordinate envelope |
| Validation | Terrain CPU/Vulkan tests and renderer scene-view tests exist | No slow-yaw sequence, far-boundary image gate, reversed-Z depth oracle, or large-coordinate Terrain fixture |

## Implementation Stages

### Stage 0: Freeze semantics, fixtures, and migration inventory

- [ ] Capture editor and runtime reproductions showing the current hard far-plane
  cut and slow-yaw edge instability with exact Terrain dimensions, spacing,
  transform, camera transform, viewport, and near/far values.
- [x] Freeze supported default and maximum values for editor/runtime near clip,
  projection far clip, Terrain render distance, transition start, and required
  far-plane safety margin.
- [x] Select 3D or horizontal radial distance and specify exact AABB closest-
  distance math, equality behavior, invalid fallback, and partial-patch rules.
- [x] Select and record the distance-boundary concealment path using still images
  and a slow-yaw sequence; reject paths that require unplanned temporal state or
  translucent sorting.
- [x] Freeze a reversed-Z matrix oracle for representative perspective views,
  including near, midpoint, render-distance, and far depth values.
- [x] Inventory every main-scene depth clear, compare, resolve, reconstruct,
  unproject, sky, editor-grid, picking, overlay, and test-matrix consumer; classify
  shadow-only consumers as intentionally forward Z.
- [ ] Measure Terrain screen-space vertex movement at the origin, at the maximum
  supported world coordinate, and with the maximum supported sample spacing;
  freeze the camera-relative precision fixture and pixel-error limit.
- [x] Record baseline CPU preparation, Terrain hardware draws, selected triangles,
  and GPU frame time for near, far, and transition views.

#### Acceptance Gate

- Reproduction assets and numeric fixtures deterministically expose the current
  defects.
- Distance semantics, concealment technique, reversed-Z convention, supported
  coordinate envelope, and objective image/motion tolerances are unambiguous.
- Every known main-scene depth consumer has a selected migration or an explicit
  evidence-backed exemption.

### Stage 1: Centralize view projection and migrate main-scene depth

- [x] Add one tested perspective projection builder supporting the frozen
  reversed-Z finite-far convention and use it from runtime Camera and the level
  editor viewport.
- [x] Carry explicit depth convention and validated clip distances in
  `FSceneView`; reject inconsistent matrices/settings at view construction.
- [x] Change main-scene depth attachment clearing and geometry pipeline compare
  operations atomically so no intermediate path mixes forward and reversed Z.
- [x] Migrate StaticMesh, SkeletalMesh, SplineMesh, Terrain, sky/background,
  editor grid, overlays, picking/unprojection, and any depth-reading post process
  identified in Stage 0.
- [x] Retain forward-Z directional-shadow projection, clear, raster comparison,
  sampling comparison, and bias behavior behind explicit shadow-view state.
- [x] Update frustum extraction and projected-size math only where the reversed-Z
  matrix changes plane extraction; retain conservative near/far equality rules.
- [x] Replace copied test projection matrices with shared builders or clearly
  named local forward/reversed fixtures as ownership requires.

#### Acceptance Gate

- CPU projection tests match every frozen depth oracle and round-trip world/
  screen projection within tolerance.
- Near and far geometry depth-test correctly in main-scene Vulkan images with no
  ordering regression across supported primitive families.
- Frustum visibility, Terrain LOD selection, editor picking, sky, grid, overlays,
  and directional shadows pass their focused existing tests.
- Raising the projection far clip to the frozen value introduces no visible
  Z-fighting in the qualification scene.

### Stage 2: Make Terrain GPU positioning camera relative

- [x] Split each Terrain patch position into a double-precision CPU anchor and
  bounded patch-local sample offset before conversion to shader floats.
- [x] Extend the existing instanced origin payload or transform binding with the
  minimum camera-relative data needed by the selected representation.
- [x] Reconstruct clip position without forming an avoidably large float world
  position; preserve exact integer height texel loads and sample identity.
- [x] Keep homogeneous direct-instancing and its 256-instance chunk ceiling;
  account for any increased instance bytes in diagnostics and qualification.
- [ ] Add CPU representation tests and Vulkan images at the frozen origin and
  large-coordinate fixtures, including slow yaw and sub-pixel camera movement.

#### Acceptance Gate

- Origin and camera-relative paths produce equivalent Terrain geometry, UVs,
  normals, LOD, stitching, material results, and picking/collision bounds.
- The large-coordinate motion sequence stays within the frozen pixel-error and
  frame-to-frame stability limits.
- No NaN/Inf, height-texel disagreement, topology-cache expansion, scalar draw
  fallback, or unbounded retained allocation is introduced.

### Stage 3: Add radial Terrain distance selection and boundary concealment

- [x] Add validated Terrain view-distance settings at the selected ownership
  boundary and expose the editor/runtime controls frozen in Stage 0.
- [x] Classify patch conservative world bounds as inner, transition, or rejected
  using radial distance before resource preparation, while retaining ordinary
  screen-frustum rejection.
- [x] Compute and upload the selected continuous transition value without adding
  per-frame retained Terrain state.
- [x] Implement the frozen concealment path for opaque and masked Terrain with
  defined material interaction and deterministic equality behavior.
- [x] Keep LOD adjacency inputs complete for culled neighbors so distance
  rejection cannot reopen Terrain cracks at the surviving boundary.
- [x] Add counters for inner, transition, radial-rejected, frustum-rejected,
  invalid-setting fallback, and concealed logical patches; reconcile totals.
- [x] Make the editor grid and any horizon/background parameters agree with the
  selected distance transition where the Stage 0 visual contract requires it.

#### Acceptance Gate

- With a fixed camera location, a full yaw sweep does not change radial patch
  classification, and visible edge changes are limited to screen-frustum entry
  and exit.
- Slow-yaw and forward-motion sequences expose neither an empty far-plane strip
  nor one-frame patch flashing above the frozen image-difference tolerance.
- Near and transition geometry remain crack-free across mixed LOD, partial edge
  patches, mirrored transforms, and positive/negative height scale.
- Counters conserve all candidates and rejected patches perform no hidden GPU
  resource preparation or draw.

### Stage 4: Integrate settings, diagnostics, and editor/runtime parity

- [x] Persist runtime Camera clip settings and the selected Terrain distance
  policy through the ordinary reflected serialization path.
- [x] Give the level editor explicit bounded controls or project defaults rather
  than another private hard-coded far clip.
- [x] Present effective near/far, Terrain render distance, transition range,
  reversed-Z convention, and fallback counters in existing renderer/viewport
  diagnostics at bounded cost.
- [x] Verify multiple editor viewports and auxiliary views do not share mutable
  distance or projection state.
- [x] Define compatibility behavior for levels and Cameras serialized before the
  new defaults, including whether old explicit `FarClip` values are preserved.

#### Acceptance Gate

- Reloaded editor and cooked runtime views produce the same effective settings
  and Terrain classification for the same serialized content.
- Invalid edits are clamped or rejected with actionable diagnostics and never
  publish a partially inconsistent view.
- Multiple views with different distances and clip ranges render independently
  in one frame.

### Stage 5: Qualification, lasting contracts, and rollout

- [x] Run focused projection, scene visibility, Terrain primitive, render-
  resource, Vulkan, viewport interaction/projection, sky, grid, picking, and
  directional-shadow tests according to the repository testing workflow.
- [ ] Run the Terrain domain and Renderer/RHI suites selected by test metadata,
  then the required editor and cooked-game smoke paths.
- [x] Re-measure the Stage 0 performance fixtures and enforce bounded regression
  gates for CPU preparation, instance bytes, hardware draws, GPU time, and
  pipeline/cache growth.
- [ ] Record qualification adapter, build configuration, resolution, warm-up,
  sample count, image/motion results, and known platform limits.
- [x] Publish lasting projection/depth behavior in Viewport Rendering and lasting
  Terrain distance/precision behavior in Terrain Rendering; update Camera/editor
  workflow documentation where controls become user-visible.
- [x] Remove temporary comparison switches and captures that are not part of a
  bounded diagnostic contract.

#### Acceptance Gate

- All required focused, domain, Vulkan, editor, Cook, and game validation passes
  on the named profile.
- The frozen defect scenes meet the distance, stability, depth, precision, and
  performance gates in editor and runtime.
- Lasting behavior is documented outside this plan and no obsolete forward-Z or
  hard-coded Terrain visibility assumption remains in active documentation.

## Validation Matrix

| Area | Required cases | Evidence |
| --- | --- | --- |
| Projection | finite reversed-Z perspective; near/mid/render/far depths; invalid ranges; round trip | CPU matrix and scene-view tests |
| Depth ordering | overlapping near/far opaque and masked primitives; clear value; equality policy | Vulkan offscreen images and RHI state assertions |
| Shadow isolation | all cascades, compare sampler, bias, caster culling | Existing shadow CPU/Vulkan baselines plus explicit convention assertions |
| Terrain distance | inner/transition/end equality/outside; rotated camera; transformed/mirrored bounds; partial patches | CPU preparation and counter-conservation tests |
| Boundary appearance | slow 360-degree yaw; forward/backward crossing; pitched horizon; wireframe diagnostic | Deterministic image sequence and bounded image-difference report |
| Precision | origin and frozen large world coordinate; maximum spacing; sub-pixel camera motion | Vertex/clip CPU oracle and Vulkan motion sequence |
| LOD and cracks | flat/coarse, mixed 2:1, all stitch masks, distance-rejected neighbor, negative height scale | Existing Terrain topology/render tests plus new boundary fixtures |
| Editor/runtime parity | default and custom settings, save/reload, multiple views, Cook/Game | Editor automation and runtime smoke evidence |
| Performance | near, transition, far, rejected, 256-instance chunk boundary | Renderer counters, Tracy CPU scopes, and GPU timing |

## Definition of Done

- Projection far distance is configurable and no longer the visible Terrain
  boundary under valid settings.
- Main-scene reversed Z is explicit, consistently applied, and isolated from
  forward-Z shadow depth.
- Terrain uses deterministic radial distance selection with a qualified
  transition that hides rejection against the horizon/background.
- Pure camera rotation cannot make radial Terrain patches oscillate between
  selected and rejected states.
- Terrain remains visually stable within the frozen large-coordinate envelope
  and preserves exact height sampling, LOD, stitching, material, collision, and
  instancing contracts.
- Editor and runtime defaults, controls, serialization, diagnostics, and Cook
  behavior are documented and validated.
- Required tests and measured qualification gates pass, lasting contracts are
  updated, and all plan checklists reflect recorded evidence.

## Deferred Follow-ups

- Infinite-far reversed-Z projection if finite-far reversed Z does not satisfy a
  measured supported-distance gate.
- General camera-relative transforms for StaticMesh, SkeletalMesh, splines,
  lights, and editor overlays beyond regressions required by this plan.
- Atmospheric scattering, volumetric fog, TAA, occlusion/HZB, GPU-selected
  Terrain LOD, heightmap streaming, world partition, and origin rebasing.
- Temporal patch hysteresis with explicit multi-view state ownership.
- Automatic generation or streaming of Terrain beyond the finite authored XY
  extent.

## Related Documentation

- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Editor Grid](../Runtime/Rendering/EditorGrid.md)
- [Terrain Patch LOD Plan](TerrainPatchLOD.md)
- [Terrain Draw Submission Scalability Plan](TerrainDrawSubmissionScalability.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/CameraComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/CameraComponent.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/RenderCore/Private/SceneViewProjection.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ViewPreparationMath.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ViewPreparationMath.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/SkyBoxRendering.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderPrimitiveTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportProjectionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
