# Editor Startup Performance Plan

Summary: Reduce DurinEditor time to a usable shell while preserving startup ownership, rendering, and default-workspace behavior.

Last reviewed: 2026-08-10

Status: Active
Completed:

## Current Status

Startup investigation on the existing `Win64-Debug-DurinEditor-Tests` binary
identified synchronous editor-host work as the largest user-visible delay. A
Sandbox launch reached `Durin engine initialized` 1,826 ms after the first
runtime log record, while a Project Browser launch reached the same point in
1,078 ms. The approximately 748 ms project-specific difference includes
workspace registration, mounted-content scanning, and synchronous default-level
open. The measured cold scan was 179 ms, while recent warm scans were normally
16--24 ms, so registry enumeration is not the primary steady-state cost.

Other observed Debug intervals were approximately 388 ms through RHI startup,
266 ms between Mona initialization and Renderer module load while the default
material is synchronously loaded, and 362 ms from the last editor module load
to native swapchain creation in the Project Browser control run. Ten retained
Sandbox launches ranged from 1,513 ms to 2,102 ms.

These numbers are provisional. The sampled Debug executable was produced on
2026-08-08, while the checkout contains later RHI and asset-format changes. The
current logging surface also does not distinguish process entry, a presentable
editor shell, default-workspace readiness, and the first successfully presented
frame. Stage 0 therefore rebuilds current source, adds stable boundaries, and
records a reproducible baseline before startup ordering changes begin.

Stage 0 startup instrumentation is now implemented. The process, PreInit, RHI,
default-material, Renderer, editor-shell, workspace-registration, registry-scan,
default-document, native-viewport, and first-successful-present owners publish
stable profiling boundaries and first-observation monotonic milestones. One
fixed-field `StartupTiming` summary is emitted only after both the native shell
has presented successfully and the default workspace is ready. Debug and
Release Profiling `all` builds and the focused profiling adapter tests pass;
the default-document boundary is additionally split into asset-load,
compatibility-policy, and activation sub-operations for the Stage 3 decision;
the earlier pre-commit Sandbox launch was a smoke test and is not part of the
authoritative baseline matrix.

The Stage 0 cache protocol is locked as follows. A cold Sandbox sample is the
first launch after a machine restart, before another Durin process touches the
workspace content or runtime caches. Warm samples use one unrecorded priming
launch followed by five recorded, serial launches of the same committed binary
and project. Every launch exits cleanly before the next begins; simultaneous
processes may not share mutable cache state. The protocol never deletes authored
content, generated asset payloads, or individual cache entries. Project Browser
controls use the same serial priming and five-run rule without selecting a
project.

The authoritative warm baseline uses commit
`ad63f013d132fd7668f0d4a7a2866e9f6ad4a8fc`, preset
`Win64-Debug-DurinEditor-Tests`, Sandbox project `Sandbox/Sandbox.dproject`, and
eight normal engine ticks followed by a clean automated exit. One priming run
was discarded before each five-run set. Sandbox first present measured median
1,592.591 ms, minimum 1,513.960 ms, and maximum 1,631.003 ms; default-workspace
readiness measured median 1,323.566 ms, minimum 1,248.015 ms, and maximum
1,365.352 ms. Project Browser first present measured median 878.517 ms, minimum
860.215 ms, and maximum 932.402 ms; its no-project workspace-completion control
measured median 670.962 ms, minimum 656.495 ms, and maximum 709.582 ms.

Within the Sandbox set, default-document work measured median 577.723 ms
(564.628--590.611 ms). Asset load accounted for median 576.264 ms
(563.246--589.142 ms), while compatibility policy measured 0.013 ms and level
activation 1.385 ms. Registry scan measured median 24.868 ms and default
material 276.887 ms. A matching Release Profiling capture at
`Build/Profiling/Tracy/DurinEditor-startup-sandbox-baseline-20260810-012807.tracy`
contains the stable RHI-through-first-present zones; its default-document asset
load was 51.658 ms of the 52.539 ms default-document zone. Tracy's on-demand
client cannot record the already-ended process-entry and PreInit zones before
the client starts listening, but their source zones and Debug monotonic
milestones remain present and owned.

