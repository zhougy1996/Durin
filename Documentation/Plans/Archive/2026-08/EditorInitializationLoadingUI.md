# Editor Initialization Loading UI Plan

Summary: Move editor workspace and default-level loading back into `DEditorEngine::Init()` while a bounded pre-main-loop pump presents phase-based progress through the existing ApplicationCore, RHI, Mona, and ImGui stack.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

Stages 0-4 are implemented. `DEditorEngine::Init()` now drives the MainFrame
bootstrap to `Ready` or `Failed`, and ordinary `DEditorEngine::Tick()` performs
no startup transition. Launch supplies a narrow startup-frame callback that
processes events and submits a Mona/ImGui-only frame through the production RHI
begin/end and synchronization path. Visible startup waits for a real
FirstPresent; hidden startup bypasses the presentation gate.

The production loading host presents a themed three-phase label and progress
bar before workspace activation and default-Level opening. Default-document
failure remains terminal `Failed` and returns an owning engine-init diagnostic.
A real `WM_CLOSE` during initialization returned process code 0 and completed
the partial-startup unwind. The invalid-default-Level fixture returned process
code 1 and shut down MainFrame, modules, rendering, RHI, and workers cleanly.

The final `Win64-Debug-DurinEditor` `all` build passed together with focused
EditorShell and Launch process tests, visible/hidden project and Project Browser
startup, inline-RHI startup, and the visible PIE lifecycle diagnostic. Visible
timings preserved `FirstPresent` before workspace/default-document readiness.

The matching DurinGame preset also completed an `all` build and a hidden
two-tick startup/ordinary-exit run. A prior launcher-only build left the Sandbox
project module stale and produced a misleading shutdown timeout; that run is
superseded by the complete game build and 1.95-second successful qualification.
The final visible editor timing recorded shell=255.539 ms,
FirstPresent=1274.793 ms, workspace-ready=1333.852 ms, and
default-document-ready=1532.982 ms, preserving the intended presentation-first
ordering while making engine initialization the readiness boundary.

## Goal

- Make successful editor `Init()` mean that the editor host, workspace, and
  configured default Level document are ready for ordinary ticking.
- Preserve the fast first visible editor frame by pumping window events and
  rendering a minimal loading host from inside initialization.
- Use the production ApplicationCore, RHI, rendering-thread, Mona, and ImGui
  path so the first visible frame qualifies the same stack used by the editor.
- Keep the bootstrap pump strictly smaller than the normal engine loop and make
  its admitted work, completion, cancellation, and failure rules explicit.
- Give automation and diagnostics an authoritative readiness/result boundary
  instead of requiring tick-count guesses or polling unrelated globals.

## Scope

- Replace the tick-driven MainFrame bootstrap entry with an initialization-
  driven, forward-only step interface and a structured result.
- Run that bootstrap to completion from `DEditorEngine::Init()` before it
  returns.
- Add a narrow startup-pump callback/context supplied by Launch so editor code
  can process application events and submit loading-only frames without
  depending on Launch-private implementation.
- Split ordinary frame submission from startup-frame submission while sharing
  the RHI begin/end, Mona render, synchronization, and render-counter protocol.
- Show a compact loading view with the current phase, phase count/progress, and
  failure text using the existing MainFrame root widget.
- Define project-browser, hidden-window, close-request, default-document
  failure, and partial-initialization behavior.
- Remove normal-tick bootstrap advancement from `DEditorEngine::Tick()` and
  update diagnostics to consume explicit readiness.
- Preserve and extend startup timing milestones around first present,
  workspace registration, default-document load, and readiness.
- Add focused transition, failure, headless, and runtime qualification coverage.

## Non-Goals

- Showing UI before ApplicationCore, RHI, the rendering thread, or Mona has
  initialized.
- Adding a native Win32 splash window, a second top-level loading window, or a
  platform-specific startup renderer.
- Making Level or general asset loading asynchronous in this plan.
- Claiming smooth animation while a synchronous workspace or asset operation
  holds the game thread.
- Reporting fabricated byte, package, or time percentages when the underlying
  operation exposes no measurable progress.
- Running World ticks, PIE, ordinary diagnostics, garbage collection, deferred
  game-thread budgets, asset-service completion budgets, FPS accounting, or
  normal frame pacing from the bootstrap pump.
- Changing game-runtime default-level startup unless shared engine-init result
  plumbing requires a signature-only adaptation.
