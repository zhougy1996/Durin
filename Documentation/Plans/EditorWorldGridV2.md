# Editor World Grid V2 Plan

Last reviewed: 2026-07-23

## Current Status

Stages 0 and 1 are implemented and validated. The fragment-depth contract passes Slang compilation and the real Vulkan grid pipeline. The renderer now uploads validated, mutually inverse `WorldToClip` and `ClipToWorld` transforms through the V2 uniform layout and skips the grid draw before uniform allocation when the view-projection transform is invalid. The existing world-space grid triangle remains the baseline until the V2 path passes the validation matrix.

## Goal

Replace the Level Editor's camera-following finite world-space grid triangle with a screen-space infinite-grid renderer that remains locked to world coordinates, preserves scene depth occlusion and adaptive grid density, and does not exhibit geometry or LOD jumps when the camera rotates close to the grid plane.

## Scope

- Replace the current vertex-stage construction of a large `Z = Height` world triangle with a true fullscreen triangle.
- Reconstruct near and far world positions from clip coordinates in the fragment shader.
- Intersect each view ray with the horizontal editor grid plane and reject rays that do not reach a visible intersection.
- Project the intersection back to clip space and output its depth so existing scene geometry continues to occlude the grid.
- Retain distance fading, grazing-angle fading, derivative-based antialiasing, decimal LOD blending, world axes, alpha blending, and depth writes disabled.
- Add focused shader-compilation coverage for fragment depth output and update existing view-publication tests for the V2 uniform contract where appropriate.
- Validate the result in the real Vulkan editor at low altitude, near the near plane, above and below the grid, and across ordinary overview distances.
- Update the long-lived grid reference after implementation details are verified.

## Non-Goals

- Orthographic editor viewports.
- Arbitrarily rotated construction planes or local-coordinate grids.
- User-configurable base units, `1/2/5` engineering scales, or visual grid snapping integration.
- Large-world origin rebasing or double-precision shader reconstruction.
- Changing scene depth conventions, the camera projection convention, FXAA, or general post-processing.
- Replacing the existing decimal LOD appearance unless V2 validation exposes a separate defect in it.

## Design Decisions and Invariants

### Screen-space coverage

- The vertex shader emits the shared fullscreen triangle directly in clip space and forwards its clip-space XY coordinates to the fragment shader.
- The vertex shader does not construct or project grid geometry.
- The renderer continues to issue one indexed draw with the shared post-process triangle buffers.

### Ray reconstruction

- The renderer supplies both `WorldToClip` and `ClipToWorld`; `ClipToWorld` is the inverse of the current view-projection matrix computed on the CPU before conversion to the shader matrix layout.
- The fragment shader unprojects the interpolated clip XY at NDC depth `0` and `1`, performs homogeneous division, and forms the ray from the near point toward the far point. Using near and far points keeps the contract compatible with a future orthographic projection even though orthographic viewports are not part of this plan.
- The selected work plane remains `Z = Grid.Height`. V2 does not generalize grid line coordinates to arbitrary plane bases.
- Rays whose vertical direction is within a small epsilon of parallel, whose intersection lies behind the near point, or whose projected intersection depth lies outside the active `0..1` depth range produce no grid fragment.
- Intersection distance remains bounded by the existing fade distance, which stays below the editor far clip distance.

### Depth and render ordering

- The fragment shader projects the world intersection through `WorldToClip` and outputs `clip.z / clip.w` through `SV_Depth`.
- The grid pipeline keeps depth testing enabled and depth writes disabled. The shader-provided grid depth participates in the normal scene depth test without contaminating the depth attachment.
- Static meshes render before the grid; editor overlay lines, icons, and gizmos render after it.
- Stage 0 must prove that the Slang-to-SPIR-V path, Vulkan RHI pipeline, and current render-pass layout accept a color output paired with `SV_Depth`. If this contract fails, implementation pauses for a documented renderer-level decision rather than silently switching to manual scene-depth sampling.

### Stability and appearance

- World-space derivatives are evaluated from the reconstructed continuous intersection position before selecting a discrete decimal spacing.
- Discrete LOD spacing never participates in `ddx`, `ddy`, or `fwidth` input expressions.
- Grazing-angle fade is derived directly from the ray direction's vertical component. This is equivalent to the current hit-to-camera elevation ratio for a perspective view and avoids dividing by an increasingly large intersection distance near the horizon.
- Nearly parallel and beyond-range intersections fade out before hard rejection wherever a finite transition is available; invalid or non-finite intersections are still discarded.
- Minor, major, X-axis, and Y-axis colors retain their current semantic roles and blend ordering.
- The grid remains world-locked: rotating the camera may change projection and selected screen-space density, but it must not translate the underlying world grid.

