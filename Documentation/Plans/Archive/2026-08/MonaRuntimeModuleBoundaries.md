# Mona Runtime Module Boundaries Plan

Summary: Re-establish MonaCore as the reusable UI foundation, make Engine's Mona runtime integration explicit, and keep MonaImGui outside the game runtime module closure.

Last reviewed: 2026-08-21

Status: Archived
Completed: 2026-08-21

## Current Status

All stages and acceptance gates are complete. MonaCore now contains only the selected reusable
contracts, while Mona owns application, native-window, window-renderer, and
viewport integration. The public include spellings remained stable without
forwarding headers. `MonaCoreBoundaryTests` proves that linking and loading
MonaCore alone does not create an application, initialize a dynamic RHI, or
install a UI backend; `MonaViewportTests` proves the complete Mona consumer
still links and its backend-absent/texture bridge behavior remains valid. Engine
now declares Mona privately while retaining MonaCore publicly, and the
descriptor boundary assertion plus the 104-case viewport suite protect that
direction and existing viewport/input integration.

Launch now owns the concrete editor backend selection and unloads MonaImGui
before Mona. Generic Mona contains no MonaImGui name or dependency. Backend
registration rejects duplicate and mismatched owners, while five Mona viewport
cases prove backend-absent no-op frames and exactly-once forwarding. Two bounded
hidden editor runs completed normal startup and shutdown; both logs show
MonaImGui unloaded before Mona shutdown.

Runtime-variant selection remains the packaging foundation: MonaImGui is an
optional Launch dependency, editor modules select it into the DurinEditor
closure, and DurinGame does not select it.

Final qualification passed the MonaCore-only and Mona viewport contracts, the
104-case viewport suite, the 141-case concurrency/module-retirement suite, and
the six-case native-window suite. Full Debug Editor and Debug Game builds
passed. Visible Editor PIE and Game native-lifecycle smokes passed, as did
hidden Editor/Game startup and shutdown in threaded and inline RHI modes. The
generated DurinEditor closure and runtime directory contain MonaImGui; the
generated DurinGame closure, runtime directory, and all three Game smoke logs
contain no MonaImGui module or load attempt. The stable ownership and lifecycle
rules are published in Code Modules, Runtime Variants, Runtime Lifecycle, and
Viewport Rendering.

### Stage 0 Inventory and Frozen Classification

| Owner | Public surface and implementation |
| --- | --- |
| MonaCore | `MWidget`, `FMonaEventHandler`, `IMonaUIBackend`, `IViewportDisplaySource`, backend registration/access, and foundation forward declarations |
| Mona | `FMonaApplication`, `MWindow`, `FMonaRenderer`, `FMonaRHIRenderer`, `FMonaViewportInfo`, `MViewport`, `MFunctionWidget`, native-window helpers, module lifetime, and frame facade |
| MonaImGui | ImGui context, Mona/platform adapter, RHI adapter, texture bridge implementation, styling, editor helpers, and its module-owned backend instance |
| Launch | Required editor backend selection and failure reporting, backend-before-Mona shutdown, and no-backend game composition |

- Expected module edges are MonaCore public `Core`, `RHI`, and
  `ApplicationCore`; Mona public `Core`, `RHI`, `ApplicationCore`, and
  `MonaCore`, with private `RenderCore`; Engine public `MonaCore` and private
  `Mona`; MonaImGui public `MonaCore` and private `Mona`; Launch private
  `MonaCore` and `Mona`, with optional private `MonaImGui`.
- All current application/window/renderer consumers already declare Mona except
  Engine, which will gain the selected private edge, and TextureEditor, whose
  concrete runtime use is supplied only through existing dependencies.
- DurinEditor treats a missing or failed MonaImGui load as fatal after Mona
  renderer initialization. Failure unwinds the renderer and then the initialized
  process owners. Headless editor startup retains this same module requirement;
  window visibility suppression is not a backend-selection mode.
- DurinGame pumps platform events, ticks gameplay and Mona application state,
  redraws and presents scene viewports, handles input and resize, and shuts down
  with `NewFrame` and `Render` as no-ops when no backend is registered.
