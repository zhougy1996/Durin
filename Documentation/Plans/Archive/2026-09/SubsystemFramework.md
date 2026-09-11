# Subsystem Framework Plan

Summary: Extract reusable subsystem lifecycle machinery from World services and integrate Engine and Editor scopes with explicit host ownership.

Last reviewed: 2026-09-08

Status: Archived
Completed: 2026-09-08

## Current Status

All stages and the completion audit are complete. The audit additionally fixed
immediate World admission closure (including unpublished Worlds), retirement
from constructors, and notification callback retirement boundaries. The latest
73-target affected selection passed, including 141 WorldTests, and the full all
build and documentation validators passed on MacOS-arm64-Debug-DurinEditor.
Application launch and non-macOS builds were not run; Runtime isolation was
verified using the current DHT module closure. Final evidence is recorded in
the Completion Audit section below.

## Goal

Allow modules to provide services whose instances follow an Engine, World, or
Editor host lifetime, using shared creation and retirement machinery without
putting feature-specific ownership into the host classes. Preserve the existing
World contract and demonstrate the new Editor scope with a real service.

## Scope And Non-Goals

Implement `DSubsystem`, common collection and registration machinery,
`DEngineSubsystem`, and `DEditorSubsystem`; migrate `DWorldSubsystem` onto the
common foundation. Integrate explicit initialization, failure cleanup, GC
retention, and shutdown into the existing hosts.

Do not introduce GameInstance or LocalPlayer hosts, a global service locator,
cross-scope dependency graphs, reflection-wide discovery, live insertion into
existing collections, a scheduler, or a wholesale conversion of managers.
Subsystem instances remain transient native objects, not serialized assets.
GameInstance and LocalPlayer scopes require separate selected work once their
host lifetimes exist. These are future directions, not completion gates here.

## Selected Design

### Ownership And Module Boundaries

- Put `DSubsystem` and shared machinery in Engine. Put `DEngineSubsystem` and
  the migrated World scope there too. Put `DEditorSubsystem` and its collection
  adapter in DurinEd; Engine must not depend on Editor types.
- Each host owns its collection and reports its objects to GC. Outer identifies
  the host but is not a substitute for explicit retention. No process-global
  subsystem instances are introduced.
- `DEditorEngine` owns distinct Engine and Editor collections. Engine services
  have one instance per `DEngine`, including an editor engine; Editor services
  have one instance per editor host. World services remain per World, including
  separate Editor, PIE, and Preview instances.
- Keep host-specific callback admission and destruction coordination in scope
  adapters. In particular, the current World shutdown request from subsystem
  `BeginDestroy` and readiness delegation cannot become a generic instruction
  to destroy arbitrary hosts.

### Common Kernel And Scope Adapters

- Extract common result/state vocabulary, descriptor identity and dependencies,
  registration tokens, frozen membership, exact lookup, deterministic ordering,
  construction, rollback, GC enumeration, and retirement into reusable code.
  Use a common collection base with typed scope adapters; avoid duplicating the
  dependency algorithm in each scope.
- `DSubsystem` owns the shared Initialize/Deinitialize contract, work gate, and
  provider lifetime protection. Names and typed wrappers are finalized in
  Stage 0, before implementation changes public headers.
- World type eligibility, BeginPlay/EndPlay, Level attachment, Tick phases,
  frame admission, and operation reentry remain in the World adapter. Common
  descriptors do not contain physics Tick groups or Editor/Preview flags.
- Preserve current World public entry points and behavior through forwarding
  wrappers or aliases where needed. `GetSubsystem<T>()` remains an exact,
  non-constructing query and only exposes successfully initialized objects.
  Add scope/type constraints so a typed accessor cannot accept a subsystem from
  an unrelated scope. Editor lookup must not hide the inherited Engine lookup.
- Tick is opt-in and driven by the relevant host. Do not add a Tick virtual to
  every subsystem merely to share World behavior. Add an Editor update adapter
  only if the selected consumer requires it, with one documented host phase.

### Registration, Dependencies, And Failure

- Retain module-owned explicit registration tokens and providing-module names.
  Snapshot eligible descriptors once per collection; later registration changes
  affect future collections only. Validate each registered class against the
  selected scope before construction.
- Dependencies name concrete types within the same collection. Reject wrong
  scope, duplicates, missing dependencies, and cycles with categorized errors
  identifying involved types. Preserve deterministic tie-breaking.
- Host boot order coordinates cross-scope access; do not resolve dependencies
  by implicitly constructing services in another host. Engine services must
  not require a current World during initialization. Editor services must not
  assume a PIE World exists.
