# Scene Post-Processing and Editor Assistance Boundary Plan

Last reviewed: 2026-07-23

## Current Status

Stage 3 is complete. Opaque scene rendering now ends before scene post-processing, and the complete editor-assistance phase is prepared and composed afterward using preserved scene depth and output-specific pipelines. The final pass restores the constrained content viewport and scissor, retains grid/X-Ray/visible ordering, and owns the Present or ShaderReadOnly transition. Obsolete scene-layout assistance pipelines and pre-FXAA draw calls are removed.

## Goal

Establish a renderer-level boundary where scene anti-aliasing and other scene post-processing complete before depth-aware editor assistance is composed, so the editor grid and overlays remain crisp, retain correct scene occlusion, and stay outside future temporal history and motion-vector contracts in both window-backed and offscreen viewports.

## Scope

- Separate scene rendering, scene post-processing, editor-assistance composition, and final output responsibilities in Renderer code and naming.
- Preserve scene depth across the scene pass and make it available to the editor-assistance phase as a depth attachment.
- Move the editor world grid, overlay lines, overlay icons, and gizmo primitives after FXAA or the post-process copy path as one coherent phase.
- Preserve the existing order within editor assistance: world grid first, followed by X-Ray and visible overlay variants with their current relative visibility behavior.
- Support both presentable window backbuffers and shader-readable offscreen viewport targets.
- Keep editor assistance out of scene anti-aliasing input, future temporal history, and motion-vector production.
- Add focused layout and phase-contract coverage where it can be tested without visual capture, then validate the real Vulkan editor and the FXAA visual cases.
- Update long-lived architecture and grid reference documentation to describe the landed implementation.

## Non-Goals

- Implementing TAA, motion-vector generation, temporal history allocation, or camera-jitter policy.
- Replacing FXAA or changing its quality parameters.
- Redesigning grid LOD, line antialiasing, gizmo geometry, icon assets, selection visualization, or X-Ray styling.
- Moving application UI or ImGui into Renderer composition.
- Generalizing the renderer into a full render graph.
- Adding a new public RHI abstraction unless the existing attachment load/store and layout contracts prove insufficient.

## Design Decisions and Invariants

### Phase ownership

- Scene rendering produces scene-owned color and depth only. Editor-only visualization does not enter scene color before post-processing.
- Scene post-processing consumes scene color and writes the final scene image. FXAA and its disabled copy path occupy the same phase boundary.
- Editor assistance consumes the post-processed scene image as its color base and uses preserved scene depth for fixed-function depth testing.
- Presentation or offscreen shader-read transition happens only after editor assistance is complete.

### Editor-assistance membership and ordering

- `Editor World Grid`, `Overlay Lines`, `Overlay Icons`, and `Gizmo Primitives` are members of one editor-assistance phase even though they retain separate shaders and pipelines.
- The grid renders before other editor assistance.
- Existing X-Ray variants render before depth-tested visible variants so visible portions retain their current emphasis.
- Assistance pipelines keep alpha blending enabled where currently required, depth writes disabled, and depth testing enabled or disabled according to their existing visible/X-Ray semantics.

### Depth lifetime

- The opaque scene pass stores its depth attachment instead of discarding it.
- The editor-assistance phase loads the preserved depth without clearing or modifying it.
- Scene depth remains a depth attachment for this migration; manual depth sampling in every assistance shader is not the selected design.
- The final assistance phase may discard depth after composition when no later renderer phase requires it.

### Output variants

- Window-backed output ends in the present layout and access state.
- Offscreen editor and auxiliary viewport output ends shader-readable for Mona composition.
- Render-target layouts and graphics pipelines explicitly cover both final-output variants; no pipeline is used with an incompatible render-pass layout.
- Letterboxed view rectangles keep the existing viewport and scissor behavior. Full-target clearing and black bars remain output responsibilities rather than editor-assistance geometry.

### Future temporal anti-aliasing contract

- A future TAA phase belongs to scene post-processing and executes before editor assistance.
- Editor assistance does not write motion vectors and is not sampled into temporal history.
- Scene depth, motion vectors, jittered transforms, and history validity remain scene-owned data even when editor assistance later uses scene depth for occlusion.

### Failure behavior

- Failure to create a required post-process or editor-assistance pipeline retains the existing logged, no-draw behavior; it must not fall back to feeding only part of the assistance set through scene FXAA.
- If preserved depth cannot be attached compatibly to a final output, implementation pauses for an explicit RHI decision rather than silently disabling assistance depth testing.

