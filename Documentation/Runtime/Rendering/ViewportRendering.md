# Viewport Rendering

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

Scene post-processing produces the image that is then composed with editor assistance for both window-backed and render-target-backed viewports. Editor assistance is a Renderer phase, not Mona or ImGui content: it loads preserved scene depth for occlusion, but remains outside scene anti-aliasing and any future temporal history. The final assistance pass restores the view's constrained content viewport and scissor after the fullscreen post-process pass, so fixed-aspect black bars remain untouched. Window-backed output then transitions to Present; render-target-backed output becomes ShaderReadOnly and continues through `MonaUI::DrawTexture(...)` without exposing intermediate scene targets to the widget layer.

The editor-assistance draw order is grid first, then X-Ray gizmos, lines, and icons, followed by their depth-tested visible variants. Main and auxiliary viewports reuse size-keyed scene intermediates sequentially, while each output target receives its own post-process and final assistance passes.

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

## Design Rules

- Keep `MWindow` focused on native window state and widget content.
- Put viewport widget behavior in `MViewport`.
- Keep scene rendering state in `FSceneViewport` and engine/rendering code.
- Keep primary viewport semantics separate from auxiliary editor views; auxiliary clients must explicitly provide their view and do not fall back to the world's active camera.
- Do not make Mona widgets depend on Engine types.
- Do not make `MViewport` own the scene viewport lifetime.
- Use `MonaUI::DrawTexture(...)` only for render-target-backed UI display.
- Window-backed game rendering should continue to present through the native RHI viewport.

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
