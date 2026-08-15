# Module Lifecycle Interface Simplification Plan

Summary: Restore parameterless module lifecycle hooks while preserving generation-bound registration, asynchronous drain, and fail-closed DLL retirement behind a manager-owned startup scope.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

Stages 0 through 3 are implemented. Public module lifecycle hooks are
parameterless; Core installs a nested, exception-safe startup owner scope and
`FModuleStartup` is the sole production attribution facade. Shutdown policy
and diagnostics remain manager-owned. Production modules have migrated, the
shared owner uses `ModuleOwner`/`FModuleOwnerState` terminology, and tests now
use `FModuleTestOwner` or `FModuleTestHarness` from `ModuleTestSupport`.

Focused startup-scope coverage proves nested restoration, exception unwinding,
and rejection outside startup. The final full `all` build,
`CoreConcurrencyTests` (141 tests), `fast-all`, routine `test all`, and both
dynamic DLL unload qualification targets pass. The Core runtime contracts now
describe parameterless lifecycle, scoped startup attribution, and manager-owned
shutdown policy. Changed-document and all-plan validation pass.

## Goal

Give module authors a minimal and uniform lifecycle API:

```cpp
class IModuleInterface
{
public:
    virtual ~IModuleInterface() = default;
    virtual auto StartupModule() -> void {}
    virtual auto ShutdownModule() -> void {}
};
```

Owner-bound registrations and asynchronous groups remain attributable to the
exact module load generation through a startup-only facade whose identity is
installed and restored by `FModuleManager`.

## Scope

- Replace context-bearing `IModuleInterface` lifecycle hooks with parameterless
  hooks.
- Add a private, stack-disciplined current-module startup scope owned by Core.
- Add a narrow public startup facade for feature registration, specialized
  callback ownership, asynchronous operation groups, and current module name.
- Remove the production `FModuleContext` and `FModuleShutdownContext` types.
- Preserve existing module-retirement ordering, failure categories, owner
  generations, async drain, retained-resource audit, and fail-closed unload.
- Rename `FeatureOwner`/`FModularFeatureOwnerState` to
  `ModuleOwner`/`FModuleOwnerState` across Core and its tests so the shared
  owner accurately represents feature, callback, async, and unload ownership.
- Migrate production modules and remove empty or redundant lifecycle code.
- Separate registry-owner unit-test support from complete module-lifecycle test
  support and migrate tests away from direct context construction.
- Update the lasting Core runtime contract after the implementation is
  validated.

## Non-Goals

- Redesign modular-feature cardinality, invocation, registration tokens, or
  specialized registry selection.
- Permit production feature registration from arbitrary runtime code or after
  a module's startup callback returns.
- Relax final unload audits or restore a module to `Active` after retirement
  has begun.
- Add Unreal Engine hot-reload, pre-unload, post-load, or automatic-shutdown
  APIs that Durin does not otherwise require.
- Change rendering-thread, RHI, reflected-object, or application shutdown
  ordering beyond adapting call signatures.
- Replace module-owned registration tokens with convention-only cleanup.

## Design Decisions and Invariants

### Parameterless lifecycle surface

`IModuleInterface::StartupModule()` and `ShutdownModule()` take no arguments.
The interface remains limited to lifecycle notification; owner attribution is
not stored in or exposed through the base module object.

### Scoped current startup identity

Core installs `Detail::FScopedModuleStartup` immediately around each manager-
initiated startup callback. The scope carries the authoritative module name,
load generation, and owner state from `FModuleInfo`. It is an implementation
detail and is never constructed or retained by production modules.

The scope follows stack semantics. If module A synchronously loads module B
from A's startup callback, B temporarily becomes current and A is restored
after B returns. Normal return and exception unwinding both restore the prior
scope. The current identity lives in Core rather than in header-local state so
all native modules observe the same stack.

Module load remains restricted to the module-control thread. Startup-scope
access verifies that the caller is in the matching startup phase; missing,
stale, retiring, or wrong-phase access is a programming error and cannot
produce an unattributed registration.

### Startup-only facade

Production modules use a stateless `FModuleStartup` facade with this conceptual
surface:

```cpp
class FModuleStartup final
{
public:
    template<CModularFeature T>
    static auto RegisterFeature(T& Implementation)
        -> FModularFeatureRegistration;

    static auto CreateOwnedCallbackRegistration(FName DomainName)
        -> FModuleOwnedCallbackRegistration;

    static auto CreateAsyncOperationGroup(
        FName GroupName,
        FAsyncOperationGroupOptions Options = {})
        -> FAsyncOperationGroup;

    static auto GetModuleName() -> FName;
};
```