## Current Foundations and Gaps

### Existing foundations

- `FPostProcessRendererState::FSceneTargets` already owns per-size scene color and depth intermediates.
- RHI attachment layouts already describe load/store actions, initial/final layouts, and access states.
- VulkanRHI already maps those attachment contracts to Vulkan render passes and pipeline compatibility.
- FXAA and the disabled copy path already share fullscreen buffers and have present/offscreen pipeline variants.
- Grid, line, icon, and gizmo renderers already keep depth writes disabled and provide depth-tested or X-Ray pipeline variants as appropriate.
- `RenderView()` already owns the scene pass, final output pass, viewport dimensions, and present/offscreen distinction.

### Gaps to close

- `MakeSceneRenderTargetLayout()` currently discards scene depth at the end of the scene pass.
- `MakePostProcessRenderTargetLayout()` has no depth attachment and cannot host the current depth-tested assistance pipelines.
- All editor-assistance pipelines are created against the scene render-target layout rather than final present/offscreen layouts.
- `RenderScene()` mixes opaque scene drawing with editor-assistance preparation and drawing.
- FXAA currently consumes scene color after all editor assistance has already been blended into it.
- No focused test currently asserts the intended phase membership, depth lifetime, or present/offscreen final-layout contracts.

## Implementation Stages

### Stage 0: Establish testable phase and attachment contracts

- [x] Extract or expose narrowly scoped render-target layout builders so scene, scene-post-process, and final editor-assistance contracts can be tested without starting the editor.
- [x] Define names that distinguish scene targets from final viewport output and distinguish scene rendering from editor-assistance preparation and drawing. The selected phase names are scene rendering, scene post-process, editor-assistance preparation, and editor-assistance drawing/composition; Stage 1 introduces the latter two entry points.
- [x] Add focused tests for scene-depth store/load requirements and present versus offscreen final layouts.
- [x] Confirm through a minimal Vulkan pipeline/render-pass path that a final color target and preserved `D32` depth attachment are compatible for both output variants.
- [x] Record any RHI or Vulkan constraint discovered before migrating draw order. No new RHI abstraction is required; existing load/store, initial/final layout, and access contracts represent the boundary.

#### Acceptance Gate

- Focused tests identify incompatible load/store or final-layout changes, and the real backend accepts the selected final color-plus-depth render-target layouts without Pipeline or Vulkan Validation errors.

### Stage 1: Separate scene and editor-assistance submission

- [x] Refactor `RenderScene()` so it submits scene-owned geometry only.
- [x] Introduce an editor-assistance preparation step for dynamic overlay line and icon buffers without drawing them into scene color.
- [x] Introduce one editor-assistance draw step that preserves the existing grid, X-Ray, and visible ordering.
- [x] Keep this stage behaviorally neutral until the new final-output render pass and pipelines are ready.
- [x] Add focused source-level or renderer-helper coverage for phase membership and ordering where practical.

#### Acceptance Gate

- Opaque scene work and editor-assistance work have separate entry points, every existing assistance primitive belongs to the assistance entry point, and no visible ordering or resource-lifetime regression is introduced.

### Stage 2: Preserve depth and create final-composition pipelines

- [x] Store scene depth at the end of the opaque scene pass.
- [x] Define final-composition render-target layouts that load the appropriate color contents, load preserved scene depth, and finish in Present or ShaderReadOnly according to the output type.
- [x] Create present and offscreen compatible pipelines for the grid, overlay lines, overlay icons, and gizmo primitives.
- [x] Keep visible pipelines depth-tested, X-Ray pipelines depth-test disabled, and all assistance depth writes disabled.
- [x] Update renderer resource release and pipeline-failure paths for the expanded pipeline set.

#### Acceptance Gate

- All assistance pipelines are compatible with their actual final render passes, preserved scene depth is loaded without clearing, and focused plus real-backend validation reports no render-pass, pipeline, layout, or synchronization error.

### Stage 3: Move editor assistance after scene post-processing

- [x] Render opaque scene color and depth without editor assistance.
- [x] Execute FXAA or the post-process copy path using scene color only.
- [x] Compose the complete editor-assistance phase into the post-processed output while using preserved scene depth.
- [x] Preserve viewport/scissor behavior for constrained aspect ratios and both primary and auxiliary viewports.
- [x] Ensure final Present or ShaderReadOnly transitions happen after assistance composition.
- [x] Remove obsolete scene-layout assistance pipelines and old pre-FXAA draw calls.

