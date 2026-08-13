# Viewport Presentation Decoupling Plan

Summary: Decouple Engine scene-view production from Mona widget presentation through one MonaCore-owned neutral display-source port without adding an Engine/Mona dependency.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

The viewport presentation boundary now ships as:

```text
Engine -> MonaCore <- Mona
       no direct edge
```

MonaCore owns one neutral display-source port. Engine produces an
offscreen display texture through that port, Mona will consume it through
`MViewport`, and editor/host composition code that already knows both objects
will connect them. Window-backed scene viewports remain an Engine concern and
continue to present directly through their native RHI viewport.

`IViewportDisplaySource`, explicit `FSceneViewport` factories, Engine-owned
extent normalization, and Mona-owned registration are implemented. The old
Mona viewport interface, render-mode vocabulary, Engine `Mona/SceneViewport`
paths, and direct Engine/Mona dependency are removed. The earlier game-window
documentation claimed an `MViewport` composition that the implementation did
not create; the lasting documentation now records the implemented direct
window-backed path as the migration base.

Validation completed on 2026-08-12: focused Mona/Engine header-boundary,
viewport, Material, StaticMesh, SkeletalMesh, editor-shell, editor-rendering,
Renderer, and process-boundary targets passed; the default-granularity native
aggregate and full `all` build passed; and the built DurinEditor remained live
during the runtime smoke check before controlled shutdown.

## Goal

Remove direct Engine/Mona type knowledge from the viewport path while preserving
window presentation, offscreen editor rendering, same-frame resize behavior,
main and auxiliary view semantics, texture registration, input ownership, and
shutdown safety.

After the plan:

- Engine depends on MonaCore but not Mona.
- Mona depends on MonaCore but not Engine.
- MonaCore exposes a small presentation port with no World, Scene,
  `FViewportClient`, editor, or concrete Mona widget vocabulary.
- `FSceneViewport` does not include, store, or query `MViewport`.
- `MViewport` does not include or name an Engine type.
- Editor and preview owners explicitly connect the scene viewport to the widget.
- Window-backed output still renders and presents directly to the native RHI
  viewport.
- Offscreen output still allocates for the latest widget extent before the
  renderer records that frame's scene work.

## Scope

- Replace `Mona::IMonaViewport` with a neutral MonaCore-owned display-source
  contract.
- Remove `EMonaViewportRenderMode` from the MonaCore public surface.
- Separate Engine's window/offscreen output policy from Mona's display contract.
- Make offscreen desired extent explicit state pushed from `MViewport` into
  `FSceneViewport`.
- Move UI-backend texture registration and unregistration out of Engine and
  into the Mona consumer boundary.
- Move `FSceneViewport` source paths out of Engine's `Mona/` directory.
- Migrate Level Editor, camera preview, Material, StaticMesh, and SkeletalMesh
  preview composition.
- Preserve standalone game and detached Play-window behavior.
- Correct Engine, Mona, and MonaCore public/private module dependencies.
- Add focused contract, resize, replacement, lifetime, and shutdown coverage.
- Update the lasting viewport-rendering documentation after implementation.

## Non-Goals

- Redesigning `FViewportClient`, `FSceneView`, Renderer scene ownership, editor
  picking, navigation, Play input capture, or camera fallback.
- Moving the Mona application, window, event, renderer-backend, or widget
  systems between modules.
- Introducing an `EngineMona`, bridge, host, or other new source module.
- Making MonaCore depend on Engine or Mona.
- Making Mona depend on Engine.
- Changing swapchain creation, Present policy, render-target format, scene
  render order, post-processing, or editor-assistance composition.
- Changing `DEngine` ownership of the main and auxiliary `FSceneViewport`
  lifetimes.
- Generalizing the display-source port into a streaming, multi-plane, CPU-image,
  video, or asynchronous presentation framework.
- Refactoring unrelated Engine/Mona application and input coupling beyond what
  is required to remove the direct Engine-to-Mona module dependency.

## Design Decisions and Invariants

### MonaCore owns a neutral port, not an Engine viewport abstraction

