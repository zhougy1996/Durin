# Launch Process Boundary Refactor Plan

Summary: Refactor Launch into a typed command-line boundary, an explicit process runner, a private engine loop, and responsibility-named private components without obscuring startup or shutdown order.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

Architecture review completed on 2026-08-11. No implementation has started.
The current process entry in `Engine/Source/Runtime/Launch/Private/Launch.cpp`
is 245 lines and owns four distinct concerns:

- process crash-context and handler installation;
- parsing and partially validating eighteen command-line option spellings;
- applying process, project, automation, startup-command, and diagnostic policy;
- driving engine initialization, ticking, shutdown, editor relaunch, logger
  shutdown, and crash-handler restoration.

The command-line loop spans lines 21-146, but moving that loop into a helper
would not by itself repair the boundary. Parsed values currently write directly
into `FEngineStartupParams`, while validation and side effects remain interleaved
with process lifetime. Unknown arguments are collected but rejected only when a
startup command is present; an ordinary launch silently discards them. Duplicate
and malformed options also have inconsistent behavior: the two supported
`--project` spellings can both be present, repeated scalar options may overwrite
or become silently unrecognized, and command-line contract failures return a
mix of process exit codes 1 and 2 without a user-facing diagnostic.

`FEngineStartupParams` now mixes normal host configuration with four lifecycle
smokes and native-crash injection state. `FEngineLoop` copies that diagnostic
state into private booleans and string views. Its public header is included only
by Launch's own entry and implementation translation units, so the type and the
global `GEngineLoop` are exposed more broadly than their actual use requires.
The completed predecessor plan reduced `LaunchEngineLoop.cpp` to 232 lines, but
subsequent gameplay and crash diagnostics have already grown it to 308 lines.
This demonstrates that extraction alone does not prevent new process options
from flowing through the engine loop.

The executable boundary is also indirect. `DurinLauncher` is created from the
zero-byte `Private/Hello.cpp`, links the shared `Launch` module, and obtains the
exported C runtime `main` symbol from that DLL. The arrangement works, but it
hides executable ownership and leaves the launcher source name unrelated to its
purpose.

Private Launch filenames repeat the module name without consistently describing
their role. `LaunchFrame` owns render-frame submission, `LaunchGameplayValidation`
contains two different lifecycle smokes, and `LaunchTaskSchedulerValidation`
is specifically an opt-in shutdown smoke. `LaunchRuntimeStorage` additionally
knows the literal filenames of ImGui, MainFrame, LevelEditor, and Core project
history settings even though those features own their load/save behavior.

Existing focused coverage exercises runtime-storage migration and Windows
native-crash policy/characterization. There is no unit-level coverage for the
Launch command-line grammar, duplicate handling, conflict rules, typed mapping,
or early-exit diagnostics.

## Goal

- Make the executable `main()` a direct, minimal delegate to one exported
  Launch process function.
- Parse all process arguments into one owning, typed request without changing
  globals, installing feature state, waiting on another process, or starting
  engine services.
- Keep command grammar, semantic validation, side-effect application, engine
  lifetime, automation, and diagnostics as distinct reviewable boundaries.
- Preserve the explicit startup, frame, and shutdown order documented for
  `FEngineLoop` while making failure cleanup complete and consistent.
- Keep normal engine startup parameters free of qualification and crash-fixture
  state.
- Rename private files and types by responsibility while retaining names whose
  module identity is part of a real public or build-system contract.
- Establish deterministic tests for accepted, rejected, conflicting, and
  unavailable command-line forms before the new boundary becomes authoritative.

## Scope

- Move the C runtime entrypoint into the `DurinLauncher` executable source and
  replace `Hello.cpp` with a purpose-named `Main.cpp`.
- Export one narrow Launch function for the executable to call.
- Add a Launch-private typed argument model, pure parser, semantic validator,
  and error result.
- Separate normal host options, process coordination, automation/startup
  commands, and opt-in diagnostics in that model.
