# Viewport Rendering

Summary: Define window-backed viewports, render targets, presentation, resize recovery, and editor assistance rendering.

Modules: ApplicationCore, Mona, Renderer, RHI

This document explains how Durin connects Mona widgets, scene viewports, and RHI render targets for both standalone game windows and editor viewport panels.

## Mental Model

The viewport stack intentionally mirrors the broad Unreal Engine split between a widget-level viewport and an engine-level scene viewport:

- `MWindow` owns native window state and top-level widget content.
- `MViewport` is the widget-level viewport host.
- `FSceneViewport` is the engine-level viewport implementation.
- `DEngine::MainSceneViewport` owns the active scene viewport lifetime.
- `DEngine::RedrawViewports()` drives rendering for the active scene viewport.

`MWindow` should not own or expose a scene viewport directly. A window can contain an `MViewport`, and the `MViewport` can reference a viewport implementation through `Mona::IMonaViewport`.

## Ownership

`MViewport` stores a weak pointer to `Mona::IMonaViewport`. It is a UI widget, not the lifetime owner of the engine viewport.

`FSceneViewport` is held by the engine through `DEngine::MainSceneViewport`. This keeps the scene viewport alive independently of transient widget references.

Editor-only secondary views can be registered through the engine's auxiliary scene viewport list. The main viewport remains the semantic owner of input, PIE destination switching, and active-camera fallback; auxiliary viewports render only when their own viewport client supplies a valid view.

The current paths are:

- game startup: `MWindow -> MViewport -> FSceneViewport`
- editor viewport panel: `MLevelEditor -> MViewport -> FSceneViewport`

This keeps the widget composition path consistent while still allowing each viewport to choose a different render mode.

### Renderer Ownership and Scene Vocabulary

`World` is Durin's high-level owner of actors, levels, components, and game
logic. The renderer-facing representation of that world remains `IScene` in
public contracts and `FScene` in the Renderer implementation. `FScene` owns
render-thread SceneInfo/SceneProxy pairs and authoritative typed views for
primitives, directional lights, and SkyBoxes; it is not a second gameplay
scene abstraction. See
[Renderer Scene Representation](SceneRepresentation.md) for its identity,
ownership, and mutation contracts.

`FSceneRenderer` is distinct from `FScene`: it executes one view and owns the
resources and feature renderers used to produce that view. `FRendererModule`
is the module-lifecycle and public-interface adapter around this private
composition:

```text
FRendererModule
`-- FSceneRenderer
    |-- FRendererResourceCoordinator
    |-- FDefaultTextureResources
    |-- FFullscreenGeometryResources
    |-- FStaticMeshRenderer
    |-- FSkyBoxRenderer
    |-- FTextureCubeThumbnailRenderer
    |-- FPostProcessRenderer
    `-- FEditorAssistanceRenderer
        |-- FEditorGridRenderer
        |-- FGizmoRenderer
        |-- FOverlayLineRenderer
        `-- FOverlayIconRenderer