- Backend installation accepts one instance only; removal must identify that
  same instance. Duplicate, mismatched, or late operations fail without
  replacing or retaining a dangling active pointer.
- Existing tests affected by the boundary are the viewport targets, UI/editor
  targets, module lifecycle coverage, and runtime-variant dependency tests. New
  focused MonaCore/Mona boundary coverage will be registered with the native
  test workflow.

## Goal

Make Mona reusable by Engine, editor hosts, games, and standalone Durin tools
without depending on the Engine module, while expressing Engine's use of the
complete Mona runtime honestly and preventing MonaImGui from entering a game
runtime unless a future target explicitly selects it.

## Scope

- Define and enforce the responsibilities of MonaCore, Mona, and MonaImGui.
- Move concrete application, native-window, RHI-renderer, and runtime lifecycle
  ownership out of MonaCore where required by the selected boundary.
- Keep low-level widget, event, layout, and backend-facing contracts in
  MonaCore when they do not require Mona runtime ownership.
- Declare Engine's public MonaCore contract dependency and private Mona runtime
  dependency according to actual header and implementation use.
- Remove knowledge of the concrete `MonaImGui` module name and editor selection
  policy from generic Mona code.
- Preserve editor startup, game window and scene presentation, headless startup,
  resize, input, and shutdown behavior.
- Prove that DurinGame does not build, link, load, or package MonaImGui, while
  DurinEditor still initializes the backend when UI presentation is enabled.

## Non-Goals

- Build a retained-mode game UI renderer, UMG-equivalent layer, or new runtime
  widget library.
- Make Mona independent of Durin Core, ApplicationCore, RHI, or RenderCore.
- Replace ImGui or change existing editor workflows and visual design.
- Introduce an `EngineMona` bridge module before a second engine/UI integration
  or a headless dependency requirement demonstrates that the extra binary
  boundary is needed.
- Rename Mona, MonaCore, or MonaImGui solely to communicate the new boundary.
- Redesign the renderer frame graph, swapchain ownership, or native application
  abstraction beyond moves required to establish module ownership.
- Make MonaImGui available in DurinGame by default.

## Design Decisions and Invariants

### Dependency direction

- MonaCore and Mona never depend on Engine, DurinEd, Launch, or MonaImGui.
- Mona depends publicly on MonaCore and privately on the Durin platform and
  rendering modules required by its implementation.
- Engine keeps MonaCore as a public dependency only where Engine public headers
  expose or derive from MonaCore contracts. Engine declares Mona as a private
  dependency for application, window, input-routing, and viewport integration.
- MonaImGui depends on the Mona contracts it implements or consumes; neither
  MonaCore nor Mona acquires a reverse dependency on MonaImGui.
- Launch and editor roots compose Engine, Mona, and the selected UI backend.
  Optional dependencies never become a substitute for an undeclared source
  include or link requirement.

### Module ownership

- MonaCore owns reusable UI primitives and contracts: widget identity and tree
  semantics, UI events, layout primitives as they are introduced, forward
  declarations, and backend-facing interfaces that do not own the process or a
  native window.
- Mona owns `FMonaApplication`, concrete Mona window/native-window integration,
  RHI-backed Mona renderer implementation, higher-level viewport widgets,
  process/runtime hooks, and the complete application-facing facade.
- MonaImGui owns ImGui context, input translation, texture registration,
  renderer backend, editor widgets, property presentation helpers, styling,
  and third-party ImGui headers.
- A source move is not complete until export macros, public includes, module
  dependencies, reflection inputs, tests, and runtime load order agree with the
  new owner.

### Backend selection and runtime behavior

- Generic Mona code never loads, unloads, or branches on the string
  `"MonaImGui"`.
- The host composition layer selects and loads an available UI backend before
  backend-dependent frames are submitted, and shuts it down before Mona window
  and renderer teardown.
- DurinEditor retains its current required/optional behavior for visible and
  headless startup. Any change to failure policy must be recorded in Stage 0
  before implementation.
- DurinGame initializes Mona application, window, input, RHI presentation, and
  scene viewport services without requiring an active UI backend. Backend frame
  hooks are explicit no-ops when none is installed.
