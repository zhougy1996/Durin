# Modular Feature and DLL Unload Safety Roadmap

Summary: Establish typed modular services, explicit asynchronous abort semantics, and enforceable DLL unload quiescence across dynamic modules.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Milestones 1 and 2 are complete. Core now separates typed synchronous feature
retirement from explicit asynchronous operation cancellation and proves task,
typed-result, Worker-wrapper, and selected Game Thread callable quiescence before
native release. Module-manager tests cover successful cooperative cancellation
and fail-closed async timeout in addition to the synchronous retirement matrix.

Milestone 3 is complete. All six Runtime Engine authoring callback families use
typed modular features owned by StandardAssetImport or GeometryBuild, and
Terrain asynchronous work uses a module-owned operation group. Milestone 4 is
active with the specialized registry and callback inventory audit.

| Milestone | Status | Active child plan |
| --- | --- | --- |
| 1. Modular feature registry and module retirement | Complete | [Modular Feature Registry and Module Retirement](../Plans/ModularFeatureRegistryAndModuleRetirement.md) |
| 2. Explicit asynchronous operation drain | Complete | [Module Async Operation Drain](../Plans/ModuleAsyncOperationDrain.md) |
| 3. Engine authoring feature migration | Complete | [Engine Authoring Modular Feature Migration](../Plans/EngineAuthoringModularFeatureMigration.md) |
| 4. Specialized registry integration and callback audit | Active | [Dynamic Module Registry Safety Audit](../Plans/DynamicModuleRegistrySafetyAudit.md) |
| 5. Dynamic DLL unload qualification | Blocked on Milestones 1-4 | None |

## Outcome

Dynamic modules expose typed services without placing Plugin `std::function`
objects in process-global Engine storage. Feature retirement prevents new
cross-module calls and waits for already admitted synchronous calls to return.
Asynchronous cancellation remains an explicit operation-level decision rather
than an implicit consequence of module lifetime. `FModuleManager` releases a
DLL only after a fail-closed audit proves that no current or future execution,
continuation, callback destruction, virtual cleanup, or retained provider
object can enter that DLL.

The target unload sequence is:

```text
Module Active
  -> close module load/unload admission
  -> retire every feature owned by the module
  -> wait for admitted synchronous feature invocations
  -> drain reflected objects while the DLL remains mapped
  -> call ShutdownModule()
       -> close service and operation admission
       -> request explicit Cancel or Drain semantics
       -> process or discard Game Thread continuations
       -> destroy queued callable storage and captures
       -> stop private threads, timers, watchers, and external executors
       -> release registrations and services
  -> pass the module shutdown audit
  -> destroy the module instance
  -> FreeLibrary
```

If any required drain or audit fails, unload fails and the DLL remains mapped.

## Scope

- Add a Core-owned typed modular feature registry with explicit module owner
  identity, identity-bearing registration tokens, and internal invocation
  quiescence gates.
- Replace `IModuleInterface`'s context-free lifecycle with explicit startup and
  shutdown contexts, or an equivalent manager-created owner surface that
  cannot be forged by feature providers.
- Replace the `bIsReady`-only unload model with explicit module states and a
  diagnostic unload result.
- Add explicit asynchronous operation groups on top of the task system's
  cancellation tokens and task scopes.
- Add Game Thread drain support that proves canceled or invalidated deferred
  callable storage has been detached and destroyed.
- Migrate the Runtime Engine authoring callback slots owned by
  `StandardAssetImport` and `GeometryBuild` to typed modular features.
- Integrate semantically specialized registries with the common owner,
  retirement, and unload-audit primitives without erasing their domain-specific
  selection rules.
- Qualify explicit DLL unload and reload with deterministic concurrency tests
  and a real dynamic-module integration test.

### Target Feature Contracts

