# Asset Compiling Manager Refactor Plan

Summary: Replace the family-neutral AssetBuildCore BuildHost with an Engine-owned object-aware asset compilation manager while preserving safe module and task lifetimes

Last reviewed: 2026-08-26

Status: Archived
Completed: 2026-08-26

## Current Status

Completed. Runtime `Engine` now owns the process `FAssetCompilingManager`, its
object-aware `IAssetCompilationDomain` contract, dependency ordering, bounded fair processing,
selected finish/cancel operations, successful post-compile event, aggregate
diagnostics, owner-gated scoped registrations, and terminal shutdown. Material
is the built-in Runtime domain and Texture2D is an optional, unload-safe
`TextureBuild` domain. `EngineLoop` owns the only normal-frame pump; MainFrame
no longer participates in compilation lifecycle. The former AssetBuildCore
BuildHost surface, implementation, contribution, tests, and lasting ownership
statements are removed.

Validation on 2026-08-26 passed the full DurinEditor and DurinGame builds,
hidden-window editor and game smoke runs, the 58-target `fast-all` aggregate,
focused `AssetCompilingManagerTests` (1/1), `AssetBuildCoreTests` (5/5),
`MaterialTests` (93/93), `TextureTests` (87/87), `EditorRenderingTests` (77/77),
`SkyBoxTests` (11/11), and `TextureCookIntegrationTests` (1/1). Isolated
`CoreConcurrencyTests` passed 141/141 after a parallel-load-only first failure.
The optional `MaterialVulkanTests` run confirmed all compilation-lifecycle
assertions after migration but remains red on its pre-existing HDR highlight
calibration checks (measured peak 464 versus threshold 690); those checks were
already failing before the test lifecycle adaptation and are outside this
refactor. Editor/game configuration, deployment inspection, and source-symbol
audits confirm Engine has no new Developer/Editor dependency and DurinGame does
not deploy TextureBuild, AssetBuildCore, or DerivedDataCache.