- A future game UI backend must be selected as its own module; it must not make
  MonaImGui an implicit runtime dependency.

### Lifecycle and failure

- Launch remains the process-level startup and shutdown ordering owner.
- Mona owns the lifetime of Mona application/window/rendering state after the
  host admits it; the selected backend owns its own state and unregisters before
  Mona rendering and windows are destroyed.
- Partial startup failure unwinds only initialized participants in reverse
  order and never leaves an active backend pointing at a destroyed renderer or
  window.
- Existing first-presentation adoption, render-command admission, resize, and
  reverse module-shutdown contracts remain unchanged.

## Current Foundations and Gaps

| Area | Existing foundation | Gap |
| --- | --- | --- |
| Runtime variants | DurinEditor and DurinGame select root modules and resolve dependency closures independently. | There is no qualification proving MonaImGui remains absent from every game output and load path after the refactor. |
| Optional backend | Launch lists MonaImGui as an optional private dependency. | Generic Mona still names, loads, and unloads MonaImGui under `DURIN_WITH_EDITOR`. |
| MonaCore | Owns widgets, events, application, windows, backend interfaces, and an RHI renderer. | Concrete runtime integration makes Engine's MonaCore-only dependency misleading and leaves MonaCore broader than a reusable foundation. |
| Mona | Owns module hooks, rendering initialization, `MViewport`, and `MFunctionWidget`. | The module is a thin composition layer while core application and window responsibilities live below it. |
| Engine integration | Engine public viewport contracts use MonaCore and Engine implementation uses the Mona application/window system. | `Engine.dmodule` does not declare the full Mona runtime dependency used semantically by Engine. |
| Runtime UI | Game startup can operate without loading MonaImGui. | No explicit backend-absent contract proves scene presentation, input, resize, and shutdown remain valid. |
| Documentation | Runtime lifecycle and viewport documents describe the current startup order. | They do not define the reusable Mona boundary or host-owned backend selection rule. |

## Implementation Stages

### Stage 0: Freeze the ownership and lifecycle map

- [x] Inventory every MonaCore, Mona, and MonaImGui public type, source include,
  module dependency, dynamic module operation, runtime-variant root, and native
  test target affected by the boundary.
- [x] Classify each public type as reusable UI foundation, Mona runtime
  integration, concrete ImGui backend, or host composition; record any type
  that cannot move without a compatibility shim.
- [x] Freeze the editor visible/headless backend startup policy, including the
  required response to missing MonaImGui, backend initialization failure, and
  shutdown after partial initialization.
- [x] Freeze the backend-absent DurinGame frame contract for event pumping,
  application tick, scene viewport presentation, resize, input, and no-op UI
  frame calls.
- [x] Record the exact public/private dependency result expected for MonaCore,
  Mona, Engine, Launch, MonaImGui, and editor consumers before moving code.
- [x] Identify public include paths that require compatibility forwarding and
  set a bounded removal policy rather than silently breaking repository-owned
  consumers.

#### Acceptance Gate

- Every affected type, include, module edge, load operation, runtime variant,
  and test has one selected owner; editor failure behavior and game
  backend-absent behavior are unambiguous, and no open decision can reverse the
  dependency direction during implementation.

### Stage 1: Establish the MonaCore and Mona source boundary

- [x] Move `FMonaApplication`, concrete window/native-window integration, and
  the RHI-backed Mona renderer implementation from MonaCore to Mona.
- [x] Keep or move backend contracts according to the Stage 0 classification,
  ensuring MonaCore interfaces do not acquire process-lifecycle ownership.
- [x] Keep higher-level viewport and function widgets in Mona unless Stage 0
  proves they are backend-neutral primitives required by an independent
  MonaCore consumer.
- [x] Update export macros, forward declarations, umbrella headers, include
  paths, and public/private dependency declarations without using accidental
  transitive includes.
- [x] Add focused compile/link tests for MonaCore-only consumers and complete
  Mona runtime consumers.
- [x] Verify MonaCore can be loaded and its contract tests can run without
  starting a Mona application, creating a native window, or initializing RHI
  presentation.