- Preserve one-shot collection initialization, reverse cleanup, cleanup of the
  service whose Initialize failed, and terminal failure. Failed hosts must not
  be published as usable. Define both returned-error and exception paths.
- Registration, lookup, initialization, callbacks, and lifecycle mutation stay
  on the game thread. Host callbacks may request shutdown; close admission
  immediately and defer destructive cleanup until active dispatch unwinds.

### Retirement And Host Ordering

- Preserve detached worker captures, cancellation gates, and provider/runtime
  code leases through asynchronous retirement and physical object destruction.
  Closing a gate rejects publication; it neither joins workers nor releases
  their captures. Do not add game-thread waits for game-thread completions.
- Start Engine services after their declared base endpoints exist and before
  creating the initial World. Start Editor services after their required editor
  endpoints exist and before exposing dependent editor consumers. Stage 0 must
  resolve the actual split in the current base/derived Init sequence.
- On shutdown, close work admission first, retire dependent consumers while
  required services remain available, then deinitialize services before their
  host endpoints are released. World cleanup must retain access to Engine
  services; Editor consumers must release Editor services before that collection
  is deinitialized. Record the exact host sequence in Stage 0.
- Use explicit host shutdown with idempotent GC fallback. Preserve repeated PIE
  lifetimes, suspended EditorWorld behavior, and World operation boundaries.

## Stage 0 Handoff: Selected Boundaries

Source trace: `Engine.cpp`, `EditorEngine.cpp`, `WorldCore.cpp`,
`WorldOperation.h`, Launch `EngineLoop.cpp`, `Notification.cpp`, and MainFrame
`EditorNotificationOverlay.cpp`. This is design evidence, not native execution.

| Phase | Selected order and failure boundary |
| --- | --- |
| Launch | Construct and root the engine; globals identify the booting host, while loop state remains Initializing until Init succeeds. Application/Mona/RHI endpoints precede host Init. |
| Engine Init | Load Engine providers, cooked-mesh manager, catalog, default materials, renderer and scene; initialize Engine collection; only then create, initialize and publish MainWorld. A subsystem failure retires the collection and returns Failed without publishing a World. |
| Editor Init | Load project roots and AssetForgeBuiltins before base Init; retain EditorWorld; load MainFrame provider module; initialize Editor collection before CreateEditorHost exposes notification consumers. Transactor already exists from construction. No Editor service requires PIE or a created shell. |
| Callback reentry | Close admission immediately on shutdown requests. Host operation depth defers consumer destruction and collection cleanup until initialization/update dispatch unwinds. Repeated Init fails; exceptions become initialization errors and trigger reverse cleanup. |
| Editor shutdown | Close both collections' gates; stop PIE and shut down EditorWorld; destroy editor shell/notification consumers while services remain available; retire Editor collection; base shutdown retires remaining World and Engine collection before releasing base endpoints. |
| Launch exit | PrepareForShutdown must precede cooked-mesh and task-system shutdown, so subsystem cleanup retains declared base endpoints. Root retirement and GC follow task shutdown. |
| GC fallback | Host BeginDestroy reuses idempotent explicit retirement. Collection objects are enumerated by each host. World subsystem BeginDestroy still requests World shutdown and delegates readiness; generic DSubsystem never destroys its Outer. |
| Partial Editor failure | Returned errors, cancellation and exceptions retire any created shell, World and collections through the same host shutdown path; Launch does not enter Running. |

The shared public names are `DSubsystem`, `FSubsystemWorkGate`,
`ESubsystemState`, `ESubsystemError`, `FSubsystemResult`,
`FSubsystemDescriptor`, `FSubsystemRegistration`, and `FSubsystemCollection`.
The kernel owns frozen descriptor ordering, construction, initialized-only exact
Find, CloseWork, Shutdown and AddReferencedObjects. Typed adapters supply the
scope class and eligibility. Common descriptors carry Type, Provider and
Dependencies only. World descriptors retain their current aggregate layout and
convert to common descriptors, preserving designated initializer compatibility.
World result/state/error/work-gate names remain aliases. World callback and Tick
metadata remain in the World adapter. Typed accessors constrain their scope;
Editor uses GetEditorSubsystem while inherited GetSubsystem selects Engine.

Selected consumer: `DEditorNotificationSubsystem` owns `FNotificationManager`.
`GetNotificationManager` forwards to it; the host no longer constructs a manager.
Keep the existing MainFrame notification update phase (after transaction event
publication, before overlay drawing), routed through the Editor collection's
scoped dispatch. No generic Tick virtual is needed. Retirement closes producer
admission and clears queued commands/actions; detached producers must retain a
work gate and detached data, never a raw retired manager. Existing standalone
manager tests remain valid.