The facade resolves only the current scoped owner. It does not accept a module
name, generation, owner pointer, or context supplied by a caller. Owner-bearing
registry and async constructors remain internal implementation surfaces.

The facade is intentionally not a general service locator: it is valid only
during a manager-controlled startup callback and exposes only module-lifetime
attribution operations.

### Manager-owned shutdown policy

`ShutdownModule()` receives no context. The module callback performs ordered
business cleanup: close service admission, unregister specialized registry
entries, destroy escaped instances and result handles, flush native queues when
required, and release module-owned objects.

The manager exclusively owns owner-wide retirement evidence and policy. It
continues to close feature and task admission before the callback, run the
reflected-object drain while the image is mapped, invoke parameterless
shutdown, drain owner-bound asynchronous work, audit retained callable and
result storage, audit feature and resource retirement, and authorize native
release only after every gate passes. Diagnostics remain available through
`FModuleShutdownResult` and `FModuleUnloadResult` instead of a callback
parameter.

A module that needs ordering within its own cleanup uses the specific
`FAsyncOperationGroup` it already owns; it does not perform an owner-wide drain
through a shutdown context.

### Exact-generation ownership remains mandatory

Removing public contexts does not remove the module owner. Every modular
feature, specialized callback gate, and async group remains bound to the exact
Core-created load generation. Reloading the same logical module produces a new
owner, and stale handles cannot mutate or authorize the replacement.

The shared internal owner terminology describes its actual responsibility
rather than only its historical modular-feature origin. This refactor renames
`FeatureOwner`/`FModularFeatureOwnerState` to `ModuleOwner`/
`FModuleOwnerState` throughout Core, production call sites, tests, diagnostics,
and documentation. The rename is semantic clarification only: it does not
change owner generation, admission, retirement, or resource-audit behavior.

### Test seams are explicit and separate

Low-level registry and async tests use a test-only owner fixture that can
create, retire, snapshot, and drain an isolated owner without pretending to be
a production startup callback.

Tests of concrete module behavior use a module lifecycle harness. The harness
installs the same scoped startup identity as production, invokes parameterless
startup, and performs shutdown through the production retirement sequence.
Ordinary renderer, editor, and asset tests do not construct lifecycle contexts
or directly pair startup and shutdown callbacks.

### Cleanup remains token-based and fail-closed

Registration handles, specialized callback ownership, escaped instances, async
groups, and retained result handles remain explicit module-owned state. Process
and authoring rules guide their cleanup order, while the manager's final audit
continues to enforce correctness before DLL unmap.

## Current Foundations and Gaps

### Foundations to preserve

- `FModuleInfo` already owns the authoritative logical name, generation, owner,
  native handle, load order, and lifecycle state.
- Module load and shutdown already run on a designated control thread.
- Startup failure already retires the just-created owner before native release.
- Shutdown already closes synchronous feature admission and asynchronous group
  admission before calling module cleanup.
- Reverse load-order process shutdown and explicit unload use the same
  retirement machinery.
- Final feature, retained-resource, task, result, deferred-callable, and Worker
  callable audits already block native unload on failure.

### Gaps to close

- Owner-bound creation is reachable only through a passed `FModuleContext`.
- The lifecycle virtual interface exposes capabilities that several modules do
  not need and every shutdown implementation ignores.
- `FModuleShutdownContext` duplicates manager-owned drain and diagnostic
  responsibilities.
- `FeatureOwner` naming understates the shared owner used by async and
  specialized callback systems.
- `FDefaultModuleImpl` adds no behavior beyond `IModuleInterface` and currently
  has one production use.
- Several module headers retain named context parameters even where definitions
  ignore them, and MainFrame has an empty startup override.
- Test helpers expose synthetic production contexts and encourage direct
  lifecycle callback invocation.

## Implementation Stages

### Stage 0: Lock the public boundary and regression inventory

- [x] Record the exact production and test call sites for context-bearing
  lifecycle hooks, context creation, context member use, and direct callback
  invocation.
- [x] Confirm `FModuleStartup` is the sole production owner-attribution entry
  point and that no use case requires retaining startup authority.
- [x] Classify existing tests into low-level owner tests and complete module
  lifecycle tests before changing their helpers.
- [x] Add or identify coverage for nested module loading, startup failure,
  owner-generation reload, and outside-startup registration rejection.