- Introduce an explicit process runner that applies a validated request and
  owns early-return cleanup.
- Move `FEngineLoop` and its startup input to Launch-private headers, remove the
  process-global `GEngineLoop`, and track enough lifecycle state for explicit,
  idempotent shutdown and failure unwind.
- Move diagnostic configuration and retained smoke state out of normal engine
  startup parameters while keeping named call sites at the required lifecycle
  boundaries.
- Rename and, where responsibilities differ, split private Launch components.
- Remove editor-feature filename knowledge from generic runtime-storage
  preparation and move each legacy migration to the earliest owning load path.
- Add focused parser/runner tests and preserve existing storage, crash,
  startup-command, project relaunch, frame, and lifecycle-smoke behavior.
- Update the runtime lifecycle, build/run argument documentation, and module
  routing descriptions after the implementation is stable.

## Non-Goals

- Renaming the `Launch` module. It remains the correct owner of process
  bootstrap and application lifetime.
- Renaming `LaunchAPI.h`; module API headers follow a repository-wide export
  macro convention.
- Adding CLI11, another third-party command-line framework, or a reflection-
  driven global option registry.
- Rewriting the Core startup-command handler registry or the feature handlers
  registered by editor modules.
- Introducing a generic startup/shutdown phase registry, callback bus, or
  subsystem auto-discovery mechanism.
- Reordering frame execution, render submission, asset completion pumping,
  garbage collection, or subsystem shutdown.
- Changing the semantics of project selection, editor relaunch, delayed
  startup-command handler admission, or the native-crash artifact format.
- Renaming `FPaths::LaunchDir()`, `LaunchSavedDir()`, `LaunchConfigsDir()`, or
  `LaunchLogsDir()` in this plan. Those 31 cross-module uses require a separate
  Core path-vocabulary change if `ExecutableDir`/`Runtime*Dir` terminology is
  adopted.
- Converting the Windows entrypoint to `wmain` or changing command-line Unicode
  behavior.
- General cleanup of standalone program parsers such as `DurinAssetTool`.

## Design Decisions and Invariants

### Executable and public boundary

- `Engine/Source/Editor/DurinLauncher/Private/Main.cpp` owns the actual C
  runtime `main(int, char**)` symbol.
- `main()` performs no parsing or subsystem work. It delegates to one exported
  function declared by Launch, tentatively `Durin::RunApplicationProcess(int,
  char**)`, and returns that function's result.
- The exported function remains narrow. The typed parser, engine loop, process
  runner, diagnostics, and storage components stay private to Launch.
- `LaunchAPI.h` continues to define `LAUNCH_API`; a responsibility-named public
  header declares the exported process function.

### Typed command-line request

- Parsing produces owned values. No `std::string_view` stored in the request or
  engine loop may depend on `argv` lifetime.
- The request separates at least these domains:

  | Domain | Current options | Consumer |
  | --- | --- | --- |
  | Process coordination | `--wait-for-process` | process runner before engine startup |
  | Normal host startup | `--project`, `--project-browser`, `--hidden-window` | project/host initialization |
  | Automation | `--exit-after-ticks`, startup command and repeated startup-command arguments | run loop controller |
  | Diagnostics | lifecycle smokes and `--native-crash-*` controls | named diagnostic components |

- The parser recognizes syntax and converts bounded scalar values. A separate
  semantic validation pass checks duplicates, required companions, conflicts,
  build availability, and empty values. Neither phase mutates global state.
- `--project=<path>` and `--project <path>` remain supported and normalize into
  one field. Any second project option, including the other spelling, is an
  error.
- Every scalar or flag option is accepted at most once. Only
  `--startup-command-arg=<value>` is repeatable.
- Startup-command arguments without exactly one non-empty startup-command name
  are rejected. Existing startup-command incompatibilities remain explicit
  semantic rules rather than ad hoc tests in `main()`.
- Unknown options are rejected for every launch mode. There is currently no
  downstream consumer for them, so silent acceptance does not provide a real
  extension boundary. A future passthrough must name its consumer and use an
  explicit delimiter or option rather than revive implicit swallowing.
