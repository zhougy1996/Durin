# Runtime Lifecycle

Summary: Define application startup, frame execution, project admission, and shutdown ownership.

Modules: Launch, Projects, ApplicationCore, Engine, MonaCore, Mona, MonaImGui

Last reviewed: 2026-11-13

This document defines Durin's process startup, frame entry, lifecycle
integration boundaries, and explicit process-exit ordering.

Engine starts its object-aware asset-compilation aggregate after the task
system. Material and Texture2D use built-in typed compiling managers routed from
their reflected asset classes;
editor and headless authoring roots may load TextureCompressor's typed synchronous
Texture2D, VolumeTexture, and TextureCube recipes without transferring task,
PostLoad, object, or publication ownership. Launch
pumps the aggregate while dependent objects are alive and shuts it down before
build sessions, objects, modules, and tasks are torn down. Detailed aggregation and
module lifetime are defined by [Asset Compilation](../Assets/Build/AssetCompilation.md),
while payload ownership remains in
[Asset Data Lifecycle and Storage](../Assets/Packages/AssetDataLifecycle.md).
`DEngine::PreExit()` is the owner-driven consumer-detachment hook;
Launch does not name an import or build module.

## Boot Flow

Process entry is the minimal C runtime `main()` in
`Engine/Source/Editor/DurinLauncher/Private/Main.cpp`. It delegates directly to
the exported `Durin::RunApplicationProcess()` boundary in Launch.

Before profiling or subsystem startup, the process runner initializes Core's
bounded crash state and installs the platform process-crash owner. It then parses
all arguments into one owned request and validates the complete command-line
contract before waiting, publishing diagnostic state, configuring automation,
or starting the engine. Runtime storage preparation later publishes
`Saved/Crashes` without leaving a partially visible path. A lifetime guard
restores the prior handlers exactly once on every ordinary return. Native fault
ownership, phase values, and artifact safety are defined by
[Native Crash Diagnostics](../Core/NativeCrashDiagnostics.md).

`ApplicationRunner.cpp` constructs one private, local `FEngineLoop` and drives
it through:

- `PreInit()`
- `Init()`
- `Tick()`
- `Exit()`

The runner owns process coordination, startup-command dispatch, automated exit,
editor relaunch, conditional logger finalization, and process result selection.
Concrete engine selection remains inside `FEngineLoop::Init()`; no global engine
loop or public engine-loop header exists.

## Collection Requests And Execution Regions

`RequestGarbageCollection(bForcePurge)` records a Game Thread request for the
`DEngine::ConditionalCollectGarbage()` frame-end safe point. Engine callers use
`ForceGarbageCollection(bForcePurge)`. Startup and no-World frames share the process
frame counter, advanced before admitted frame work. Engine records each scheduling
check, even when no collection runs; Core records actual collection separately.
Explicit collection prevents redundant collection in that frame without changing
the Engine's checked-frame record. Requests remain serviceable with automatic
policy disabled and coalesce full-purge intent only. KeepFlags are chosen explicitly
by execution; requests never change retention. Pending destruction advances on
subsequent frames independently of the next reachability scan.

