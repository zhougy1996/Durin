# Viewport Rendering

Summary: Define window-backed viewports, render targets, presentation, resize recovery, and editor assistance rendering.

Modules: ApplicationCore, Engine, LevelEditor, MonaCore, Mona, RenderCore, Renderer, RHI

This document explains how Durin connects Mona widgets, scene viewports, and RHI render targets for both standalone game windows and editor viewport panels.

## Mental Model

The viewport stack intentionally mirrors the broad Unreal Engine split between a widget-level viewport and an engine-level scene viewport:

- `MWindow` owns native window state and top-level widget content.
- `MViewport` is the widget-level viewport host.
- `FSceneViewport` is the engine-level viewport implementation.
- `DEngine::MainSceneViewport` owns the active scene viewport lifetime.
- `DEngine::RedrawViewports()` drives rendering for the active scene viewport.

Each logical scene viewport also owns an optional renderer view-state token.
Identity, transactional previous metadata, cuts, and shutdown ordering are
defined by [Persistent view state](PersistentViewState.md).

`MWindow` does not own or expose a scene viewport. Offscreen widgets observe a
neutral `IViewportDisplaySource`; native windows are connected directly to an
Engine scene viewport by the runtime or editor composition owner.

## Ownership

`MViewport` stores a weak pointer to `IViewportDisplaySource`. It is a UI
consumer, not the lifetime owner of the Engine producer. It owns only the UI
backend registration for the currently published texture.

`FSceneViewport` is held by the engine through `DEngine::MainSceneViewport`. This keeps the scene viewport alive independently of transient widget references.

Editor-only secondary views can be registered through the engine's auxiliary scene viewport list. The main viewport remains the semantic owner of input, PIE destination switching, and active-camera fallback; auxiliary viewports render only when their own viewport client supplies a valid view.

The composition paths are:

- game and detached Play windows: `composition owner -> MWindow + FSceneViewport`
- editor and asset-preview panels: `composition owner -> MViewport + FSceneViewport`

`FSceneViewport` alone chooses window or offscreen output. Mona does not know
that policy and never branches on it.

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

## Output Policies

Window-backed viewports render directly to the native window backbuffer. Render-target-backed viewports render into an offscreen texture that can later be shown by UI code.

Window resize follows a request/prepare boundary. Platform framebuffer callbacks
overwrite the window's latest pending extent without enqueueing RHI work. Immediately
before a scene or ImGui window submits its draw, `PrepareViewportForDraw()` consumes
that extent and queues `RHIResizeViewport()` ahead of the draw and present commands.
An accessor never resizes a viewport, and intermediate native sizes do not accumulate
as render commands.

On Windows, the native move/resize modal loop continues to request engine frames on
its timer. Each modal continuation drains its render and RHI work before returning to
the window procedure, so the operating system cannot advance the surface extent while
Vulkan is still creating the swapchain for the current callback. Ordinary frames keep
the normal pipelined end-of-frame synchronization.

`FSceneViewport` exposes unambiguous Engine-owned factories:

- `CreateWindowBacked(FViewportClient*, std::shared_ptr<MWindow>)` creates a native-window viewport.
- `CreateOffscreen(FViewportClient*, IScene*)` creates an optional isolated-scene offscreen viewport.

## Scene View Settings

Output mode and scene-view policy have separate ownership. `FSceneViewport`
selects window-backed or render-target-backed output, while its
`FViewportClient` owns persistent shading, rasterization, and post-process
choices. When the engine builds an `FSceneView`, it copies those choices into
`FSceneView::Settings` before enqueueing the render command.

The renderer consumes only that immutable per-view snapshot. Settings are
grouped by feature ownership (`Mode`, `PostProcess`, `Terrain`,
`DirectionalShadow`, and `AmbientOcclusion`) while the outer value remains the
single submission snapshot. Two viewports may therefore render the same
`IScene` with independent Lit/Unlit,
Solid/Wireframe, FXAA, and Off/HalfResolution/FullResolution GTAO choices, and
a later UI change cannot alter an
already-enqueued view. Renderer-global state remains limited to shared GPU
resources and size-keyed intermediate caches rather than semantic view policy.