#### Acceptance Gate

- FXAA toggling changes scene-edge treatment without visibly softening the grid, icons, lines, or gizmos; meshes still occlude depth-tested assistance; X-Ray variants and final output paths retain their previous semantics.

### Stage 4: Regression validation and documentation

- [ ] Run focused RenderCore, Renderer/Engine, grid, and viewport tests affected by layout and ordering changes.
- [ ] Complete the repository full `all` build through the root BuildTool workflow.
- [ ] Run `DurinEditor` from the same profile with `--hidden-window` and verify Shader, Pipeline, Vulkan Validation, Error, and Fatal logs remain clean.
- [ ] Manually compare FXAA enabled and disabled in low-altitude grid, horizon, mesh-occlusion, icon, selection-line, camera-frustum, and transform-gizmo scenarios.
- [ ] Validate both an offscreen Level Editor viewport and a window-backed runtime viewport.
- [ ] Update Runtime Architecture, Viewport Rendering, and Editor World Grid reference documents to describe the landed boundary and remove migration-pending wording.

#### Acceptance Gate

- Automated checks, full build, hidden-window Vulkan smoke, and the visual matrix pass; long-lived documents describe the implemented ordering and temporal exclusion contract rather than the former pre-FXAA integration.

## Validation Matrix

| Scenario | Expected Result | Evidence |
| --- | --- | --- |
| FXAA disabled | Scene copy completes before crisp editor assistance | Editor capture |
| FXAA enabled | Scene edges receive FXAA while grid and overlays retain their local sharpness | Matched editor capture |
| Low camera near grid | Fine grid lines remain stable without FXAA softness or new aliasing | Slow camera recording |
| Horizon view | Existing angle and distance fades remain smooth | FXAA on/off recording |
| Mesh over grid | Mesh depth occludes grid after post-processing | Editor capture |
| Visible and X-Ray overlays | Existing two-pass emphasis and occlusion behavior remain intact | Selection and camera overlay captures |
| Transform gizmo | Handles remain crisp and retain visible/X-Ray ordering | Hover and drag captures |
| Overlay icons | Icons remain crisp and their visible/X-Ray variants preserve depth behavior | Light and camera icon captures |
| Fixed-aspect viewport | Content viewport and black bars remain correctly scoped | Camera-preview capture |
| Auxiliary viewport | Main and preview targets do not cross-use color or depth intermediates | Simultaneous viewport capture |
| Window-backed output | Final assistance composition transitions to Present | Vulkan smoke and log |
| Offscreen output | Final assistance composition transitions to ShaderReadOnly for Mona | Level Editor smoke and log |
| Future temporal contract | Assistance phase has no motion-vector or history producer/consumer binding | Code review and focused contract test |

## Definition of Done

- Scene rendering, scene post-processing, editor assistance, and output transitions are separate named responsibilities in Renderer code.
- FXAA and the disabled copy path consume scene color without editor-only visualization.
- Grid, lines, icons, and gizmos render after scene post-processing as one assistance phase.
- Preserved scene depth continues to occlude depth-tested assistance without being modified by it.
- Present and offscreen targets use compatible render-target layouts and finish in their required states.
- Editor assistance is explicitly excluded from future temporal history and motion-vector contracts.
- Focused tests, full build, Vulkan editor smoke, and the visual validation matrix pass.
- Architecture and reference documents describe the landed behavior, and the archived V2 plan remains available as evolution history.

## Deferred Follow-ups

- TAA implementation, jitter policy, motion-vector targets, reactive masks, and history invalidation.
- A render graph or generalized pass scheduler if renderer complexity later justifies it.
- Scripted camera paths and automated image capture for renderer visual regressions.
- Independent editor-assistance render resolution or compositing into HDR scene output.
- Profiling whether a combined final pass or separate post-process and assistance passes is preferable after correctness is established.

## Related Documentation

- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Architecture/ViewportRendering.md`
- `Documentation/Reference/EditorWorldGridShader.md`
- `Documentation/Plans/Archive/EditorWorldGridV2.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderPass.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Programs/Tests/RenderCoreTests/Private/RenderTargetLayoutTests.cpp`
- `Engine/Source/Programs/Tests/EngineTests/Private/EditorGridRenderingTests.cpp`
