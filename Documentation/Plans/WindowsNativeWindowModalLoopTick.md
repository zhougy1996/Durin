# Windows Native Window Modal-Loop Ticking Plan

Summary: Keep gameplay, editor UI, and rendering advancing while Windows owns a native GLFW window move/resize modal loop, without re-entering the platform event pump.

Last reviewed: 2026-08-16

Status: Active
Completed:

## Current Status

New-window PIE, standalone game windows, and detached ImGui viewports are all
backed by GLFW-created native windows. On Windows, dragging a decorated window
enters the operating system's move/resize modal loop from inside
`glfwPollEvents()`. The ordinary `FEngineLoop::Tick()` cannot reach gameplay,
UI, or rendering until that call returns, so every Durin window appears frozen
or updates only intermittently during the drag.

The selected path is a Windows-only native timer installed by ApplicationCore
for the duration of `WM_ENTERSIZEMOVE` through `WM_EXITSIZEMOVE`. Timer delivery
requests a guarded Launch-owned continuation frame that deliberately omits
native event polling. No implementation work has started. Stage 0 freezes the
cross-layer callback and frame-state contracts before the native hook is added.

## Goal

Make native window movement remain visually responsive while preserving normal
engine time, gameplay, UI, rendering, and presentation behavior for all other
windows. New-window PIE is the primary qualification case: its World and image
must continue advancing while either the Play window or another detached
window is dragged.

The result must retain one game thread, one process-global GLFW event pump, the
existing rendering-thread/RHI-thread protocol, and deterministic shutdown.

## Scope

- Detect and bound the Windows native move/resize modal-loop lifetime for every
  `FGlfwWindow`.
- Wake the game thread at a bounded cadence while the operating system owns the
  modal loop.
- Continue the ordinary game, deferred-work, UI, render, frame-counter,
  garbage-collection, FPS, and profiling phases without calling
  `glfwPollEvents()` recursively.
- Preserve the WndProc chain used by Durin, GLFW, and detached ImGui viewports.
- Preserve final native position, framebuffer extent, cursor mode, focus, PIE
  input, swapchain recovery, and window-destruction behavior.
- Add unit coverage for phase ordering and re-entrancy plus Windows integration
  coverage for native timer/hook lifetime.
- Qualify new-window PIE, standalone, main editor, and detached ImGui windows.
- Publish the lasting modal-loop frame contract in the owning runtime and
  viewport documentation.

Primary source ownership is bounded to ApplicationCore for native window state,
MonaCore for the application callback boundary if routing is required, and
Launch for frame execution. Engine, MonaImGui, RenderCore, RHI, and VulkanRHI
remain consumers exercised by qualification rather than new ownership sites.

## Non-Goals

- Replacing GLFW or carrying a patched GLFW fork.
- Moving gameplay or UI to a second thread.
- Implementing a custom borderless title bar or application-driven drag
  gesture.
- Reworking normal-frame pacing, Vulkan present-mode selection, or global
  frame-rate limits.
- Guaranteeing a particular refresh rate above the cadence Windows timers and
  the selected Vulkan present mode can sustain.
- Redesigning swapchain creation or live-resize quality. This plan only bounds
  resize work, preserves final extent, and prevents the modal loop from
  starving unrelated windows.
- Extending the continuation mechanism to file dialogs, arbitrary nested OS
  menus, ImGui modal popups, or blocking application work.
- Changing PIE world isolation, pause/single-step semantics, or mouse-capture
  policy.

## Design Decisions and Invariants

### ApplicationCore owns native modal-loop detection

`FGlfwWindow` installs a Windows-only WndProc subclass after GLFW creates the
`HWND`. It handles only Durin's private timer and move/resize lifetime messages,
then chains all unrelated messages to the previously installed procedure.
ApplicationCore does not include or call Launch code.

The native hook is installed for every Durin GLFW window rather than special
casing PIE. Detached ImGui windows may install a later WndProc subclass; the
required chain is:

```text
ImGui WndProc -> Durin modal-loop WndProc -> GLFW WndProc
```

Destruction kills the timer, clears native properties, and restores the prior
WndProc before GLFW destroys the native window. Enter, exit, duplicate entry,
duplicate exit, and destruction are idempotent.

### Launch owns continuation-frame execution