| Typed feature | Provider module | Replaces |
| --- | --- | --- |
| `IStaticMeshAuthoringFeature` | `StandardAssetImport` | File build, uncooked post-load, and source-reference mutation callbacks |
| `IStaticMeshCollisionBuildFeature` | `GeometryBuild` | Static-mesh collision product callback |
| `ITexture2DAuthoringFeature` | `StandardAssetImport` | Texture2D uncooked post-load callback |
| `ITextureCubeAuthoringFeature` | `StandardAssetImport` | TextureCube uncooked post-load callback |
| `ITerrainHeightmapAuthoringFeature` | `StandardAssetImport` | Terrain uncooked post-load, load wait, and source-reference callbacks |
| `ISkeletalDerivedDataFeature` | `GeometryBuild` | Skeletal-mesh and animation-clip uncooked payload loaders |

Static-mesh authoring and collision remain separate contracts because they are
implemented and unloaded by different modules.

## Non-Goals

- Do not use a module lease, module-loaded flag, or feature-registration state
  to express application-level abort semantics.
- Do not turn the modular feature registry into a general event bus, delegate
  system, task scheduler, or service locator for ordinary intra-module code.
- Do not replace format recognition, provider selection, import provenance, or
  other domain-specific registry behavior with arbitrary "first feature wins"
  selection.
- Do not make `std::shared_ptr` ownership of a Plugin object stand in for native
  code lifetime safety.
- Do not require all process shutdown paths to physically unload every DLL;
  explicit unload and reload must nevertheless be correct whenever requested.
- Do not promise stable third-party ABI compatibility during this development
  refactor.

## Program Decisions and Invariants

### Separate the three lifecycles

Feature availability, operation cancellation, and DLL mapping are independent
state machines:

```text
Feature:    Published -> Retiring -> Retired
Operation:  Accepting -> Draining | Cancelling -> Quiescent
DLL:        Loaded -> Closing -> StoppedMapped -> Unmapped
```

Feature retirement answers whether a new call may begin. An operation group's
cancellation source and abort reason answer how already accepted work should
finish. The module manager's shutdown audit answers whether native code can be
unmapped. No state substitutes for another.

### Keep invocation safety internal to the registry

The public registry does not return a raw feature pointer or a caller-retained
module lifetime handle. Typed calls use a bounded visitor such as
`InvokeSingle<T>(Callable)`. The registry admits the call under an internal
entry gate, releases its lock, invokes the feature, and records return from the
call before retirement may complete.

Registry lookup and retirement must linearize so a call either:

- enters while the feature is published and is counted until it returns; or
- observes retirement and never calls the implementation.

The internal call gate is an unload-safety mechanism only. It never reports or
requests cancellation to business code.

### Make registration identity-bearing

Registration requires a manager-created module owner and returns a move-only
token containing a unique registration identity and generation. Retirement and
reset affect only the matching entry. No parameterless `UnregisterXxx()` API may
remove a replacement provider registered by another caller.

The registry stores a non-owning interface pointer plus Core-owned metadata. It
does not own Plugin objects, Plugin deleters, or Plugin-authored callable
storage. The provider module destroys its implementation while its DLL is still
mapped.

### Make cardinality and selection explicit

The generic registry supports multiple providers. A typed consumer that
requires exactly one implementation uses `InvokeSingle<T>` and treats zero or
more than one provider as an explicit result. Features that require ranking,
identity matching, contract versions, or provenance retain a specialized typed
facade rather than relying on registration order.

### Express abort through operation groups

Every asynchronous service owns one or more explicit operation groups. A group
provides root-work admission, a cancellation source, an abort reason, a task
scope, descendant/continuation ownership, publication validity, drain, and a
diagnostic snapshot. Representative abort reasons include user cancellation,
supersession, asset destruction, source change, application shutdown, and
module shutdown.

Feature methods that start asynchronous work must transfer that work into an
operation group before the synchronous feature invocation returns. Work may not
escape into an untracked raw task, detached thread, timer, or executor callback.

### Define quiescence to include callable destruction

An operation is not unload-quiescent merely because all task states are
terminal. Quiescence requires all of the following:

- no queued, waiting, or running worker task owned by the group;
- no executable or terminal Game Thread continuation owned by the group;
- no retained Plugin callable, capture, deleter, cancellation callback, or
  coroutine frame in Core queues or task states;