- Command-line syntax, value, duplicate, conflict, and unavailable-option
  failures return exit code 2. Runtime/bootstrap failures return 1. Successful
  application completion returns the engine or startup-command result as it
  does today.
- Failures before logger initialization write one actionable diagnostic to
  stderr, including the option name and reason. They do not start the engine
  merely to obtain logging.
- This plan does not require a data-driven option table. Direct parsing remains
  acceptable when it writes only to the typed request and every branch has
  deterministic unit coverage. Shared low-level command-line primitives move
  to Core only after another program demonstrates matching grammar and error
  requirements.

### Side effects and process lifetime

- Crash-context initialization and crash-handler installation remain the first
  process operations so `process-entry` characterization retains coverage.
- Successful crash-handler installation immediately establishes an explicit
  lifetime guard or equivalent single cleanup path. Every ordinary return
  restores the previous handlers exactly once; native faults continue through
  the crash-safe handler rather than C++ unwinding.
- Crash diagnostic configuration, saved-root override, wait-for-process,
  startup-command publication, project/engine startup, and run-loop automation
  occur only after the complete request has parsed and validated.
- Logger shutdown remains after engine/module/application shutdown and after a
  pending editor relaunch attempt, so those paths can still report failures.
  The runner tracks whether logger initialization succeeded and never relies on
  an unqualified static destructor.
- The delayed startup-command rule remains: feature modules may register during
  ordinary initialization, and after 120 completed ticks the missing-handler
  case becomes terminal.
- Automated tick exit remains subordinate to an already requested engine exit,
  and startup-command result selection remains authoritative for the process
  result.

### Engine-loop ownership and rollback

- `FEngineLoop` is constructed locally by the process runner. There is no
  `GEngineLoop` because no external consumer exists.
- `EngineLoop.h` is Launch-private. A public include is added only if a concrete
  non-Launch consumer appears and requires a documented ABI contract.
- The loop records explicit lifecycle state sufficient to distinguish
  uninitialized, pre-initialized, initialized/running, shutting down, and
  exited states.
- `Shutdown()`/`Exit()` remains an explicit call and preserves the readable
  subsystem order. Lifecycle state makes repeated cleanup safe; it does not
  replace the ordered body with destructor magic or a generic phase registry.
- Each failed startup stage unwinds every earlier stage it admitted, including
  project authoring ownership and process services owned by the loop. The
  process runner owns only outer process resources such as crash-handler and
  logger finalization.
- Successful startup and ordinary shutdown retain current GameThread affinity,
  frame ordering, render-thread admission, asset-service drain, task-system
  drain, object retirement, module unload, RHI exit, and application shutdown
  invariants.

### Diagnostics boundary

- Normal host startup input contains only semantic production choices such as
  window suppression and project initialization.
- Native-crash fixtures use a typed phase enum after parsing; engine-loop code
  does not compare arbitrary user-provided strings at multiple sites.
- Task-scheduler, engine-asset-service, editor PIE, native gameplay, and crash
  diagnostics own their configuration and retained state in Launch-private
  components.
- Required lifecycle call sites remain named and visible in `PreInit()`,
  `Init()`, `Tick()`, and `Exit()`. A diagnostic facade may reduce parameter
  plumbing, but it must expose specific operations rather than a generic
  `OnPhase` callback.
- Shipping builds reject diagnostic-only options during semantic validation
  before any diagnostic side effect. Normal Shipping startup does not allocate
  diagnostic workload or retain smoke state.

### Naming and file layout