#### Acceptance Gate

- Every existing context use has a named destination: startup facade, test
  owner fixture, lifecycle harness, shutdown result, or deletion.
- No unresolved production use requires a stored context or shutdown-wide
  drain API.
- The selected public API and misuse behavior are recorded in this plan before
  implementation changes begin.

### Stage 1: Introduce the scoped startup foundation

- [x] Implement the Core-owned current startup stack and
  `Detail::FScopedModuleStartup` with nested save/restore and exception-safe
  teardown.
- [x] Implement `FModuleStartup` as a stateless exported facade over the current
  owner.
- [x] Restrict raw owner-bearing feature, callback, and async creation APIs to
  Core internals and test-only friends.
- [x] Change `IModuleInterface` to parameterless hooks and update manager load,
  startup-failure, shutdown, and test-install call paths.
- [x] Delete `FModuleShutdownContext` and move any remaining diagnostic access
  to operation result or test-owner APIs.
- [x] Preserve the exact existing retirement transition and callback ordering.

#### Acceptance Gate

- A manager-started module can create all three owner-bound resource kinds
  without receiving a context.
- Calls through `FModuleStartup` outside the matching startup scope fail
  deterministically without publishing or retaining anything.
- Nested A-to-B startup attributes B's resources to B and restores A before A
  continues.
- Startup exception unwinding clears the current scope and preserves existing
  owner retirement and unload-block behavior.
- Parameterless shutdown still runs between admission closure/reflected-object
  drain and the manager's final async and feature audits.

### Stage 2: Migrate production modules and simplify module code

- [x] Convert every module declaration and definition to parameterless startup
  and shutdown hooks.
- [x] Replace production context member calls with `FModuleStartup` calls.
- [x] Remove empty startup overrides and unused declarations.
- [x] Remove `FDefaultModuleImpl` and inherit directly from
  `IModuleInterface` where a custom module class remains necessary.
- [x] Verify specialized-registry registrations, async groups, and modular
  feature tokens remain module-owned and are cleaned up in the required order.
- [x] Rename every `FeatureOwner` field/local and
  `FModularFeatureOwnerState` type reference to `ModuleOwner` and
  `FModuleOwnerState`, including Core internals, tests, diagnostics, comments,
  and lasting documentation.

#### Acceptance Gate

- Repository production sources contain no context-bearing lifecycle override
  and no `FModuleContext` or `FModuleShutdownContext` construction.
- Repository source and active documentation contain no `FeatureOwner` or
  `FModularFeatureOwnerState` identifier outside explicit historical
  provenance.
- Modules that do not create owner-bound state include no startup-facade
  dependency.
- Every owner-bound production registration still reports the correct logical
  module and load generation.
- Existing shutdown callbacks retain their service, registry, rendering, and
  native-resource cleanup order.

### Stage 3: Replace synthetic contexts in tests

- [x] Replace `FModuleTestContextFactory` with narrowly named low-level owner
  and complete lifecycle fixtures.
- [x] Migrate modular-feature and async-operation tests to the isolated test
  owner fixture.
- [x] Migrate direct Renderer and editor module startup/shutdown pairs to the
  lifecycle harness.
- [x] Ensure lifecycle tests observe shutdown and unload evidence through
  `FModuleShutdownResult`/`FModuleUnloadResult` rather than shutdown context
  snapshots.
- [x] Add focused tests for current-scope nesting, restoration, wrong-phase
  access, startup exception, reload generation, and final unload audit.

#### Acceptance Gate

- Ordinary engine tests neither construct production lifecycle contexts nor
  call a context-bearing hook.
- Direct lifecycle callback invocation exists only inside the Core-owned test
  harness implementation.
- Low-level tests can still exercise retirement races and retained-resource
  failure without installing a production module record.
- Dynamic DLL success and failure qualification continue to prove physical
  unmap authorization and fail-closed rejection.

### Stage 4: Validate and publish the lasting contract

- [x] Build affected Core, module, editor, and native-test targets using the
  repository build workflow.
- [x] Run focused Core modular-feature, async-operation, and module lifecycle
  tests.
- [x] Run affected Renderer/editor/asset tests that adopt the new lifecycle
  harness.
- [x] Run dynamic DLL unload success and failure qualification according to the
  native-test workflow.
- [x] Update the Core modular-feature/module-retirement contract with the
  parameterless lifecycle and scoped startup attribution rules.
- [x] Run changed-document and all-plan validation.

#### Acceptance Gate

