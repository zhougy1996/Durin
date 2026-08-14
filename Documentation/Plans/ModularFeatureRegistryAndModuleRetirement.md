# Modular Feature Registry and Module Retirement Plan

Summary: Implement Core-owned typed feature registration, bounded synchronous invocation, owner retirement, and fail-closed module unload without defining asynchronous abort through module lifetime.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Stage 0 is ready to begin. The parent roadmap has selected the architectural
boundary: this plan owns feature availability, synchronous invocation
quiescence, and module retirement only. It deliberately does not introduce a
module lease as an abort signal and does not attempt to drain asynchronous task
graphs or Game Thread continuations.

No production code has been changed. The initial implementation must first
stabilize the public Core types and test seams because the module lifecycle
context affects every `IModuleInterface` implementation.

## Goal

Provide a typed modular feature framework in Core and integrate it with
`FModuleManager` so that:

- a module registers an implementation under a manager-created owner identity;
- registration returns a move-only identity/generation token;
- callers invoke features only through a bounded synchronous visitor;
- retirement atomically rejects new calls and waits for admitted calls to
  return;
- module shutdown has explicit states, ordering, and categorized failure;
- `FreeLibrary` is never called after a failed synchronous retirement or module
  audit; and
- the design leaves asynchronous cancellation and drain to the next child plan.

## Scope

- Add the public and private Core modular-feature types.
- Add explicit feature name/version identity without relying on cross-DLL RTTI.
- Add manager-created module owner identity for one load generation.
- Add move-only feature registration and retirement state.
- Add `InvokeSingle` and `InvokeAll` bounded visitor APIs with explicit
  unavailable and ambiguous results.
- Add internal per-registration call admission and in-flight accounting.
- Add owner-wide feature retirement and diagnostic snapshots.
- Add explicit module startup/shutdown contexts carrying the unforgeable owner
  surface.
- Replace `bIsReady` as the only lifecycle representation with a module state
  machine.
- Return categorized results from shutdown and unload operations.
- Update all repository module implementations and direct lifecycle tests for
  the new context-bearing interface.
- Add Core concurrency tests and the smallest existing unload regression tests
  needed to validate the changed manager contract.

## Non-Goals

- Do not add `FAsyncOperationGroup`, async abort reasons, task-scope drain, or
  Game Thread continuation cleanup; those belong to the next child plan.
- Do not migrate StaticMesh, texture, Terrain, skeletal, provider, or build-host
  callbacks in this plan.
- Do not expose a module lease, feature lease, raw implementation pointer, or
  caller-retained reference from the public registry.
- Do not use feature retirement as cancellation of work previously submitted by
  an implementation.
- Do not replace delegates or domain-specific provider registries.
- Do not add feature priority or registration-order selection.
- Do not add stopped-module restart. A failed or explicitly stopped module may
  remain mapped but unavailable; restart requires a later selected design.
- Do not provide a stable external Plugin ABI during this development refactor.

## Design Decisions and Invariants

### Core owns identity and retirement state

`FModuleManager` creates one opaque owner identity for every successful module
load generation. Modules receive that identity through `FModuleContext`; they
cannot construct or reuse another module's owner. A reload receives a new
generation even when the logical module name is unchanged.

Feature entries and owner state live in Core memory. The registry stores a
non-owning `IModularFeature*` plus Core-owned identity, state, counters, and
diagnostics. It never owns the implementation, a Plugin deleter, or a
Plugin-authored `std::function`.

### Public feature identity does not depend on RTTI

Each typed interface declares a stable feature name and interface version. The
registry key is derived from those declared values rather than `typeid`, DLL
addresses, or registration order. Register and invoke templates validate that
the interface derives from `IModularFeature` and supplies the required static
identity.

### Invocation is bounded by a visitor

The public shape is equivalent to:

```cpp
template<CModularFeature T, typename F>
auto InvokeSingle(F&& Visitor) -> TFeatureInvokeResult<InvokeResult<F, T&>>;

template<CModularFeature T, typename F>
auto InvokeAll(F&& Visitor) -> TFeatureInvokeAllResult<InvokeResult<F, T&>>;
```

The registry does not expose `GetFeature<T>()`, `AcquireFeature<T>()`, or an
iterator of raw pointers. The feature reference passed to a visitor is valid
only for the dynamic extent of that invocation and must not be retained.

`InvokeSingle` distinguishes unavailable, invoked, and ambiguous outcomes.
`InvokeAll` pins the exact published registration set under the registry lock,
releases the lock, invokes each admitted entry, and returns per-entry outcomes.
No provider is selected merely because it registered first.

### The invocation gate is internal and synchronous

Each registration has an internal state:

```text
Published -> Retiring -> Retired
```

Lookup and retirement linearize under a defined lock order. A call either
enters while `Published` and increments the entry's in-flight count, or observes
retirement and never enters Plugin code. The count is decremented in Core after
the visitor returns or unwinds.

This gate proves synchronous-call quiescence only. It carries no cancellation
token, abort reason, task ownership, or business-visible liveness state.

### Registration tokens are identity-bearing and Core-destructible

Registration returns a move-only `FModularFeatureRegistration` containing the
entry identity and generation. Its destructor and `Reset()` implementation
reside in Core. Reset is idempotent and can retire only its matching entry. A
stale, moved-from, foreign, or previous-generation token cannot remove a current
provider.

Explicit retirement is split from waiting so self-retirement can close
admission without synchronously waiting on its own active call. The manager's
owner-wide retirement detects self-unload and rejects it before waiting.

### Module lifecycle is serialized and fail closed

Module load, shutdown, and unload remain serialized on the module-control
thread: the startup thread before Game Thread identity is installed and the
Game Thread afterward. Feature invocation and retirement accounting are
thread-safe.

The selected lifecycle states are equivalent to:

```text
Registered -> Loading -> Active -> Retiring -> StoppedMapped -> Unloaded
                         |             |
                         |             -> UnloadBlocked
                         -> LoadFailed
```

Exact enum spelling may change during Stage 0, but the following distinctions
must remain observable:

- registered metadata versus a loaded instance;
- loading versus callable active state;
- retirement in progress;
- stopped but still mapped;
- unload blocked with categorized evidence; and
- native library released.

Admission retirement is irreversible for a load generation. A shutdown failure
does not restore the module to `Active`.

### Shutdown ordering is fixed

For this plan's synchronous boundary, `ShutdownModule` and `UnloadModule` use:

1. transition the module from `Active` to `Retiring`;
2. retire every modular feature registered by its owner;
3. reject self-unload from an invocation owned by that module;
4. wait with a bounded policy for all admitted synchronous invocations;
5. run the existing reflected-object pre-shutdown callback while the DLL is
   mapped;
6. invoke `IModuleInterface::ShutdownModule(FModuleShutdownContext&)`;
7. require all owned feature entries to be retired and all invocation counts to
   be zero;
8. transition to `StoppedMapped`;
9. for physical unload, destroy the module instance and then call
   `FreeLibrary`;
10. report `Unloaded` only after the native handle is released.

Milestone 2 will insert explicit async and external-work drain inside the
shutdown callback/audit boundary without changing the feature retirement
semantics.

### Shutdown and unload return structured results

The public API no longer returns `void` for an operation that can be rejected.
The selected result distinguishes at least:

- not found or not loaded;
- already stopped;
- wrong control thread;
- recursive/self-owned execution;
- feature invocation drain timeout;
- reflected-object drain rejection;
- shutdown callback failure if the callback contract becomes fallible;
- outstanding feature registration or invocation audit failure; and
- successful stopped-mapped or unloaded completion.

The result carries the logical module name, observed state, a diagnostic
message, and the relevant retirement snapshot. `FModuleManager` does not erase
the module record or release the DLL on failure.