### Failure behavior

- If the view-projection matrix is non-finite or non-invertible, the renderer skips the grid draw for that view rather than uploading an invalid inverse.
- Shader or pipeline creation failure retains the existing logged failure behavior and produces no grid draw.
- V2 does not retain a runtime toggle between the old and new algorithms after acceptance. Source control is the rollback path during implementation.

## Current Foundations and Gaps

### Existing foundations

- `FSceneView` already publishes the view-projection matrix, camera location, viewport dimensions, and editor-grid settings.
- Renderer code already owns a dedicated grid shader map, pipeline, uniform buffer, and one-draw submission path.
- The shared post-process triangle is already bound by the grid draw.
- The current shader already contains the required derivative-safe antialiasing, three-level decimal LOD blend, distance fade, axis rendering, and grazing-angle fade concepts.
- `SceneViewProjection::BuildViewportRay()` documents and tests the engine's NDC depth convention: near depth `0`, far depth `1`.

### Gaps to close

- The current vertex shader reinterprets fullscreen vertices as a large camera-following world triangle, so it is still clipped as finite geometry.
- The current uniform has no `ClipToWorld` matrix.
- No checked-in shader currently demonstrates fragment depth output through `SV_Depth`.
- The current grazing-angle calculation uses the vector from each world fragment to the camera and is coupled to the finite geometry path.
- Runtime validation does not yet cover camera heights at, below, and just above the editor near clip distance.

## Implementation Stages

### Stage 0: Prove the fragment-depth contract

- [x] Add or extend a RenderCore shader-compilation test with a minimal fragment entry point that returns color and `SV_Depth`.
- [x] Confirm reflection and pipeline layout generation do not treat the depth semantic as a descriptor or color attachment.
- [x] Confirm the Vulkan graphics pipeline accepts the grid fragment shader while depth testing is enabled and depth writes are disabled.
- [x] Record any backend constraint discovered by the spike in this plan before changing the grid algorithm. No backend constraint was discovered.

#### Acceptance Gate

- A focused native test compiles the depth-writing fragment shader successfully, and a minimal real-backend grid pipeline can be created without Shader, Pipeline, or Vulkan Validation errors.

### Stage 1: Establish the V2 uniform and matrix contract

- [x] Replace the finite-plane extent data in `FEditorGridUniform` with the data needed for reconstruction: `WorldToClip`, `ClipToWorld`, plane height, camera position, fade distance, and existing colors.
- [x] Compute and validate the inverse view-projection matrix in `DrawEditorGrid()` before allocating the dynamic uniform buffer.
- [x] Preserve CPU-to-shader matrix transposition conventions for both matrices.
- [x] Skip the grid draw when inversion would upload non-finite values.
- [x] Update focused tests or test helpers that assert the editor-grid view contract.

#### Acceptance Gate

- The renderer builds, the V2 uniform layout matches the Slang layout, valid editor views upload mutually inverse transforms within float tolerance, and invalid transforms do not issue a grid draw.

### Stage 2: Replace finite geometry with ray-plane reconstruction

- [ ] Emit the shared triangle positions directly as clip-space positions in `VertexMain` and forward clip XY for interpolation.
- [ ] Unproject NDC near and far positions with `ClipToWorld` in `FragmentMain`.
- [ ] Intersect the reconstructed ray with `Z = Height`, handling parallel, behind-near, non-finite, and outside-depth-range cases explicitly.
- [ ] Reproject the intersection through `WorldToClip` and return its normalized depth as `SV_Depth` alongside the grid color.
- [ ] Keep the draw count, alpha blending, culling, depth-test, and depth-write states unchanged unless Stage 0 proves a required backend adjustment.

#### Acceptance Gate

- The grid has no finite geometry edge, remains visible from above and below when looking toward the plane, disappears when the view ray cannot reach the plane, and is correctly occluded by static meshes.

### Stage 3: Port adaptive appearance and stabilize boundary behavior

- [ ] Drive distance fade from the reconstructed world hit to the camera position.
- [ ] Drive grazing-angle fade from the reconstructed ray direction and preserve a smooth transition before the parallel-ray rejection threshold.
- [ ] Compute world-position derivatives before decimal LOD selection and reuse them for line antialiasing.
- [ ] Port the existing three-level decimal LOD blend without changing its level-renaming continuity across decade boundaries.
- [ ] Port X/Y world axes and existing color priority.
- [ ] Ensure invalid lanes, horizon-adjacent lanes, and hard discard thresholds do not introduce a new quad-derivative seam.