- All focused and affected native tests pass with no retirement category or
  lifecycle-order regression.
- Physical unload qualification still proves exact-generation replacement and
  successful image unmap; failure qualification still leaves rejected images
  mapped.
- Repository search finds no retired lifecycle context type, context-bearing
  hook signature, or production raw-owner construction.
- Lasting behavior is documented in the Core runtime contract and this plan's
  implementation checklists contain evidence for every completed stage.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Public API | All production lifecycle declarations and definitions use parameterless hooks; retired context types have no references. |
| Startup scope | Focused Core tests cover normal access, outside-scope rejection, nested startup restoration, and exception unwinding. |
| Generation ownership | Reload tests prove the same logical module receives a distinct owner generation and stale handles cannot affect it. |
| Modular features | Existing identity, cardinality, invocation race, retirement, and retained-resource tests pass through the new owner seams. |
| Async operations | Existing admission, cancel/drain, self-wait, deferred callable, retained result, and failure-category tests pass. |
| Production modules | Affected Runtime, Developer, and Editor module targets compile and retain their previous cleanup ordering. |
| Renderer/editor fixtures | Tests formerly constructing Renderer contexts pass through the lifecycle harness without cross-test retained owners. |
| Native unload | Dynamic DLL unload success and failure qualification preserve physical mapping and fail-closed evidence. |
| Documentation | `doc validate --scope changed` and `doc plan validate --scope all` pass; the Core runtime contract reflects the implemented design. |

Build and test selection, process-conflict checks, target invocation, and result
reporting follow the repository agent workflows rather than commands copied
into this plan.

## Definition of Done

- `IModuleInterface` exposes only parameterless startup and shutdown hooks.
- `FModuleContext` and `FModuleShutdownContext` are removed from production and
  test APIs.
- `FModuleStartup` is the only production facade that creates owner-bound
  registration and async state during startup.
- The internal current-module scope is nested, exception-safe, control-thread
  checked, and inaccessible to production modules.
- No production caller can select or forge a module owner or load generation.
- Shared owner fields and state types consistently use the
  `ModuleOwner`/`FModuleOwnerState` terminology with no behavioral change.
- Manager-owned shutdown policy and final unload audits remain behaviorally
  unchanged.
- Production modules contain no empty lifecycle override or redundant default
  module base introduced by the previous interface shape.
- Tests use explicit low-level owner fixtures or the complete lifecycle harness,
  not synthetic lifecycle contexts.
- Focused, affected, and physical-unload qualification tests pass.
- The implemented lifecycle and attribution contract is published under
  `Documentation/Runtime/Core/` and all documentation validators pass.

## Deferred Follow-ups

- Consider shorter public names for specialized callback ownership only after
  the lifecycle refactor stabilizes; naming changes must not merge its
  semantics with generic modular features.
- Consider whether additional UE-style lifecycle notifications are useful only
  when a concrete reload or unload workflow requires them.
- Consider a repository lint for direct lifecycle callback invocation after the
  new test harness establishes the allowed boundary.

## Related Documentation

- [Modular Features and Module Retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Task System](../../../Runtime/Core/TaskSystem.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- [`ModuleManager.h`](../../../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [`ModuleManager.cpp`](../../../../Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp)
- [`ModuleTestSupport.h`](../../../../Engine/Source/Runtime/Core/Public/Modules/ModuleTestSupport.h)
- [`ModuleTestSupport.cpp`](../../../../Engine/Source/Runtime/Core/Private/Modules/ModuleTestSupport.cpp)
- [`ModularFeature.h`](../../../../Engine/Source/Runtime/Core/Public/Modules/ModularFeature.h)
- [`AsyncOperationGroup.h`](../../../../Engine/Source/Runtime/Core/Public/Modules/AsyncOperationGroup.h)
- [`RendererModule.h`](../../../../Engine/Source/Runtime/Renderer/Public/RendererModule.h)
- [`RendererModule.cpp`](../../../../Engine/Source/Runtime/Renderer/Private/RendererModule.cpp)
- [`ModularFeatureTests.cpp`](../../../../Engine/Tests/Native/CoreTests/Private/ModularFeatureTests.cpp)
- [`AsyncOperationGroupTests.cpp`](../../../../Engine/Tests/Native/CoreTests/Private/AsyncOperationGroupTests.cpp)
- [`DynamicUnloadFixtureModule.cpp`](../../../../Engine/Tests/Fixtures/DynamicDllUnload/DynamicUnloadFixtureModule.cpp)