#### Acceptance Gate

- MonaCore exposes only the selected reusable foundation, Mona owns the complete
  runtime integration, both modules compile without dependency cycles, and a
  MonaCore-only test produces no application, window, rendering, or backend
  side effects.

### Stage 2: Make Engine integration explicit

- [x] Update Engine public headers to include only the MonaCore contracts they
  expose and update Engine implementation to include Mona runtime types from
  their canonical Mona paths.
- [x] Declare MonaCore as an Engine public dependency only for surviving public
  API use and declare Mona as an Engine private dependency for concrete runtime
  integration.
- [x] Preserve `DGameEngine`, editor engine, scene viewport, primary-window
  adoption, raw input, mouse capture, and resize behavior through the ownership
  move.
- [x] Remove direct MonaCore application/window implementation use from Engine
  and reject compatibility headers as the permanent dependency route.
- [x] Add dependency-boundary tests or generated-graph assertions that fail if
  Engine implementation uses Mona without declaring it or if Mona gains an
  Engine dependency.

#### Acceptance Gate

- Engine's descriptors and includes match its actual use: public MonaCore
  contracts remain transitively available where required, concrete UI runtime
  use is supplied by private Mona linkage, Mona remains independently usable,
  and existing viewport/input behavior passes focused tests.

### Stage 3: Move concrete backend selection to the host

- [x] Remove all concrete `MonaImGui` module-name knowledge and editor-only load
  branches from Mona.
- [x] Give Launch or the selected editor host explicit ownership of backend
  selection, load, initialization, failure reporting, and pre-Mona shutdown in
  the lifecycle order frozen by Stage 0.
- [x] Preserve MonaImGui's optional module dependency status so its absence does
  not break linking or runtime symbol resolution in DurinGame.
- [x] Make backend registration and replacement explicit and reject duplicate,
  late, or teardown-racing registrations without leaving global dangling
  pointers.
- [x] Prove a backend-absent Mona frame remains valid for DurinGame and that
  editor frames still execute MonaImGui exactly once per admitted frame.
- [x] Prove partial editor startup and repeated shutdown unload backend state
  before Mona renderer, viewport, window, ApplicationCore, RenderCore, and RHI
  teardown.

#### Acceptance Gate

- Generic Mona contains no MonaImGui name, include, link edge, or editor policy;
  the editor host selects MonaImGui successfully, the game host selects no UI
  backend, and both paths start, render/present as applicable, and shut down in
  the established order.

### Stage 4: Qualify runtime variants and publish the contract

- [x] Run focused Mona, viewport, input, window, module-lifecycle, and runtime
  dependency tests through the repository testing workflow.
- [x] Build the DurinEditor and DurinGame variants from clean generated module
  metadata and inspect their resolved module closures and produced binaries.
- [x] Run a visible DurinEditor smoke covering startup, editor UI, scene
  viewport, resize, input, stable frames, and orderly shutdown.
- [x] Run a visible DurinGame smoke covering startup, scene presentation,
  resize, input, stable frames, and orderly shutdown without loading or
  publishing MonaImGui.
- [x] Run headless startup/exit coverage and both dedicated-RHI and
  `DURIN_RHI_EXECUTION=inline` modes required by the runtime lifecycle contract.
- [x] Publish the stable Mona module boundary, backend selection, runtime
  variant, startup, frame, and shutdown rules in the owning Runtime and
  Development documents; update Code Modules after implementation matches the
  new ownership.
- [x] Run changed/all documentation and all-plan validation after lasting
  contracts are updated.

#### Acceptance Gate

- Focused tests, both runtime-variant builds, editor/game/headless runtime
  smokes, both RHI execution modes, binary/module-closure inspection, shutdown
  checks, and documentation validation pass; DurinGame contains no MonaImGui
  binary or runtime load attempt.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| MonaCore-only consumer | Builds and runs contract tests without Mona, native windows, RHI presentation, or backend startup | MonaCore compile/link and unit tests |