- Redesigning the project browser, editor shell visual language, asset
  compatibility policy, or document ownership.
- Solving pre-RHI delays. A native splash remains a separate option only if
  measured RHI, driver, shader, or DLL startup later becomes long enough to
  require presentation before the production renderer exists.

## Design Decisions and Invariants

### Initialization ownership and result

- `FEngineLoop::Init()` still owns ApplicationCore, RHI, rendering-thread, Mona,
  concrete engine construction, and outer rollback.
- Launch supplies a narrow `FEngineInitContext` or equivalent callback surface
  when it calls the concrete engine initialization function. The context can
  pump one startup frame but cannot expose the Launch engine loop itself.
- `DEditorEngine::Init()` creates the editor shell, then repeatedly advances the
  MainFrame bootstrap and invokes the supplied startup pump until the bootstrap
  reaches a terminal state. Default workspace activation and
  `OpenDefaultDocument()` therefore execute within the dynamic extent of
  `DEditorEngine::Init()`.
- Engine initialization publishes a structured success, cancellation, or
  failure result with an owning diagnostic string. Do not add another global
  boolean or require callers to inspect MainFrame state after a `void Init()`.
- A project launch succeeds only when its default document reaches `Ready`.
  Workspace registration failure, default-document load/compatibility/
  activation failure, or an invalid transition fails engine initialization and
  enters the existing partial-startup unwind path.
- A project-browser launch has no default document. It succeeds once the browser
  host is constructed and, for a visible window, its first frame has presented.
- A user close request during initialization is a cancellation, not an
  initialization success. It produces an ordinary clean process exit rather
  than continuing into the main loop or reporting a fatal startup error.

### Bounded bootstrap pump

- The pump runs only after RHI, render-command admission, the rendering thread,
  Mona, the root window, and its native viewport exist.
- One pump iteration may:

  1. process ApplicationCore/Mona window events;
  2. observe close/exit/minimized state;
  3. submit one loading-only UI frame when presentation is available;
  4. synchronize that frame using the existing render-frame protocol; and
  5. yield or wait briefly when no presentation can pace the loop.

- It must not call `GEngine->Tick()`, `GEngine->RedrawViewports()`, any World
  tick, diagnostics `Tick()`, normal deferred-work pumps, normal asset-service
  completion pumps, garbage collection, or profiler frame publication.
- Startup and running frame paths share one internal RHI begin/end implementation
  and frame-counter contract. The startup mode draws Mona/ImGui only; the
  running mode additionally redraws engine viewports.
- The pump is forward-only and terminates on `Ready`, `Failed`, process exit, or
  an explicit bounded timeout used by tests. Production startup must not use a
  magic tick count as its readiness condition.
- The first loading frame is submitted before workspace activation. Another
  frame is submitted after publishing `LoadingDefaultDocument` and before
  calling the synchronous default-document operation.
- If a synchronous step blocks, the last presented loading frame remains
  visible. Continuous animation during that step is deliberately not promised.

### Progress and loading UI

- MainFrame remains the owner of the root-window loading presentation. Launch
  owns frame mechanics and DurinEd owns initialization orchestration; neither
  duplicates MainFrame visual policy.
- Progress is derived from named bootstrap states, not elapsed time. The
  initial contract uses a stable phase index/count or equivalent normalized
  fraction for shell presentation, workspace activation, and default-document
  loading.
- The UI includes a concise phase label and progress bar. It may include the
  current document/project name when already available, but it does not expose
  internal filesystem paths by default.
- `Failed` retains the actionable error in structured bootstrap state long
  enough to log it and render one failure frame when safe. The process does not
  enter normal editor ticking after failure.
- MainFrame state must not transition to `Ready` when
  `EEditorDefaultDocumentState::Failed`; the two state values have one consistent
  terminal interpretation.
- The implementation reuses MonaImGui theme, DPI scaling, persisted window
  bounds, and the existing editor root window. It does not create or swap a
  second window after initialization.

### Visible, minimized, and hidden hosts

- A visible project/editor launch waits for a real first present before starting
  the first blocking workspace/default-document step. This preserves the
  existing perceived-startup behavior.
- `--hidden-window` does not require an active native window or a FirstPresent
  milestone. It advances the same initialization state machine headlessly and
  skips presentation-dependent waits, so automation cannot deadlock waiting for
  a frame that policy intentionally suppresses.
- Minimized or temporarily non-presentable windows continue processing events
  and use a bounded wait. Initialization progress is semantic and does not
  depend on a present counter once the visible first-present gate has passed.