- Names are changed by responsibility, not by mechanically deleting `Launch`.
  The selected mapping is:

  | Current | Target |
  | --- | --- |
  | `DurinLauncher/Private/Hello.cpp` | `DurinLauncher/Private/Main.cpp` |
  | `Launch/Private/Launch.cpp` | `Launch/Private/ApplicationRunner.cpp` |
  | new parser mixed into `main()` | `Launch/Private/LaunchArguments.h/.cpp` |
  | `Launch/Public/LaunchEngineLoop.h` | `Launch/Private/EngineLoop.h` |
  | `Launch/Private/LaunchEngineLoop.cpp` | `Launch/Private/EngineLoop.cpp` |
  | `LaunchFrame.h/.cpp` | `EngineFrame.h/.cpp` |
  | `LaunchRuntimeStorage.h/.cpp` | `RuntimeStorage.h/.cpp` |
  | `LaunchTaskSchedulerValidation.h/.cpp` | `Diagnostics/TaskSchedulerLifecycleSmoke.h/.cpp` |
  | `LaunchGameplayValidation.h/.cpp` | split into `Diagnostics/EditorPIELifecycleSmoke.*` and `Diagnostics/NativeGameplayLifecycleSmoke.*` |

- `WindowsProcessCrashHandler` and `WindowsProcessCrashPolicy` already name
  their platform and responsibility and remain unchanged unless the diagnostic
  extraction reveals a tighter private folder boundary.
- Private type names follow the same rule. For example,
  `FLaunchRuntimeStorageResult` becomes `FRuntimeStoragePreparationResult`, and
  `FLaunchTaskSchedulerValidationState` becomes
  `FTaskSchedulerLifecycleSmokeState`.

### Runtime-storage ownership

- Generic runtime-storage preparation owns creation of the runtime Saved,
  Configs, and Logs roots; migration of the runtime app config and legacy log
  directory; app-config path selection; and pass-local warnings.
- It does not own literal feature settings for ImGui, MainFrame, LevelEditor,
  or ProjectHistory.
- Each feature migrates its legacy file at the earliest owning load path before
  reading the new location. Core owns ProjectHistory migration, MonaImGui owns
  `imgui.ini`, MainFrame owns editor host settings, and LevelEditor owns session
  settings.
- A small Core filesystem migration helper is permitted only if these owners
  otherwise duplicate the same checked rename/copy/remove contract. The helper
  must not know feature filenames or Launch policy.
- Migration remains idempotent, preserves an existing destination, and reports
  recoverable warnings after logging is available where possible.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Process entry | Launch already owns crash setup and the complete run loop. | Exported DLL `main`, zero-byte executable source, repeated early-return cleanup. |
| Command grammar | All current options are recognized in one visible loop. | No typed request, pure seam, uniform duplicate/error policy, early diagnostic contract, or parser tests. |
| Semantic startup | Project and hidden-window choices already enter `FEngineStartupParams`. | Automation and diagnostics share the same structure and grow the loop for every new fixture. |
| Engine lifecycle | Startup/frame/shutdown order is direct and documented. | Global loop, unnecessarily public header, partial-startup cleanup split across callers. |
| Diagnostics | Strong end-to-end smokes and native-crash characterization exist. | String phases and boolean plumbing are embedded in normal startup/loop state. |
| Runtime storage | Preparation has explicit result/warnings and focused tests. | Common bootstrap hard-codes settings owned by four different feature areas. |
| Naming | `LaunchAPI.h` and Windows crash files are precise. | Most other private filenames repeat the module rather than state their responsibility. |

## Implementation Stages

### Stage 0: Freeze the command-line and lifetime contract

- [ ] Inventory every repository-owned producer of Launch arguments, including
  DurinDevTool run/scene flows, project relaunch, native-crash child processes,
  build/run documentation, and runtime smoke commands.
- [ ] Record the current accepted forms, consumers, exit codes, conflicts, and
  side-effect order in a parser test matrix before moving implementation.
- [ ] Specify expected characterization cases for unknown ordinary arguments,
  repeated scalar options, mixed project spellings, empty scalar values,
  numeric zero/overflow/trailing text, startup-command companion rules, and
  Shipping-only rejection so Stage 1 can land them with the parser seam.
- [ ] Confirm that no external module or supported workflow consumes silently
  ignored arguments. If a real consumer is found, amend the plan with an
  explicit passthrough contract before implementation.