Stage 3 is locked to `DefaultDocument.AssetLoad`. The capture observed 33
recursive `Asset.LoadPackage` calls, and code inspection found that each
nonresident package first loads its complete bytes and then calls
`ReadAssetPackageHeader(PhysicalPath)`, reopening and rereading the same file
before codec load. The selected optimization will reuse the already loaded byte
buffer and resolved codec for header validation and registry metadata. It will
not change game-thread package skeleton creation, `DObject` publication,
compatibility reporting, dependency order, or rollback. No competing startup
leaf will be implemented in Stage 3.

The documented cold Debug Sandbox launch remains pending because the locked
protocol requires a machine restart. On 2026-08-10 the user explicitly approved
deferring that restart so implementation could continue; the cold sample remains
an open final-validation requirement and is not treated as passed evidence.

Stage 1 now presents the persisted, styled native shell before loading concrete
editor modules. `DEditorEngine::Tick()` owns the forward-only bootstrap;
Project Browser takes the direct `WaitingForFirstPresent -> Ready` path and does
not load project workspaces. Maximized state is applied while the window is
hidden and before viewport creation, eliminating the previous
1600x1000-to-3840x2019 startup swapchain recreation.

Against the Stage 0 Sandbox baseline, the final five-run warm first-present
sample measured median 852.200 ms, minimum 847.327 ms, and maximum 892.320 ms,
a 46.49 percent median improvement. Workspace admission began 3.128--8.580 ms
after successful present. Default-workspace readiness measured median 1,472.576
ms, minimum 1,440.319 ms, and maximum 1,488.136 ms. That is 11.26 percent above
the Stage 0 median and 16.653 ms above the eventual 10-percent limit, so Stage 2
must recover that margin while separating workspace and document readiness.
Five measured launches, a Project Browser control, and a one-tick close while
waiting for workspace activation all exited cleanly. `EditorShellTests` passed
all 29 cases, including the new transition matrix coverage.

Stage 2 extends the project path through `LoadingWorkspace -> WorkspaceReady ->
LoadingDefaultDocument -> Ready`. LevelEditor session and panel construction no
longer opens the configured default level; the MainFrame bootstrap admits that
operation independently after a workspace-ready frame. Default-document state
is published separately as `Pending`, `Loading`, `Ready`, or `Failed`, while
document-dependent commands retain their existing `Context.Level` readiness
checks.

The Stage 2 five-run warm Sandbox sample measured first-present median 951.916
ms (937.439--1,040.410 ms), default-workspace-ready median 1,037.261 ms
(976.300--1,165.421 ms), and default-document-ready median 1,617.901 ms
(1,573.675--1,760.696 ms). Workspace readiness is 21.63 percent faster than the
Stage 0 baseline and comfortably passes the baseline-plus-10-percent gate.
Missing and incompatible default-level controls each retained the registered
Level workspace, emitted exactly one actionable error, and exited normally.
A focused truncated-package regression passed, and the existing cross-world
level-ownership assertion continues to cover activation rejection; a standalone
invocation of that World test reached the 10-minute process timeout before
producing test output, so it is not counted as passed runtime evidence here.
A six-tick close after module registration and registry scan but before default
document admission skipped the document load and completed reverse-order
shutdown with zero deferred objects. All 29 `EditorShellTests` and the complete
79-case `EditorAssetWorkflowTests` target passed (one existing skipped case).

Stage 3 removes the second package-file read from every nonresident
`Asset.LoadPackage`: the already resolved codec now reads the header from the
same byte buffer later supplied to live loading. `FAssetLoadReport` publishes a
bounded `PackageFileReadCount`; the dependency-closure regression proves that
an owner plus one external dependency performs exactly two package reads while
preserving dependency residency and unload protection.

The equivalent five-run warm Sandbox sample reduced the selected asset-load
median from the Stage 2 value of 538.908 ms to 523.821 ms (519.435--549.808 ms),
a 2.80 percent wall-clock improvement. Default-document work fell from 540.271
ms to 525.215 ms. First-present measured median 989.306 ms
(983.593--1,114.951 ms), still 37.88 percent faster than Stage 0, and workspace
readiness measured median 1,036.114 ms (1,029.761--1,160.559 ms), 21.72 percent
faster than Stage 0. The matching Project Browser control measured
first-present median 1,117.198 ms and shell-completion median 1,117.767 ms; all
five runs retained `-1` registry/default-document fields and exited normally.
Its completion milestone now intentionally follows first present under the
Stage 1 state machine, so it is not directly comparable to the pre-Stage-1
completion boundary.