- Startup qualification keeps visible and hidden cases separate. A diagnostic
  that explicitly tests mouse capture or active-window behavior is not run under
  `--hidden-window`.

### Failure and teardown

- Every MainFrame step returns a typed status and owning diagnostic; no failure
  is represented only by logging or a state mutation that the caller may miss.
- `FEngineLoop::Init()` maps editor initialization failure into its existing
  initialization rollback and releases the root window, engine object, Mona,
  rendering thread/RHI, ApplicationCore, project ownership, and earlier
  services in their established order.
- Cancellation caused by a close request uses the same exact-once cleanup but
  remains distinguishable from a load error for process result selection.
- Destroying a partially constructed MainFrame is idempotent. No bootstrap
  callback or weak root-widget capture may run after its context is released.
- Exceptions are not introduced as startup control flow.

### Threading and future asynchronous work

- Bootstrap state transitions and document activation stay on the game thread.
  Render work remains enqueued to the rendering thread through the existing
  command boundary.
- Any worker completion that a specific startup stage requires must be awaited
  explicitly by that stage. The bootstrap pump does not silently admit the
  normal per-frame deferred or asset completion budgets.
- A future asynchronous Level-load plan may enrich the same progress model with
  measured package/subtask progress. It must preserve the `Init()` completion
  contract or explicitly supersede this plan before moving readiness back to
  normal ticks.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Render readiness | ApplicationCore, RHI, render thread, and Mona start before `GEngine->Init()`. | No supported loading-only frame callback is available during concrete engine initialization. |
| Editor shell | MainFrame creates, themes, sizes, binds, and shows the production root window. | Workspace/default-document readiness is deferred to ordinary engine ticks. |
| Loading view | `DrawEditorLoadingState()` already renders bootstrap text through ImGui. | It has no formal phase-progress/error contract and is driven after `Init()` returns. |
| State machine | MainFrame publishes explicit forward states and default-document state. | `LoadingDefaultDocument` transitions outer state to `Ready` even when the document state is `Failed`. |
| First present | Profiling arms and records the real editor first present. | Normal ticking is currently used as the mechanism for waiting and subsequent advancement. |
| Frame submission | Launch `EngineFrame` owns RHI begin/end, Mona, viewport redraw, sync, and counters. | Startup needs UI-only submission without World/viewport redraw or duplication of the RHI protocol. |
| Diagnostics | Visible PIE smoke proves the manual startup path can load and play the default Level. | Hidden mouse-capture smoke is semantically incompatible, and readiness is inferred rather than returned. |
| Rollback | The private engine loop tracks partial startup and explicit cleanup. | Concrete engine initialization currently returns `void`, so editor load failure has no typed path to the loop. |

## Implementation Stages

### Stage 0: Characterize startup and freeze the bootstrap contract

- [x] Record the current visible project, project-browser, hidden project, close-
  during-startup, workspace failure, and default-document failure behavior.
- [x] Inventory every caller and implementation affected by changing the engine
  initialization result/context contract, including editor and game variants.
- [x] Capture current startup milestone ordering and verify that FirstPresent is
  emitted from production frame presentation rather than inferred from a tick.
- [x] Define the exact terminal result and process-exit mapping for success,
  user cancellation, workspace failure, and default-document failure.
- [x] Freeze which operations are admitted in the bootstrap pump and identify
  any startup step that currently relies on a normal Tick-side completion pump.

#### Acceptance Gate

- The characterization matrix names every supported host mode, the engine-init
  API migration surface is complete, and no required initialization operation
  depends implicitly on ordinary World/diagnostic/deferred ticking.

### Stage 1: Add typed initialization and progress contracts

Dependencies: Stage 0.

- [x] Introduce a structured engine initialization result that distinguishes
  success, cancellation, and failure and owns its diagnostic text.
- [x] Introduce the narrow startup-pump context/callback without exposing Launch
  internals or making Editor modules depend on Launch.
- [x] Adapt `DEngine`, `DGameEngine`, `DEditorEngine`, and `FEngineLoop` to the
  result contract while preserving non-editor startup behavior.
- [x] Replace `TickDefaultMainFrameBootstrap()` with a step/run contract whose
  result contains terminal state, phase progress, and failure text.
- [x] Correct bootstrap transition invariants so document failure cannot publish
  overall `Ready`.
- [x] Add focused pure tests for every legal/illegal transition, progress phase,
  result mapping, repeated terminal call, and failure propagation path.