The Level Editor View menu mirrors that ownership. Features with subordinate
quality or route policy own one submenu containing their boolean `Enabled`
checkbox and mutually exclusive radio choices. Independent visibility toggles
such as grid and collision remain checkboxes. Instantaneous commands use plain
actions. Checkbox and radio controls do not close their popup hierarchy,
allowing repeated A/B changes without reopening the menu.

For the main scene viewport, a valid view supplied by its `FViewportClient`
still has first priority. If it supplies none, Engine resolves the active
World's local `APlayerController` view target and uses that Actor's live
`DCameraComponent`. A missing, foreign, pending-destroy, destroyed, or
camera-less target is ignored. Engine then falls back to the active Level's
primary `ACameraActor`, followed by the existing identity/no-camera behavior.
Auxiliary editor viewports remain explicit and never inherit this fallback.

Viewport policy and view-matrix ownership remain independent on the fallback
path. When a viewport client declines to build matrices, Engine retains that
client's complete `FSceneViewSettings` and combines them with the gameplay
camera. A null client alone selects default settings. This lets embedded PIE
keep the Level Editor viewport's diagnostic policy without letting its editor
camera override the runtime view.

For each valid non-zero output, `FSceneRenderer` preserves this order:

1. Resolve size-keyed Scene Color and depth targets and fit the view to the
   output.
2. Draw the submission-local environment override or scene SkyBox, then PBR
   StaticMesh and SkeletalMesh geometry into Scene Color. An explicit
   environment overrides the scene SkyBox only for that view. Geometry shares
   the hidden Engine studio-environment asset for lighting; it does not replace
   or follow the visible SkyBox.
3. Prepare demanded editor-assistance operations after the scene pass.
4. Apply manual exposure and the ACES fitted display transform, with optional
   FXAA in bounded display-linear space, into the final SDR output.
5. When assistance has drawable work, load the preserved color and depth and
   draw Grid, X-Ray Gizmo/Line/Icon, then visible Gizmo/Line/Icon.
6. Transition only the final pass to Present for a window-backed output or
   ShaderReadOnly for an offscreen output.

When the production hybrid-deferred route is active, its GBuffer is completed
inside step 2 before deferred lighting. Requested directional contact
visibility is produced there by synchronous compute, or by its fragment
fallback, and consumed immediately by deferred lighting before retained
forward geometry. The visibility route is independent of the final output:
main, camera-preview, other auxiliary offscreen, and window-backed Present
views use the same route decision and compute-to-graphics handoff. Display
mapping and editor assistance remain after deferred/retained composition and
never consume the storage target directly.

## Game Window Path

Launch creates the hidden primary `MWindow` and native platform window before
RHI initialization. `DGameEngine::Init()` adopts and shows that window, then
creates a window-backed `FSceneViewport`. The game renders directly to the
native backbuffer and does not create an `MViewport` or UI texture registration.
Detached Play windows use the same composition.

Flow:

```text
FEngineLoop::Init()
  creates the hidden primary MWindow and native window
  passes a presentation initialization context to RHIInit()
  creates and qualifies the startup surface on the RHI thread

DGameEngine::Init()
  adopts and shows the primary MWindow
  explicitly adopts the initialization surface into its native RHI viewport
  calls FSceneViewport::CreateWindowBacked(...)
  sets DEngine::MainSceneViewport

DEngine::RedrawViewports()
  updates FSceneViewport
  begins drawing the native viewport
  clears/renders the backbuffer
  presents through the RHI viewport
```

Windowed startup passes an explicit `FRHIInitializationContext` containing the
primary native handle. Vulkan creates the real startup surface on the RHI
thread before selecting a device and queue family, and both Windows and macOS
qualify presentation support against that exact surface. On macOS,
ApplicationCore first installs the primary window's `CAMetalLayer` while window
creation is still on the AppKit main thread; Windows needs no equivalent
platform preparation.

The surface remains in a move-only `FVulkanPresentationCandidate` until the
startup viewport explicitly sets
`FRHIViewportCreateInfo::bAdoptInitializationPresentationCandidate`. Adoption
transfers ownership exactly once. The candidate's native handle is only a
defensive wrong-window check: `FGenericWindow` has no presentation identity or
token. A mismatched or duplicate adoption fails without silently creating a
replacement surface. Detached and later viewports do not request adoption and
create independent surfaces. Headless initialization creates no presentation
candidate.

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