ApplicationCore exposes a narrow, non-owning modal-loop tick callback. Launch
installs it only after `FEngineLoop` reaches `Running` and clears it before
consumer detachment or application shutdown. A missing callback is a no-op.
The callback is invoked only on the game/window thread.

The callback never calls `FEngineLoop::Tick()` recursively. Launch factors the
post-event frame body behind a distinct continuation entry point. Normal frames
retain this order:

```text
native event pump -> gameplay/deferred work -> UI -> render -> GC/statistics
```

Modal continuation frames retain the same post-event order but omit the native
event pump. Messages already dispatched by the Windows modal loop continue to
use the existing synchronous ApplicationCore handlers.

### Re-entrancy is explicit and bounded

The loop publishes an internal state equivalent to `Idle`,
`PumpingPlatformEvents`, `RunningFrame`, and `RunningModalFrame`. A modal
continuation is accepted only while the outer regular frame is inside
`PumpingPlatformEvents`. A timer delivered during a modal frame, rendering,
window creation/destruction, shutdown, or another continuation is dropped.

The outer regular frame remains valid after `glfwPollEvents()` eventually
returns. It completes once through the normal body; because modal frames update
`LastTickTime`, its next delta is naturally small. Delta clamping, logic and
render counters, one-frame input transition consumption, diagnostics, and
profiling retain their existing owners.

### The timer is a wakeup, not a second loop

`WM_ENTERSIZEMOVE` starts one window-owned `SetTimer` with a bounded interval in
the Windows `USER_TIMER_MINIMUM` to 16 ms range. The exact constant is frozen in
Stage 0 and covered by diagnostics. `WM_TIMER` is consumed only when both its
window and timer identity match. `WM_EXITSIZEMOVE` stops the timer and requests
one final continuation so cached geometry and rendering observe the terminal
state.

The implementation does not change system-wide timer resolution. Vulkan
present and existing frame synchronization remain allowed to provide
additional pacing; the timer handler must not busy-wait or enqueue unbounded
catch-up frames.

### Move and resize share lifetime but retain distinct work

Native move events update cached window position through the existing path and
do not request a swapchain resize. Framebuffer resize events continue to retain
only the latest pending extent. At most one pending resize is applied for a
viewport in one accepted continuation frame, and the final extent is applied
after modal-loop exit.

Recoverable out-of-date or temporarily unavailable backbuffers skip only the
affected viewport under the existing viewport contract. They do not stop
gameplay, UI, or rendering for other windows. Any broader swapchain or
live-resize redesign requires a separate plan.

### Failure and shutdown stay fail-safe

Failure to install the WndProc hook or native timer is reported once and falls
back to current behavior; it does not prevent window creation or engine startup.
No callback owns the `FEngineLoop`, `FGlfwWindow`, `MWindow`, or PIE session.
Shutdown first closes continuation admission, then removes timers/hooks through
normal window destruction.

Non-Windows implementations retain compile-time no-op behavior and do not
acquire new platform dependencies.

## Current Foundations and Gaps

- `FEngineLoop::Tick()` already keeps platform input, gameplay, UI, rendering,
  GC, FPS calculation, and minimized pacing in explicit phases.
- `RunInteractiveFramePhases()` already provides a value-testable ordering seam,
  but it has no event-free continuation variant or re-entrancy state.
- `FMonaApplication::PollEvents()` correctly calls GLFW once because the GLFW
  event pump is process-global.
- GLFW 3.4 exposes the native Win32 handle and handles
  `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` only for cursor restoration. It does not
  continue Durin's engine frame.
- `FGlfwWindow` currently has no native WndProc lifecycle or modal-loop state.
- Detached ImGui viewports already subclass their Windows WndProc, so any new
  hook must preserve deterministic chaining and teardown order.
- Window resize requests are already coalesced as a latest pending extent in
  `FMonaRHIRenderer`, and unavailable viewport output already skips only that
  viewport.
- Existing frame-phase unit tests cover ordinary ordering and exit after input,
  but not modal continuation, nested callback rejection, or shutdown admission.
- Existing PIE lifecycle and Vulkan viewport tests cover creation, resize,
  presentation, and teardown outside an active native move/resize loop.

## Implementation Stages

### Stage 0: Freeze the continuation and native-hook contracts

- [ ] Add value-level frame-state and phase helpers that describe ordinary and
  modal continuation admission without creating a native window.
- [ ] Select and name the modal timer interval, private timer identity, accepted
  loop state, nested-callback behavior, and exit behavior.