#### Acceptance Gate

- Initialization success is explicit, cancellation and failure cannot be
  mistaken for readiness, MainFrame terminal states are internally consistent,
  and focused tests cover the complete transition/result table.

### Stage 2: Establish loading-only startup frame submission

Dependencies: Stage 1.

- [x] Refactor `EngineFrame` so startup and running modes share RHI begin/end,
  Mona submission, synchronization, and render-counter updates.
- [x] Implement startup mode without `GEngine->RedrawViewports()` or any normal
  game/frame lifecycle work.
- [x] Add application event processing, exit observation, minimized pacing, and
  presentation-availability handling to the narrow startup pump.
- [x] Preserve real FirstPresent profiling for visible startup and provide an
  explicit headless path that does not wait for it.
- [x] Add focused frame tests or the smallest deterministic seam for proving
  startup mode draws UI only, maintains begin/end balance, and stops on exit.

#### Acceptance Gate

- A production editor window can process events and present repeated Mona/ImGui
  loading frames before normal ticking, while render counters and begin/end
  boundaries remain balanced and no engine viewport or World work is submitted.

### Stage 3: Run editor bootstrap entirely inside `Init()`

Dependencies: Stages 1-2.

- [x] Make `DEditorEngine::Init()` create the root host, present the initial
  loading frame, and run MainFrame bootstrap steps to a terminal result.
- [x] Preserve the visible FirstPresent gate before workspace/default-document
  blocking work and skip that gate for hidden-window startup.
- [x] Present the workspace and default-document phases before invoking their
  synchronous operations.
- [x] Make project-browser mode complete initialization after its visible first
  frame or immediately in headless mode.
- [x] Remove bootstrap advancement from `DEditorEngine::Tick()` and remove any
  obsolete early-tick state or comments.
- [x] Route workspace/default-document error text and user close cancellation
  through the typed result into `FEngineLoop` cleanup.
- [x] Update readiness consumers and lifecycle diagnostics to use the explicit
  initialization result rather than polling globals or waiting arbitrary ticks.

#### Acceptance Gate

- When `FEngineLoop::Init()` returns success, a project editor has an active
  default Level and a project-browser editor has a ready browser host; normal
  Tick performs no startup transition, and every failure/cancellation path
  unwinds without entering the main loop.

### Stage 4: Complete loading UI, failure, and automation coverage

Dependencies: Stage 3.

- [x] Finalize the MainFrame loading presentation with themed phase text, a
  truthful phase progress bar, and DPI-aware compact layout.
- [x] Render actionable failure text for one safe final frame where possible,
  while preserving the same error in logs and the engine-init result.
- [x] Add deterministic MainFrame tests for project/project-browser, visible/
  hidden, success/failure, close, and repeated cleanup cases.
- [x] Add a diagnostic or test seam that proves no World tick, PIE tick,
  diagnostics tick, garbage collection, or normal completion-budget pump occurs
  during bootstrap.
- [x] Qualify visible PIE lifecycle smoke separately from hidden/headless
  readiness automation and remove any documentation that combines hidden mode
  with active-window mouse-capture requirements.

#### Acceptance Gate

- The UI communicates every real synchronous phase without fabricated progress;
  visible, hidden, failure, and close paths are deterministic; and automated
  evidence proves the bootstrap pump admits only its frozen operation set.

### Stage 5: Integrate, document, and qualify runtime variants

Dependencies: Stages 1-4.

- [x] Update Runtime Lifecycle with the initialization result, bounded bootstrap
  pump, loading-only render path, readiness boundary, and rollback behavior.
- [x] Update Workspace Framework with the initialization-owned state machine,
  progress semantics, project-browser exception, and document failure rule.
- [x] Update build/run diagnostic examples so visible-window tests and headless
  tests use compatible expectations.
- [x] Run changed-document validation and the all-plan validator.
- [x] Run the smallest affected native test targets during implementation.
- [x] Complete a full `all` build because the change is user-visible and crosses
  Launch, Engine, Mona/RHI frame submission, DurinEd, MainFrame, and LevelEditor.
- [x] From the same final editor build, qualify visible project startup, visible
  project-browser startup, hidden project startup, bounded ordinary exit,
  inline-RHI startup, visible PIE lifecycle smoke, and failure/cancellation
  cleanup.
- [x] Build and run the matching game variant to prove the shared initialization
  result/context did not alter game startup.