- The contract represents a UI-displayable RHI texture source. It does not
  represent World, Scene, viewport-client, input, editor, or window semantics.
- The selected working name is `Durin::IViewportDisplaySource` in
  `MonaCore/Public/Rendering/ViewportDisplaySource.h`. Stage 0 may refine the
  name once, but the final name must remain neutral and outside the
  `Durin::Mona` namespace.
- The port contains only the operations required to publish the current logical
  display extent, prepare the matching display resource before same-frame
  rendering, and observe the resulting texture.
- The initial candidate is:

  ```cpp
  namespace Durin
  {
      class IViewportDisplaySource
      {
      public:
          virtual ~IViewportDisplaySource() = default;
          virtual auto PrepareDisplay(const FVector2f& DesiredSize) -> void = 0;
          virtual auto GetDisplayTexture() const -> const FTextureRHIRef& = 0;
      };
  }
  ```

- `GetRenderMode`, `IsWindowBacked`, `GetDesiredSize`, and
  `UpdateRHIViewport` do not survive on this neutral port. Engine may retain
  equivalent private or `FSceneViewport`-specific queries where its redraw
  orchestration requires them.

### Output policy remains in Engine

- `FSceneViewport` remains the Engine-level owner of viewport-client binding,
  window/offscreen output selection, RHI viewport or offscreen texture,
  optional render scene, and desired output extent.
- Window versus offscreen is an Engine-local policy. Mona never branches on it.
- Window-backed construction retains `MWindow` from MonaCore and continues to
  obtain the native RHI viewport from the MonaCore application/renderer path.
- Offscreen construction accepts no `MViewport`. The last sanitized desired
  extent is stored by `FSceneViewport` and used consistently by
  `DEngine::RedrawViewports`, view construction, and render-target allocation.
- Construction becomes unambiguous through named factories or distinct config
  types rather than overloads whose second `shared_ptr` argument selects policy.
  The working API is `CreateWindowBacked` and `CreateOffscreen`.
- `FSceneViewport` moves from `Public/Mona` and `Private/Mona` to an Engine-owned
  path such as `Public/Client` and `Private/Client`.

### Widget layout pushes extent; Engine never queries a widget

- `MViewport` owns logical widget size. It publishes that size to the display
  source during `Draw` before asking for the display texture.
- `PrepareDisplay` must make a correctly sized offscreen texture observable in
  that same UI-build phase. The renderer may populate it later in the same
  frame, preserving the established no-extra-frame resize behavior.
- `DEngine::RedrawViewports` reads the extent retained by `FSceneViewport`; it
  never calls back into widget layout.
- Extent sanitization has one Engine owner so view matrices and texture
  allocation cannot round or clamp differently.
- Hidden or source-less widgets do not extend the scene viewport lifetime.
  `MViewport` retains a weak display-source reference, while `DEngine` or the
  preview owner retains the scene viewport.

### Mona owns UI texture registration

- Engine owns the RHI texture lifetime and replacement decision.
- `MViewport` owns registration of the texture with the active UI backend
  because registration is a presentation-consumer concern.
- `MViewport` retains the currently registered texture identity and performs an
  exact old-unregister/new-register transition only when identity changes.
- Repeated draws of one texture do not repeat registration.
- Source replacement, source expiration, null output, widget destruction, and
  Mona consumer detachment release registration exactly once when the backend
  is available.
- Backend shutdown ordering must not cause an Engine destructor to call a dead
  UI backend. Engine destruction only releases its RHI texture.
- This plan does not create a process-global registration registry or move
  general texture ownership into Mona.

### Composition remains at existing owners

- Level Editor and asset-preview owners already depend on Engine and Mona and
  remain the composition roots for their widget/scene-viewport pairs.
- Composition creates an offscreen `FSceneViewport`, creates an `MViewport`,
  and calls a neutral `SetDisplaySource` API.
- The main viewport continues to be published to `DEngine`; auxiliary previews
  continue to register and unregister explicitly.
- Standalone game and detached Play-window paths create only a window-backed
  `FSceneViewport`; they do not need a display-source binding or an offscreen UI
  texture.