- [ ] Define the ApplicationCore callback registration/lifetime API without a
  reverse dependency on Launch.
- [ ] Define WndProc install, chain, duplicate enter/exit, timer failure, and
  destruction behavior for plain GLFW and ImGui-subclassed windows.
- [ ] Record baseline Tracy evidence for ordinary and new-window PIE dragging,
  including the blocked game-thread stack and observed update cadence.

#### Acceptance Gate

- The selected callback has one owner, one thread, and explicit admission and
  teardown rules.
- Tests can distinguish ordinary, accepted modal, rejected nested, and
  shutdown frame requests without Win32 or Vulkan.
- Baseline evidence confirms the stall is the native modal event loop rather
  than scene cost or swapchain resize during position-only movement.

### Stage 1: Add the ApplicationCore Windows modal-loop bridge

- [ ] Implement platform-neutral callback registration and invocation with no
  owning reference to Launch.
- [ ] Install the Windows WndProc subclass for every `FGlfwWindow` after native
  creation and before consumers can install later hooks.
- [ ] Start and stop the private timer across
  `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`, consuming only matching `WM_TIMER`
  messages.
- [ ] Preserve GLFW cursor behavior and all unhandled native messages through
  the previous WndProc.
- [ ] Make hook/timer teardown exact-once for normal close, requested PIE close,
  application shutdown, and partial initialization failure.
- [ ] Add Windows-focused tests for hook state, timer identity, callback
  absence, duplicate transitions, and destruction cleanup.

#### Acceptance Gate

- A hidden or test window produces bounded modal callback requests between
  synthetic enter and exit messages and none before entry or after exit.
- GLFW and ImGui WndProc chains remain intact, with no duplicate window/input
  callbacks and no timer surviving window destruction.
- Non-Windows targets compile without Win32 types leaking through public
  headers.

### Stage 2: Continue complete engine frames without repolling events

- [ ] Factor the ordinary post-event game/UI/render/maintenance body so it is
  shared by regular and modal continuation frames.
- [ ] Add the explicit engine-loop frame state and reject nested or out-of-state
  continuation requests.
- [ ] Register the callback on entry to `Running` and clear admission before
  `FEngineLoop::Exit()` begins consumer detachment.
- [ ] Preserve delta time, deferred-work pumps, input-transition consumption,
  diagnostics, logic/render counters, GC, FPS calculation, profiler marks, and
  minimized behavior.
- [ ] Extend frame-phase tests for event-free continuation, nested delivery,
  exit during the outer pump, and the outer frame's eventual completion.

#### Acceptance Gate

- During a native modal loop, accepted continuation frames execute gameplay,
  UI, rendering, and maintenance in order and execute no GLFW/native event
  poll.
- A synchronously triggered nested timer cannot recurse into ImGui, Renderer,
  RHI, GC, or the event pump.
- Ordinary frame order and existing startup/shutdown behavior remain unchanged.

### Stage 3: Harden viewport, resize, input, and PIE lifecycle behavior

- [ ] Verify position-only movement never schedules a Vulkan viewport resize.
- [ ] Bound live-resize application to one latest extent per accepted frame and
  force the final extent after modal-loop exit.
- [ ] Preserve per-viewport unavailable/recovery behavior when the active
  resizing surface becomes out of date.
- [ ] Verify GLFW cursor restoration and Durin PIE capture/release state across
  move, resize, focus loss, Escape, Stop, and Play-window close.
- [ ] Extend the existing new-window PIE lifecycle smoke or add a focused
  diagnostic that proves gameplay and render counters advance during a modal
  window interval.
- [ ] Exercise main, Play, standalone, and detached ImGui viewport creation and
  teardown without changing their present-mode policies.

#### Acceptance Gate

- New-window PIE World and render counters advance continuously while its native
  window is moved, and the editor remains responsive.
- Live resize reaches the exact final framebuffer extent without unbounded
  swapchain rebuilds or blocking unrelated viewports.
- Stop/restart PIE, window close, cursor capture, focus changes, and renderer
  teardown remain deterministic before, during, and after modal-loop exit.

### Stage 4: Qualify performance and publish lasting contracts

- [ ] Run focused native tests, affected Vulkan/viewport/PIE tests, the full
  native suite, and the repository build gates through the owning agent
  workflows.
- [ ] Capture before/after Tracy data for a sustained position-only drag and a
  sustained live resize.