### Context-bearing module lifecycle is explicit

The preferred interface is:

```cpp
class IModuleInterface
{
public:
    virtual ~IModuleInterface() = default;
    virtual void StartupModule(FModuleContext& Context) {}
    virtual void ShutdownModule(FModuleShutdownContext& Context) {}
};
```

The startup context exposes the owner-scoped feature registration facade. The
shutdown context exposes retirement diagnostics required by this plan and is
reserved for the operation-drain surface added by the next plan. Neither
context exposes a mutable native-library handle.

Direct module lifecycle tests must use a Core test context/factory rather than
constructing owner identities themselves.

### Locking and callbacks do not overlap

No registry mutex or module-map mutex is held while invoking feature code,
module startup/shutdown code, reflected-object callbacks, or logging code.
Registry state is pinned with Core-only objects before locks are released.
Document and test the order between the module-manager lock, registry lock, and
entry gate lock; no code path may acquire them in the reverse order.

## Current Foundations and Gaps

- [`FModuleManager`](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
  already owns module instances and native handles but exposes only `bIsReady`
  and `void` shutdown/unload operations.
- [`FModuleManager::UnloadModule`](../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
  currently resets the module instance and calls `FreeLibrary` after
  `ShutdownModule()` without a feature-retirement audit.
- [`PreShutdownModuleCallback`](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
  already provides a reflected-object rejection boundary that must be retained
  inside the new state machine.
- Core already supplies unique delegate handles and thread-safe primitives that
  can inform identity generation, but delegate binding is not the feature
  registry and does not provide unload quiescence.
- Approximately eighteen repository modules implement `IModuleInterface`; the
  context signature migration is a required repository-wide compile update.
- Existing Vulkan RHI and Engine tests exercise real calls to `UnloadModule` and
  provide regression coverage after the Core unit contract is established.

## Implementation Stages

### Stage 0: Freeze the Core contract and baselines

- [x] Bound this plan to feature availability and synchronous retirement; keep
  async abort and continuation drain in the next child plan.
- [x] Select bounded visitor invocation instead of public raw pointers or
  feature/module leases.
- [x] Select explicit manager-created owner identity and context-bearing module
  lifecycle.
- [ ] Define the exact public feature identity, invoke result, registration,
  retirement snapshot, module state, shutdown result, and unload result types.
- [ ] Define the manager/registry/entry lock order and the control-thread rule.
- [ ] Inventory every `IModuleInterface` implementation and every direct
  `StartupModule`/`ShutdownModule` call that needs a test context.
- [ ] Identify current `UnloadModule` callers whose expectations depend on the
  old `void` result or immediate reload behavior.
- [ ] Add or select Core-private test seams for deterministic invocation and
  retirement barriers without exposing owner construction publicly.

#### Acceptance Gate

- The proposed headers contain no async cancellation or task-drain semantics.
- No public API returns or stores a raw feature implementation beyond a bounded
  visitor call.
- The state machine, lock order, shutdown order, and failure result are
  unambiguous enough to implement without unresolved ownership decisions.
- The compile-impact inventory covers all repository modules and direct
  lifecycle tests.

### Stage 1: Implement the typed modular feature registry

- [ ] Add the Core public feature marker, feature identity concept, invocation
  result types, registration token, retirement snapshot, and registry facade.
- [ ] Add Core-private registry, owner, entry, invocation-gate, and generation
  state with no Plugin-owned callable storage.
- [ ] Implement identity-checked register, retire, reset, owner-wide retirement,
  bounded wait, and diagnostics.
- [ ] Implement `InvokeSingle` and `InvokeAll` with explicit cardinality results
  and no callback execution under registry locks.
- [ ] Ensure visitor failure or exception paths release in-flight accounting in
  Core after Plugin invocation has returned.
- [ ] Detect attempted synchronous wait from the currently active matching
  feature invocation and return a self-wait result.
- [ ] Add focused Core tests for identity/version validation, duplicate entries,
  ambiguity, stale tokens, moves, idempotent reset, concurrent invoke/retire,
  timeout, and visitor failure.

#### Acceptance Gate

- Registry tests deterministically cover both sides of the invoke/retire race.
- A successful retirement snapshot has zero published entries and zero in-flight
  invocations for the selected registration or owner.
- A stale or foreign token cannot affect a current entry.
- Thread-safety tooling available in the repository reports no registry data
  race or lock-order violation.

### Stage 2: Integrate owner retirement into the module manager

- [ ] Add one Core-owned owner generation and explicit lifecycle state to each
  module record.
- [ ] Add startup and shutdown context types and update `IModuleInterface`.
- [ ] Update `IMPLEMENT_MODULE` integration without transferring native-handle
  ownership into Plugin code.
- [ ] Implement structured shutdown and unload results and update manager
  callers to inspect or deliberately assert them.
- [ ] Retire owner features and wait for synchronous invocations before the
  reflected-object callback and module shutdown callback.
- [ ] Preserve the existing reflected-object rejection behavior and map it to a
  fail-closed module state/result.
- [ ] Add self-unload detection based on the currently executing feature owner.
- [ ] Keep the module record and native handle when any retirement or audit gate
  fails.
- [ ] Define `UnloadModulesAtShutdown()` behavior through the same retirement
  and shutdown state transitions while preserving its process-exit decision not
  to physically release libraries.
- [ ] Correct stopped/blocked module lookup so `LoadModule()` never returns a
  non-active module instance as if it were ready.

#### Acceptance Gate

- No `FreeLibrary` path is reachable unless owner retirement, reflected-object
  drain, module shutdown, and the synchronous feature audit succeeded.
- Module state and result distinguish stopped-mapped, blocked, and unloaded
  outcomes.
- Recursive unload from an owned feature invocation is rejected without
  blocking.
- Existing process-shutdown reverse ordering remains deterministic.

### Stage 3: Migrate module implementations and direct lifecycle callers

- [ ] Update every Runtime, Editor, Developer, and sample-project module
  implementation to the context-bearing lifecycle signature.
- [ ] Store contexts only for their documented dynamic extent; modules retain
  owner-scoped registration tokens, not context references.
- [ ] Update tests that directly instantiate module classes to use the Core test
  context/factory or an appropriate manager path.
- [ ] Update existing unload/reload callers for structured results and the
  stopped-mapped failure contract.
- [ ] Verify module descriptors and public dependencies only where the new Core
  header surface requires them; do not add higher-level module dependencies to
  Core.

#### Acceptance Gate

- All module implementations compile with no context-free lifecycle override.
- Direct lifecycle tests cannot forge a production owner identity.
- No module retains `FModuleContext&`, `FModuleShutdownContext&`, or a mutable
  native-library handle after the callback returns.
- The repository has no ignored `UnloadModule` failure at a call site that
  requires successful unload for correctness.

### Stage 4: Validate retirement and publish the lasting Core contract

- [ ] Run the focused Core modular-feature and module-manager tests.
- [ ] Run existing non-rendering module load/unload tests selected by the test
  workflow.
- [ ] Run the smallest Vulkan RHI unload/reload regression slice when its
  prerequisites are available.
- [ ] Build all affected module targets and the applicable editor runtime
  variant according to the build workflow.
- [ ] Add the implemented modular-feature and module-retirement contract to the
  Runtime Core documentation and update the Module Loader section of Runtime
  Lifecycle.
- [ ] Update the parent roadmap status and activate the async-operation child
  plan only after this plan's definition of done is satisfied.

#### Acceptance Gate

- Focused tests pass repeatedly with deterministic race barriers.
- A representative module registers a test feature, retires it, shuts down, and
  unloads through the manager without exposing a feature pointer.
- Injected timeout, reflected-object rejection, and self-unload failures retain
  the DLL handle and return actionable diagnostics.
- Documentation validation, affected builds, and selected native tests pass.

## Validation Matrix

| Area | Validation | Evidence required |
| --- | --- | --- |
| Feature identity | Core unit tests | Stable name/version validation without RTTI or DLL-address identity |
| Registration ownership | Core unit tests | Foreign and stale token rejection; owner-wide retirement affects only one load generation |
| Cardinality | Core unit tests | Explicit unavailable, invoked, and ambiguous `InvokeSingle` results; deterministic `InvokeAll` snapshot |
| Invoke/retire race | Barrier-controlled multithreaded Core tests | Entered calls are awaited; later calls are rejected; no lock is held during visitor execution |
| Timeout and self-wait | Core unit tests | Categorized failure without deadlock or admission rollback |
| Module state | Module-manager unit/integration tests | Valid transitions and fail-closed stopped/blocked states |
| Reflected-object rejection | Existing or new integration seam | Rejection occurs while the DLL is mapped and prevents module destruction/unload |
| Module interface migration | Affected target builds | Every module override and direct lifecycle call uses the context-bearing contract |
| Existing explicit unload | Selected Engine/Vulkan RHI tests | Structured success is observed and successful reload receives a new owner generation |
| Documentation | Repository validators | Active plan, roadmap, and changed documentation links and metadata are valid |

Before selecting or running tests, follow
[Agent Testing Workflow](../Agents/Testing.md). Before configuring, building,
running, or recovering targets, follow
[Agent Build and Run Workflow](../Agents/BuildAndRun.md).

## Definition of Done

- Core exposes typed modular feature registration and bounded invocation with no
  public module/feature lease or raw implementation lookup.
- Registration and owner retirement are identity/generation safe and tested
  under deterministic concurrency.
- Every module load generation has an unforgeable owner identity and explicit
  lifecycle state.
- Module startup/shutdown contexts are used by every repository module
  implementation.
- Shutdown and unload return structured results and never release a DLL after a
  failed retirement, reflected-object, or synchronous audit gate.
- Self-unload and synchronous retirement timeout are diagnosed without
  deadlock.
- Existing unload/reload behavior that remains supported passes selected
  regression tests.
- The implemented Core contract is documented in the owning Runtime
  documentation.
- The parent roadmap records Milestone 1 complete and the next child plan is
  ready to activate.

## Deferred Follow-ups

- `FAsyncOperationGroup`, abort reasons, task inheritance, Game Thread drain,
  and retained-callable destruction proof belong to
  `Documentation/Plans/ModuleAsyncOperationDrain.md` after its roadmap entry
  gate is satisfied.
- Migration of Runtime Engine authoring callback slots belongs to
  `Documentation/Plans/EngineAuthoringModularFeatureMigration.md`.
- Asset import, Asset Build, thumbnail, workspace, delegate, timer, watcher, and
  render-callback audit belongs to
  `Documentation/Plans/DynamicModuleRegistrySafetyAudit.md`.
- A real unloadable DLL stress fixture belongs to
  `Documentation/Plans/DynamicDllUnloadQualification.md`.
- Restart of a stopped-mapped or unload-blocked module requires separate
  evidence and is not implied by this plan.

## Related Documentation

- [Parent roadmap](../Roadmaps/ModularFeatureAndDllUnloadSafety.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Code Modules](../Workspace/CodeModules.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [Module manager interface](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [Module manager implementation](../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
- [Delegate identity implementation](../../Engine/Source/Runtime/Core/Private/Delegates/Delegate.cpp)
- [Core native-test target](../../Engine/Tests/Native/CoreTests/CMakeLists.txt)
- [Vulkan RHI unload regressions](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp)

Expected new implementation paths include
`Engine/Source/Runtime/Core/Public/Modules/ModularFeature.h` and
`Engine/Source/Runtime/Core/Private/Modules/ModularFeature.cpp`; they remain
code-formatted rather than linked until created.