#### Acceptance Gate

- Slow rotation at low altitude produces no grid translation, whole-region pop, sawtooth LOD boundary, or near-camera clipping wedge; the horizon fades smoothly without dense aliasing.

### Stage 4: Regression validation and documentation

- [ ] Run the focused RenderCore and Engine tests affected by the shader and view contract.
- [ ] Complete a full `all` build using the repository BuildTool workflow.
- [ ] Run `DurinEditor` from that same build profile and verify `/Engine/EditorGrid` compiles without Shader, Pipeline, or Vulkan Validation errors.
- [ ] Perform the visual matrix below with FXAA enabled and disabled.
- [ ] Update `Documentation/Reference/EditorWorldGridShader.md` from planned V2 behavior to verified implementation details, removing descriptions that only apply to the finite triangle.
- [ ] Move any new long-lived renderer constraint into the appropriate Architecture document if implementation establishes one.

#### Acceptance Gate

- Automated checks pass, the full editor build succeeds, runtime logs are clean, every visual scenario passes, and the reference describes the landed implementation rather than the pre-V2 design.

## Validation Matrix

| Scenario | Expected Result | Evidence |
| --- | --- | --- |
| Camera height well above near clip | Grid appearance and density remain comparable to V1 | Side-by-side editor capture |
| Camera height just above `NearClip` | Slow pitch/yaw rotation has no clipping wedge or whole-region pop | Recorded low-altitude rotation |
| Camera height below `NearClip` but not on the plane | Only ray-valid intersections appear; no finite-geometry jump | Recorded rotation and dolly |
| Camera exactly on `Z = Height` | Parallel/degenerate intersections are rejected without NaN artifacts or validation errors | Editor capture and runtime log |
| Camera below the grid looking upward | Grid renders from the underside with correct world axes | Editor capture |
| View approaches the horizon | Grid fades before projected cells collapse into a noisy band | Slow pitch recording, FXAA on/off |
| Camera crosses a decimal LOD boundary | Fine and major lines cross-fade without scale or brightness discontinuity | Slow dolly/rotation recording |
| Mesh intersects or stands on grid | Mesh depth occludes grid; grid does not occlude later editor overlays | Editor capture |
| Camera crosses world origin | Red X and green Y axes remain fixed and correctly oriented | Editor capture |
| Extremely wide and narrow viewports | Reconstruction and derivatives remain stable across aspect ratios | Editor captures |
| Shader compilation | Color plus `SV_Depth` compiles and reflects correctly | Focused RenderCore test |
| Repository integration | Renderer, editor, and tests share one valid build profile | Full build, tests, hidden-window smoke test |

## Definition of Done

- The editor grid is generated from screen-space ray-plane intersections rather than finite world-space grid geometry.
- Rotating close to the grid plane does not cause the grid to jump, translate, expose a geometry edge, or abruptly change whole-region visibility.
- Grid fragments use the reconstructed plane depth and remain correctly occluded by scene geometry while leaving the depth attachment unchanged.
- Derivative-safe antialiasing and continuous decimal LOD transitions remain intact.
- Above-plane, below-plane, on-plane, horizon, origin, aspect-ratio, FXAA, and depth-occlusion scenarios pass.
- Focused tests, the full build, and the real Vulkan editor validation pass without Shader, Pipeline, or Validation errors.
- The reference document describes the verified V2 implementation and any long-lived renderer constraints are recorded outside this plan.

## Deferred Follow-ups

- Orthographic editor viewport reconstruction and density policy.
- Arbitrary plane normal plus a stable 2D grid basis.
- User-configurable visual units and `1/2/5` scale families.
- Visual grid and transform-snapping integration.
- Camera-relative or higher-precision reconstruction for large worlds.
- Performance profiling if fragment depth output materially changes early-depth behavior on supported GPUs.

## Related Documentation

- `Documentation/Reference/EditorWorldGridShader.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Architecture/RuntimeArchitecture.md`

## Related Code

- `Engine/Shaders/Slang/EditorGrid.slang`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/RenderCore/Public/IRendererModule.h`
- `Engine/Source/Runtime/RenderCore/Private/SceneViewProjection.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Programs/Tests/RenderCoreTests`
- `Engine/Source/Programs/Tests/EngineTests/Private/ViewportTests.cpp`