- no future path that can enqueue another child or continuation for the group;
- no pending publication into an Engine object.

Invalidating a generation or canceling a task prevents publication but does not
by itself make DLL unload safe. Skipped continuations must be detached and
destroyed before unload.

### Drain Game Thread work without a blocking wait

Module unload is serialized on the Game Thread. A module shutdown cannot call a
plain blocking wait when accepted work depends on `GameThreadDeferred`.
Operation-group drain must pump or discard owner/scope-specific continuations,
help eligible worker progress, detach terminal queue entries, and enforce a
bounded timeout. It must not execute arbitrary unrelated Plugin work merely to
make one module quiescent.

### Make module unload fail closed

`FModuleManager::UnloadModule()` returns a categorized result. Timeout,
self-unload from an owned callback, reflected-object drain failure, retained
callable storage, live registrations, live operations, or external execution
all reject `FreeLibrary`. A rejected module may remain stopped and mapped; the
manager does not pretend it was unloaded and does not roll admission back after
partial shutdown.

### Preserve object and subsystem ordering

Core closes public feature admission and waits for synchronous feature calls
before module-owned state is dismantled. Reflected class defaults and objects
are drained while the DLL is mapped. `ShutdownModule()` then drains async and
external work and releases registrations and services. The module instance is
destroyed only after the audit passes, and the native library is released only
after module destruction returns.

## Current Foundations and Gaps

| Foundation | Existing value | Gap to close |
| --- | --- | --- |
| Module loader | Reverse shutdown order, reflected-object pre-shutdown hook, explicit `FreeLibrary` path | Only `bIsReady` represents state; unload has no result, owner retirement, invocation barrier, or final audit |
| Task system | Cancellation tokens, task scopes, task attribution, worker helping, and `GameThreadDeferred` | Game Thread scope wait cannot drain dependent continuations, and terminal state does not prove deferred callable storage is gone |
| Asset import | Provider registry, provider leases, provider-specific cancellation and outstanding-lease assertions | Provider retention and operation abort are coupled locally and are not integrated with module owner retirement |
| Asset build host | Identity/generation registration tokens and service-specific stop/wait/drain callbacks | Pump and wait paths copy Plugin `std::function` objects outside the registry lock and can race module unload |
| Runtime authoring boundaries | Narrow callbacks keep Editor and Developer dependencies out of Runtime Engine | Process-global callback slots copy Plugin functions and use identity-free unregister operations |
| Editor registries | Several scoped registration handles and generation-aware entries already exist | Owner attribution and DLL unload quiescence are inconsistent across registries |

## Milestone Map

### Milestone 1: Modular feature registry and module retirement

- **Dependencies:** None.
- **Deliverable:** Core feature registry, manager-created module owner identity,
  move-only registration token, internal invocation gate, explicit module state
  machine, categorized unload result, and owner-wide feature retirement.
- **Entry gate:** Review and accept the no-raw-pointer `InvokeSingle/InvokeAll`
  API shape and module lifecycle context boundary.
- **Exit gate:** Concurrency tests prove lookup/retire linearization, stale-token
  isolation, bounded synchronous invocation drain, self-unload rejection, and
  fail-closed unload without involving asynchronous work.

### Milestone 2: Explicit asynchronous operation drain

- **Dependencies:** Milestone 1 owner identity and module state machine.
- **Deliverable:** Operation groups integrated with task scopes, explicit abort
  reasons, inherited child/continuation ownership, owner/scope-specific Game
  Thread drain, retained-callable accounting, and module shutdown diagnostics.
- **Entry gate:** Milestone 1 APIs are stable enough for task ownership and
  diagnostics to depend on them.
- **Exit gate:** Tests prove that worker-to-Game-Thread chains either publish or
  cancel deterministically, canceled continuations are destroyed before drain
  succeeds, dynamic child admission cannot escape closing groups, and timeout
  leaves the module mapped.

### Milestone 3: Engine authoring feature migration