Embedded PIE keeps the Level Editor viewport client as its render-policy owner;
settings changed during Play therefore remain on that viewport after Stop.
New-window PIE creates a lightweight session-owned viewport client, seeds it
from the previous editor viewport, and routes the Level Editor render controls
to that client while the window is active. The Play window's later changes are
discarded when it closes, and its viewport is released before its client.

Flow:

```text
MLevelEditor::Construct()
  creates MViewport
  calls FSceneViewport::CreateOffscreen(...)
  calls MViewport::SetDisplaySource(...)
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
  publishes the latest logical desired size
  prepares and obtains the matching display texture
  updates UI-backend registration when texture identity changes
  draws the registered texture
```

For editor render-target viewports, `PrepareDisplay()` sanitizes each dimension
to `max(8, ceil(value))`, retains that exact extent in `FSceneViewport`, and
synchronously creates or replaces the offscreen texture. `GetDisplayTexture()`
then exposes it. The UI frame is built before scene rendering commands are
enqueued, while ImGui samples the texture later in the same frame. Engine uses
the retained extent for view construction and rendering, so interactive resize
does not add a stale-size frame.

The Level Editor finalizes one scene-view snapshot after all of its panels have submitted UI for the logic frame. Matrix construction is independent from editor-overlay generation: navigation, gizmo interaction, projection, and picking use the lightweight view, while component visualizers traverse the level once to populate the final render snapshot. `FLevelEditorViewportClient::CalcSceneView()` reuses that snapshot when the renderer requests the same frame and quantized extent. Hover and visualization picking use the previous rendered collector with current matrices, matching the image on which the input occurred; weak object handles make cached hits harmless after object retirement. Hover color is stored on visualization primitives and resolved during composition, so changing hover does not rerun component visualizers.

### Viewport Rendering Statistics

Every `IRendererModule::RenderView` invocation may produce one bounded
`FSceneViewStatistics` value. Its visibility, mesh, terrain, shadow, and light
breakdowns are feature-owned subvalues; headline triangle and draw-call totals
still describe the complete invocation. Renderer reduces its private
visibility, geometry, light, terrain, and shadow diagnostics only after command
recording has completed successfully. RHI supplies the total draw-call value
from the monotonic number of non-empty `Draw` and `DrawIndexed` commands recorded inside
that exact invocation; the value therefore includes SkyBox, shadow, scene,
post-process, and editor-assistance graphics passes, but not ImGui or compute
dispatches. Contact-shadow statistics carry the actual `Compute`, `Fragment`,
or inactive route so editor A/B controls cannot confuse a requested preference
with the producer that completed the view.

Headline triangles count selected main-pass static, spline, skeletal, and
terrain geometry once at the rendered LOD. Shadow triangle submissions remain
a separate field so changing shadow cascade count does not redefine scene
geometric complexity. Failed or incomplete renders do not publish partial
statistics.

Engine render commands capture the exact `FSceneViewport` whose output they
render and publish a coherent latest-only snapshot after `RenderView` returns.
The snapshot carries an availability flag and revision and is synchronized for
render-thread publication and UI-thread reads without flushing render commands.
Main, window-backed, and auxiliary viewports own independent snapshots; camera
preview rendering cannot overwrite the Level Editor main-view statistics.

The Level Editor FPS badge is the statistics entry point. Activating the badge
toggles a compact frame summary right-aligned directly below it. The summary is
limited to frame time, visibility, triangles, and draw calls; its `Details...`
action opens the independently dockable Rendering Diagnostics panel. The full
badge/panel rectangle is excluded from drag/drop, selection, gizmo editing,
camera navigation, wheel input, and embedded-PIE capture before those paths
evaluate the viewport. The summary is suppressed when its minimum readable
size cannot fit inside the viewport, while the FPS badge remains available.
Expansion is an editor session preference under `SceneViewport.ShowStatistics`;
it defaults to collapsed and never dirties level or asset packages.