The lasting contract is published in [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
and routed from the runtime lifecycle, asset lifecycle, material, texture, and
workspace module documents.

## Goal

- Establish one Engine-owned authority for discovering active compilation
  domains, pumping completed work, reporting remaining assets, finishing all or
  selected objects, requesting cancellation, emitting successful post-compile
  notifications, and shutting compilation down before Core task admission
  closes.
- Replace `AssetBuildCore::BuildHost` completely; do not retain two public
  process aggregators or a forwarding compatibility facade after migration.
- Migrate both existing object compilation paths: the Runtime material compile
  domain and the Editor-selected asynchronous Texture2D compilation domain.
- Preserve concrete-domain ownership of scheduling, detached worker values,
  DDC requests, validation, generation admission, asset publication,
  diagnostics, concurrency limits, retained values, and memory budgets.
- Preserve Durin's dynamic-module safety: no callback may enter a retired
  provider module, and no registration may disappear while a domain call or
  retained completion value still owns provider code or storage.
- Give future Geometry, Shader, audio, or plugin compilation domains one stable
  integration contract without making Runtime Engine depend on Developer or
  Editor modules.

## Scope

- Add public `IAssetCompilationDomain` and `FAssetCompilingManager` contracts to
  Runtime `Engine`, plus process lifecycle, registration, diagnostics, event,
  and test seams.
- Add a move-only scoped domain registration that retains a provider module
  resource and gates every provider callback through its owner gate.
- Add unique compile-domain names, dependency-name ordering, deterministic
  registration validation, aggregate remaining-asset accounting, bounded frame
  processing, selected-object finish, advisory selected-object cancellation,
  finish-all, and terminal shutdown.
- Refactor the Engine-owned material compile service into an
  `FMaterialCompilationDomain` implementation without changing material program
  identity, single-flight sharing, last-known-good visibility, compile results,
  renderer publication, Cook payloads, or reload semantics.
- Refactor TextureBuild's compilation-domain state into an
	`FTexture2DCompilationDomain` implementation that directly owns worker
  admission and the completion mailbox.
- Route EngineLoop and MainFrame compilation lifecycle through the new global
  manager, with exactly one normal-frame pump.
- Replace BuildHost-dependent tests and test environments, then remove
  BuildHost declarations, implementation, registration, startup, pump, wait,
  snapshot, shutdown, and documentation.
- Update module-routing, runtime lifecycle, texture-system, and asset-lifecycle
  documentation to name the new authority and the remaining synchronous role of
  `AssetBuildCore`.

## Non-Goals

- Making `DerivedDataCache` asynchronous or asset-aware, moving DDC policy out
  of `FBuildSession`, or changing any bucket, key, payload, cache-hit, store, or
  trim behavior.
- Moving TextureBuild or GeometryBuild recipe implementations into Engine, or
  adding an Engine dependency on `AssetBuildCore`, `DerivedDataCache`,
  `TextureBuild`, `GeometryBuild`, AssetForge, or an Editor module.
- Changing Texture2D compilation's two-worker default,
  priority fairness, 1 GiB estimated in-flight budget, worker phases,
  cancellation polling, completion history, or DDC behavior except where its
  owner and pump entry point change.
- Redesigning material compilation, adding a material DDC, changing shader
  compilation, changing renderer publication, or altering last-known-good,
  reload, Cook, and ErrorMaterial policy.
- Converting synchronous GeometryBuild recipes into asynchronous jobs in this
  plan. Geometry receives the stable extension point but is not registered
  until it owns a real asynchronous object lifecycle.
- Registering AssetForge translators, planning passes, builders, or import jobs
  as asset compilation domains. Import operations remain transaction owners and
  may submit or wait for asset compilation through the object-level API.
- Absorbing component-local runtime work such as Terrain collision rebuilding,
  rendering-thread work, asset discovery, package loading, cooking, or generic
  Core tasks merely because those operations are asynchronous.
- Adding an editor progress window, status-bar redesign, task cancellation UI,
  package-scope event, distributed build controller, or dedicated central
  compilation thread pool. Aggregate diagnostics and events are sufficient for
  later UI work.
- Matching Unreal Engine header layout, spelling, or implementation line for
  line. UE supplies the semantic model; Durin ownership and lifecycle rules
  remain authoritative.

## Design Decisions and Invariants

### Engine owns the object-aware aggregate

The public dependency direction is:

```text
Core / CoreDObject / AssetCore
              ^
              |
           Engine
              ^
              |
      +-------+-----------+
      |                   |
AssetBuildCore       compile providers
      ^              (TextureBuild today,
      |               GeometryBuild later)
      +-------+-----------+
              |
        family recipes
```

`Engine` owns the aggregate because the contract accepts `DObject` instances,
Runtime material compilation must be present in DurinGame, and optional
Developer modules already depend on Engine. Engine never includes or links a
provider header. Provider modules register implementations after they load and
unregister before their code or storage retires.

`AssetBuildCore` remains Core-only at its public boundary and continues to own
immutable build definitions, values, functions, synchronous sessions, and DDC
policy adaptation. Removing BuildHost narrows it; it does not move recipe or
cache mechanics into Engine.

### Compile domains, not reflected-class registrations

`IAssetCompilationDomain` represents one independently scheduled compile domain.
It exposes a unique stable `FName`-style domain name and zero or more dependency
domain names. It does not expose `DClass*`, register an output class string, or
promise one manager per reflected class.

Object-level operations accept a bounded `std::span<DObject* const>`. The
global manager invokes registered domains in dependency order; each concrete
manager filters or casts the objects it owns. This permits one manager to own a
class family and permits one object to participate in more than one compile
domain later without central reflected-class policy.

Initial domain identities are stable canonical names:

```text
Durin.MaterialCompilation
Durin.TextureCompilation
```

They are process routing and diagnostics identities, not serialized asset,
Build, DDC, or Cook identities.

### Minimum manager contract

The exact C++ split follows Engine conventions, but the first supported
contract contains these semantics:

| Operation | Required behavior |
| --- | --- |
| `GetAssetTypeName` | Return the unique compile-domain identity |
| `GetDependentTypeNames` | Return domains that should process/finish before this domain |
| `Start` | Open provider admission after the process manager is running |
| `GetNumRemainingAssets` | Count live object consumers that have not reached an asset-visible terminal state, not raw worker jobs |
| `ProcessAsyncTasks` | On GameThread, consume completed detached results and publish only currently admissible results within the supplied budget |
| `FinishCompilationForObjects` | On GameThread, wait and publish only matching owned objects and required domain dependencies |
| `MarkCompilationAsCanceled` | Request best-effort cancellation for matching owned objects without promising quiescence |
| `FinishAllCompilation` | Wait and publish every accepted object in the domain |
| `Shutdown` | Stop admission, cancel or drain according to domain policy, publish required terminal callbacks, and become quiescent |

`FinishCompilationForObjects` and `FinishAllCompilation` are explicit blocking
boundaries. Worker completion alone is insufficient: they return only after all
matching GameThread admission/publication work has run and object-visible state
is terminal. Cancellation remains advisory; callers requiring quiescence must
finish the objects afterward.

### Scoped registration and module retirement

The public registration operation returns a move-only
`FAssetCompilationDomainRegistration`. External registrations require a valid
`FModuleOwnedCallbackGate`; registration retains a resource lease for the
complete registered lifetime. A registration reset:

```text
remove from new dispatch snapshots
  -> stop provider admission
  -> wait/cancel and publish terminal callbacks
  -> release manager callback captures and retained values
  -> release the module resource lease
```

Calls execute without the aggregate registry mutex held. Each dispatch first
captures an ordered list containing resource leases and owner gates, then enters
the gate immediately before invoking provider code. Reentrant query, event, and
registration calls therefore cannot deadlock or invalidate the current
iteration. Owner retirement rejects later callback entry and cannot leave a raw
provider pointer reachable from a future snapshot.

The built-in material manager is process-owned by Engine and follows the same
manager behavior, but it does not pretend to be an unloadable external module.
Its lifetime is nested inside the Engine compilation-manager lifecycle.

### Registration and dependency order

- Domain names are nonempty, canonical, and unique among live registrations.
- Dependencies are domain names, not asset-reference edges or task handles.
- Missing dependency names are permitted because optional modules may be absent;
  they produce bounded diagnostics and contribute no ordering edge.
- A dependency cycle among currently registered domains rejects the new or
  replacement registration transactionally and preserves the previous order.
- The order is a deterministic topological sort; independent domains use their
  canonical name as the tie-breaker.
- Normal processing and finish operations use dependency-first order. Shutdown
  stops admission for every domain first, then waits and drains in reverse
  dependency order so dependents release consumers before prerequisites retire.
- Registering a provider while the aggregate is running starts it before it
  becomes visible. Start failure leaves no registration. Removing and later
  re-registering an optional provider is supported before aggregate shutdown.

### Frame processing and fairness

EngineLoop owns the one process frame call. `MainFrame` no longer pumps an
independent authoring host. The call runs on GameThread after the Core task
system is available and before rendering consumes newly published asset state.

`FAssetCompileProcessParams` carries at least a normal-frame completion limit
and an optional time deadline. The initial normal-frame completion limit remains
64, preserving TextureBuild behavior. The aggregate retains a round-robin
cursor among dependency-ready managers so an always-busy earlier domain cannot
consume every frame budget. A manager reports the number of completions it
consumed and returns successfully published live objects separately from
failed, canceled, superseded, or discarded results.

Explicit finish and shutdown use an unbounded completion count but retain
domain-owned timeout and failure diagnostics. They must pump GameThread
publication while waiting and must reject unsupported off-GameThread calls
rather than deadlocking.

### Completion events and diagnostics

The aggregate exposes a post-compile event carrying weak `DObject` identities
and the producing domain name. It fires after successful, current, asset-visible
GameThread publication and outside the registry mutex/provider call. Failed,
canceled, superseded, destroyed, and stale-generation results update their
concrete domain diagnostics but do not masquerade as post-compile success.

Duplicate successful reports for the same object and domain in one aggregate
operation are coalesced. Event listeners may submit new work; that work is not
processed recursively in the current provider iteration.

Aggregate diagnostics include domain count, accepting/shutdown state, summed
remaining assets, processed completion count, and per-domain remaining counts.
Concrete material and texture diagnostics remain authoritative for phases,
timings, memory, DDC origin, failures, retained programs, and queue details.
Counts use saturating addition.

### Object identity, generation, and worker boundary

Concrete domains retain weak object identity plus a generation-qualified
owner token. Object paths are diagnostic and scheduling identities but are not
sufficient publication authority because replacement can create a new object
at the same path. A current result must match the live weak object, path where
applicable, request generation, authored/source revision, settings/target, and
manager-specific dependencies before publication.

Workers receive bounded immutable values only. They do not resolve, retain, or
mutate `DObject`, packages, editor widgets, Renderer, RHI, or registries.
Concrete domains alone own worker scheduling, priorities, task scopes,
single-flight records, memory budgets, completion mailboxes, and publication.
The aggregate owns no payload and cannot write DDC entries.

### Task scheduler and cancellation ownership

The refactor does not add a shared asset-compilation thread pool. Providers use
Core's process task scheduler and retain their own task attribution,
cancellation sources, task scopes, concurrency limits, and module-owned async
operation groups. This preserves unload auditing and lets Material and Texture
keep different resource policies.

Aggregate shutdown is ordered before Core task scheduler shutdown. Provider
shutdown cannot silently abandon a task, completion mailbox value, callback, or
weak consumer. A timeout is a hard lifecycle failure with diagnostics; it is
not converted into successful shutdown.

### Material migration preserves its compile contract

`FMaterialCompilationDomain` is an Engine-private concrete implementation. It
absorbs the lifecycle currently surfaced as initialize, pump, and shutdown
free functions. Existing typed request, cancel, state, result, diagnostics,
single-flight, last-known-good, shader reload, and publication APIs remain or
become thin typed calls into the concrete domain.

Material remaining-asset count is outstanding live consumer count, not the
number of shared program-identity flights. Selected-object finish and cancel
filter `DMaterial`; instances continue to share their root material program and
do not become separate compile consumers. The migration changes no compiler,
identity, shader cache, Cook payload, or Renderer contract.

### Texture migration consolidates one domain owner

`FTexture2DCompilationDomain` lives in TextureBuild and owns the compilation-domain
state, worker admission, queues, memory budget, and completion mailbox directly.
TextureBuild registers it with Engine during module startup using the module's
owner gate and async operation group.

The current typed functions remain supported for direct Texture workflows:
submit, diagnostic, pending query, cancel, and wait. They delegate to the
concrete domain. New requests remain latest-wins; completion callbacks remain
exactly once; successful publication remains GameThread-only. A selected-object
finish filters `DTexture2D`, waits the corresponding request, performs an
  unbounded domain completion pump, and verifies Ready state.

TextureCube, VolumeTexture, and other synchronous TextureBuild operations do
not count as pending compilation merely because their recipe module registers
with AssetBuildCore.

### Process lifecycle

The selected order is:

```text
Core task scheduler starts
  -> Engine asset-compiling aggregate starts
  -> built-in Material manager starts
  -> optional authoring modules load and register domains
  -> EngineLoop processes async compilation once per frame
  -> editor/import producers stop submitting work
  -> optional registrations reset and quiesce
  -> aggregate stops all remaining admission
  -> aggregate finishes/cancels and drains domains
  -> built-in Material manager releases retained state
  -> Core task scheduler closes
```

Launch replaces direct Material service lifecycle calls with aggregate
lifecycle. MainFrame removes BuildHost initialize, frame pump, and shutdown.
Provider registration while the global manager is not accepting requests is
rejected with a diagnostic rather than creating dormant work.

### Compatibility and deletion policy

This is a source-breaking internal refactor. No deprecated forwarding
`BuildHost.h`, alias types, dual registration, or environment switch remains at
the end. A short implementation-stage cutover may temporarily keep both paths
behind private code, but no request may be visible to both hosts and no stage
may land with two normal-frame pumps.

The following behavior remains compatible:

- Texture typed submit/pending/cancel/wait/diagnostic entry points;
- material typed request/cancel/state/diagnostic entry points;
- exactly-once accepted Texture completion callbacks;
- generation-safe latest-wins publication;
- DDC and Build result identities and bytes;
- material last-known-good and reload behavior;
- editor and game target module closure.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| Generic cache | Synchronous backend-neutral `DerivedDataCache` | None; remains below compilation |
| Recipe execution | `AssetBuildCore::FBuildSession` and function registry | Remove unrelated process host ownership |
| Global authoring host | BuildHost service callbacks, leases, aggregate wait/snapshot, ordered drain | Add object semantics, compile domains, dependencies, events, and Runtime ownership |
| Material | Bounded Engine service, weak/generation owners, single-flight, last-known-good, GameThread admission | Register as a first-class compile domain and use aggregate frame/shutdown APIs |
| Texture2D | Coordinator queues, memory budget, cancellation, mailbox, typed authoring map | Register as a first-class compile domain and use global object finish/cancel/progress |
| AssetForge | Async import jobs and string-selected providers | Remains separate; may call object-level finish when publication requires readiness |
| Core tasks | Cancellation, task scopes, attribution, module async-operation audit | Reused by each concrete domain; no new pool |
| Launch/MainFrame | Separate Material and BuildHost lifecycle/pumps | One EngineLoop aggregate lifecycle and frame pump |
| Tests | Strong BuildHost, Texture coordinator, and Material lifecycle coverage | Move host invariants to Engine manager tests and add cross-domain object behavior |

## Implementation Stages

### Stage 0: Lock the replacement contract

- [x] Select Runtime Engine as the global object-aware compilation authority.
- [x] Select compile-domain names plus `DObject` broadcast/filter routing rather
  than `DClass` or AssetForge output-class registration.
- [x] Select scoped, owner-gated registration and retained leases instead of
  raw provider pointers.
- [x] Select dependency-first processing/finish and reverse-order shutdown with
  transactional cycle rejection.
- [x] Select one EngineLoop frame pump and removal of MainFrame BuildHost pump.
- [x] Select Material and Texture2D as the initial concrete domain migrations.
- [x] Select complete BuildHost deletion with no public compatibility facade.
- [x] Lock object generation, worker isolation, cancellation, publication,
  event, diagnostics, task ownership, and target-closure invariants.

#### Acceptance Gate

- Scope, ownership, dependency direction, API semantics, migration set,
  deletion boundary, stage ordering, and validation requirements are explicit.

### Stage 1: Establish the Engine compilation aggregate

- [x] Add Engine public contracts for process parameters, aggregate diagnostics,
  post-compile data/event, `IAssetCompilationDomain`, scoped registration, and
  `FAssetCompilingManager`.
- [x] Implement unique-name validation, owner-gated registration snapshots,
  retained leases, deterministic dependency ordering, missing-dependency
  diagnostics, and transactional cycle rejection.
- [x] Implement start, late registration, scoped unregister/quiescence,
  bounded round-robin frame processing, aggregate count, selected-object finish,
  advisory cancel, finish-all, and terminal shutdown.
- [x] Ensure all provider calls and event dispatch happen outside the registry
  mutex and reject unsupported thread or lifecycle state explicitly.
- [x] Add focused Engine tests with synthetic domains for ordering, fairness,
  reentrancy, duplicate names, cycles, missing dependencies, count saturation,
  selected-object broadcast/filtering, cancel-versus-finish semantics,
  post-compile coalescing, start rollback, shutdown order, registration reset,
  and owner retirement.
- [x] Add configuration/build metadata for the new Engine sources without adding
  any Engine dependency on Developer or Editor modules.

#### Acceptance Gate

- Synthetic-manager tests prove the complete aggregate contract, including no
  callback after owner retirement and no registry lock held across provider or
  listener code.
- Engine and DurinGame dependency closure contains no new Developer or Editor
  module and no DDC path is reachable through the new public headers.

### Stage 2: Migrate Runtime material compilation

- [x] Refactor the private material compile service into an Engine-owned
  `FMaterialCompilationDomain` implementing the new domain contract.
- [x] Preserve material worker envelope, task attribution/scope, bounds,
  identity single-flight, retained result budget, weak owner generation,
  last-known-good state, reload policy, diagnostics, and GameThread admission.
- [x] Make remaining count reflect outstanding live material consumers and add
  selected `DMaterial` finish/cancel behavior with current-result publication.
- [x] Replace Launch's direct Material initialize, frame pump, and shutdown with
  global asset-compilation lifecycle and the one EngineLoop frame call.
- [x] Retain typed material request, cancel, state, reload, and diagnostic APIs;
  remove only lifecycle entry points made private or redundant by the global
  manager.
- [x] Extend material lifecycle tests for aggregate remaining count,
  selected-object finish, advisory cancellation followed by finish, successful
  post-compile event, stale/destroyed-object suppression, and aggregate shutdown.

#### Acceptance Gate

- Existing material lifecycle, renderer publication, reload, Cook/load, and
  failure tests retain their results and no test directly pumps or shuts down a
  second Material service.
- DurinGame starts, processes, and shuts material compilation through the Engine
  aggregate before Core task admission closes.

### Stage 3: Migrate Texture2D compilation

- [x] Add `FTexture2DCompilationDomain` in TextureBuild and move the global
  coordinator service state plus Texture2D compilation state under its ownership.
- [x] Register the domain during TextureBuild module startup with the existing
  module callback gate and async operation group; reset registration before
  build functions and module-owned state retire.
- [x] Preserve Texture2D worker admission, priorities,
  fairness, memory budget, phases, metrics, mailbox, cancellation polling,
  result history, and test hooks.
- [x] Route typed submit, diagnostic, pending, cancel, and wait APIs through the
  concrete domain while preserving exactly-once and supersession behavior.
- [x] Implement domain remaining count, frame completion processing, selected
  `DTexture2D` finish/cancel, finish-all, shutdown, and successful post-compile
  reporting.
- [x] Replace Texture test host helpers with scoped compilation-domain
  fixtures and migrate BuildHost snapshot assertions to aggregate plus
  texture-domain diagnostics.
- [x] Remove MainFrame BuildHost initialization, normal-frame pump, wait, and
  shutdown; EngineLoop remains the only normal process frame pump.

#### Acceptance Gate

- Texture scheduler, authoring, import/reimport, property editing, source
  replacement/relocation, DDC cold/warm/corrupt, cancellation, supersession,
  shutdown, and exactly-once completion tests pass through the new manager.
- A cross-domain test proves Material and Texture can be pending together,
  aggregate counts include both, a bounded frame cannot starve either, and
  finish/cancel routes only to owning domains.
- TextureBuild can unregister and re-register while the aggregate remains
  running, with no callback or retained value surviving provider retirement.

### Stage 4: Delete BuildHost and close consumers

- [x] Delete `BuildHost.h`, BuildHost implementation state and functions,
  Texture BuildHost contribution code, and AssetBuildCore host-only tests.
- [x] Remove all includes and calls to `InitializeBuildHost`,
  `PumpBuildHostCompletions`, `WaitForBuildHost`, `GetBuildHostSnapshot`,
  `ShutdownBuildHost`, `FBuildServiceContribution`, and
  `FBuildServiceRegistration`.
- [x] Audit save, Cook, import, editor close, asset replacement/destruction,
  module unload, test teardown, and process shutdown callers; use selected-object
  finish where identity is known and finish-all only at true global barriers.
- [x] Confirm AssetBuildCore retains only synchronous build session/function/DDC
  responsibilities and remove host-only includes, dependencies, and vocabulary.
- [x] Search source, tests, build metadata, and active lasting documentation for
  stale BuildHost symbols and dual frame pumps.

#### Acceptance Gate

- The repository contains no BuildHost API, implementation, include, call, test
  fixture, or lasting ownership statement.
- Save/Cook/import/close barriers observe asset-visible terminal publication,
  not merely worker completion, and process shutdown is quiescent before the
  task scheduler closes.

### Stage 5: Qualify targets and publish lasting contracts

- [x] Run focused Engine manager, Material lifecycle, Texture coordinator,
  Texture authoring/import, AssetBuildCore, module retirement, and task-lifetime
  tests according to the repository testing guide.
- [x] Run editor and game target builds, native aggregate tests, hidden-window
  editor smoke, and the relevant material/texture rendering and Cook/load suites
  according to repository build and test guidance.
- [x] Verify configuration-time module closure and deployed module sets:
  DurinGame contains Engine/Material compilation but excludes TextureBuild,
  AssetBuildCore, and DerivedDataCache; DurinEditor selects optional providers.
- [x] Update lasting Runtime, Workspace, and Editor ownership documents and
  remove superseded BuildHost descriptions.
- [x] Record exact validation evidence in Current Status, complete all gates,
  and only then mark and archive the plan through the documentation workflow.

#### Acceptance Gate

- Focused, aggregate, editor/game build, smoke, rendering, Cook/load, module
  closure, shutdown, documentation, all-plan, and all-roadmap validation pass.
- Lasting documents, not this plan, are the final authority for the implemented
  manager, provider, thread, object, module, and shutdown contracts.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Domain registration | Unique names, late register, scoped unregister, failed start rollback, missing dependency, cycle rejection, deterministic ordering |
| Module lifetime | Retained lease during calls, owner retirement rejection, unregister wait/drain, reload/re-register, no callbacks after retirement |
| Processing | GameThread enforcement, count/time budget, round-robin fairness, dependency order, reentrant listener, no lock across callbacks |
| Object operations | Broadcast/filter routing, selected finish publishes, advisory cancel does not imply quiescence, destroyed/replaced/stale objects cannot publish |
| Events and diagnostics | Successful post-compile only, duplicate coalescing, aggregate/per-domain counts, bounded failure diagnostics, saturating totals |
| Material | Existing generation, single-flight, last-known-good, failure, cancellation, reload, Renderer, Cook/load, and shutdown suites plus aggregate integration |
| Texture2D | Queue/fairness/budget, DDC, authoring, import/reimport, supersession, exactly-once callback, wait/publication, unload, and shutdown suites |
| Cross-domain | Material and Texture pending concurrently, aggregate progress, bounded fairness, dependency order, finish/cancel isolation, global finish/shutdown |
| AssetBuildCore | Session policies, DDC hit/miss/store/failure, function registration, and absence of BuildHost surface |
| Targets | DurinEditor and DurinGame build, native aggregate, hidden-window editor smoke, selected rendering and Cook/load targets |
| Dependency/deployment | Engine has no Developer/Editor dependency; game excludes DDC/build providers; editor selects optional providers explicitly |
| Documentation | Changed/all docs, all active plans, all roadmaps, and final symbol/link audit |

## Definition of Done

- Runtime Engine exposes one production `FAssetCompilingManager` authority and
  a module-safe `IAssetCompilationDomain` extension contract.
- Material and Texture2D use that authority for aggregate processing, count,
  selected-object finish/cancel, successful completion notification, and
  shutdown.
- EngineLoop performs exactly one normal-frame compilation pump; MainFrame and
  concrete services do not own competing process pumps.
- BuildHost is deleted and AssetBuildCore owns no object-aware or process async
  host lifecycle.
- Concrete domains preserve all existing worker, generation, publication,
  cache, compiler, diagnostic, memory, cancellation, and module-lifetime
  behavior selected by prior plans.
- Provider unload/reload, object destruction/replacement, cancellation,
  explicit finish, global finish, and process shutdown are deterministic and
  covered by tests.
- Runtime, Developer, Editor, game, and deployment dependency boundaries remain
  correct.
- All validation-matrix gates pass and lasting contracts describe the landed
  architecture before this plan is completed.

## Deferred Follow-ups

- Add Geometry compilation domains when StaticMesh, skeletal/animation, Terrain,
  or collision recipes acquire asynchronous object lifecycles.
- Decide whether RenderCore shader jobs should implement the compile-domain
  contract after they expose an object-level consumer model; do not register a
  synchronous cache merely for aggregate counts.
- Add aggregate editor progress/notification and cancellation UI using the
  manager diagnostics and post-compile event.
- Evaluate package-scope events only when package/object-handle instrumentation
  has a concrete subscriber.
- Evaluate priority promotion, shared compilation resource budgets, remote
  execution, or distributed build only after per-domain measurements show a
  cross-domain scheduling problem.
- Let AssetForge use selected-object finish at publication barriers where a
  measured correctness requirement exists; keep import job ownership separate.

## Related Documentation

- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Build and Run](../../../Agents/BuildAndRun.md)
- [Testing](../../../Agents/Testing.md)
- [Derived Data Cache Module Extraction Plan](DerivedDataCacheModuleExtraction.md)
- [Material Compile Lifecycle and Derived Data Plan](MaterialCompileLifecycleAndDerivedData.md)