Affected production modules are Engine, DurinEd, MainFrame and Launch. New
reflected headers belong to Engine (Subsystem, EngineSubsystem) and DurinEd
(EditorSubsystem, EditorNotificationSubsystem); normal module source discovery
and reflection generation own them. Runtime membership must exclude DurinEd.
Existing World fixtures belong to WorldTests; notification coverage is
owned by EditorShellTests (confirmed in the configured registry).
Extend existing native targets rather than creating another test executable.

## Implementation Stages

### Stage 0: Fix Host Boundaries And Migration Surface

- [x] Trace Engine and Editor Init, PrepareForShutdown, BeginDestroy, GC roots,
  module-provider startup, and initial World publication. Record an ordered
  startup/shutdown table, including partial failures and callback reentry.
- [x] Specify common collection operations, typed accessors, descriptor scope
  selection, GC fallback coordination, and World compatibility wrappers.
- [x] Audit `FNotificationManager` ownership, consumers, and update requirements.
  Select notification-service migration if its lifetime matches the Editor
  scope; otherwise record one concrete alternative and the reason before
  proceeding. Do not broaden the migration to multiple managers.
- [x] Record affected modules, reflection inputs, and relevant test ownership.

Completion: the plan contains concrete host ordering, public migration surface,
and one selected consumer, with no unresolved ownership or boot-cycle decisions.

### Stage 1: Extract The Kernel And Preserve World Behavior

Depends on Stage 0.

- [x] Implement the common base object, descriptor/registration infrastructure,
  collection lifecycle, errors, work gate, and code-lease retention.
- [x] Migrate the World implementation to a scope adapter while preserving its
  API, collision-debug registration, callback ordering, and host-driven Tick.
- [x] Add focused kernel tests for scope rejection, dependency diagnostics,
  failed initialization rollback, initialized-only lookup, GC retention, and
  code lifetime; reuse existing World coverage for World-specific behavior.
- [x] Validate existing World lifecycle, operation-boundary, play, Tick, and
  collision-debug behavior with the repository's applicable native tests.

Completion: World consumers retain their behavior and the common implementation
contains no World-specific callback or Tick policy. Record actual validation
evidence and any compatibility aliases in the stage handoff.

### Stage 2: Integrate Engine And Editor Hosts

Depends on Stage 1.

- [x] Add Engine and Editor scope types, collections, registration, and typed
  accessors. Ensure an editor host has both distinct collections without
  accessor ambiguity or duplicate service construction.
- [x] Wire initialization, GC enumeration, admission closure, explicit shutdown,
  and fallback destruction to the Stage 0 host sequence. Propagate startup
  failures through the existing host initialization result.
- [x] Test multiple host instances, invalid-scope registration, failed startup,
  repeated shutdown, and shutdown requested from Initialize or update callbacks.
- [x] Test World teardown access to Engine services, independent Editor/Engine
  collections, and Engine/Editor service persistence across PIE start/stop.
- [x] Verify Runtime target membership excludes Editor subsystem implementation.

Completion: both new scopes function through real host lifecycles, including
failure and destruction, with no Runtime-to-Editor dependency or implicit World
requirement in Engine initialization.

### Stage 3: Migrate One Consumer And Publish Contracts

Depends on Stage 2.

- [x] Move the selected Editor service's authoritative ownership into an Editor
  subsystem; retain forwarding facades where callers need compatibility.
  Remove duplicate initialization and shutdown ownership from the host.
- [x] Validate the consumer's normal flow, required update phase, repeated PIE,
  and editor shutdown. Confirm retained callbacks cannot publish after retirement.
- [x] Publish implemented common/Engine contracts under Runtime and Editor
  contracts under Editor/Architecture. Update World documentation to reference
  common mechanics while retaining its authoritative scope-specific behavior.
- [x] Add a concise native registration/lookup example and scope-selection
  guidance. Update documentation routing only for new distinct task triggers.
- [x] Record validation evidence, close acceptance gates, and mark this plan
  completed only after all required work has passed.

Completion: a production Editor consumer uses the framework, the existing World
consumer still works, and current contracts describe the implemented behavior.

## Stage 1 Handoff