Rendering Diagnostics separates Overview, Scene, and Render Graph inspection.
Overview reports headline frame and graph-budget values, Scene owns the
feature breakdowns removed from the compact overlay, and Render Graph provides
pass filtering, pass/resource inspection, dependency visualization, resource
lifetimes, and transition counts. Pass filtering compacts the graph to matches
plus their direct dependency context. Hovering or selecting a pass focuses its
incoming and outgoing edges by default, with an opt-out for whole-graph
inspection; dependency tooltips identify value, execution, and explicit edges
and their captured resource cause. The panel is optional in the workspace and
is also available from the Level Editor Panels menu.

Full graph inspection is explicitly sampled rather than copied every frame.
Opening the panel without a capture requests the next frame once; later
captures occur only through `Capture next frame`. `FSceneViewport` carries the
request atomically into the exact render submission and publishes an immutable
owning `FRenderGraphCapture` under a separate revision. Main, window-backed,
and auxiliary viewports therefore retain independent graph captures just as
they retain independent bounded statistics. A failed requested render
publishes an unavailable capture instead of retaining misleading stale data.

The Level Editor camera preview uses this auxiliary path. Selecting an actor with a camera component supplies a camera-backed `FViewportClient`; the resulting target follows the camera's reflected aspect-ratio mode and is drawn as a non-interactive overlay in the main scene panel. The preview stays dormant when no camera is selected, the panel is hidden, or PIE owns the active scene.

Camera aspect ratios support viewport-driven framing, common fixed presets, and a custom numeric ratio. Fixed ratios produce a centered content rectangle inside a mismatched output target. The scene pass clears the complete target first, so the unused area remains black instead of stretching the camera image. `FSceneView::ViewportX/Y/Width/Height` describe this content rectangle and must be respected by projection helpers and renderer viewport/scissor setup.

Perspective scene views use the shared finite-far reversed-Z projection
builder. `FSceneView` carries the depth convention and validated near/far
distances explicitly. Reversed-Z scene depth clears to `0`; opaque and masked
geometry plus depth-tested editor assistance use `GreaterOrEqual`. Viewport
ray construction selects clip endpoints from the explicit convention, and
CPU frustum/projected-size code does not infer the convention from matrix
coefficients. Directional-shadow caster views remain explicitly forward-Z,
clear to `1`, and compare with `Less`, so shadow bias and comparison sampling
are isolated from the main-scene migration.

Runtime Cameras serialize near/far clip, Terrain fade start, and Terrain render
distance in their reflected projection settings. The Level Editor View menu
exposes independently bounded clip and Terrain distance controls per viewport.
The defaults are near `0.1`, far `500000`, fade start `180000`, and Terrain
distance `200000`. The far-plane safety margin is five percent of the clip
range capped at `10000` world units, preserving older explicitly short camera
ranges without allowing Terrain distance to meet the projection boundary.

All viewport types share the same scene-color, material-pass, sky,
post-process, and editor-assistance contracts. Fixed-aspect views constrain
those operations to the fitted content rectangle and leave cleared bars
untouched. Detailed ownership is defined by
[HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md),
[Material System](MaterialSystem.md), [Cube Textures](CubeTextures.md),
[Editor Grid](EditorGrid.md), and
[Renderer Resource Recovery](RendererResourceRecovery.md).

Window-backed output finishes in Present. Render-target-backed output finishes
in ShaderReadOnly and is exposed to Mona through the viewport display-source
boundary without exposing intermediate scene targets.

## Recoverable Renderer Resources

Complete-or-null publication, generation-scoped retry, last-known-good
retention, device invalidation, development commands, and shutdown ownership
are defined by [Renderer Resource Recovery](RendererResourceRecovery.md).

## Interface Boundary

`MViewport` talks only to the MonaCore-owned `IViewportDisplaySource`, not to
`FSceneViewport`.

The neutral port contains only the widget-facing operations needed by Mona:

- `PrepareDisplay(const FVector2f&)`
- `GetDisplayTexture()`

MonaCore owns this contract. Engine and Mona both depend publicly on MonaCore;
Engine additionally depends privately on Mona for concrete application,
window, and window-backed presentation integration, while Mona never depends on
Engine. The port contains no World, Scene, viewport-client, editor, input,
window, or concrete widget vocabulary.

## UI Texture Registration

`FSceneViewport` owns RHI texture creation, replacement, and destruction but
never calls the UI backend. `MViewport` registers the first non-null published
texture and reuses that registration while both texture and backend identity
remain stable. Source replacement, source expiration, null publication, or
widget destruction unregisters the current texture once when its backend is
still active. A changed or unavailable backend is never called through a stale
pointer; its own shutdown owns any backend-internal cleanup.