## Goal

- Make the native editor shell visible and responsive before project workspace
  and default-document loading complete.
- Reduce the five-run warm-cache median from process entry to the first
  successfully presented editor-shell frame by at least 30 percent relative to
  the Stage 0 Debug Sandbox baseline.
- Keep the five-run warm-cache median from process entry to default-workspace
  readiness no worse than 10 percent above the Stage 0 baseline while pursuing
  actual wall-clock reductions where work can be removed or overlapped safely.
- Preserve deterministic startup failure reporting, Project Browser behavior,
  window-state restoration, and rendering ownership.

## Scope

- Stable CPU profiling zones and one bounded startup timing summary for the
  process, RHI, default-material, editor-shell, workspace, default-document,
  swapchain, and first-present boundaries.
- MainFrame construction ordering and a game-thread-owned editor bootstrap
  state machine.
- A lightweight editor shell that can render while concrete workspaces and the
  default project document are still being prepared.
- Targeted optimization of a measured default-material, default-level, or
  viewport startup leaf after Stage 0 selects one path from evidence.
- Debug and Release Profiling comparison, native regression coverage, and
  editor runtime validation.

## Non-Goals

- Moving `DObject`, package publication, or level activation onto an arbitrary
  worker thread.
- Changing the runtime requirement that the default-material service is ready
  before renderer scenes can create proxies.
- Redesigning AssetCore scanning, package formats, or the general-purpose task
  system without new evidence that they dominate the current-source baseline.
- Redesigning Vulkan device selection, capability negotiation, or swapchain
  recovery semantics.
- Adding a branded splash screen or new editor visual design.
- Optimizing DurinGame startup in this plan.

## Design Decisions and Invariants

- MainFrame continues to own the native editor root window, persisted display
  state, root content, workspace composition, and reverse-order rollback.
- Startup publication remains game-thread owned. A bootstrap coordinator may
  prepare worker-safe data asynchronously, but workspace registration,
  `DObject` publication, default-level activation, and UI ownership transitions
  occur on the game thread.
- `DEditorEngine::Tick()` advances the bounded editor bootstrap state after the
  engine enters its normal tick loop. Widget drawing observes bootstrap state
  but does not perform one-time loading or ownership mutation.
- The initial shell applies persisted size, maximized state, UI scale, and theme
  before it is shown. The existing no-normal-frame-before-maximize behavior is
  preserved.
- The shell is considered ready only after its native viewport is usable and a
  shell frame has presented successfully. Creating a native window without a
  presentable viewport does not satisfy the startup metric.
- The shell owns an explicit loading and failure surface until workspace
  composition completes. A failed workspace or default-document operation
  leaves the shell responsive, reports the actionable error once, and rolls
  back partial module registrations in reverse order.
- Default-material initialization remains before Renderer scene creation. Any
  optimization of that interval must preserve exact asset type validation,
  ErrorMaterial fallback, root ownership, and render-proxy readiness.
- Vulkan surface and swapchain mutation remains RHI-thread owned. Startup
  reordering may submit viewport work earlier but may not bypass RHI admission,
  completion, or failure reporting.
- Project Browser startup does not register or open project workspaces until a
  project has been selected and the normal relaunch path begins.
- Startup zones use stable bounded names and never encode project names, asset
  paths, object names, or process-specific identifiers.

## Current Foundations and Gaps

- `FEngineLoop` already exposes stable lifecycle entry points and initializes
  RHI, rendering, Mona, and the concrete engine in a fixed order.
- `DEngine::Init()` synchronously initializes the default material before
  loading Renderer and creating the main scene.
- `DEditorEngine::Init()` currently constructs MainFrame synchronously before
  the normal tick loop begins.