- [x] Record startup timings for shell, FirstPresent, workspace, and default
  document and compare them with the Stage 0 characterization.

#### Acceptance Gate

- Documentation and implementation agree; targeted tests and the full build
  pass; visible and hidden editor modes initialize without an early-tick
  bootstrap; the loading UI is visibly qualified; game startup is unchanged;
  and all failure/cancellation paths have direct cleanup evidence.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| State/result | Legal transitions, illegal transition rejection, terminal idempotence, success/cancellation/failure mapping, owning diagnostics. |
| Visible startup | Production root window presents before workspace/default-document blocking work and reaches ready inside `Init()`. |
| Hidden startup | No FirstPresent or active-window dependency; the same semantic phases complete without deadlock. |
| Startup frame | RHI begin/end and counters balance; Mona/ImGui draws; engine viewport redraw and normal frame work remain absent. |
| Progress | Phase label and fraction match the actual state; no percentage is reported for unavailable intra-step progress. |
| Default document | Load, compatibility, and activation success publish Ready; every failure remains Failed and propagates to engine initialization. |
| Project browser | No-project startup reaches a usable browser host without attempting workspace/default-document activation. |
| Cancellation | Closing during bootstrap exits cleanly, skips the normal loop, and releases partial state exactly once. |
| Rollback | MainFrame, engine object, Mona, render/RHI, ApplicationCore, services, and project ownership unwind in established order. |
| Diagnostics | Visible PIE smoke runs with a real active window; hidden readiness coverage does not assert mouse-capture behavior. |
| Integration | Targeted native tests, full editor build, visible/hidden/inline-RHI runs, and matching game build/run. |

## Definition of Done

- Default workspace activation and default-Level opening occur inside
  `DEditorEngine::Init()`, not `DEditorEngine::Tick()`.
- Successful `FEngineLoop::Init()` is an authoritative editor-ready boundary.
- A bounded startup pump presents the production MainFrame loading host through
  existing ApplicationCore, RHI, Mona, and ImGui infrastructure before the
  ordinary main loop begins.
- Startup rendering shares the production RHI begin/end protocol but performs
  no World tick, engine viewport redraw, diagnostics tick, normal completion
  pumping, garbage collection, or FPS/frame-pacing work.
- Visible startup waits for a real first present before blocking work; hidden
  startup has no present or active-window dependency.
- Progress is phase-based and truthful, and synchronous blocking limitations are
  documented rather than hidden behind a fake smooth percentage.
- Default-document failure cannot coexist with overall Ready and propagates an
  actionable typed initialization error.
- Close during startup cancels cleanly and partial startup cleanup is exact-once.
- Project-browser, editor project, inline-RHI, visible PIE, hidden automation,
  and game-runtime qualification all pass from the final build profile.
- Lasting lifecycle and workspace contracts are moved to Runtime Lifecycle and
  Workspace Framework when implementation completes.

## Deferred Follow-ups

- Add asynchronous or cooperative Level/package loading only if measurements or
  UX requirements demand continuous animation and intra-load progress. Reuse
  this plan's readiness and progress contracts instead of restoring tick-driven
  initialization.
- Add a minimal native splash only if measured pre-RHI initialization becomes a
  user-visible delay that cannot be addressed by the production rendering
  stack. It would hand off to, not replace, the Mona/ImGui loading host.
- Consider richer startup recovery, such as keeping the editor shell alive after
  a default-document failure, only with a separate contract for degraded editor
  readiness and user recovery actions.
- Add cancellation of an in-flight asynchronous asset load when such a load path
  exists; synchronous default-document loading cannot honor close until the
  blocking call returns.

## Related Documentation

- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Workspace Framework](../../../Editor/Architecture/WorkspaceFramework.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [UI Style](../../../Editor/Design/UIStyle.md)
- [Launch Process Boundary Refactor](LaunchProcessBoundaryRefactor.md)

## Related Code

- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineFrame.h`
- `Engine/Source/Runtime/Launch/Private/EngineFrame.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Engine.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/MainFrame/Public/Interfaces/IMainFrameModule.h`
- `Engine/Source/Editor/MainFrame/Public/MainFrameModule.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Public/Interfaces/ILevelEditorModule.h`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Runtime/Core/Public/Profiling/Profiling.h`
- `Engine/Source/Runtime/Core/Private/Profiling/Profiling.cpp`
- `Engine/Source/Runtime/Launch/Private/Diagnostics/EditorPIELifecycleSmoke.cpp`