Implemented DSubsystem, the common work gate/result vocabulary, shared
registration storage, and FSubsystemCollection. World tokens forward to the
common registry with opaque adapter policy; the kernel never interprets World
eligibility or Tick data. World descriptors retain aggregate compatibility;
World state/error/result/work-gate names are aliases, and typed World lookup
rejects unrelated types at compile time. Reflected inputs require explicit
`Engine.dmodule` entries, rather than header discovery alone.

Validation: `./DevTool test WorldTests`, MacOS-arm64-Debug-DurinEditor,
129/129 passed (2026-09-08). Includes existing callback, operation, GC,
collision-debug, detached completion and module-code-lease tests plus wrong
scope, diagnostic identity, exception rollback and Initialize-shutdown tests.
Initial reflection input omission and a failed-state shutdown recursion were
fixed; explicit World shutdown retains its prior transition to Shutdown.
No application smoke or other platform execution was run.

## Stage 2 Handoff

Engine services start after renderer/base endpoints and before initial World
construction. Editor services start after MainFrame module loading and before
shell creation. Both Init paths convert exceptions/errors to the existing host
result and retire partial state. Host operation scopes defer requested cleanup;
World operation readiness also postpones cleanup until a later host boundary.
Explicit shutdown and scope-specific GC fallback share the host sequence.
Launch calls PrepareForShutdown before Mona, cooked-mesh and task teardown.

Validation: `./DevTool test WorldTests`, MacOS-arm64-Debug-DurinEditor,
134/134 passed. CPU fixtures use real host constructors, production collection
boot helpers, GC and shutdown; they cover scope rejection, Engine/Editor
Initialize shutdown, exceptions, callback deferral, two hosts, independent
collections, teardown dependency access, and two actual Start/Stop PIE cycles.
The renderer-backed Init path is compile-covered; application launch was not run
under the macOS sandbox. DHT's `collect_enabled_modules_for_project` for
Engine/DurinGame excludes DurinEd and MainFrame and includes Engine. There is no
registered macOS DurinGame build preset; this is module-closure validation,
not a Runtime executable build.

## Stage 3 Handoff And Acceptance

DurinEd publishes DEditorNotificationSubsystem with a module-owned token. Its
manager is authoritative; the host facade forwards and MainFrame calls the
single existing notification update phase through a host operation scope.
Retirement discards pending commands and invalidates retained actions, enablement
and cancellation callbacks through shared admission/gate captures. Tests cover
normal notification flow, actual repeated PIE identity, shutdown and a callback
retained past service GC. Common and Editor contracts are published under
Runtime/Core and Editor/Architecture; World and lifecycle routing link to them.

Final validation on 2026-09-08, MacOS-arm64-Debug-DurinEditor:

- `./DevTool test affected --base 0b39fbd00`: 73/73 CTest targets passed;
  includes WorldTests and EditorShellTests. Build 102.11 s; execution 26.73 s.
- `./DevTool test WorldTests`: 135/135 cases passed after final diagnostic
  identity/formatting changes; build 3.91 s; execution 0.27 s.
- `./DevTool build`: full all target passed in 6.76 s. Executable:
  `Engine/Binaries/MacOS/Debug/Runtime/DurinEditor/DurinEditor`.
- `./DevTool doc validate --scope changed` and
  `./DevTool doc plan validate --scope all`: passed.
- `git diff --check`: passed. No macOS application smoke, GPU qualification,
  Windows build, or Runtime executable run was performed. These are not implicit
  lanes for this change; the real renderer-backed Init path has compile coverage.

Acceptance is closed for common ordering/rollback, host isolation, GC and module
code retention, callback retirement, World compatibility, Engine/Editor host
integration, Runtime module isolation, and the production notification consumer.

## Completion Audit

The completion audit found and corrected retirement gaps in host-to-World
admission, retirement from a constructor, and external notification callbacks.
Engine closure now requests non-destructive World retirement immediately,
including scoped unpublished Worlds. World operations stop forward callbacks
before unwinding. Notification callbacks postpone consumer destruction to the
next host boundary; an enablement callback that retires entries cannot invoke
the discarded action. The new regressions run through production APIs.