- `FMainFrameModule::CreateDefaultMainFrame()` currently registers every editor
  workspace, opens default workspaces, creates the native viewport, restores
  maximized state, and shows the root window as one blocking call.
- LevelEditor performs mounted-content reconciliation and opens the configured
  default level during singleton workspace construction.
- Tracy covers runtime CPU work but lacks stable startup zones for the intervals
  under investigation.
- There is no explicit editor-shell-ready, default-workspace-ready, or
  first-present milestone, and retained logs predate the current-source build.

## Implementation Stages

### Stage 0: Current-source baseline and decision lock

- [x] Build the complete current-source Debug DurinEditor runtime before taking
  the authoritative baseline.
- [x] Add stable profiling zones and monotonic timing boundaries for process
  entry, PreInit, RHI readiness, default-material load, Renderer readiness,
  editor-shell construction, workspace registration, registry scan,
  default-document load, native viewport readiness, and first successful shell
  present.
- [x] Emit one bounded startup timing summary after default-workspace readiness;
  do not add per-frame timing logs.
- [x] Define cold-cache preparation and warm-cache repetition rules that do not
  delete authored content or share mutable cache state between simultaneous
  processes.
- [ ] Capture at least five warm Debug Sandbox launches, five warm Debug Project
  Browser launches, one documented cold Debug Sandbox launch, and one Release
  Profiling capture.
- [x] Record median, minimum, maximum, and first-present/default-workspace
  milestones in this plan's Current Status.
- [x] Inspect the selected top leaf beneath default-material load,
  default-document load, or native viewport creation. Update the Stage 3 tasks
  with one evidence-backed implementation path before Stage 3 begins; do not
  implement competing speculative paths.

#### Acceptance Gate

- The baseline uses binaries built from the recorded baseline commit.
- Every required milestone has one ordering owner and is visible in both Debug
  timing output and Release Profiling zones.
- The five-run samples are complete, and the selected Stage 3 leaf accounts for
  a material share of startup time rather than log timestamp gaps alone.
- Instrumentation-disabled builds preserve no-op profiling behavior and do not
  evaluate profiling-only arguments.

### Stage 1: Presentable shell before workspace loading

- [x] Split MainFrame startup into shell construction and project-workspace
  activation phases without changing MainFrame ownership.
- [x] Construct the root window, apply host settings, install lightweight root
  content, create its native viewport, and show it before project workspace
  registration begins.
- [x] Add explicit `ConstructingShell`, `WaitingForFirstPresent`, `LoadingWorkspace`,
  `Ready`, and `Failed` bootstrap states with valid forward-only transitions.
- [x] Advance post-present startup from `DEditorEngine::Tick()` on the game
  thread; keep root-widget drawing observational.
- [x] Render a minimal loading state through the existing editor UI system until
  the workspace host is ready, then replace shell content without recreating
  the native window or swapchain.
- [x] Preserve persisted maximize behavior without displaying a transient
  normal-size frame.
- [x] Keep Project Browser startup on the shell-only path and preserve project
  selection relaunch behavior.

#### Acceptance Gate

- A Sandbox launch presents a responsive shell before LevelEditor scans mounted
  content or opens the default level.
- The first-present median meets the 30-percent improvement goal against the
  Stage 0 Debug baseline.
- Window size, maximized state, UI scale, theme, close behavior, and Project
  Browser relaunch behavior remain unchanged.
- Repeated startup and clean exit produce no leaked window, viewport, render
  resource, module registration, or deferred object.

#### Stage 1 Handoff

- Baseline commit: `746580975e10e9c2c986c4c4b3464f36f2b31175`.
- Working set: `EditorEngine`, `IMainFrameModule`, `MainFrameModule`, and
  `EditorBootstrapStateTests`.
- Key symbols: `EEditorBootstrapState`,
  `IsValidEditorBootstrapTransition`,
  `FMainFrameModule::CreateDefaultMainFrame`,
  `FMainFrameModule::TickDefaultMainFrameBootstrap`, and
  `FMainFrameModule::DestroyDefaultMainFrame`.
- Decisions: MainFrame retains root-window and workspace ownership; drawing is
  observational; the game thread admits workspace work only after a successful
  present; maximize is applied before viewport creation while hidden.