- Input authority remains the native window selected by Engine/editor Play
  policy and is independent of the display-source connection.

### Module declarations must match public headers

- Engine removes its Mona dependency after all `MViewport`, `Mona.h`, and other
  Mona-only references leave Engine source.
- Engine retains MonaCore. If an Engine public header derives from or otherwise
  exposes the MonaCore display-source type, MonaCore is an Engine public
  dependency.
- Mona has no Engine dependency. Because `MViewport` exposes the MonaCore port
  in its public API, MonaCore is a Mona public dependency.
- Public headers must compile through declared public dependencies rather than
  incidental transitive linkage or the shared PCH.

## Current Foundations and Gaps

| Area | Existing foundation | Gap addressed by this plan |
| --- | --- | --- |
| Scene viewport | `FSceneViewport` already owns window/offscreen RHI resources and `DEngine` owns main/auxiliary lifetime. | It stores a concrete `MViewport`, includes Mona headers, and mixes UI registration with Engine resource creation. |
| Widget viewport | `MViewport` already keeps a weak interface reference and draws a supplied texture. | Its interface contains Engine policy and it relies on Engine to register the UI texture. |
| Same-frame resize | `MViewport::Draw` prepares the viewport before obtaining the texture, and scene rendering fills that texture later in the frame. | Desired size is pulled by Engine from a concrete widget, preventing module decoupling. |
| Window output | Game and detached Play paths already create window-backed `FSceneViewport` instances and present directly. | Public render-mode vocabulary is shared with Mona even though the policy is Engine-only. |
| Composition | Level Editor and preview modules already create both the widget and scene viewport. | Construction passes the widget into Engine, establishing unnecessary reverse knowledge. |
| Dependency metadata | Engine already declares MonaCore and Mona as private dependencies. | Engine public headers expose cross-module concepts while dependency visibility and actual include direction are inconsistent. |
| Tests | Viewport, editor-shell, preview, Renderer, Vulkan, and process-boundary targets cover adjacent behavior. | There is little focused coverage for the display-source port, registration replacement, same-frame extent publication, and exact teardown. |

## Implementation Stages

### Stage 0: Freeze the port and ordering contract

- [x] Confirm the final neutral interface and file names, keeping the type
  outside `Durin::Mona` and excluding Engine-specific vocabulary.
- [x] Trace the UI-build, `DEngine::RedrawViewports`, render-command submission,
  ImGui sampling, and consumer-detachment order for main, auxiliary, preview,
  standalone, and detached Play-window paths.
- [x] Freeze one extent normalization rule and identify its Engine owner.
- [x] Specify texture registration state transitions for first publication,
  stable reuse, resize replacement, null output, source replacement, source
  expiration, widget destruction, and backend shutdown.
- [x] Add characterization coverage for the current same-frame resize and
  window/offscreen behavior before removing the old interface.
- [x] Record any documentation/code mismatch discovered in the game-window
  composition path and select the implemented behavior as the migration base.

#### Acceptance Gate

- The public port, namespace, extent ordering, texture-registration state
  machine, construction forms, and shutdown owner are unambiguous, and focused
  characterization tests protect the behaviors that later stages move.

### Stage 1: Introduce the neutral display-source seam

- [x] Add the MonaCore display-source header and make `FSceneViewport` implement
  it without removing the old path in the first compile step.
- [x] Add `MViewport::SetDisplaySource` and `GetDisplaySource` using a weak
  reference and migrate draw logic to the neutral contract.
- [x] Preserve same-frame preparation: publish desired size, prepare the
  offscreen resource, then obtain the texture for the current UI draw.
- [x] Add focused fake-source tests for null source, weak lifetime, size
  publication, prepare-before-get ordering, null texture, and successful draw.
- [x] Remove compatibility aliases as soon as all in-tree callers migrate; do
  not leave two long-lived viewport contracts.

#### Acceptance Gate

- Mona can draw a fake MonaCore display source without linking or including
  Engine, and Engine can implement the same port without exposing a Mona type.

### Stage 2: Remove the Engine-to-widget reference