- [ ] Visibly qualify new-window PIE, standalone play, the editor main window,
  and an ImGui detached viewport, including multi-monitor DPI transitions.
- [ ] Update Runtime Lifecycle with modal continuation ownership and ordering.
- [ ] Update Viewport Rendering with modal-loop move/resize and swapchain
  behavior; update PIE architecture only if session-specific behavior changes.
- [ ] Record validation evidence, close every acceptance gate, and complete the
  plan lifecycle metadata.

#### Acceptance Gate

- A five-second new-window PIE move retains continuous gameplay and visible
  presentation with no long game-thread interval blocked exclusively in
  `glfwPollEvents()`/`DefWindowProc`.
- Modal continuation cadence is bounded, creates no CPU busy loop, and does not
  regress normal idle or frame pacing.
- Focused, full native, build, visible, profiler, documentation, and lifecycle
  validation all pass, and lasting behavior is owned outside this plan.

## Validation Matrix

| Area | Automated evidence | Runtime/visual evidence |
| --- | --- | --- |
| Phase ordering | Ordinary and modal phase-helper tests | Tracy shows no native event repoll inside continuation frames |
| Re-entrancy | Nested timer and invalid-state requests are rejected | Dragging while ImGui creates/destroys viewports does not recurse or crash |
| Native hook | Enter/exit, timer identity, failure, duplicate transition, and destruction tests | Main, standalone, PIE, and detached windows all enter and leave cleanly |
| Gameplay time | Delta, counters, pause, step, and transition tests | Animated PIE content advances at real-time cadence during movement |
| Rendering | Existing viewport/RHI/Vulkan resize, unavailable, recovery, and teardown tests | Active and unrelated windows continue presenting during move/resize |
| Input and cursor | Existing standalone/PIE mouse-capture lifecycle tests | Escape, focus loss, Stop, close, and post-drag recapture behave consistently |
| Resize | Latest-extent coalescing and final-extent tests | Repeated edge resize and maximize/restore reach the correct image size |
| Shutdown | Callback admission closes before window/application teardown | Closing Play or exiting the editor during/after drag leaves no hang or timer |
| Platforms | Windows build plus non-Windows compile guards | Windows-only behavior; other platforms are unchanged |
| Performance | Bounded callback/frame counters and no catch-up queue | Sustained drag is smooth without idle CPU regression |

## Definition of Done

- Every Durin GLFW window has a safe Windows move/resize modal-loop lifetime
  bridge, with exact timer and WndProc cleanup.
- ApplicationCore detects and requests work; Launch alone owns engine-frame
  continuation and timing.
- No modal continuation calls `glfwPollEvents()`, recursively enters the frame
  body, or creates a second gameplay/UI thread.
- New-window PIE gameplay and presentation continue while the window is moved,
  and unrelated editor/detached windows are not starved.
- Window position, final framebuffer extent, cursor/input state, swapchain
  recovery, PIE stop/restart, and shutdown remain correct.
- Focused tests, full native tests, required builds, visible qualification,
  profiler evidence, and documentation validation pass.
- Runtime Lifecycle and Viewport Rendering contain the lasting implemented
  contract, and this plan is marked completed with evidence.

## Deferred Follow-ups

- Higher-resolution pacing mechanisms if Windows timer cadence is insufficient
  on high-refresh displays after measured qualification.
- A shared continuation policy for other operating-system modal facilities,
  such as native menus or dialogs, only after each blocking boundary is
  characterized independently.
- Custom title bars or application-driven window movement as a UI feature.
- Broader Vulkan live-resize optimization beyond latest-extent coalescing and
  per-viewport recovery.
- User-configurable background/minimized frame-rate policy.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Play In Editor](../Editor/Guides/PlayInEditor.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.h`
- `Engine/Source/Runtime/ApplicationCore/Public/Application/GenericApplication.h`
- `Engine/Source/Runtime/MonaCore/Private/Application/MonaApplication.cpp`
- `Engine/Source/Runtime/MonaImGui/Private/ImGuiMonaImpl.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.h`
- `Engine/Source/Runtime/Launch/Private/EngineFramePhases.h`
- `Engine/Source/Runtime/Launch/Private/EngineFrame.cpp`
- `Engine/Source/Runtime/MonaCore/Private/Rendering/MonaRHIRenderer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Tests/Native/EngineTests/Private/Launch/EngineFramePhaseTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