- Open question: Stage 2 must reduce or overlap at least 16.653 ms of readiness
  latency to meet the baseline-plus-10-percent gate.
- Validation: Debug `all` build; 29 `EditorShellTests`; five-run Sandbox warm
  measurement; Project Browser shell-only startup; one-tick close-before-load;
  repeated normal shutdown with reverse module unload and zero deferred objects.

### Stage 2: Deferred workspace and default-document activation

- [x] Move concrete editor-module registration and default-workspace opening
  behind the successful first-present boundary.
- [x] Preserve Level, Material, Texture, and StaticMesh registration order and
  reverse-order rollback.
- [x] Separate LevelEditor session construction from default-document open so
  the workspace host can become usable before a heavy default level is fully
  activated.
- [x] Publish workspace readiness and default-document readiness as distinct
  states; commands that require a document remain disabled until activation
  succeeds.
- [x] Preserve compatibility rejection, package-release, and error-reporting
  behavior when the configured default level is missing, incompatible, corrupt,
  or fails activation.
- [x] Ensure close or shutdown during deferred startup cancels future admission,
  drains accepted work, and releases partial ownership in normal shutdown
  order.

#### Acceptance Gate

- The editor shell remains responsive while the default workspace and document
  are pending.
- Successful Sandbox startup produces the same registered workspaces, active
  default level, layout, session settings, and command availability as the
  synchronous baseline.
- Missing and incompatible default levels leave a usable Level workspace and
  one actionable diagnostic without partial document activation.
- Median default-workspace readiness is no worse than 10 percent above the
  Stage 0 baseline.

#### Stage 2 Handoff

- Baseline commit: `cc5473661f6a67523155774c258fa535dfd7a1c0`.
- Working set: MainFrame bootstrap state/API, LevelEditor module and workspace
  construction, default-document controller result, startup timing summary, and
  bootstrap transition tests.
- Key symbols: `EEditorDefaultDocumentState`,
  `FLevelEditorModule::OpenDefaultDocument`,
  `MLevelEditor::FinalizeSessionConstruction`,
  `MLevelEditor::OpenDefaultDocument`, and
  `FLevelDocumentController::OpenDefaultLevel`.
- Decisions: workspace registration and singleton tab opening publish workspace
  readiness; default-level load/compatibility/activation is a later game-thread
  admission; document failure terminates the document state but leaves the
  workspace bootstrap `Ready`.
- Open question: Stage 3 must reduce the 540.271 ms median default-document leaf
  without changing game-thread publication or failure rollback.
- Validation: Debug `all` build; 29 `EditorShellTests`; complete
  `EditorAssetWorkflowTests`; five-run Sandbox metrics; missing and incompatible
  default-level controls; focused truncated-package regression; inspected
  cross-world activation-rejection coverage (standalone invocation timed out
  before test output); close after workspace readiness and before document
  admission; normal repeated shutdown audits.

### Stage 3: Reduce the selected synchronous hot leaf

- [x] Optimize `DefaultDocument.AssetLoad` by removing the redundant physical
  package/header reread from each nonresident `Asset.LoadPackage`: reuse the
  already loaded bytes and resolved codec for header validation and registry
  metadata. The Debug baseline is 576.264 ms across the default-level dependency
  closure versus 0.013 ms compatibility and 1.385 ms activation; preserve
  game-thread package and `DObject` publication, dependency order, compatibility
  reporting, and rollback.
- [x] Add focused regression coverage for the selected cache, load, batching,
  or submission behavior.
- [x] Demonstrate that the optimization removes or overlaps measured work; a
  moved log boundary or deferred cost alone does not count as a wall-clock
  reduction.
- [x] Re-run the Stage 0 matrix and record before/after medians for both first
  present and default-workspace readiness.

#### Acceptance Gate

- The selected leaf has a smaller median duration in an equivalent five-run
  sample and preserves its documented ownership and failure behavior.
- First-present and default-workspace metrics satisfy the plan goals without a
  cold-start regression greater than 10 percent.
- No new unbounded log, profiling label, cache key, or startup thread affinity
  is introduced.

#### Stage 3 Handoff

- Baseline commit: `62ffa40c`.
- Working set: `FAssetManager::LoadPackageInternal`, structured asset-load
  reporting, and the package dependency/rollback regression target.