- [x] Add explicit window-backed and offscreen `FSceneViewport` construction.
- [x] Store the sanitized offscreen extent in `FSceneViewport` and use it for
  view construction and render-target allocation.
- [x] Remove the `MViewport` constructor argument, weak member, include, and
  size query from Engine.
- [x] Migrate Level Editor main viewport and camera preview composition.
- [x] Migrate Material, StaticMesh, and SkeletalMesh preview composition.
- [x] Preserve custom preview scenes, main/auxiliary registration, camera
  fallback policy, dormant preview behavior, and object lifetime ownership.
- [x] Update window-backed game and detached Play-window construction without
  introducing a display-source binding.

#### Acceptance Gate

- Engine source contains no `MViewport` reference; all offscreen consumers
  render at their latest published extent; window-backed and detached-window
  output retain direct native presentation.

### Stage 3: Transfer UI texture registration and clean ownership

- [x] Add one `MViewport`-owned registered-texture state transition helper.
- [x] Register the first non-null display texture, retain stable registration,
  and replace registration exactly once when Engine recreates the texture.
- [x] Unregister on source replacement/expiration, null publication, widget
  destruction, and ordinary Mona consumer detachment.
- [x] Remove `MonaUIBackend`, `GActiveUIBackend`, `RegisterTexture`, and
  `UnregisterTexture` use from `FSceneViewport`.
- [x] Verify Engine render-target destruction does not call UI code and that
  Mona shutdown leaves no registered stale texture or backend access.
- [x] Add resize/replacement, repeated-frame, reverse-destruction-order, and
  backend-unavailable tests.

#### Acceptance Gate

- Engine exclusively owns RHI output resources, Mona exclusively owns UI
  registration, and every supported replacement and teardown order is exact,
  leak-free, and free of use-after-shutdown access.

### Stage 4: Remove obsolete vocabulary and dependency edges

- [x] Delete `Mona::IMonaViewport` and `EMonaViewportRenderMode`.
- [x] Move `FSceneViewport` headers and sources from Engine's `Mona/` paths to
  the selected Engine-owned client/rendering path and repair includes.
- [x] Rename `SetViewportInterface`/`GetViewportInterface` to the selected
  display-source API and remove compatibility names.
- [x] Remove Engine's Mona module dependency and classify MonaCore correctly as
  public or private from the final public-header surface.
- [x] Classify MonaCore as a Mona public dependency and remove redundant direct
  dependencies only when public-header compilation proves they are unnecessary.
- [x] Add a dependency regression check or equivalent build coverage proving
  Mona public headers compile without Engine and Engine public headers compile
  without Mona.
- [x] Update `ViewportRendering.md` with the lasting ownership, composition,
  resize ordering, registration, and dependency contract.

#### Acceptance Gate

- The module graph has no Engine/Mona edge in either direction, no obsolete
  Mona viewport interface or render-mode type remains, public headers match
  declared dependencies, and the lasting documentation describes the shipped
  behavior rather than the migration.

### Stage 5: Qualify editor, runtime, and shutdown behavior

- [x] Run the smallest affected viewport, Mona, Engine, editor-shell, preview,
  Renderer, and process-boundary native targets through the root build/test
  workflow.
- [x] Exercise main and camera-preview offscreen viewports, interactive resize,
  hidden/reopened panels, Material/StaticMesh/SkeletalMesh previews, repeated
  stable frames, and orderly editor shutdown.
- [x] Exercise standalone and detached Play-window presentation, resize,
  input-capture independence, window close, and shutdown.
- [x] Because the change is user-visible and crosses runtime/editor module
  boundaries, run the required native aggregate at default target granularity
  and a full `all` build before handoff.
- [x] Validate documentation and inspect the final dependency/include search
  for forbidden Engine/Mona references.
- [x] Move lasting rules into `ViewportRendering.md`, record evidence in
  `Current Status`, and complete the plan only after every acceptance gate
  passes.

#### Acceptance Gate

- Focused and aggregate tests, the full build, editor/runtime validation,
  dependency checks, shutdown qualification, and documentation validation all
  pass with no viewport behavior or one-frame resize regression.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Neutral port | A fake display source can be consumed by Mona without Engine headers or linkage | Mona/MonaCore focused tests |
