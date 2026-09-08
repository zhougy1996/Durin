# Subsystem Framework Plan

Summary: Extract reusable subsystem lifecycle machinery from World services and integrate Engine and Editor scopes with explicit host ownership.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

Planning only; no implementation stages have started. The existing World
framework already provides native registration, dependency ordering, rollback,
GC retention, play and Level callbacks, Tick, work cancellation, and provider
code leases. `DCollisionDebugSubsystem` is an existing production consumer.
The selected work extends this foundation rather than replacing its behavior.
One bounded plan covers the common kernel, three supported scopes, and one
additional production consumer; a roadmap is unnecessary for this scope.

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

## Implementation Stages

### Stage 0: Fix Host Boundaries And Migration Surface

- [ ] Trace Engine and Editor Init, PrepareForShutdown, BeginDestroy, GC roots,
  module-provider startup, and initial World publication. Record an ordered
  startup/shutdown table, including partial failures and callback reentry.
- [ ] Specify common collection operations, typed accessors, descriptor scope
  selection, GC fallback coordination, and World compatibility wrappers.
- [ ] Audit `FNotificationManager` ownership, consumers, and update requirements.
  Select notification-service migration if its lifetime matches the Editor
  scope; otherwise record one concrete alternative and the reason before
  proceeding. Do not broaden the migration to multiple managers.
- [ ] Record affected modules, reflection inputs, and relevant test ownership.

Completion: the plan contains concrete host ordering, public migration surface,
and one selected consumer, with no unresolved ownership or boot-cycle decisions.

### Stage 1: Extract The Kernel And Preserve World Behavior

Depends on Stage 0.

- [ ] Implement the common base object, descriptor/registration infrastructure,
  collection lifecycle, errors, work gate, and code-lease retention.
- [ ] Migrate the World implementation to a scope adapter while preserving its
  API, collision-debug registration, callback ordering, and host-driven Tick.
- [ ] Add focused kernel tests for scope rejection, dependency diagnostics,
  failed initialization rollback, initialized-only lookup, GC retention, and
  code lifetime; reuse existing World coverage for World-specific behavior.
- [ ] Validate existing World lifecycle, operation-boundary, play, Tick, and
  collision-debug behavior with the repository's applicable native tests.

Completion: World consumers retain their behavior and the common implementation
contains no World-specific callback or Tick policy. Record actual validation
evidence and any compatibility aliases in the stage handoff.

### Stage 2: Integrate Engine And Editor Hosts

Depends on Stage 1.

- [ ] Add Engine and Editor scope types, collections, registration, and typed
  accessors. Ensure an editor host has both distinct collections without
  accessor ambiguity or duplicate service construction.
- [ ] Wire initialization, GC enumeration, admission closure, explicit shutdown,
  and fallback destruction to the Stage 0 host sequence. Propagate startup
  failures through the existing host initialization result.
- [ ] Test multiple host instances, invalid-scope registration, failed startup,
  repeated shutdown, and shutdown requested from Initialize or update callbacks.
- [ ] Test World teardown access to Engine services, independent Editor/Engine
  collections, and Engine/Editor service persistence across PIE start/stop.
- [ ] Verify Runtime target membership excludes Editor subsystem implementation.

Completion: both new scopes function through real host lifecycles, including
failure and destruction, with no Runtime-to-Editor dependency or implicit World
requirement in Engine initialization.

### Stage 3: Migrate One Consumer And Publish Contracts

Depends on Stage 2.

- [ ] Move the selected Editor service's authoritative ownership into an Editor
  subsystem; retain forwarding facades where callers need compatibility.
  Remove duplicate initialization and shutdown ownership from the host.
- [ ] Validate the consumer's normal flow, required update phase, repeated PIE,
  and editor shutdown. Confirm retained callbacks cannot publish after retirement.
- [ ] Publish implemented common/Engine contracts under Runtime and Editor
  contracts under Editor/Architecture. Update World documentation to reference
  common mechanics while retaining its authoritative scope-specific behavior.
- [ ] Add a concise native registration/lookup example and scope-selection
  guidance. Update documentation routing only for new distinct task triggers.
- [ ] Record validation evidence, close acceptance gates, and mark this plan
  completed only after all required work has passed.

Completion: a production Editor consumer uses the framework, the existing World
consumer still works, and current contracts describe the implemented behavior.

## Validation And Handoff

Follow [agent build guidance](../Agents/BuildAndRun.md) before configuring,
building, or running targets and [agent testing guidance](../Agents/Testing.md)
before selecting native tests. Use the smallest applicable suites for each
stage, broadening only for actual failures or newly affected behavior. Record
target, configuration, test selection, result, and any unavailable platform
validation in the stage handoff; source inspection alone is not execution proof.

The essential acceptance dimensions are dependency/rollback correctness,
host-instance isolation, GC and code lifetime, non-reentrant retirement,
unchanged World behavior, real Engine/Editor integration, and one production
consumer. No performance benchmark or migration of unrelated managers is required.

Follow [documentation workflow](../Agents/Documentation.md) for document and
plan validation. Keep implementation status and stage evidence in the same
commit as changes, using the exact plan and stage trailers required by the
repository. Planning alone does not close implementation checkboxes.

## Related Code And Contracts

- [World subsystem contract](../Runtime/World/WorldSubsystems.md)
- [World operation and Level contract](../Runtime/World/LevelSystem.md)
- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [World subsystem API](../../Engine/Source/Runtime/Engine/Public/Engine/WorldSubsystem.h)
- [World collection implementation](../../Engine/Source/Runtime/Engine/Private/Engine/WorldSubsystem.cpp)
- [World host lifecycle](../../Engine/Source/Runtime/Engine/Private/Engine/WorldCore.cpp)
- [Engine host implementation](../../Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp)
- [Editor host API](../../Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h)
- [Editor host implementation](../../Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp)
- [World subsystem tests](../../Engine/Tests/Native/EngineTests/Private/World/WorldSubsystemTests.cpp)