- **Dependencies:** Milestones 1 and 2.
- **Deliverable:** The six typed feature contracts in this roadmap, provider
  implementations owned by module instances, consumer-side bounded invocation,
  and removal of the corresponding process-global `std::function` slots and
  parameterless unregister APIs.
- **Entry gate:** The feature registry supports singleton diagnostics and the
  operation-group drain can represent Terrain and import publication chains.
- **Exit gate:** Static mesh, collision, Texture2D, TextureCube, Terrain, and
  skeletal uncooked authoring tests pass; no migrated call site copies or
  retains a Plugin callback or raw feature pointer.

### Milestone 4: Specialized registry integration and callback audit

- **Dependencies:** Milestone 3 establishes the production pattern.
- **Deliverable:** Asset import providers, build-host contributions, local build
  functions, thumbnail providers, workspace registrations, delegates, timers,
  file watchers, render callbacks, and other cross-DLL execution paths are
  classified and either migrated to typed features or connected to common
  owner retirement and operation drain.
- **Entry gate:** The authoring migration demonstrates stable Core APIs and
  actionable unload diagnostics.
- **Exit gate:** A repository-targeted audit finds no process-retained Plugin
  callable without explicit owner attribution, retirement, and destruction
  proof. Specialized registries retain their domain selection semantics.

### Milestone 5: Dynamic DLL unload qualification

- **Dependencies:** Milestones 1-4.
- **Deliverable:** A real test module that can block synchronous calls, run and
  cancel worker/GT chains, retain destructor-sensitive captures, unload,
  reload, and verify a new generation without invoking old code.
- **Entry gate:** The callback audit has no required unresolved owner path.
- **Exit gate:** Repeated unload/reload stress passes under sanitizers or the
  strongest available Windows diagnostics; every injected drain failure
  prevents `FreeLibrary`; lasting lifecycle contracts are moved into Runtime
  Core and relevant Editor architecture documentation.

## Child Plan Boundaries

Create a child plan only when its entry gate is satisfied and the work is ready
to become active.

| Proposed child plan | Owns | Entry gate |
| --- | --- | --- |
| [Modular Feature Registry and Module Retirement](../Plans/ModularFeatureRegistryAndModuleRetirement.md) | Milestone 1 Core API, state machine, manager integration, and tests | Complete; exit gate passed |
| [Module Async Operation Drain](../Plans/ModuleAsyncOperationDrain.md) | Milestone 2 task ownership, explicit abort, GT drain, and callable destruction proof | Complete; exit gate passed |
| [Engine Authoring Modular Feature Migration](../Plans/EngineAuthoringModularFeatureMigration.md) | Milestone 3 contracts, providers, consumers, and legacy API deletion | Complete; exit gate passed |
| [Dynamic Module Registry Safety Audit](../Plans/DynamicModuleRegistrySafetyAudit.md) | Milestone 4 inventory, classification, and specialized-registry integration | Active; Milestone 3 exit gate passed |
| `Documentation/Plans/DynamicDllUnloadQualification.md` | Milestone 5 real DLL fixture, stress testing, and lasting documentation | Milestone 4 exit gate passes |

## Program Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Registration identity | A stale or foreign token cannot retire a replacement implementation |
| Invocation race | A call racing retirement either enters and is awaited or is rejected before Plugin code executes |
| Registry cardinality | Zero, one, and ambiguous providers produce explicit typed results without registration-order selection |
| Explicit abort | Operation state and abort reason, not module availability, determine cancel/drain behavior |
| Worker drain | Closing a group rejects new root work and waits or cancels every accepted worker and descendant |
| Game Thread drain | Ready continuations are processed according to policy; canceled/invalid continuations are detached and their Plugin captures destroyed |
| Destructor safety | Custom callable captures and deleters record destruction before unload is permitted |
| Self-unload | Unload requested from the module's own invocation, task, continuation, or destructor is rejected without deadlock |
| Timeout behavior | Every drain timeout returns a categorized failure and leaves the native library mapped |
| Reflected objects | Class defaults and module-owned virtual objects drain before module instance destruction |
| Real DLL lifecycle | A dynamic fixture unloads and reloads repeatedly without calling a prior generation or retaining prior callable storage |
| Functional regression | Existing asset import, post-load, source relocation, Build, cook, and editor workflows retain their expected results |