| Public dependencies | Engine public headers compile without Mona; Mona public headers compile without Engine | Module/header build checks |
| First offscreen frame | Latest widget extent is published and a matching texture is available before same-frame scene submission | Viewport contract test and editor smoke |
| Stable extent | Repeated frames reuse the texture and UI registration | Focused state-transition test |
| Interactive resize | One replacement texture is created and old/new UI registration transitions exactly once without an extra stale-size frame | Viewport test and editor runtime evidence |
| Main editor viewport | Active-camera fallback, scene view settings, interaction image, and editor assistance remain unchanged | Viewport and editor rendering tests |
| Auxiliary preview | Camera preview uses its own extent/client/scene policy and remains dormant when unavailable | Viewport tests and editor smoke |
| Asset previews | Material, StaticMesh, and SkeletalMesh previews retain isolated scenes and expected output | Owning preview test targets |
| Window-backed game | Native RHI viewport is updated and presented directly; no display-source/UI texture registration is required | Process/runtime viewport test |
| Detached Play window | Presentation and input-window authority remain independent and close safely | Editor Play/process test |
| Source lifetime | Widget weak reference does not extend Engine lifetime; either destruction order is safe | Focused lifetime test |
| Backend shutdown | Registered textures detach before backend loss and Engine destruction performs no UI call | Shutdown test and runtime smoke |
| Main/auxiliary replacement | `DEngine` ownership and explicit auxiliary registration/unregistration remain exact | Engine viewport test |

## Definition of Done

- Engine and Mona share only the MonaCore display-source port and have no direct
  module dependency in either direction.
- The neutral port contains no World, Scene, viewport-client, editor, window,
  concrete widget, or Engine output-policy vocabulary.
- `FSceneViewport` owns window/offscreen policy, RHI resources, desired extent,
  and optional render scene without storing or querying `MViewport`.
- `MViewport` owns logical widget size and UI texture registration without
  including or naming Engine types.
- Main, auxiliary, editor preview, standalone, and detached-window paths retain
  their rendering, input, lifetime, and camera-fallback behavior.
- Same-frame offscreen resize behavior is proven and no additional stale-size
  frame is introduced.
- Texture publication, registration replacement, source expiration, consumer
  detachment, and shutdown are exact and safe.
- `IMonaViewport`, `EMonaViewportRenderMode`, old API aliases, and Engine's
  `Mona/SceneViewport` paths are removed.
- Focused tests, required aggregate validation, full build, editor/runtime
  smoke, and documentation validation pass.
- Stable viewport ownership and ordering rules live in the Runtime Rendering
  documentation, and this plan records final evidence before completion.

## Deferred Follow-ups

- Removing Engine's remaining MonaCore application/window/input dependency
  through a separate runtime-host boundary, if later module goals require it.
- General UI texture-registration ownership consolidation for thumbnails and
  texture-editor previews.
- A reusable non-viewport image-source port if multiple independent consumers
  demonstrate the same lifetime and resize contract.
- Moving viewport tests to module-owned test targets as part of the broader
  EngineTests ownership cleanup.
- Replacing the global active UI backend with an injected presentation service.

## Related Documentation

- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/MonaCore/Public/Rendering/ViewportDisplaySource.h`
- `Engine/Source/Runtime/MonaCore/Public/MonaUIBackend.h`
- `Engine/Source/Runtime/Mona/Public/Widgets/MViewport.h`
- `Engine/Source/Runtime/Mona/Private/Widgets/MViewport.cpp`
- `Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp`
- `Engine/Source/Runtime/Engine/Public/Client/Viewport.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Engine.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp`
- `Engine/Source/Editor/StaticMeshEditor/Private/Widgets/StaticMeshPreview.cpp`
- `Engine/Source/Editor/SkeletalMeshEditor/Private/Widgets/SkeletalAssetPreview.cpp`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Mona/Mona.dmodule`
- `Engine/Source/Runtime/MonaCore/MonaCore.dmodule`
- `Engine/Tests/Native/EngineTests/Private/Viewport/`