```

Feature renderers own their shader maps, pipelines, RHI payloads, keyed
caches, geometry, retry state, diagnostics, and release paths. Shared
facilities are explicit resource owners rather than anonymous feature globals.
All GPU resource mutation, invalidation, retry, and release remains confined
to the rendering thread.

## Render Modes

`Mona::IMonaViewport` exposes the render mode through `GetRenderMode()` and `IsWindowBacked()`.

Window-backed viewports render directly to the native window backbuffer. Render-target-backed viewports render into an offscreen texture that can later be shown by UI code.

`FSceneViewport` currently supports both modes:

- `FSceneViewport(FViewportClient*, std::shared_ptr<MWindow>)` creates a window-backed viewport.
- `FSceneViewport(FViewportClient*, std::shared_ptr<MViewport>)` creates a render-target-backed viewport.

## Scene View Settings

Output mode and scene-view policy have separate ownership. `FSceneViewport`
selects window-backed or render-target-backed output, while its
`FViewportClient` owns persistent shading, rasterization, and post-process
choices. When the engine builds an `FSceneView`, it copies those choices into
`FSceneView::Settings` before enqueueing the render command.

The renderer consumes only that immutable per-view snapshot. Two viewports may
therefore render the same `IScene` with independent Lit/Unlit,
Solid/Wireframe, and FXAA choices, and a later UI change cannot alter an
already-enqueued view. Renderer-global state remains limited to shared GPU
resources and size-keyed intermediate caches rather than semantic view policy.

For the main scene viewport, a valid view supplied by its `FViewportClient`
still has first priority. If it supplies none, Engine resolves the active
World's local `APlayerController` view target and uses that Actor's live
`DCameraComponent`. A missing, foreign, pending-destroy, destroyed, or
camera-less target is ignored. Engine then falls back to the active Level's
primary `ACameraActor`, followed by the existing identity/no-camera behavior.
Auxiliary editor viewports remain explicit and never inherit this fallback.

For each valid non-zero output, `FSceneRenderer` preserves this order:

1. Resolve size-keyed Scene Color and depth targets and fit the view to the
   output.
2. Draw SkyBox, PBR StaticMesh, then TextureCube preview proxies into Scene
   Color. StaticMesh draws share the hidden Engine studio-environment asset;
   it lights surfaces but does not replace or follow the scene SkyBox.
3. Prepare demanded editor-assistance operations after the scene pass.
4. Copy or apply FXAA from Scene Color to the final output.
5. When assistance has drawable work, load the preserved color and depth and
   draw Grid, X-Ray Gizmo/Line/Icon, then visible Gizmo/Line/Icon.
6. Transition only the final pass to Present for a window-backed output or
   ShaderReadOnly for an offscreen output.

## Game Window Path

`DGameEngine::Init()` creates the standalone game window, creates an `MViewport` widget, installs that widget as the window content, then creates a window-backed `FSceneViewport`.

The important detail is that the game still renders directly to the window. The `MViewport` exists so the runtime follows the same widget-level viewport composition model as the editor, not because the game is rendered through an ImGui image.

Flow:

```text
DGameEngine::Init()
  creates MWindow
  creates native RHI viewport for the window
  creates MViewport and sets it as window content
  creates FSceneViewport from MWindow
  assigns FSceneViewport to MViewport
  sets DEngine::MainSceneViewport

DEngine::RedrawViewports()
  updates FSceneViewport
  begins drawing the native viewport
  clears/renders the backbuffer
  presents through the RHI viewport
```

In this path, `MViewport::Draw()` only updates the referenced viewport. It exits before drawing a texture because the viewport is window-backed.

## Editor Viewport Path

`MLevelEditor` owns the Level Editor panel and creates an `MViewport` widget for the editor scene view.

The editor viewport is render-target-backed because it must be displayed inside an ImGui dockable panel rather than presented directly to a native window.

During Play, rendering destination and mouse ownership remain separate. Both
embedded and new-window Play assign exactly one native window as the Engine's
gameplay-input authority, but input stays disabled until a primary click on
the game surface captures that window. Capture uses the platform
`Captured` cursor mode, while ordinary ImGui cursor-shape updates skip only
that native window. `Escape`, focus loss, pause, stop, Play-window close, and
teardown restore `Free` mode synchronously before the window or Play world is
released. Focus restoration and resume require another click rather than
recapturing automatically.

Flow:

```text
MLevelEditor::Construct()
  creates MViewport
  creates FSceneViewport from MViewport
  assigns FSceneViewport to MViewport
  sets DEngine::MainSceneViewport

MLevelEditor::Draw()
  opens the Level Editor panel
  updates MViewport desired size from ImGui content region
  calls MViewport::Draw()

DEngine::RedrawViewports()
  updates FSceneViewport
  creates/resizes the offscreen render target when needed
  clears/renders the offscreen render target
  updates registered auxiliary scene viewports
  renders each valid auxiliary view into its own offscreen target

MViewport::Draw()
  updates the referenced viewport
  asks the viewport interface for the display texture
  calls MonaUI::DrawTexture(...)
