# SkyBoxComponent Plan

Summary: Reflected skybox component ownership, cube-texture rendering, and editor workflow.

Last reviewed: 2026-07-26

## Current Status

All stages are complete. The implemented slice includes the documented face
convention, transactional six-face assets, revisioned render resources and
scene snapshots, fullscreen sky rendering, deterministic conflict selection,
and the editor import/create/assign/save/reload workflow.

Stage 6 validation is complete. A Vulkan pixel test reads back every
compressed face and mip, renders all six principal directions, verifies camera
translation invariance and component rotation, preserves fitted letterboxing,
and proves foreground geometry occludes the sky. The test also exercises
render-pass final-layout tracking and render-thread resource retirement with
Vulkan Validation enabled and no diagnostics. `EngineTests` passes 220/220 and
`RenderCoreTests` passes 48/48. A full `all` build and a 10-second
`DurinEditor --hidden-window` smoke from the same preset pass without logged
errors or validation messages.

The interactive visibility matrix passed on 2026-07-26 in the main editor
viewport, camera preview, embedded PIE viewport, and new game window across the
required aspect ratios and view modes. It revealed no orientation, parallax,
stretching, or draw-order errors. The apparent black bands between the
directionally labeled convention faces were traced to the outermost three black
pixels authored into each test image, not missing cube samples. Dedicated
cross-face edge processing remains deferred as planned. The plan was completed
and archived on 2026-07-26.

## Goal

Users can place a `DSkyBoxComponent` in a level, assign a `DTextureCube` asset built from six LDR images, and see a static sky background without translation parallax in the main editor viewport, camera previews, and game viewports. Camera rotation changes the viewing direction, component rotation adjusts the sky orientation, and scene geometry and editor overlays always cover the sky correctly.

## Scope

- A first-class `DTextureCube` asset and render-resource path.
- Import six equally sized, identically formatted square PNG/JPEG/BMP/TGA images as an RGBA8 cubemap.
- Create, upload all mips for all six faces, sample, and destroy cube textures in RHI and VulkanRHI.
- Reflection, serialization, registration, visibility, and scene updates for `DSkyBoxComponent`.
- An `ASkyBoxActor` that is easy to place in a level and owns a `DSkyBoxComponent` by default.
- A dedicated sky shader, parameters, pipeline state, and draw entry point in the renderer.
- A Content Browser workflow for importing or creating cube textures and assigning them in Details.
- Automated tests, validation against real Vulkan, and visible-result checks in DurinEditor.

## Non-Goals

The following capabilities must not enter the first version as incidental expansion:

- Sky lights, diffuse environment lighting, specular IBL, reflection probes, or scene capture.
- HDR/EXR source files, floating-point cube textures, exposure controls, or HDRI prefiltering.
- Automatic conversion of equirectangular panoramas to cubemaps.
- Sky Atmosphere, atmospheric-scattering LUTs, aerial perspective, volumetric clouds, fog, a sun disc, a moon, or animated stars.
- Skydome meshes, sky materials, or a general-purpose sky material graph.
- Cube texture arrays, volume textures, virtual textures, or texture streaming.
- Blending, transitions, spatial regions, priority systems, or independent per-view overrides for multiple skyboxes.
- An advanced asset previewer or dedicated seamless-edge filtering.

## Design Decisions and Invariants

### Resource Contract

- The first version uses a real `TextureCube`; it does not use equirectangular Texture2D sampling as an interim implementation.
- Face order is fixed as `+X, -X, +Y, -Y, +Z, -Z`. Asset UI and error messages must show names so users never depend on numeric indices.
- All six faces must be square, have identical dimensions, and decode to RGBA8. If any face fails, the entire import fails without saving a partially valid cube asset.
- The first version builds a full mip chain as an sRGB color texture. Each face may reuse the Texture2D color-mip rules, but the implementation does not claim to solve cross-face edge filtering.

### RHI Contract

- `TextureCube` represents one texture with six physical array layers. `CreateCube()` must produce a create description that satisfies this constraint. Public cube-array semantics are deferred for separate design.
- The 2D subresource upload contract must explicitly accept `MipIndex` and `ArraySlice`. Texture2D continues to use only slice 0, while cube textures use slices 0-5; VulkanRHI must not infer a face from call order.
- Vulkan images must use the cube-compatible flag, the image view must cover all six layers, and every mip/face must enter the correct layout before and after copying.
- Invalid dimensions, layer counts, mip indices, slice indices, and unsupported formats must fail near the public boundary rather than passing invalid descriptions to Vulkan.