Business callbacks request GC and return to their owner before physical collection.
They must not invoke synchronous collection or reenter frame scheduling. The owner
establishes the safe point after complete operations, exception cleanup and borrowed
storage have ended. There is no business deferral counter or scope-exit collection.
`TryCollectGarbage` rejects collector reentry, not arbitrary business operations.
See the [safe-point and lifetime contract](../ObjectSystem/GarbageCollection.md#safe-point-and-lifetime-contract).

## Startup Responsibilities

`FEngineLoop::PreInit()` handles early engine setup, DLL search paths, config
loading, path mount points, `RenderCore` loading, and reflected object
initialization. It explicitly initializes the final Authored (Editor) or Cooked
(Game) asset domain before `DObjectInit` constructs CDOs, without an intervening
asset shutdown. The [asset bootstrap contract](../Assets/Packages/AssetPackages.md#runtime-bootstrap)
defines constructor-time access and lifetime admission.
Before mount publication or renderer initialization, Projects freezes the source
project's plugin selection and validates that subset against the independently
built available-artifact inventory. Compatible override-only changes require
restart without a build or inventory republication. Admission also prepares
immutable content/shader roots and ordered config inputs before their consumers.
The no-project
Browser admits ordinary modules only. Providers run before ordinary project
roots; EngineReady runs before InitializeProjectContent; EditorReady runs after
workspace module loading and before final editor subsystem/workspace admission.
Game runs Providers after object/asset-domain initialization and before Mona's
first rendering consumer, so native plugin Shader declarations exist before
the cooked runtime inventory freezes. Later phase calls remain idempotent.
The [native plugin startup contract](../../Workspace/Plugins.md#frozen-native-startup)
owns identity, paths, restart semantics, and the source/staged boundary. These
hooks use the existing host initialization and failure paths. They introduce no
additional readiness state machine. Existing host/asset/task retirement precedes
module shutdown, and admitted plugin code remains mapped for reflected objects
and metadata through process exit.

Its private runtime-storage component creates Saved directories
and, when the writable application configuration is absent, copies it from the
runtime variant's deployed immutable template. Existing writable configuration
is never overwritten. `PreInit()` loads the canonical Saved config before logger
startup and emits pass-local preparation warnings only after the logger is ready.

The loop publishes coarse crash phases around pre-initialization, engine
initialization, running, consumer detachment, service and task shutdown, asset
manager shutdown, object collection, module/render/RHI shutdown, and
application shutdown. Typed breadcrumbs distinguish the two shutdown
collections and their adjacent drain boundaries without changing the direct
shutdown protocol.

Worker-scheduler startup creates both the CPU pool and the bounded blocking-I/O
pool in one lifetime; partial pool startup fails closed. After startup,
`PreInit()` installs the bounded
`GameThreadDeferred` executor. Failure to install either executor aborts
startup; the engine does not expose a partially initialized task system.

`FEngineLoop::Init()` handles common runtime startup, including
`ApplicationCore`, the platform half of `Mona`, the initially hidden primary window,
`RHI`, the rendering thread, Mona rendering, and `GEngine`. Launch creates the
final primary `MWindow` and its native window before `RHIInit()`. Launch retains
that window only during initialization; Mona and the concrete host own its running
lifetime and ordinary shutdown. Launch then passes an
explicit presentation `FRHIInitializationContext` containing the native handle.
Vulkan creates and owns a startup presentation candidate before device
selection. Launch carries one explicit adoption intent into Mona rendering, and
the startup viewport transfers that candidate exactly once; later viewports
create their own surfaces. Explicit RHI-only Headless initialization creates
no surface; normal engine startup always uses the presentation context.
Render-command admission opens immediately after `RHIInit()` and before Mona
rendering, editor previews, or engine initialization can enqueue work.
If the selected RHI backend cannot load or initialize, Launch reports the
backend diagnostic, returns initialization failure and unwinds the hidden startup
window without entering project startup. Mona uses the ordinary render flush
barrier before destroying windows. RenderCore treats a stopped queue as already
drained, including before RHI startup; callers do not inspect queue admission.
Repeated Mona shutdown is safe. Normal exit
still destroys windows and rendering resources before closing render admission.

Mona rendering initialization is backend-neutral. In an editor runtime, Launch
then loads MonaImGui and requires it to install the active backend before
`GEngine` initialization can submit UI frames. A missing or failed editor
backend aborts initialization through the ordinary partial-startup unwind. A
game runtime selects no backend: Mona still owns application, window, input,
resize, and RHI presentation, while its UI `NewFrame` and `Render` calls are
explicit no-ops.

Primary-window, platform-surface, and first-viewport ownership is defined by
[Viewport Rendering](../Rendering/Pipeline/ViewportRendering.md). Vulkan instance,
device, queue, portability, and presentation admission are defined by
[RHI Capabilities And Vulkan Startup](../Rendering/RHI/RHICapabilitiesAndVulkanStartup.md).

Current engine selection is semantic:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine overrides.

Concrete engine initialization returns `bool` for service construction and logs
errors where they occur. Launch supplies an `FEngineInitContext` with the primary
startup window and any initial project-admission error. Host startup readiness is
separate: `GetStartupState()` reports Pending, Ready, or Failed. Editor performs
project and workspace initialization synchronously inside `Init()` and publishes
its terminal service outcome before returning. It has no bootstrap-phase enum
or per-frame startup override. Service-only fixtures begin in Ready. Game retains
its ordinary-frame `AdvanceStartup()` for queued project startup and scene
presentation. Both hosts observe callback-driven shutdown before admitting later
initialization work.
`GetCapabilities()` derives normal-tick, tool, World-editing and workspace-display
admission from service readiness and the current shutdown, map-change and Play
facts. Presentation receipts do not gate editor capabilities. Map changes keep normal frame services available but close
tools and editing; Play keeps control tools available while closing editor-World
editing. Domain-specific transaction and document vetoes still apply. Editor
modules do not depend on Launch internals.
Editor and Game adopt that same window rather than creating
a replacement after RHI initialization. The parsed `FEngineStartupParams` is
passed directly to `FEngineLoop::PreInit()` without an intermediate conversion.

`PreInit()` and `Init()` return false on failure; the process runner calls
`Exit()` once to unwind partially initialized process services before returning.
Project edit-ownership failure aborts `PreInit()` rather than replacing the
requested project with the Project Browser. Required Renderer, MainFrame, and
DurinMCP modules use checked loading. Startup does not catch unexpected C++
exceptions to convert them into ordinary initialization or project errors;
expected failures use the explicit result paths.
Concrete engines retain their local host-retirement guards. Launch treats an
exit request present when concrete initialization returns false as cancellation
and selects a clean process result; otherwise it selects failure. A concurrent
close request therefore takes precedence over an initialization error for exit
code selection, while the error remains logged.

DurinEd owns project-service initialization and workspace activation. MainFrame
presents the phase/error state, but its first presentation is not an initialization
dependency. Once workspace services and consumers are ready, the editor enters
Ready and submits the configured default Level as an ordinary asset-editor open
request. LevelEditor owns loading, document association, history, selection,
viewport restoration, progress and errors through the same path as later opens.
Document failure does not retire the usable editor workspace. Startup advancement
stops at service readiness; it never polls, caches terminal outcomes, or reports
loading errors on behalf of the document controller. Empty default-Level settings
submit no document request.

Automation has a separate `GetStartupCompletion(OutError)` observation boundary.
Editor completion refreshes the submitted ordinary request through
`DAssetEditorSubsystem::ResolveOpenResult`, which observes failed, canceled or
retired requests without advancing work. A completed outcome stays completed
when its document closes. Editor automation separately checks `IsAssetOpen` before
accepting the initial document, then requires the active
scene's actual primary-window presentation. `FSceneViewport::QueryPresentation()`
validates scene generation and render outcome, then delegates current-window
receipt matching to Mona. An auxiliary viewport, old generation, or replaced or
removed window mapping cannot satisfy it. Without a scene, automation observes
Mona's read-only current-window first-presentation check. Service-only fixtures
remain service-ready but must supply presentation endpoints for automation
completion, just like production hosts. Game retains its presentation-based host readiness.
The process runner records a baseline in the EngineLoop running-tick counter
once automation startup completes. Tick counts are derived from that baseline;
there is no second advancing counter or separate completion flag. Later ordinary
document changes do not reenter the initial presentation gate. None of these checks read profiling.

`DEngine::Init()` constructs minimum runtime/renderer services without admitting
project content or creating the initial World. After schema providers are active,
`InitializeProjectContent()` requires a successful catalog refresh, initializes
the default-material service, then constructs the complete Engine/World subsystem
sets and initial World. Editor invokes this during project initialization; Game invokes
it after loading its project providers. Material loading, fallback, and render-proxy semantics are defined by
[Material System](../Rendering/Materials/MaterialSystem.md).

`DGameEngine::Init()` constructs base services, its window/scene viewport and
input endpoint, then queues project startup. Its bool reports only service
construction. The first ordinary `AdvanceStartup()` frame runs `StartGame()`;
it loads every enabled project root module before project classes are used.
Module, settings, content, map and presentation failures all become the same
explicit Failed host outcome with their original diagnostic. Game readiness is
derived from its queued-start, awaiting-scene and terminal phases, not from error
string emptiness. Settings and the optional exact `Game.NativeModule` / fully
qualified `Game.GameModeClass` pair are validated before content construction.
A missing pair selects lifecycle-only play. The default map request then owns
BeginPlay, while simulation/input wait for successful scene presentation. Empty
default-level configuration completes without a map request. Shutdown and process
exit stop advancement without reporting readiness or capturing input; an existing
exit request takes precedence when the process runner selects a startup result.

`DEditorEngine::Init()` constructs MainFrame, initializes project services and
workspaces in one synchronous operation, and publishes Ready or Failed service
readiness. Its bool still reports host construction: expected project/workspace
failures preserve the interactive error/browser UI. Editor loads
`AssetForgeBuiltins` and project roots before the atomic catalog scan, then
reconciles subsystems before creating workspace consumers. Project Browser skips
project and workspace loading. Once Ready, Editor submits the ordinary optional
default-document request; its loading remains asynchronous in the normal frame
loop. Presentation does not gate service initialization.

Editor default/open-Level and authored/cooked Game loading share Engine-owned
map changes. `OpenLevel(FMapChangeRequest)` is the only admission boundary. It
accepts one operation, and the owner frame automatically prepares and publishes
it after successful registration and the optional `BeforeCommit` veto. Callers
can observe `GetMapChangeProgress` and cancel through `CancelPendingMapChange`;
there is no public Prepare/Commit/readiness-polling protocol or commit-request
flag. Closed admission or an already pending change rejects a new request
without changing existing progress. Opening never publishes a World inline.

The private `FMapChangeHandler` retains the asynchronous asset handle and
initializes an isolated World/Scene. Resources own preparation, publication and
diagnostics. Cooked materials cache programs through their compilation owner and
existing retry queue; textures begin asynchronous platform-data caching in PostLoad
and publish through `UpdateResource`. Mesh components request residency on registration.
MapChange does not traverse products, retain program requests, impose resource
budgets or wait for resource/PSO readiness. Components own delayed proxy creation.

The ordinary Engine frame advances preparation after completions; UI/modal
callbacks never drive stages. Object loading and World registration authorize
commit without a candidate offscreen render, render receipt or retries.
Components with pending required PSOs appear after their normal owner notification
creates the proxy. Failed required requests keep that component withheld and
diagnosable without failing or rolling back the map change.
View-dependent renderer preparation runs through the committed viewport, and
Game startup and editor automation still wait for its actual presentation receipt.
Failure or cancellation retires the candidate and preserves
the old document.

Requests may supply a `BeforeCommit` veto and an `OnComplete` notification.
Editor document/transaction and viewport coordination uses these callbacks rather
than a separate polling state machine. The Level document controller keeps one
pending action and one path for either Level opening or project switching. A
terminal completion consumes that action and clears the path before notifying
the document owner; there is no additional pending-document flag.
Callbacks run on GameThread under the host lifecycle/GC guard. Completion is
delivered once at an ordinary frame boundary after releasing the Handler; it
may start another change. Host shutdown discards callbacks before retiring their
owners. Closing an editor document invalidates its deferred callback lifetime.

Expected loading failures remain explicit results and phases; internal steps do
not throw to signal Pending, cancellation or a veto. Unexpected preparation
exceptions become one failed progress diagnostic at their owner boundary. Actual
World publication remains outside the recoverable preparation catch: a publication
fault cannot promise rollback. Completion
notification exceptions are logged after releasing the Handler and never
rewrite the result or redeliver the callback, including when it started another
change before throwing. All failed/canceled results carry a diagnostic, so
callers do not need exception handling or duplicate fallback messages. A callback
that cancels and then reports an error still completes as Canceled, without a
second failure notification.

`GetMapChangeProgress` returns a value snapshot of current preparation or the
last terminal result, with frozen terminal timing. MainFrame/LevelEditor project
this business progress directly into SlowTask display. On successful commit,
asset residency transfers to the active World and is released when its Level is
replaced or detached. No completed loading Handler is retained by callers.
Native World initialization/registration callbacks
and package object application remain atomic owner-thread work. Game BeginPlay
runs once on the candidate; spawned resources use the same owner preparation paths.
Simulation and input wait until the committed generation has actually presented.

Automated running-tick counts and startup-command dispatch begin only after startup
completion, including document and presentation checks. Required startup failure
returns a failed automated result while the interactive Editor keeps its workspace
or error/browser UI available. Game startup failure also
terminates the process. A malformed configured default Level remains a failure,
distinct from an intentionally empty default-Level setting.

## Frame Lifecycle

VSync does not provide a work budget for an unsynchronized swapchain. The Editor
leaves visible focused running frames uncapped by default. Editor Settings >
Display offers an optional maximum FPS, persisted in `EditorHostSettings.yaml`
as `Display.FrameRateLimit` (zero means unlimited), applied immediately and
independently of VSync. Lifecycle throttles still limit startup/error frames to 30 Hz,
unfocused frames to 10 Hz and minimized service frames to 5 Hz. Game running
frames retain their presentation policy; Game startup uses 30 Hz and minimized
frames use 20 Hz. Event waits wake for native input, while the monotonic deadline
prevents an input storm from bypassing render pacing. Modal continuations do not
wait or advance operations/simulation. Automated tick counts count real running
ticks, excluding event-only pacing iterations and canceled startup.

`FEngineLoop::Tick()` measures and clamps real frame delta time before calling
`DEngine::Tick()`. Active game worlds receive an `FWorldTickContext` containing
delta time and the Engine-owned raw input snapshot, then route admitted stable
Actor and Component Tick functions through serial PrePhysics, Physics, and
PostPhysics groups. The local player controller evaluates its session-owned
[input actions](../Gameplay/InputActions.md) and translates the logical snapshot
into bounded pawn-control intent. Raw one-frame transitions are cleared only after
the World call, so one advancing tick or single-step can observe each edge at
most once.

Before host startup advancement and `DEngine::Tick()`, the engine pumps low-priority
`GameThreadDeferred` continuations using the configured item and time budgets.
It then calls the Engine-owned asset-service completion pump. These precede host
advancement and admitted simulation; the frame counter advances for loading
frames as well as running frames. Together these are the normal-frame GameThread
completion safe point, in this fixed order. Deferred callbacks are not
frame-critical synchronization, do not run inline at submission, and may be
carried into a later frame when the budget is exhausted. Asset-service mailboxes
retain their own admission, payload, frame-budget, explicit-wait, and shutdown
drain policies rather than storing large results in the deferred executor.

Launch keeps the frame render decision visible in `FEngineLoop::Tick()`, while
its private `EngineFrame` component owns begin/end render-thread callbacks, UI
and viewport submission, end-frame synchronization, and the render counter.
Startup uses the same render-frame path; available viewports determine scene
submission, while host readiness controls simulation admission. Modal continuation
frames do not reenter startup advancement or simulation.
Responsibility-specific diagnostics own editor PIE, native gameplay, and
task-scheduler retained state. Typed native-crash phases are resolved at the
argument boundary rather than compared as arbitrary strings by the loop. These
components do not change
`FEngineLoop::Exit()` ownership of explicit process shutdown ordering.

MonaCore owns only reusable widget, event, backend, and display-source
contracts. Mona owns the concrete application, native windows, window renderer,
and higher-level viewport widgets. Engine exposes MonaCore display-source
contracts publicly and links Mona privately for its concrete application and
window integration; MonaCore and Mona never depend on Engine or MonaImGui.

On Windows, an operating-system window move/resize loop may remain inside the
outer GLFW event pump while dispatching native messages. ApplicationCore
subclasses every GLFW window before later viewport hooks. `WM_MOVING` classifies
a pure move and keeps the window-owned continuation timer disabled, leaving the
UI thread and its native message queue available for pointer-driven position
updates while DWM moves the last presented surface. After each custom-frame
`WM_MOVING` update, ApplicationCore flushes that calling window's queued DWM
changes so visible movement is paced by desktop composition instead of
graphics-driver delivery jitter. Repeated min/max
validation during that move also stays on the native fast path instead of
querying shell app bars. `WM_SIZING` starts a 16 ms continuation timer that
requests Launch work. Launch accepts that non-owning callback only while the
ordinary frame is in its platform-event phase, runs the same game, deferred
work, UI, rendering, garbage-collection, statistics, and profiling body without
polling native events again, and rejects callbacks during that body or during
shutdown. Sizing and final continuations drain render and RHI work because
Windows cannot advance the surface extent until the callback returns. The exit
message stops any sizing timer and requests one final continuation; window
destruction removes the timer and WndProc hook before GLFW destroys the native
handle. Launch clears callback admission before consumer detachment.

The same function owns the CPU-profiler frame mark and stable top-level zones.
Core forwards engine-owned thread names and queued-task execution through the
profiler-neutral surface in `Profiling/Profiling.h`.

World play states, native gameplay roles, level transitions, Actor and
Component dispatch, and lifecycle mutation are defined by
[Level System](../World/LevelSystem.md). Tick groups, same-frame mutation,
cancellation, ordering, and World admission are defined by
[Tick Scheduling](../World/TickScheduling.md). Editor-hosted sessions add the
isolation and restoration rules in
[Play In Editor Architecture](../../Editor/Architecture/LevelEditing/PlayInEditorArchitecture.md).

## Logging Integration

`PreInit()` loads application configuration before logger startup and emits
runtime-storage preparation warnings only after the logger is ready. The process runner owns
conditional finalization after engine exit. Record ordering, bounded admission,
structured history, sink durability, and shutdown behavior are defined by
[Logging](../Core/Logging.md).

The validated process request accepts the unique `--startup-diagnostics` flag
before subsystem startup. By default only the first-presentation/host-ready
summary is emitted; detailed milestone and object-load collection is opt-in.
See [CPU profiling](../../Development/Build/Profiling.md#startup-and-load-diagnostics).

## Module Integration

Projects owns project descriptors, validated current-project context, and edit
ownership. Launch selects history restoration only for Editor startup; ordinary
Game/tool descriptor loading does not restore history. Editor, Game and the
asset tool call IProjectManager::LoadModulesForProject for ordinary roots before
their existing content/provider consumers. Core executes native module loads
and validates/publishes mount values without reading `.dproject` fields.
The principal project interfaces follow UE bool/output-parameter conventions;
internal structured errors remain implementation choices. See
[Project metadata and management](../../Workspace/Plugins.md#ordinary-project-management).

Relaunch requests retain detached argument values in Projects and are consumed
once by Launch after the ordinary exit protocol. MainFrame/LevelEditor retain
their existing unsaved-work vetoes. The actual child-process launch and wait-for-
previous-process argument are Launch responsibilities.

Launch loads modules by logical name using
`<RuntimeVariant>-<ModuleName>.dll`. Cross-module changes must preserve both
CMake dependencies and runtime load-order expectations. Typed feature
invocation, owner-bound asynchronous drain, state transitions, failure
categories, and physical unload are defined by
[Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md).
This document owns only the module pass's placement in startup and process exit.

## Rendering And UI Integration

RHI, VulkanRHI, RenderCore, Renderer, ApplicationCore, Mona, and an optional UI
backend participate in the startup and exit order defined here. Launch, rather
than Mona, owns concrete backend selection. Standalone runtime
window ownership belongs to `DGameEngine`; editor host and workspace ownership
belongs to editor systems. Viewport composition is defined by
[Viewport Rendering](../Rendering/Pipeline/ViewportRendering.md), and render/RHI command
recording, replay, flush, and completion are defined by
[RHI Command Execution](../Rendering/RHI/RHICommandExecution.md).

## Engine Exit Protocol

Callbacks request host retirement through `DEngine::RequestShutdown()` and request
GC through `RequestGarbageCollection()`. They never execute host cleanup or immediate
GC. After the complete interactive frame (including native/UI callbacks) returns,
Launch services host retirement with `PreExit()` and requests process exit. Failed
Engine initialization also receives explicit `PreExit()` after `Init` returns.
Host methods and callback wrappers do not perform cleanup on scope exit.


`FEngineLoop::Exit()` is the single process-level ordering owner. Launch first retires asset compilation and calls host PreExit,
which closes subsystem work, retires World/editor consumers and deinitializes
services while base endpoints remain available. It then shuts down the selected UI backend, which unregisters its exact backend instance
and releases backend state. Mona's module shutdown callback then closes its
windows and renderer, while the stopped Mona module instance remains loaded
through process exit. The ordinary post-object-drain module pass skips it. The function expresses the
shutdown order directly:

| Step | Boundary |
| --- | --- |
| Stop asset compilation | Close every compiling manager, finish accepted object publication in reverse canonical-name order, and release provider values before Core task admission closes. |
| Retire host services | After asset compilation retires, prepare the engine host before UI/backend, cooked-mesh and task teardown; see [Subsystems](../Engine/Subsystems.md). |
| Detach render consumers | Shut down the selected UI backend, then Mona to destroy windows and viewports and detach world, preview, thumbnail, and scene consumers. |
| Release Engine defaults | After Engine consumer detachment, stop default-material bindings and release the retained asset/proxy before Engine shutdown. |
| Release class defaults | Clear `DClass` ownership derived-first before the first GC; reflection cleanup is owned by this explicit exit stage. |
| Drain objects | Release roots, run `partial GC -> render flush -> full purge -> final GC`, and require zero deferred object destruction. |
| Shut down modules | Call module Shutdown in reverse Startup-completion order after object cleanup; keep task executors and render/RHI threads available for module-owned waits. |
| Close render admission | Enqueue the final RenderCore audit while admission is still open, then atomically close it. |
| Stop rendering | Stop the rendering thread after accepted commands, deferred C++ cleanup, and its final RHI deletion submission drain. The RHI thread remains alive. |
| Stop RHI | Atomically install backend shutdown with RHI admission `Draining`, wait its exact serial, then stop and join the RHI thread. |
| Stop CPU work | After render/RHI teardown, shut down both CPU and blocking-I/O pools through the process [task system](../Core/TaskSystem.md) in `Drain` mode. Keep task-event admission open until all render fences have retired. |
| Close the application | Shut down the platform application and publish successful process termination. |

There is no global exit phase or pre-exit callback registry. Each owner closes
its own admission as part of its ordinary shutdown and releases its own
references:

- resource-owning `DObject` instances submit release from `BeginDestroy()` and
  report incomplete local fences through destruction readiness;
- process, subsystem, and module-static owners release through subsystem
  shutdown or their owning module's `ShutdownModule()`;
- consumers retain stable counted RHI references or non-owning snapshots only
  while their component, viewport, scene, or preview owner remains attached.

CPU-work owners close admission and join work that can publish render/RHI commands
during host/service or module teardown, before render admission closes. The task
system remains open through render/RHI teardown because render fences create
task events. Standalone future readiness and observer events do not replace
owner producer joins; accepted task/command captures must retire before their
resources or modules are released. Work that accesses objects must finish during host/service
or object teardown before those objects are collected; retaining the executors
for later module cleanup does not extend object lifetimes. Owners are responsible
for their manager mailboxes, providers, caches, objects, render and RHI work. The
[CPU Task System](../Core/TaskSystem.md) defines process shutdown, continuation dispatch,
pumping, waits and diagnostics; this document defines where that process-level
boundary occurs.

The shutdown object drain starts with partial collection to begin object-owned
release, then flushes accepted render work before fully draining pending destruction.
A final full collection handles objects retired by release callbacks. These full
drains require readiness to progress independently of the blocked Game Thread;
caller-owned fences and admissions must already be released. The collector does
not hide incomplete destruction behind a successful full-purge return. The final
pending-destruction assertion remains before module shutdown. See the
[pending destruction contract](../ObjectSystem/GarbageCollection.md#pending-destruction).

Module shutdown is a separate ownership channel from `DObject` destruction.
Modules shut down in reverse Startup-completion order after the object channel is empty.
The general pass retains DLL mappings; physical runtime unload is opt-in.
Their callbacks may still submit resource release commands because render
admission remains open until every module callback returns. The final RenderCore
command then validates the registry and deferred cleanup queue; it never sweeps
unknown resources to make shutdown pass.

CoreDObject installs the newly-loaded-object callback during `DObjectInit` so
reflected registration and CDO creation precede `StartupModule()`. It does not
install a module pre-shutdown drain. Process exit explicitly releases all class
and struct defaults before shutdown GC. Runtime DLL unload is disabled by default;
a reflected module must implement a complete object/type teardown protocol before
opting in. `FModuleManager` does not supply that protocol.

## Render And RHI Shutdown Integration

Generic RenderCore resource state, deferred C++ cleanup, producer teardown, and
registry auditing are defined by
[Render Resource Lifecycle](../Rendering/Pipeline/RenderResourceLifecycle.md).
Render-command admission, accepted-work drain, RHI deferred deletion, the
terminal backend marker, and render/RHI thread shutdown are defined by
[RHI Command Execution](../Rendering/RHI/RHICommandExecution.md). This document
owns their placement relative to object and module teardown through the exit
protocol above.

## Validation Expectations

- UI or rendering changes require a successful DurinEditor build and runtime
  smoke test, not compilation alone.
- Rendering lifecycle validation must cover command-admission close, accepted
  work drain, resource registry and C++ cleanup drain, RHI deferred deletion,
  rendering-thread stop, and repeated clean process exit.
- Runtime validation covers both the normal threaded mode and the explicit
  `DURIN_RHI_EXECUTION=inline` diagnostic mode. Runtime path assumptions and
  output layout are documented in [Build And Run](../../Development/Build/BuildAndRun.md).

## Related Documentation

- [Logging](../Core/Logging.md)
- [Native Crash Diagnostics](../Core/NativeCrashDiagnostics.md)
- [CPU Task System](../Core/TaskSystem.md)
- [Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md)
- [Level System](../World/LevelSystem.md)
- [Viewport Rendering](../Rendering/Pipeline/ViewportRendering.md)
- [Render Resource Lifecycle](../Rendering/Pipeline/RenderResourceLifecycle.md)
- [RHI Command Execution](../Rendering/RHI/RHICommandExecution.md)
- [Play In Editor Architecture](../../Editor/Architecture/LevelEditing/PlayInEditorArchitecture.md)
- [Build System](../../Development/Build/BuildSystem.md)
- [Runtime Variants](../../Development/Build/RuntimeVariants.md)
- [CPU Profiling](../../Development/Build/Profiling.md)