## Related Code

- [`AssetCompilingManager.h`](../../../../Engine/Source/Runtime/Engine/Public/Asset/AssetCompilingManager.h)
- [`AssetCompilingManager.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetCompilingManager.cpp)
- `Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildHost.h` (removed)
- [`DerivedDataBuild.cpp`](../../../../Engine/Source/Developer/DerivedDataCache/Private/DerivedDataBuild.cpp)
- [`MaterialCompileLifecycle.h`](../../../../Engine/Source/Runtime/Engine/Public/Materials/MaterialCompileLifecycle.h)
- [`MaterialCompileLifecycle.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp)
- [`Texture2DCompilation.h`](../../../../Engine/Source/Developer/TextureBuild/Public/Texture/Texture2DCompilation.h)
- [`Texture2DCompilation.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/Texture2DCompilation.cpp)
- [`Texture2DCompilationDomain.h`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/Texture2DCompilationDomain.h)
- [`Texture2DCompilationDomain.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/Texture2DCompilationDomain.cpp)
- [`EngineLoop.cpp`](../../../../Engine/Source/Runtime/Launch/Private/EngineLoop.cpp)
- [`MainFrameModule.cpp`](../../../../Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp)
- [`DerivedDataBuildTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/DerivedDataBuildTests.cpp)
- [`MaterialCompileLifecycleTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Materials/MaterialCompileLifecycleTests.cpp)
- [`TextureBuildTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp)