Each child plan selects the smallest relevant native-test targets according to
[Agent Testing Workflow](../Agents/Testing.md) and validates builds according to
[Agent Build and Run Workflow](../Agents/BuildAndRun.md).

## Risks and Control Gates

### Hidden Plugin code in destruction paths

`std::function`, move-only callables, allocator state, custom deleters, virtual
destructors, coroutine frames, and captured smart pointers may execute Plugin
code during destruction. No drain is accepted until Core has destroyed all such
storage while the DLL is mapped. Tests must include destructor-sensitive
captures, not only callbacks that increment execution counters.

### Game Thread reentrancy and deadlock

Pumping unrelated deferred work during unload can create new work or reenter
systems being dismantled. Milestone 2 cannot exit until drain selects the owner
or scope being closed, rejects recursive pumping, and reports unsupported
self-waits instead of blocking.

### Asynchronous work escaping after a synchronous feature call

A feature invocation may return after submitting work. That work must already
belong to a closing-aware operation group. APIs returning a Plugin-owned
polymorphic session require a separately tracked retirement contract or must be
redesigned to return Engine-owned data and opaque operation handles.

### Partial shutdown failure

Once feature admission is retired, rollback to `Active` is unsafe because
callers may have observed unavailability and module teardown may be partial.
Failed unload therefore transitions to a stopped-but-mapped diagnostic state.
Recovery or restart, if later required, needs its own evidence-gated design.

### Legacy and new paths coexisting

Compatibility wrappers may temporarily invoke the new registry, but a migrated
feature cannot remain registered in both the old callback slot and the new
registry. Each child plan defines one cutover point and deletes its legacy
storage before passing its exit gate.

### Specialized registry over-generalization

Import providers and build functions carry identity, version, matching,
provenance, and request-owner semantics that a generic feature list does not
replace. Milestone 4 must reuse common unload primitives without weakening
those domain contracts.

## Completion Criteria

This roadmap is complete when:

- all five required milestones pass their exit gates;
- every proposed child plan is completed and retained as active or archived
  provenance;
- `FModuleManager` refuses unsafe unloads with actionable diagnostics and never
  calls `FreeLibrary` after a failed audit;
- the six Runtime Engine authoring callback families use typed modular features
  with no process-global Plugin callable storage;
- asynchronous abort is represented exclusively by operation state,
  cancellation tokens, and explicit abort reasons;
- Game Thread drain proves both terminal state and callable destruction;
- the specialized-registry audit has no required unresolved cross-DLL path;
- a real DLL unload/reload fixture passes repeated stress validation; and
- implemented long-lived contracts are documented in their owning Runtime Core
  and Editor architecture documents.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [Module manager interface](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [Module manager implementation](../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
- [Task system interface](../../Engine/Source/Runtime/Core/Public/Threading/Task.h)
- [Task system implementation](../../Engine/Source/Runtime/Core/Private/Threading/Task.cpp)
- [Static-mesh authoring callbacks](../../Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshAuthoring.cpp)
- [Texture2D post-load callback](../../Engine/Source/Runtime/Engine/Private/Texture/Texture2DPostLoad.cpp)
- [TextureCube post-load callback](../../Engine/Source/Runtime/Engine/Private/Texture/TextureCubePostLoad.cpp)
- [Terrain authoring callbacks](../../Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapPostLoad.cpp)
- [Skeletal payload callbacks](../../Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalAssetPostLoad.cpp)
- [Standard asset import registrations](../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [Terrain asynchronous authoring policy](../../Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp)
- [Geometry build module](../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildModule.cpp)
- [Asset build host](../../Engine/Source/Developer/AssetBuildCore/Private/AssetBuildCore.cpp)
- [Asset import provider registry](../../Engine/Source/Editor/AssetImportCore/Private/AssetImportCore.cpp)