| Requirement | Authoritative implementation and execution evidence |
| --- | --- |
| Stage 0 ownership, provider boot and consumer selection | Host sequence above; Engine.cpp, EditorEngine.cpp and EngineLoop.cpp order; DurinEdModule.cpp owns the notification registration token. |
| Common kernel with preserved World API | Subsystem.h/cpp own shared registration/order/rollback/retention; WorldSubsystem.h retains descriptor layout and aliases. WorldTests covers dependency ordering, duplicate registration, eligibility, play, Level and Tick behavior. |
| Correct diagnostics and one-shot rollback | RejectsWrongScopeAndReportsMissingDependencyIdentity, WrongScopeDependencyIsRejectedBeforeConstruction, ExceptionRollsBackFailedObjectBeforeDependencies and ConstructorRetirementDoesNotEnterInitializeWithAnOpenGate pass. |
| Exact typed lookup and host-instance isolation | Compile-time lookup constraints cover all scopes; HostsKeepIndependentCollectionsAcrossWorldLifetimesAndGC verifies distinct host instances and actual repeated PIE. |
| GC and physical code lifetime | HostGarbageFallbackRetiresBothCollections, RejectsProviderRetirementUntilGarbageObjectsReleaseCode and LateDetachedCompletionCannotPublishAfterWorldRetirement cover host fallback, provider leases and detached work. |
| Non-reentrant host and World retirement | HostShutdownDuringInitializationAndDispatchIsDeferred, EngineRetirementImmediatelyStopsWorldCallbackAdmission and UnpublishedWorldClosesAdmissionWhenItsEngineRetires cover callbacks and unpublished startup. |
| Cross-scope cleanup ordering | WorldAndEditorCleanupRetainEngineDependencies verifies that dependent cleanup can still query Engine services. |
| Runtime exclusion | DHT Engine/DurinGame module closure includes Engine and excludes DurinEd/MainFrame; new reflected headers are owned by their respective module manifests. |
| One production consumer and update phase | DEditorNotificationSubsystem owns the only host manager; MainFrame calls UpdateNotifications once after transaction publication. NotificationServiceOwnsFacadeAndRejectsRetainedActionsAfterShutdown and NotificationShutdownCallbackRetiresAtTheNextHostBoundary pass. EditorShellTests covers normal flow and standalone retirement. |
| Published contracts and handoff | Runtime/Core/Subsystems.md, Editor/Architecture/EditorSubsystems.md, WorldSubsystems.md and RuntimeLifecycle.md describe the implemented boundaries. Final validation receipt follows. |

Final audit receipt (2026-09-08, MacOS-arm64-Debug-DurinEditor):

- `./DevTool test affected`: all 73 CTest targets passed, including the new
  World and notification regressions. Build 26.61 s, tests 26.72 s; log
  `Build/.agent-state/logs/20260908-084646-633366-92848-ctest.log`.
- WorldTests passed all 141 cases, both in the affected run and the focused
  `./DevTool test WorldTests` run preceding it.
- `./DevTool build`: the full all target passed in 0.26 s; log
  `Build/.agent-state/logs/20260908-084757-217152-93701-cmake.log`.
- DHT's current Engine/DurinGame module closure was rechecked and contains
  Engine without DurinEd or MainFrame.
- Changed-document validation, all-plan validation and `git diff --check`
  passed. The application/platform execution limitations recorded above remain
  unchanged; no unrun application smoke is claimed as native execution evidence.

## Validation And Handoff

Follow [agent build guidance](../../../Agents/BuildAndRun.md) before configuring,
building, or running targets and [agent testing guidance](../../../Agents/Testing.md)
before selecting native tests. Use the smallest applicable suites for each
stage, broadening only for actual failures or newly affected behavior. Record
target, configuration, test selection, result, and any unavailable platform
validation in the stage handoff; source inspection alone is not execution proof.

The essential acceptance dimensions are dependency/rollback correctness,
host-instance isolation, GC and code lifetime, non-reentrant retirement,
unchanged World behavior, real Engine/Editor integration, and one production
consumer. No performance benchmark or migration of unrelated managers is required.

Follow [documentation workflow](../../../Agents/Documentation.md) for document and
plan validation. Keep implementation status and stage evidence in the same
commit as changes, using the exact plan and stage trailers required by the
repository. Planning alone does not close implementation checkboxes.

## Related Code And Contracts

- [World subsystem contract](../../../Runtime/World/WorldSubsystems.md)
- [World operation and Level contract](../../../Runtime/World/LevelSystem.md)
- [Runtime lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [World subsystem API](../../../../Engine/Source/Runtime/Engine/Public/Engine/WorldSubsystem.h)
- [World collection implementation](../../../../Engine/Source/Runtime/Engine/Private/Engine/WorldSubsystem.cpp)
- [World host lifecycle](../../../../Engine/Source/Runtime/Engine/Private/Engine/WorldCore.cpp)
- [Engine host implementation](../../../../Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp)
- [Editor host API](../../../../Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h)
- [Editor host implementation](../../../../Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp)
- [World subsystem tests](../../../../Engine/Tests/Native/EngineTests/Private/World/WorldSubsystemTests.cpp)