### Component and Scene Contract

- `DSkyBoxComponent` derives from `DSceneComponent`. Its translation and scale have no rendering meaning; only world rotation affects the cubemap direction.
- The first-version properties are limited to `TextureCube`, `Tint`, `Intensity`, and inherited Transform/Actor Visibility. `Intensity` is nonnegative in linear space, and `Tint` is not used as sky-light input.
- The scene stores an `FSkyBoxSceneData` snapshot that the renderer can read safely instead of reading reflected objects during drawing. A cross-thread lifetime proxy keeps the asset render resource alive.
- At most one skybox is active per scene in the first version. If edited content contains multiple visible, registered `DSkyBoxComponent` instances, the scene selects the component with the smallest stable Component/Scene ID and reports the conflict in the editor. It must not use a load-order-dependent rule such as "last registered wins."
- Component registration, unregistration, actor visibility, transform rotation, texture assignment, and editable parameter commits must update the scene and reject stale queued commands.

### Renderer Contract

- The sky uses a fullscreen triangle and does not introduce a cube-mesh or skydome-mesh asset dependency.
- The sky is drawn in the Scene Color render pass, after setting the current view's viewport/scissor and before opaque geometry. It performs no depth test and writes no depth, so later geometry, the editor grid, and overlays naturally cover it.
- The shader reconstructs a world-space view ray from the view/projection data, removes camera translation, then applies the inverse component rotation to sample the cubemap. Camera translation must not change the sky image.
- The sky is visible in Lit, Unlit, and Wireframe view modes. This stage does not define a dedicated Sky view mode.
- If the asset cannot be resolved, the resource is not ready, or the resource is being replaced, use a renderer-owned 1x1 black fallback cubemap. If no component is active, preserve the existing Scene Color clear result.
- Decode the cubemap from sRGB to linear color, multiply it by `Tint * Intensity`, write it to Scene Color, and continue through the existing post process. Do not establish a second tone-mapping path for the skybox.

## Current Foundations and Gaps

| Layer | Reusable Foundation | Required First-Version Gap |
| --- | --- | --- |
| Image/Asset | `DTexture2D` RGBA8 decoding, mip generation, and asset-import pattern | Shared image-building logic, `DTextureCube` data, and transactional six-face import |
| Render Resource | Texture2D shared proxy, revisions, and render-thread upload | Cube platform data, six-layer upload, and a black fallback cube |
| RHI | `ETextureDimension::TextureCube` and create descriptions | Six-layer semantics, a slice-aware upload interface, and constraint validation |
| VulkanRHI | Cube image-view type mapping | Cube-compatible images and layer-range transitions, copies, and views |
| Component | `DSceneComponent` registration, transforms, and actor visibility | `DSkyBoxComponent`, `ASkyBoxActor`, and property-change notifications |
| Scene | Scene entry points for primitives and directional lights | Skybox IDs, snapshots, updates, conflict rules, and release handling |
| Renderer | Slang, texture/sampler binding, and Scene Color/Depth/Post Process | Skybox shader, pipeline, draw order, and resource fallback |
| Editor | Content Browser asset operations and reflected Details | Cube import UI, asset recognition, Actor/Component creation, and conflict diagnostics |

## Implementation Stages

### Stage 0: Lock Down Coordinate, Face, and Test Contracts

This stage produces no user-visible result. It prevents repeated flipping of the six faces' axes, orientation, and texture origin in later stages.

- [x] Document the mapping between Durin world coordinates, camera Forward/Up, and Vulkan cubemap faces.
- [x] Define the orientation, up direction, and required flipping for the six source images `+X, -X, +Y, -Y, +Z, -Z`.
- [x] Add a small, directionally unambiguous six-color cubemap with labeled edge markers to the test-data directory.
- [x] Define CPU direction-to-expected-face/UV cases as the ground-truth table for visual shader validation.
- [x] Put the final convention in cube-texture import errors and user documentation, not only in shader comments.

#### Acceptance Gate