- [ ] Freeze process-entry crash installation, logger shutdown, editor relaunch,
  startup-command timeout, and automated-exit ordering as integration
  invariants.

#### Acceptance Gate

- The plan and test matrix name every supported option family and deliberate
  behavior change; no required workflow depends on implicit unknown-argument
  swallowing or the exported DLL `main` symbol.

### Stage 1: Introduce the pure typed parser

Dependencies: Stage 0.

- [ ] Add owning request/result types for process coordination, normal host
  startup, automation, startup command, and diagnostics.
- [ ] Implement parsing from an argument span without global writes or process
  operations.
- [ ] Implement one semantic validation pass for duplicates, empty values,
  required companions, incompatible modes, bounded numeric values, typed crash
  phases, and build availability.
- [ ] Normalize both supported project spellings into one owned path.
- [ ] Return structured error text and the command-line exit classification to
  the caller.
- [ ] Add deterministic unit tests for every accepted and rejected row in the
  Stage 0 matrix, including repeatable startup-command argument ordering.

#### Acceptance Gate

- Parser tests prove that parsing is deterministic and side-effect free, every
  accepted request is fully owned, all unsupported/duplicate/conflicting forms
  fail with exit code 2 and actionable text, and no recognized existing workflow
  changes unintentionally.

### Stage 2: Establish the executable and process-runner boundary

Dependencies: Stage 1.

- [ ] Replace `Hello.cpp` with `Main.cpp` containing the actual C runtime entry
  and update the executable target source.
- [ ] Replace the exported DLL `main` with one responsibility-named Launch
  process function and keep its public header minimal.
- [ ] Move validated-request application and run-loop control into
  `ApplicationRunner.cpp`.
- [ ] Establish exact-once crash-handler restoration and conditional logger
  finalization for every ordinary return.
- [ ] Apply crash options, wait-for-process, startup-command publication, and
  engine startup only after complete validation.
- [ ] Emit early diagnostics to stderr and normalize command-line failures to
  exit code 2 without changing runtime failure or startup-command results.
- [ ] Add runner-level tests or child-process characterizations for parse
  failure, wait failure, startup-command configuration failure, bounded tick
  exit, and normal shutdown.

#### Acceptance Gate

- The executable owns `main`, the Launch DLL exports only the named process
  function, all early returns restore installed process resources once, and
  existing editor/game launches plus startup-command automation reach the same
  engine and process results.

### Stage 3: Make the engine loop private and locally owned

Dependencies: Stage 2.

- [ ] Move `FEngineLoop` and its semantic startup type under Launch `Private/`.
- [ ] Construct one loop in the process runner and remove `GEngineLoop`.
- [ ] Add explicit lifecycle state and make the terminal cleanup call safe for
  every admitted startup state without hiding shutdown order.
- [ ] Audit each `PreInit()` and `Init()` failure edge and unwind every earlier
  owned service, mount/project ownership, object/module state, application,
  render/RHI admission, and task executor as applicable.
- [ ] Preserve the current successful startup, tick, minimized pacing, frame
  render decision, and shutdown sequence.
- [ ] Add focused failure-injection tests at the smallest existing seams; add a
  new seam only when a stage otherwise cannot be qualified deterministically.

#### Acceptance Gate

- No public or external source includes the engine-loop header, no process-global
  loop remains, all injected startup failures leave admitted services stopped
  and project ownership released, repeated terminal cleanup is harmless, and
  the successful lifecycle order remains unchanged.

### Stage 4: Isolate diagnostics from normal startup state

Dependencies: Stage 3.

- [ ] Introduce typed private diagnostic configuration, including a native-crash
  phase enum resolved during command validation.
- [ ] Move diagnostic retained state out of `FEngineStartupParams` and
  `FEngineLoop` data members into responsibility-specific components.
- [ ] Split editor PIE and native gameplay smokes into separately named files.
- [ ] Rename the task-scheduler qualification component to its precise lifecycle
  smoke role.