- Key symbols: `FAssetLoadReport::PackageFileReadCount`,
  `GActivePackageFileReadCount`, and
  `Private::FAssetPackageCodec::ReadHeader`.
- Decisions: reuse the already loaded bytes and already resolved codec; retain
  header validation before skeleton publication; count successful physical
  package reads across the root dependency closure without adding logs, cache
  state, or thread-affinity changes.
- Open question: the required cold Sandbox sample remains deferred until the
  user permits a machine restart; Stage 4 must not mark that gate passed from a
  warm launch.
- Validation: Debug `all` build; focused dependency-count and truncated-package
  cases; complete 97-case `AssetPackageTests`; one priming plus five measured
  Sandbox launches; one priming plus five measured Project Browser launches;
  clean normal shutdown for every run.

### Stage 4: Integration validation and lasting documentation

- [ ] Add or update focused native tests for bootstrap state transitions,
  rollback, close-during-load, and command readiness.
- [ ] Validate threaded RHI execution and the documented inline RHI diagnostic
  mode.
- [ ] Complete a successful full `all` build and normal DurinEditor runtime
  validation from the same Agent Build Profile.
- [ ] Run repeated Sandbox and Project Browser startup/exit cycles and verify
  the final render-resource, RHI, module, task, and object-lifecycle audits.
- [ ] Move lasting editor startup ownership and state-machine rules into the
  owning Editor architecture document, and update Runtime documentation only
  if a runtime contract changed.
- [ ] Record final evidence, complete every passed checklist, and mark this plan
  completed only after all required gates pass.

#### Acceptance Gate

- All required build, test, profiling, runtime, and repeated-exit validation is
  successful.
- The verified editor executable comes from the same successful full build used
  for handoff.
- Long-lived behavior is documented outside the plan, and no implementation
  rule relies only on historical plan text.

## Validation Matrix

| Area | Required validation |
| --- | --- |
| Debug Sandbox baseline | One documented cold launch and at least five warm launches with shell-present and default-workspace milestones. |
| Debug Project Browser control | At least five warm launches with no project workspace or default-document activation. |
| Release Profiling | One matching Tracy capture showing stable startup zones and the selected hot leaf. |
| Window behavior | Normal and maximized startup, UI scale/theme restoration, close during shell load, and no transient normal-size frame. |
| Workspace behavior | Successful default level, missing default level, incompatible default level, registration rollback, and command readiness. |
| Rendering modes | Normal threaded RHI plus the documented inline diagnostic mode. |
| Lifecycle | Repeated clean startup/exit with zero unexplained render resources, deferred objects, task work, or module registrations. |
| Repository validation | Focused native tests, relevant broader tests, full `all` build, and DurinEditor runtime smoke validation through DurinDevTool. |

## Definition of Done

- Current-source baseline and final evidence are recorded against named commits
  using the same machine, preset, project, and cache protocol.
- The warm Debug Sandbox first-present median improves by at least 30 percent.
- Default-workspace readiness is no worse than 10 percent above baseline and
  cold startup is no worse than 10 percent above baseline.
- The shell is visible, responsive, correctly styled, and safely closable while
  project work is pending.
- Default workspace, default level, compatibility policy, rollback, shutdown,
  and Project Browser behavior remain correct.
- Profiling-disabled builds retain zero-work adapter behavior.
- Focused tests, full build, runtime smoke, profiling capture, and repeated-exit
  audits pass.
- Lasting ownership and lifecycle rules are documented in the owning contract.

## Deferred Follow-ups

- DurinGame startup optimization.
- A branded splash screen or editor loading artwork.
- General asynchronous package publication or `DObject` construction.
- Vulkan loader, physical-device enumeration, or driver-level startup tuning
  unless later evidence establishes a separate RHI bottleneck.
- Repository-wide asset registry or package-format redesign.

## Related Documentation

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Editor/Architecture/WorkspaceFramework.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`

## Related Code

- `Engine/Source/Runtime/Launch/Private/Launch.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/DefaultMaterialService.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Runtime/MonaCore/Private/Rendering/MonaRHIRenderer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp`