- The documentation uniquely determines the face and orientation of all six source images without relying on rendered output.
- Each of the six principal-axis view directions has a defined expected color/face for later automated or manual validation.

### Stage 1: Complete the RHI and VulkanRHI Cube-Texture Foundation

Depends on Stage 0. This stage proves only that the GPU can correctly create, upload, and sample a six-layer texture; it does not introduce a UObject.

- [x] Make `FRHITextureCreateDesc::CreateCube()` establish the six-layer convention and validate that Width/Height are equal and nonzero.
- [x] Add an explicit `ArraySlice` to the RHI upload interface while preserving the behavior of existing Texture2D slice-0 call sites.
- [x] Validate mip/slice/region/source pitch at the public RHI boundary and provide actionable diagnostics for invalid calls.
- [x] Make Vulkan cube images include `eCubeCompatible`, with image views covering six layers from base layer 0.
- [x] Update Vulkan staging copies and layout transitions to operate only on the specified mip/slice without disrupting other uploaded faces.
- [x] Check descriptor/view dimension mapping so a shader-declared `TextureCube` never receives a 2D image view.
- [x] Add RHI unit tests for create descriptions and invalid subresources.
- [x] Add a minimal Vulkan smoke path for cube creation, six-layer upload, and sampling with validation enabled.

#### Acceptance Gate

- Existing Texture2D single-layer upload tests do not regress.
- All six faces and multiple mips can be uploaded independently and sampled through a cube view.
- Vulkan Validation reports no errors for image creation flags, subresource ranges, layouts, copies, or descriptor dimensions.

### Stage 2: Implement the DTextureCube Asset and Render Resource

Depends on Stage 1. This stage completes the non-editor path from six source images to a cube RHI resource usable by the render thread.

- [x] Extract reusable RGBA8 decoding, dimension limits, and color-mip generation from Texture2D into a shared Engine image-building utility instead of duplicating codec selection and error handling.
- [x] Define `FTextureCubeSourceData`, `FTextureCubePlatformData`, and per-face/per-mip storage with an explicit face enum.
- [x] Implement six source-file references, reflection/serialization, `PostLoad` rebuilding, and error reporting for `DTextureCube`.
- [x] Implement atomic import: validate all six faces before creating/saving the asset and copying source files; clean up all artifacts from the attempt if any step fails.
- [x] Generate a full mip chain for all six faces and validate every level's dimensions, row pitch, data length, and pixel format.
- [x] Implement an `FTextureCubeRenderResource` shared-lifetime proxy so build/rebuild/release accesses the RHI only on the render thread.
- [x] Add revision rejection for rapid consecutive rebuilds so stale commands cannot overwrite a newer cube resource.
- [x] Ensure cube asset references are serialized, dependency-tracked, and retained correctly by GC, with no dangling references after assets are deleted or moved.
- [x] Add tests for import, dimension mismatch, nonsquare inputs, missing faces, reload, serialization, move, delete, and stale revisions.

#### Acceptance Gate

- Six valid faces can be imported, saved, reloaded after restart, and rebuilt as a complete cube RHI resource.
- Invalid imports leave no partial package/source files, and errors identify the specific face and cause.
- Asset rebuilding and destruction never involve game-thread access to the RHI or queued commands using a destroyed UObject.

### Stage 3: Implement DSkyBoxComponent and the Scene Snapshot

Depends on Stage 2. This stage establishes the data boundary between game objects and the renderer scene, initially without drawing.

- [x] Add the reflected class `DSkyBoxComponent : DSceneComponent` with a `DTextureCube` reference, Tint, and Intensity.
- [x] Assign the component a stable scene ID and define the minimal rendering snapshot in `FSkyBoxSceneData`: ID, cube render-resource proxy, rotation, Tint, Intensity, and revision.
- [x] Extend `IScene`/`FScene` with add-or-replace, remove, and active-sky queries while mutating the underlying container only on the render thread.
- [x] Implement component handling for `OnRegister`, `OnUnregister`, `OnOwnerVisibilityChanged`, and transform changes.
- [x] Integrate `PostEditChangeProperty` or an equivalent unified dirty-marking entry point so interactive and committed Details edits update the scene promptly while coalescing meaningless duplicate updates.
- [x] Implement the smallest-ID selection rule for multiple registered components and ensure hiding or deleting the active component selects the next one.
- [x] Add `ASkyBoxActor`, creating a default `DSkyBoxComponent` in its constructor without placing rendering logic in the actor.
- [x] Update Engine module reflection inputs and generated metadata; do not hand-write substitutes for DHT output.
- [x] Test registration/unregistration, visibility, rotation, property updates, multi-component selection, scene release, and stale revisions.