- [ ] Keep explicit named diagnostic calls at process entry, pre-init,
  logger-running, running/tick, consumer detachment, task shutdown, and object
  collection boundaries as required.
- [ ] Verify that disabled diagnostics add no workload and Shipping rejects
  their options before configuration.

#### Acceptance Gate

- Normal startup types contain no smoke/crash-fixture fields, engine lifecycle
  code has no arbitrary crash-phase string comparisons, every existing
  diagnostic passes at its original boundary, and the explicit shutdown body
  remains readable without a generic phase dispatcher.

### Stage 5: Complete semantic naming and storage ownership

Dependencies: Stage 4.

- [ ] Apply the selected private file/type mapping and repair build metadata,
  includes, tests, and direct documentation links.
- [ ] Rename `LaunchFrame` to the selected render-frame responsibility name.
- [ ] Rename `LaunchRuntimeStorage` and its types to preparation-focused names.
- [ ] Restrict runtime-storage preparation to common roots, app config, legacy
  logs, and explicit warnings.
- [ ] Move ImGui, MainFrame, LevelEditor, and ProjectHistory legacy-file
  migration to their owning load paths, sharing only a filename-agnostic Core
  helper if duplication proves real.
- [ ] Preserve storage fallback, destination-wins, rename, copy/remove,
  idempotence, and post-logger warning behavior with focused tests.

#### Acceptance Gate

- Private filenames reveal their actual roles, the only retained `Launch*`
  names express a module/public/build contract, generic storage contains no
  editor-feature filename literals, and all storage/feature migration tests
  pass without changing the selected config paths.

### Stage 6: Integrate, document, and qualify both runtime variants

Dependencies: Stages 1-5.

- [ ] Update Runtime Lifecycle with the final executable, parser, process
  runner, private loop, diagnostics, and cleanup ownership.
- [ ] Update Build And Run with the strict unknown/duplicate/error contract and
  any intentionally changed exit-code behavior.
- [ ] Update Code Modules so `DurinLauncher` is described as the executable
  entry for the configured runtime variant rather than only an editor entry.
- [ ] Run changed-document validation and the all-plan validator.
- [ ] Run the smallest affected native test targets during implementation,
  including parser/runner, Core startup-command/path, Engine storage, and crash
  tests where their boundaries change.
- [ ] Complete a full `all` build because this plan changes the Launch export
  boundary and executable entrypoint.
- [ ] Run bounded DurinEditor and DurinGame lifecycles, project-browser/project
  selection, project relaunch/wait, delayed startup command, normal and inline
  RHI frame shutdown, each lifecycle smoke, and native-crash characterization
  from the same final build profile where applicable.
- [ ] Record final file sizes, public includes, export symbols, test/build/runtime
  evidence, and any deliberate compatibility change in Current Status.

#### Acceptance Gate

- Documentation and implementation agree; targeted tests and the full build
  pass; editor and game variants parse, initialize, tick, automate, diagnose,
  relaunch, and shut down cleanly; and every Definition of Done item has direct
  evidence.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Grammar | Accepted forms, ownership, duplicates, empty/malformed values, overflow, unknown options, repeatable command arguments. |
| Semantic policy | Project normalization, startup-command companions/conflicts, Shipping availability, exit-code classification. |
| Parser purity | Repeated parse calls retain no state and perform no waiting, global configuration, logging, project, or engine operation. |
| Process lifetime | Early parse/wait/config/start failures restore handlers once and shut down only services that started. |
| Executable ABI | `main` is defined by `DurinLauncher`; Launch exports the named process function; editor and game link and start. |
| Engine rollback | Injected PreInit/Init failures unwind all admitted stages; ordinary exit retains documented order. |
| Automation | Tick-bound exit, delayed handler admission, startup-command result propagation, hidden window, and project selection. |
| Diagnostics | Every lifecycle smoke and native-crash phase remains opt-in, deterministic, and absent from normal/Shipping work. |
| Storage | Root creation, config selection, rename/copy fallback, existing destination, feature-owned migration, idempotence, warnings. |
| Integration | Targeted native tests, full build, both runtime variants, normal/inline RHI, relaunch/wait, crash child processes. |