| Mona standalone runtime | Creates application/window/rendering services without Engine and can shut them down cleanly | Mona integration test or standalone fixture |
| Engine public headers | Compile from a consumer with declared transitive MonaCore contracts and no undeclared Mona include | Header compile tests |
| Engine implementation | Links Mona privately and exercises game/editor viewport and input integration | Engine viewport and lifecycle tests |
| Editor backend selection | Host loads and registers MonaImGui before UI frames and unregisters it before Mona teardown | Editor startup and shutdown tests |
| Game backend absence | Window, input, scene rendering, resize, and shutdown work with no active UI backend | DurinGame runtime smoke |
| Variant closure | DurinEditor contains MonaImGui; DurinGame neither resolves nor publishes it | Generated metadata and binary inspection |
| Failure unwind | Missing/failed editor backend and partial Mona initialization unwind only initialized owners in reverse order | Failure-injection lifecycle tests |
| Resize and presentation | First-viewport adoption, interactive resize, swapchain recreation, and offscreen display remain unchanged | Viewport tests and visible runtime smoke |
| Module direction | MonaCore/Mona do not depend on Engine or MonaImGui; Engine declares every Mona edge it uses | Descriptor/schema and dependency-graph tests |
| Shutdown | Backend, UI consumers, windows, render resources, modules, rendering thread, and RHI retire in the established order | Repeated-exit tests and validation-enabled smoke |

## Definition of Done

- MonaCore and Mona have distinct, enforced foundation/runtime ownership and
  neither depends on Engine or MonaImGui.
- Engine exposes only required MonaCore contracts publicly and declares its
  complete Mona runtime use privately.
- Concrete MonaImGui selection belongs to the editor host; generic Mona has no
  knowledge of the backend module name or editor policy.
- DurinEditor preserves its UI behavior and DurinGame preserves windowed scene
  presentation without building, linking, loading, or packaging MonaImGui.
- Mona can initialize and shut down without Engine, and MonaCore can be used
  without starting Mona runtime services.
- Startup failure, resize, rendering, input, module unloading, and shutdown
  preserve their established ordering and lifetime contracts.
- Focused tests, both variant builds, runtime smokes, dependency inspection,
  and documentation validation pass, and lasting behavior is documented under
  the owning contract domains.

## Deferred Follow-ups

- A retained-mode game UI renderer or higher-level runtime UI module.
- A second non-ImGui backend and policy for selecting among multiple available
  backends.
- Removing compatibility forwarding headers after all external or project
  consumers have migrated.
- Splitting an `EngineMona` bridge if a future headless Engine product or second
  UI framework requires Engine binaries with no Mona runtime dependency.
- Making Mona portable outside Durin's Core, ApplicationCore, RHI, and
  RenderCore foundations.
- Renaming modules after the stabilized responsibilities demonstrate that a
  different public vocabulary materially improves routing.

## Related Documentation

- [Code Modules](../../../Workspace/CodeModules.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Runtime Variants](../../../Development/Build/RuntimeVariants.md)
- [Build System](../../../Development/Build/BuildSystem.md)
- [Window Frames](../../../Runtime/Core/WindowFrames.md)
- [Modular Features and Module Retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Test Execution](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Engine.dproject`
- `Engine/Source/Runtime/MonaCore/MonaCore.dmodule`
- `Engine/Source/Runtime/MonaCore/Public/Application/MonaApplication.h`
- `Engine/Source/Runtime/MonaCore/Public/MonaUIBackend.h`
- `Engine/Source/Runtime/MonaCore/Public/Rendering/MonaRHIRenderer.h`
- `Engine/Source/Runtime/MonaCore/Public/Widgets/MWindow.h`
- `Engine/Source/Runtime/Mona/Mona.dmodule`
- `Engine/Source/Runtime/Mona/Private/Misc/MonaGlobals.cpp`
- `Engine/Source/Runtime/Mona/Public/Widgets/MViewport.h`
- `Engine/Source/Runtime/MonaImGui/MonaImGui.dmodule`
- `Engine/Source/Runtime/Launch/Launch.dmodule`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineFrame.cpp`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`
- `Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp`
- `Engine/Tests/Native/EngineTests/CMake/WorldPhysicsAndViewportTests.cmake`