#### Acceptance Gate

- Tests prove that scene snapshots remain consistent with component state without enabling renderer drawing.
- The render thread never touches a `DSkyBoxComponent` or `DTextureCube` UObject while reading sky data.
- Selection among multiple components in one scene remains deterministic after reloads and visibility changes.

### Stage 4: Implement the Sky Rendering Stage

Depends on Stages 1 and 3. This stage produces the first visible skybox result.

- [x] Add a dedicated Slang skybox shader and binding declarations that generate fullscreen coverage from a fullscreen triangle.
- [x] Pass the inverse view/projection data needed for ray reconstruction, plus skybox rotation, Tint, and Intensity.
- [x] Implement direction reconstruction and cube sampling from the Stage 0 ground-truth table; do not "tune" undocumented negations or axis swaps in the shader until the result looks right.
- [x] Create a dedicated pipeline state with no vertex buffer, depth testing/writes, blending, or face culling, and with a target format matching Scene Color.
- [x] Add a renderer-owned linear sampler and 1x1 black fallback cubemap, releasing both safely when the module shuts down.
- [x] In `RenderScene()`, draw the skybox before static meshes, the editor grid, and overlays; issue no skybox draw when no component is active.
- [x] Respect `FSceneView::ViewportX/Y/Width/Height` so letterbox regions for fixed aspect ratios stay black and the sky is not stretched across the full target.
- [x] Use the same skybox background policy in Lit, Unlit, and Wireframe without coupling it to successful initialization of the static-mesh pipeline.
- [x] Ensure the sky writes Scene Color before the existing post process and test sRGB decoding plus linear Tint/Intensity multiplication.

#### Acceptance Gate

- With camera translation only, a given pixel direction samples the same sky; with camera rotation, the six principal-axis faces match the Stage 0 convention.
- Static meshes, the grid, gizmos, lines, and icon overlays are never covered by the sky.
- The main viewport, secondary camera preview, and direct-to-window path use consistent sky orientation and aspect-ratio behavior.
- When a resource is unready, rebuilding, or deleted, the renderer neither crashes nor binds a dangling descriptor and displays the black fallback.

### Stage 5: Complete the Editor Creation and Assignment Workflow

Depends on Stages 2-4. This stage makes the feature usable without test code or hard-coded assets.

- [x] Add a `Texture Cube` creation/import action to the Content Browser.
- [x] Provide explicit file-selection slots for all six faces, displaying `+X/-X/+Y/-Y/+Z/-Z` and orientation guidance before and after selection.
- [x] Validate missing files, decode failures, differing dimensions, and nonsquare inputs before user confirmation, and prevent submission of invalid combinations.
- [x] Make the Content Browser recognize the cube-texture type and provide at least a stable type icon plus a dimensions/mip/face summary in the first version.
- [x] Expose `ASkyBoxActor` in the actor-creation workflow and make the `DTextureCube` field in Details show only compatible assets.
- [x] Provide normal reflected editing for Tint, Intensity, and rotation, refreshing the viewport live and marking the package dirty correctly after changes.
- [x] When a scene contains multiple visible skyboxes, show a nonblocking diagnostic that identifies the active and ignored components.
- [x] Add an editor workflow test that imports a cube, creates an actor, assigns the asset, saves the level, reloads it, and verifies the scene snapshot.

#### Acceptance Gate

- A new user can complete "six images -> Texture Cube -> SkyBox Actor -> viewport sky" entirely through the editor.
- After saving and restarting the editor, asset references, component properties, rotation, and active-component selection remain unchanged.
- Invalid asset combinations produce actionable errors before saving instead of failing during Vulkan creation.

### Stage 6: Complete End-to-End Validation and Cleanup

Depends on all preceding stages. This stage does not expand the effect; it closes reliability, regression, and documentation gaps.