## Definition of Done

- `DurinLauncher/Private/Main.cpp` owns a minimal C runtime entrypoint and no
  zero-byte placeholder source remains.
- Launch exposes one named process function instead of exporting `main` from a
  shared library.
- `ApplicationRunner.cpp` reads as the explicit process protocol, and argument
  parsing is a pure, fully covered boundary.
- Unknown, duplicate, malformed, conflicting, and unavailable options produce
  deterministic diagnostics and exit code 2.
- Normal host startup, process coordination, automation, and diagnostics are
  separate typed request domains with owned strings.
- `FEngineLoop` is private, local, explicitly stateful, and safely unwinds every
  admitted startup state without hiding successful shutdown ordering.
- Normal startup parameters contain no lifecycle-smoke or native-crash fixture
  state.
- Private files/types use responsibility names; `LaunchAPI.h`, the Launch module,
  and precise platform crash names remain intact.
- Common runtime storage no longer owns ImGui or editor-feature filename policy.
- Parser/runner, startup command, storage, crash, and affected lifecycle tests
  pass; the full build and both runtime-variant qualification matrix pass.
- Lasting contracts are moved to Runtime Lifecycle, Build And Run, and Code
  Modules; this plan records only execution history and evidence when complete.

## Deferred Follow-ups

- Rename `FPaths::LaunchDir()` to an executable-directory term and the
  `Launch*Saved/Configs/Logs` accessors to runtime-storage terms in a separate
  Core-led plan if the vocabulary change remains desirable after this refactor.
- Consider a shared Core command-line option reader only after Launch and at
  least one standalone program demonstrate identical option grammar, duplicate,
  ownership, and diagnostic requirements.
- Consider a Windows `wmain` boundary and explicit UTF-16-to-UTF-8 normalization
  as a separate platform contract.
- Add `--help`/`--version` only with an explicit user-facing output and
  no-engine-start contract; they are not required to establish the new boundary.
- Revisit the `DurinLauncher` CMake target/source-module name separately if the
  configured editor/game variant entry continues to be confused with a
  product-specific launcher after Code Modules is corrected.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Native Crash Diagnostics](../Runtime/Core/NativeCrashDiagnostics.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Code Modules](../Workspace/CodeModules.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Engine Loop Orchestration Cleanup](EngineLoopOrchestrationCleanup.md)
- [Native Process Crash Diagnostics](NativeProcessCrashDiagnostics.md)
- [Native Gameplay Core](NativeGameplayCore.md)

## Related Code

- `Engine/Source/Editor/DurinLauncher/CMakeLists.txt`
- `Engine/Source/Editor/DurinLauncher/Private/Hello.cpp`
- `Engine/Source/Runtime/Launch/Private/Launch.cpp`
- `Engine/Source/Runtime/Launch/Public/LaunchAPI.h`
- `Engine/Source/Runtime/Launch/Public/LaunchEngineLoop.h`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchFrame.h`
- `Engine/Source/Runtime/Launch/Private/LaunchFrame.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchRuntimeStorage.h`
- `Engine/Source/Runtime/Launch/Private/LaunchRuntimeStorage.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchGameplayValidation.h`
- `Engine/Source/Runtime/Launch/Private/LaunchGameplayValidation.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchTaskSchedulerValidation.h`
- `Engine/Source/Runtime/Launch/Private/LaunchTaskSchedulerValidation.cpp`
- `Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashHandler.h`
- `Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashHandler.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/StartupCommand.h`
- `Engine/Source/Runtime/Core/Private/Misc/StartupCommand.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Project.h`
- `Engine/Source/Runtime/Core/Private/Misc/Project.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Tests/Native/EngineTests/Private/Launch/`
- `Engine/Tests/Native/CoreTests/Private/StartupCommandTests.cpp`