## Resize Behavior

`MViewport::SetDesiredSize()` records the size requested by the widget layout.

For editor render-target viewports, `MViewport::Draw()` passes that size to
`FSceneViewport::PrepareDisplay()`. Engine is the sole normalization owner and
stores the quantized extent used by both view construction and texture
allocation.

For game window viewports, `FSceneViewport::UpdateRHIViewport()` asks `FMonaRenderer` for the RHI viewport associated with the `MWindow`. Native window resize events are handled by `FMonaApplication` and the renderer.

Native window identity, Cocoa view/layer installation, and resize notification
remain owned by the main/UI side. Vulkan surface creation/destruction and
swapchain mutation are RHI-thread operations; on macOS they consume the
already-installed layer rather than calling GLFW's AppKit-mutating surface
helper from the RHI thread.
Viewport creation uses a synchronous executor operation. Resize retains its
main-to-render notification, then the render command synchronously marshals the
backend phase after all earlier recorded work; no game or rendering thread may
mutate the swapchain directly.

Windows native move and resize share the ApplicationCore modal-loop lifetime,
but pure movement prioritizes native message dispatch and does not run modal
continuation frames. DWM moves the last presented surface until the terminal
continuation after `WM_EXITSIZEMOVE`. Only framebuffer-size notifications
request viewport resize. Position-only messages update the cached window
position and never schedule swapchain work.
Resize requests retain only the latest non-zero extent, and each accepted
event-free continuation frame can consume that retained request at most once
when the viewport is acquired for drawing. The continuation requested after
`WM_EXITSIZEMOVE` observes the terminal framebuffer notification, so the final
extent is not stranded when the operating system releases its modal loop.
Temporarily unavailable or out-of-date output still skips only the affected
viewport; gameplay, UI, and unrelated viewport rendering continue.

Vulkan creates or replaces viewport output transactionally. A candidate owns
the new swapchain, images, image views, acquire and rendering-done semaphores,
present fences, and frame state until every required part succeeds. Only then
does the viewport atomically commit the candidate. Failure before native
swapchain creation may retain the old complete output when Vulkan still permits
its use. Failure after native replacement has retired that old output exposes an
explicitly unavailable viewport with no backbuffer.

When swapchain maintenance present fences are available and no acquired or
failed-to-enqueue frame remains, resize commits the new candidate without first
draining the graphics or present queue. The replaced swapchain, image views,
semaphores, fences, and frame state move together into a retired generation.
Each frame polls the generation's present fences and destroys it only after the
presentation engine releases every pending resource. Retired generations are
bounded; sustained resize waits for the oldest generation at the bound rather
than allowing unbounded presentation memory growth. Unsupported devices and
ambiguous acquire or failed-present states retain the synchronous idle fallback.

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
and framebuffer views must use that published actual format. MonaImGui caches
one graphics pipeline per observed backbuffer format and creates its render-pass
layout from the same format; the Vulkan selector prefers either RGBA or BGRA
sRGB output before a linear fallback. Main and detached viewport qualification records public clear/present work in addition to
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
- Keep UI texture registration in `MViewport`; Engine must not call a UI backend.
- Keep primary viewport semantics separate from auxiliary editor views; auxiliary clients must explicitly provide their view and do not fall back to the world's active camera.
- Do not make Mona widgets depend on Engine types.
- Keep Engine's concrete Mona integration private; never add a reverse Mona-to-Engine dependency.
- Do not make `MViewport` own the scene viewport lifetime.
- Window-backed game rendering should continue to present through the native RHI viewport.

The provisioned Win64 graphics/presentation family and later-surface rejection
rule are documented in
[RHI Capabilities and Vulkan Startup](RHICapabilitiesAndVulkanStartup.md).

## Related Code

- `Engine/Source/Runtime/Mona/Public/Widgets/MWindow.h`
- `Engine/Source/Runtime/MonaCore/Public/Rendering/ViewportDisplaySource.h`
- `Engine/Source/Runtime/Mona/Public/Widgets/MViewport.h`
- `Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`

## User Guide

- `Documentation/Editor/Guides/SceneViewportNavigation.md`