- [x] Run targeted tests related to Texture2D, RHI, VulkanRHI, assets, reflection, scene, renderer, and editor behavior.
- [x] Add repeatable rendered-image or pixel-sampling tests covering all six principal axes, camera translation, camera rotation, component rotation, and geometry occlusion.
- [x] Perform manual visibility checks at different aspect ratios in the main editor viewport, camera preview, and PIE/game window.
- [x] Rapidly replace, rebuild, and delete cube assets; repeatedly hide, show, and delete the SkyBox Actor; then inspect stale-command handling and resource lifetime.
- [x] With Vulkan Validation enabled, verify six layers, multiple mips, descriptor binding, layout transitions, and module destruction.
- [x] Complete a full `all` build with one preset, then launch `DurinEditor` from the same preset for the hidden-window runtime smoke test.
- [x] Update this plan's checkboxes, Current Status, and `Last reviewed`, and move long-lived contracts into the appropriate runtime documentation.
- [x] Update the Cube Map entry under Later Scope in `Documentation/Plans/TextureSupport.md` so the two plans do not contradict the implemented feature.

#### Acceptance Gate

- All targeted tests, the full build, Vulkan Validation, and the DurinEditor smoke test pass.
- The manual visibility matrix reveals no face-orientation errors, translation parallax, aspect-ratio stretching, or draw-order errors.
- Runtime documentation describes the implemented thread, resource, and rendering boundaries, and this plan can be marked complete.

## Validation Matrix

| Dimension | Required Cases | Primary Evidence |
| --- | --- | --- |
| Asset input | Valid six faces, missing face, nonsquare image, dimension mismatch, corrupt file | Asset/Editor automated tests |
| RHI subresources | 6 faces x all mips, invalid slice/mip/region | RHI unit tests and Vulkan Validation |
| Resource lifetime | Initial build, rapid rebuild, replacement, deletion, module release | Revision/render-thread integration tests |
| Scene state | Registration, hiding, rotation, parameter changes, multi-component conflicts | Scene snapshot tests |
| View rays | Six principal axes, camera translation/rotation, component rotation | Pixel-sampling or image tests |
| Draw order | Static meshes, grid, gizmos, and overlays occlude the sky | Rendered images and manual inspection |
| Viewport | Main viewport, camera preview, direct-to-window, fixed aspect ratio | Visible DurinEditor smoke test |
| Failure fallback | No component, empty asset, unready asset, rebuild failure, asset deletion | Automated tests plus black-fallback check |
| View mode | Lit, Unlit, Wireframe | Editor viewport inspection |

## Definition of Done

The "simple SkyBoxComponent" is complete only when all of the following conditions are satisfied:

- [x] Users can import a six-face cube texture in the editor, create an `ASkyBoxActor`, and assign the asset in Details.
- [x] The skybox asset, Tint, Intensity, and rotation remain correct after saving and reloading the level.
- [x] The main viewport, camera preview, and game window display the same correctly oriented static sky.
- [x] Camera translation has no parallax, and camera/component rotation follows the documented coordinate convention.
- [x] The sky does not cover scene geometry, the editor grid, or overlays, and does not stretch fixed-aspect-ratio views.
- [x] No path allows direct game-thread access to the skybox RHI or render-thread reads from reflected UObjects.
- [x] Missing or unready resources use a stable black fallback, and rapid rebuilding/deletion produces no dangling resources.
- [x] Targeted tests, the full `all` build, Vulkan Validation, and the DurinEditor runtime smoke test all pass.
- [x] Long-lived resource, scene, and renderer contracts from the actual implementation have moved into runtime documentation.

## Deferred Follow-ups

The following items require separate design and scheduling after this plan is complete. They do not count as incomplete conditions for this plan:

- HDR/EXR import and floating-point `DTextureCube`.
- Equirectangular HDRI-to-cubemap conversion and offline edge processing.
- `DSkyLightComponent` and diffuse/specular IBL.
- Skydome materials and procedural Sky Atmosphere.
- Multi-skybox transitions, spatial regions, and view-level overrides.
- Cube-texture derived-data caching, asynchronous builds, and residency management.
- An interactive cube-texture previewer and more advanced asset-editing workflows.

## Related Documentation

- [Implementation Plan Documentation Guide](../../AGENTS.md)
- [Texture Support Plan](../../TextureSupport.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRHIPrivate.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/DirectionalLightComponent.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
