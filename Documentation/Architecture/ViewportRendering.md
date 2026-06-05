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
  marks the render target ready

MViewport::Draw()
  updates the referenced viewport
  asks the viewport interface for the display texture
  calls MonaUI::DrawTexture(...)
```

`FSceneViewport` keeps the last ready display texture alive with `FTextureRHIRef`. This is deliberate: during resize, a newly-created render target may not yet have been rendered and transitioned for UI sampling. Continuing to display the last ready texture avoids showing an undefined image layout to ImGui.

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