```

For editor render-target viewports, `FSceneViewport::GetDisplayTexture()` simply exposes the current offscreen render target. The UI frame is built before scene rendering commands are enqueued, while ImGui samples the texture later in the same frame, so using the current render target removes the extra-frame resize lag while still letting the render pass populate it before sampling.

The Level Editor finalizes one scene-view snapshot after all of its panels have submitted UI for the logic frame. Matrix construction is independent from editor-overlay generation: navigation, gizmo interaction, projection, and picking use the lightweight view, while component visualizers traverse the level once to populate the final render snapshot. `FLevelEditorViewportClient::CalcSceneView()` reuses that snapshot when the renderer requests the same frame and quantized extent. Hover and visualization picking use the previous rendered collector with current matrices, matching the image on which the input occurred; weak object handles make cached hits harmless after object retirement. Hover color is stored on visualization primitives and resolved during composition, so changing hover does not rerun component visualizers.

The Level Editor camera preview uses this auxiliary path. Selecting an actor with a camera component supplies a camera-backed `FViewportClient`; the resulting target follows the camera's reflected aspect-ratio mode and is drawn as a non-interactive overlay in the main scene panel. The preview stays dormant when no camera is selected, the panel is hidden, or PIE owns the active scene.

Camera aspect ratios support viewport-driven framing, common fixed presets, and a custom numeric ratio. Fixed ratios produce a centered content rectangle inside a mismatched output target. The scene pass clears the complete target first, so the unused area remains black instead of stretching the camera image. `FSceneView::ViewportX/Y/Width/Height` describe this content rectangle and must be respected by projection helpers and renderer viewport/scissor setup.

Renderer scene-color and depth intermediates are cached by viewport dimensions. This allows the main editor view and a smaller camera preview to render sequentially without recreating the shared intermediate targets twice every frame. The cache is deliberately bounded so interactive resizing does not retain every transient dimension.

When a scene has an active skybox, the renderer draws it into Scene Color using
the fitted content viewport and scissor before opaque meshes. The draw has no
depth interaction, so geometry replaces the background normally. The complete
target is still cleared first; fixed-aspect regions outside the content
rectangle remain black. Main, auxiliary camera-preview, and window-backed views
all use this same `FSceneView`-driven sky path.

Lit StaticMesh output uses the same direct-light and image-based PBR surface
contract in the main level viewport, auxiliary camera preview, Material
Preview, and thumbnails. Unlit mode uses BaseColor plus Emissive. Each view
independently prepares Opaque, Masked, and back-to-front Translucent section
buckets before drawing them in that order. Mask coverage, straight-alpha
blending, depth-write overrides, authored culling, mirrored winding, and
Solid/Wireframe state therefore apply identically to present and offscreen
outputs, including fitted fixed-aspect content rectangles.

All of those consumers resolve an unassigned StaticMesh slot through the same
Engine-retained `/Engine/Materials/DefaultMaterial` proxy. Broken material
state uses the asset-independent unlit magenta ErrorMaterial, so preview or
thumbnail rendering cannot disguise an invalid surface as an ordinary neutral
default and remains diagnosable when Engine Content is unavailable.

Scene post-processing produces the image that is then composed with editor assistance for both window-backed and render-target-backed viewports. Editor assistance is a Renderer phase, not Mona or ImGui content: it loads preserved scene depth for occlusion, but remains outside scene anti-aliasing and any future temporal history. The final assistance pass restores the view's constrained content viewport and scissor after the fullscreen post-process pass, so fixed-aspect black bars remain untouched. Window-backed output then transitions to Present; render-target-backed output becomes ShaderReadOnly and continues through `MonaUI::DrawTexture(...)` without exposing intermediate scene targets to the widget layer.

The editor-assistance draw order is grid first, then X-Ray gizmos, lines, and icons, followed by their depth-tested visible variants. The grid keeps its world-space plane exact but biases only its emitted depth away from the camera, so coplanar scene geometry wins the preserved scene-depth test without shifting the visual origin. Main and auxiliary viewports reuse size-keyed scene intermediates sequentially, while each output target receives its own post-process and final assistance passes.

Editor-assistance demand is derived from the immutable `FSceneView` submitted
for the current output. An empty view initializes no assistance feature or
pipeline and omits the final assistance pass. Grid, Gizmo, Line, and Icon base
resources belong to one Renderer-private, render-thread-owned assistance
renderer, while dynamic geometry counts and available operations belong to the
prepared result for one view. The shared fullscreen geometry used by Post
Process and Grid has its own explicit Renderer-private lifetime.

Each demanded operation requests only the pipeline identified by its feature,
current Present or Offscreen output, depth mode, and Gizmo topology. Base and
pipeline failures are isolated to their feature or exact key, so every
independent available operation remains drawable. Failed creation is suppressed
for the same relevant resource generation rather than retried every frame.
Shader or explicit manual invalidation permits a lazy retry and retains a valid
last-known-good payload if refresh fails; device invalidation clears dependent
RHI payloads before retry. Renderer shutdown resets payloads, generation-scoped
attempts, dynamic capacities, and diagnostics together.

## Recoverable Renderer Resources

Nullable RHI creation is a complete-or-null boundary. Vulkan buffer, texture,
shader, graphics-pipeline, sampler, and vertex-declaration factories publish a
reference only after all native handles and allocations required by that object
exist. Expected creation failure returns null without failing the RHI executor;
device loss, command replay, submission, presentation, and invariant failures
remain terminal.

`RHICreateGraphicsPipelineState` is a creation-only factory. Its `DebugName`
labels diagnostics and captures but does not select, reuse, or retain a PSO;
every successful request returns a distinct complete pipeline. Renderer slots
and explicit Renderer-owned payloads hold the logical strong reference. A
recorded draw may hold a transient reference until replay finishes, after which
replacement, reset, device invalidation, and shutdown flow through the ordinary
RHI pending-delete and Vulkan frame-safe deferred-deletion paths. Vulkan may
continue caching structural descriptor layouts and compatible render passes,
but those caches do not own logical graphics PSOs.

Fixed Renderer resources, static-mesh shader and pipeline identities, editor
assistance, shared fullscreen geometry, and Texture Editor preview resources
use `TRenderResourceCreationSlot`. A slot constructs a complete candidate in
local ownership and publishes it only after every shader binding, RHI resource,
and pipeline required by that identity succeeds. Callers therefore observe
either the prior complete payload, a newly committed complete payload, or no
payload; partially initialized aggregates are never visible.

Each owner tracks independent shader, device, and manual generations. A failed
attempt records the selected generation, error category, context, identity,
owned diagnostic text, retry dependencies, and whether a fallback remains.
Another lookup in the same relevant generation does not call the factory or log
the same failure again. A later relevant generation permits one new lazy
attempt. Shader and manual refresh failures may retain a complete
last-known-good payload as stale-ready; device-generation changes discard old
RHI payloads before attempting replacement.

Last-known-good retention is limited to a slot's declared dependency contract.
A shader or manual refresh may retain a complete payload only while its device
generation remains current. Device invalidation clears every device-dependent
payload, so stale RHI objects are never used as fallback across a device
generation. This invalidation seam coordinates resource reconstruction; it does
not recover a lost Vulkan device or a failed RHI executor.

Size-keyed scene color and depth targets use the same device-dependent slot
semantics. A failed color/depth pair publishes no cache tombstone, and another
lookup in the same device/manual generation is suppressed instead of allocating
every frame. Device or manual invalidation, or normal bounded identity eviction,
makes a later attempt eligible. The cache continues to retain at most eight size
identities.

Renderer owns these development commands:

- `renderer.reload-shaders changed` advances shader generation and lets normal
  dependency fingerprints select changed output on the next demanded lookup.
- `renderer.reload-shaders all` advances shader generation and forces
  compilation for each next-demanded shader candidate in that generation.
- `renderer.retry-resources` advances manual generation for failed resources
  whose retained error permits explicit retry.

Console callbacks enqueue one render command. Views submitted before that
command retain the old generation, while later views observe the new one;
resource construction remains synchronous and demand-driven on the render
thread. New failures and changed failure fingerprints produce one structured
diagnostic, retained stale-ready failures identify their fallback, and a
successful retry reports one recovery transition.

`FRendererResourceCoordinator` owns command admission and the shader, device,
and manual generation counters. `FSceneRenderer` receives each accepted
request and explicitly fans it out to the concrete resource owners. Shader and
manual invalidation advance their relevant generation and leave reconstruction
lazy. Device invalidation releases every dependent payload before advancing
the device generation, then recreates only startup defaults; feature resources
are rebuilt on their next demand.

The internal device-invalidation request is a tested Renderer seam rather than
a claim of Vulkan device-loss recovery. It clears fixed and keyed payloads,
scene targets, assistance state, dynamic capacities, fullscreen geometry, and
diagnostics before publishing the new device generation. Renderer shutdown
first closes command admission and unregisters the development commands, then
enqueues resource release and flushes rendering work. Texture Editor retains
module ownership of its preview slot and releases it through its own ordered
shutdown path.

## Interface Boundary

`MViewport` talks only to `Mona::IMonaViewport`, not to `FSceneViewport`.

`IMonaViewport` contains the widget-facing operations needed by Mona:

- `GetRenderMode()`
- `IsWindowBacked()`
- `GetDesiredSize()`
- `UpdateRHIViewport()`
- `GetDisplayTexture()`

This prevents the Mona widget layer from depending on the Engine module. `FSceneViewport` implements the interface on the Engine side.

## Resize Behavior

`MViewport::SetDesiredSize()` records the size requested by the widget layout.

For editor render-target viewports, `FSceneViewport::UpdateRHIViewport()` reads that desired size and recreates the offscreen render target when the size changes. Sizes are clamped to a small non-zero minimum before RHI texture creation.

For game window viewports, `FSceneViewport::UpdateRHIViewport()` asks `FMonaRenderer` for the RHI viewport associated with the `MWindow`. Native window resize events are handled by `FMonaApplication` and the renderer.

Native window identity and resize notification remain owned by the main/UI
side, but Vulkan surface and swapchain mutation are RHI-thread operations.
Viewport creation uses a synchronous executor operation. Resize retains its
main-to-render notification, then the render command synchronously marshals the
backend phase after all earlier recorded work; no game or rendering thread may
mutate the swapchain directly.

Vulkan creates or replaces viewport output transactionally. A candidate owns
the new swapchain, images, image views, acquire and rendering-done semaphores,
present fences, and frame state until every required part succeeds. Only then
does the viewport atomically commit the candidate. Failure before native
swapchain creation may retain the old complete output when Vulkan still permits
its use. Failure after native replacement has retired that old output exposes an
explicitly unavailable viewport with no backbuffer.

Each candidate first revalidates the startup-provisioned queue family against
its concrete surface, then queries one fresh capabilities/formats/present-modes
snapshot. A value-only selector chooses the format, policy-compatible present
mode, extent, image count, current transform, and a supported composite-alpha
mode (opaque, pre-multiplied, post-multiplied, then inherit). Color-attachment,
sampled, and transfer-destination image usages are all required by the existing
backbuffer contract. Empty query sets, unsupported required usage, invalid
extent/count ranges, or no supported alpha choice reject the candidate before
`vkCreateSwapchainKHR`; required usages are never silently dropped.

The published backbuffer format is derived from the actual selected swapchain
image format, which may differ from the caller's preferred format. Render-pass
and framebuffer views must use that published actual format. Main and detached
viewport qualification records public clear/present work in addition to
candidate failure, unavailable output, resize, recovery, and teardown.

Backbuffer acquisition resolves synchronously before drawing is recorded. An
unavailable result skips only that viewport for the frame; other window-backed,
render-target-backed, and auxiliary viewports continue. Identical creation
failures are suppressed until resize, an out-of-date/suboptimal surface event,
an extent/fullscreen/present-mode change, or an explicit recreate/retry makes a
new attempt eligible. Successful recovery commits one complete output and emits
one recovery diagnostic. Device loss and unrecoverable surface/device failures
remain terminal rather than entering the swapchain retry state.

Viewport teardown waits only the affected swapchain to become idle, then
destroys that viewport's image views, rendering-done and acquire semaphores,
fences, swapchain, and surface on the RHI thread. It never forces a device-wide
deferred-deletion sweep. Device shutdown remains the only owner of a complete
immediate deletion-queue drain.

The lasting diagnostic, timing, snapshot, and cross-viewport conformance rules
are defined by [RHI Diagnostics and Conformance](RHIDiagnosticsAndConformance.md).

## Design Rules

- Keep `MWindow` focused on native window state and widget content.
- Put viewport widget behavior in `MViewport`.
- Keep scene rendering state in `FSceneViewport` and engine/rendering code.
- Keep primary viewport semantics separate from auxiliary editor views; auxiliary clients must explicitly provide their view and do not fall back to the world's active camera.
- Do not make Mona widgets depend on Engine types.
- Do not make `MViewport` own the scene viewport lifetime.
- Use `MonaUI::DrawTexture(...)` only for render-target-backed UI display.
- Window-backed game rendering should continue to present through the native RHI viewport.

The provisioned Win64 graphics/presentation family and later-surface rejection
rule are documented in
[RHI Capabilities and Vulkan Startup](RHICapabilitiesAndVulkanStartup.md).

## Related Code

- `Engine/Source/Runtime/MonaCore/Public/Widgets/MWindow.h`
- `Engine/Source/Runtime/Mona/Public/Widgets/MViewport.h`
- `Engine/Source/Runtime/MonaCore/Public/Rendering/RenderingCommon.h`
- `Engine/Source/Runtime/Engine/Public/Mona/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`

## User Guide

- `Documentation/Editor/Guides/SceneViewportNavigation.md`
