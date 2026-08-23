# Modular Features and Module Retirement

Summary: Define typed feature invocation, owner-bound asynchronous drain, and fail-closed native module shutdown.

Modules: Core

Last reviewed: 2026-08-24

## Feature Contract

A modular feature derives from `IModularFeature` and declares a stable
`FeatureName` plus a non-zero `FeatureVersion`. These declared values form the
cross-DLL interface identity; RTTI, interface addresses, and registration order
are not identities.

Module lifecycle hooks are parameterless. Immediately around each manager-
initiated `StartupModule()`, Core installs a stack-disciplined current startup
identity carrying the logical module name and exact load-generation owner.
Modules register implementations only through `FModuleStartup`; callers cannot
select or retain an owner. Registration outside the current startup scope is a
programming error and cannot publish an unattributed entry. Registration
returns a move-only `FModularFeatureRegistration`. Moving, retiring, or
resetting a token affects only its exact entry. Low-level tests use an isolated
`FModuleTestOwner`; concrete module tests use `FModuleTestHarness`. Neither can
retire a production module generation.

Consumers call `FModularFeatureRegistry::InvokeSingle<T>` or `InvokeAll<T>`.
The feature reference exists only during the visitor call and must not be
stored. `InvokeSingle` reports unavailable, invoked, ambiguous, or visitor
failure. `InvokeAll` admits the exact published set as one snapshot and reports
each invocation independently. The registry never selects the first provider
implicitly and exposes no raw lookup or lease API.

## Synchronous Invocation Gate

Each registration moves irreversibly through:

```text
Published -> Retiring -> Retired
```

Lookup and retirement linearize under the Core registry mutex. Admission of a
published entry increments its in-flight count before the mutex is released.
The visitor then runs without a registry or module-map lock. Core decrements
the count on every normal or exceptional return. Retirement first closes
admission, then waits with a bounded policy for admitted visitors.

Retirement from inside the matching invocation closes admission but reports
`SelfWait` instead of blocking. Timeout likewise leaves admission closed. This
gate proves only synchronous visitor return; it is not cancellation and does
not own or drain tasks, continuations, timers, or external work.

## Module Lifecycle

Every module record exposes one of these states:

```text
Registered -> Loading -> Active -> Retiring -> StoppedMapped -> Unloaded
                  |                    |
                  -> LoadFailed        -> UnloadBlocked
```

Before Game Thread identity is installed, load and retirement run on the
thread that created `FModuleManager`. Afterward they run on the Game Thread.
Calls from another thread return `WrongControlThread` without changing state.
Stopped or blocked modules are never returned as active by `LoadModule` or
`GetModule`.

The startup identity is nested and exception-safe. If module A loads module B
from A's startup callback, B temporarily becomes current and Core restores A
after B returns or unwinds. The stack lives in Core so every native module sees
the same identity rather than a DLL-local copy.

Shutdown performs this fixed sequence:

1. transition `Active` to `Retiring`;
2. retire feature admission and wait for synchronous visitors;
3. close every owner-bound asynchronous operation group using its declared
   drain or cancel shutdown policy;
4. run the reflected-object drain callback while the library is mapped;
5. call parameterless `ShutdownModule()` without Core locks so the module can
   release result handles and clean up its owned services and registrations;
6. drain all owned asynchronous operations and audit active tasks, result
   handles, Worker callables, and Game Thread callables;
7. audit that no owned feature entry is published or in flight;
8. transition to `StoppedMapped`;
9. for explicit unload, destroy the module instance and release the library;
10. publish `Unloaded` only after native release.

Wrong-thread, self-owned execution, timeout, reflected-object rejection,
shutdown-callback failure, and final-audit failure return categorized evidence.
After retirement begins, a failure transitions to `UnloadBlocked`; it never
restores `Active`, destroys the module instance, or calls `FreeLibrary`.
Process-exit reverse ordering uses the same shutdown transition but deliberately
leaves libraries mapped for operating-system teardown.

## Locking

The module-map mutex protects map membership and lookup snapshots. The
registry mutex protects feature publication, admission counts, and retirement.
If both are ever needed, module-map order precedes registry order. Current
callback paths release the module-map lock before entering the registry, and
all registry and module-map locks are released before logging or calling
feature, reflected-object, startup, or shutdown code.

## Asynchronous Operation Boundary

Returning from a feature visitor does not prove that work submitted by the
implementation has finished. A module creates `FAsyncOperationGroup` instances
through `FModuleStartup` during its startup callback. Each group owns one task
scope, explicit cancellation source, stable abort reason, close policy, and
diagnostic identity under the module load generation. A root task explicitly
selects the group's scope and cancellation token; accepted descendants and
continuations inherit the scope under the Task System rules.

Closing a group is irreversible. `Drain` rejects later roots and lets accepted
work finish; `Cancel` additionally requests cooperative cancellation and records
the first explicit abort reason. A call to `Drain` from one of the group's own
tasks returns `SelfWait`. A non-Game Thread drain that encounters retained
`GameThreadDeferred` work returns `UnsupportedThread`. Timeouts preserve the
closing state and their evidence; they never reopen admission.

Game Thread drain selects only entries belonging to the closing task scope.
Drain-mode entries execute without pumping unrelated queues. Cancel-mode and
stale entries publish cancellation, detach from the queue, and destroy their
callable storage before success. Worker queue ownership tags similarly remain
outstanding until the erased wrapper and discard callback are destroyed.

Successful group drain requires zero active tasks, zero retained typed-result
states, zero selected deferred callables, and zero Worker wrappers. The module
manager repeats this owner-wide proof after `ShutdownModule` and before the
final feature audit. `AsyncOperationDrainTimeout`, `AsyncOperationSelfWait`,
`AsyncOperationUnsupportedThread`, and `OutstandingAsyncOperationAudit` all
leave the module `UnloadBlocked` and its native library mapped.

## Specialized Registries

Domain registries whose selection depends on provider identity, asset class,
ranking, provenance, route, or build key remain specialized. They do not expose
their providers through generic modular-feature cardinality. An unloadable
module instead creates an `FModuleOwnedCallbackRegistration` during startup and
passes its copyable gate to every foreign registry that retains its callable,
virtual provider, raw observer, factory, or custom deleter.

A retained entry follows all of these rules:

- registration first admits through the owner gate and retains a persistent
  `FModuleOwnedResourceLease` for the full storage lifetime;
- invocation enters `FModuleOwnedCallbackInvocation` and, when provider or
  callable storage is copied beyond the registry lock, retains a temporary
  resource lease until that copy is destroyed;
- metadata enumeration strips executable callbacks instead of returning a
  callable snapshot;
- a plan, session, workspace proxy, customization instance, viewport mode, or
  other escaped object retains a resource lease until its Plugin-owned state is
  destroyed;
- removal uses an identity- and generation-bearing handle so a stale owner
  cannot remove its replacement; and
- `ShutdownModule` destroys registrations and escaped instances before the
  manager performs its final owner audit.

The manager's synchronous retirement closes all gates before module shutdown.
Late registration and invocation therefore fail, already admitted calls finish,
and any forgotten stored entry or escaped instance leaves a non-zero retained
resource count that blocks native release. A `shared_ptr` alone is never DLL
lifetime evidence because its deleter and virtual destructor may be Plugin code.

This contract currently covers asset-import translators, pipelines, and typed
factories; build-host contributions and local build functions; Editor
workspaces and routes; rendered thumbnail providers; customizations; viewport
modes; startup and console commands; asset-reference stores; and asset-move
observers. Each registry keeps its existing domain-specific matching and
ordering behavior.

Interchange component leases cover more than callback duration. Submitted jobs,
detached products, and reusable preview products retain the exact component and
module resource until their values are destroyed. Component unregistration
closes admission and clears matching preview cache entries before the exact
registration generation retires; provider code can therefore neither execute
nor run a product destructor after native release.

## External and Deferred Execution

A callback invocation that launches work must attach the root task to an
owner-created `FAsyncOperationGroup` before returning. The module may keep task
result handles during normal operation, but `ShutdownModule` must cancel or
wait them and destroy those handles before the manager's final async drain.
Local task scopes remain valid for native tests and objects whose code is not
unloadable; they are not a production substitute for module attribution.

Render commands are bounded submissions rather than registry entries, but
their callable storage still contains producer code. Every unloadable producer
must stop admission and call `FlushRenderingCommands` while its DLL and the RHI
backend remain mapped. If an object owns worker-produced GPU uploads, its
shutdown first cancels and waits the worker handles, then flushes accepted
render commands, then destroys publication state.

Process shutdown preserves this ordering mechanically. The general reverse-load
pass shuts down and unloads render-command producers while the rendering and RHI
threads are alive. `VulkanRHI` is explicitly deferred from that pass. After all
producer shutdown callbacks have flushed, Launch stops the rendering thread and
calls `RHIExit`; RHI exit flushes the RHI queue, completes the terminal backend
shutdown marker, stops the RHI thread, deletes `GDynamicRHI`, and only then asks
the module manager to unload `VulkanRHI`. Failure leaves the backend module
mapped.

## Physical DLL Qualification

The unload contract is qualified with a test-only Windows DLL loaded through
the production `FModuleManager::LoadModule` path. Host-side observation uses
process-resident feature interfaces, POD lifecycle events, manager-owned owner
generations, host-assigned fixture instance serials, and `GetModuleHandleW`.
The host never retains a function pointer into the fixture after unload.

Successful qualification proves that an admitted synchronous call completes
while late admission is rejected, a Worker-to-Game-Thread chain and its
destructor-sensitive capture drain before module destruction, and the native
image is absent after `UnloadModule`. Reload produces a higher owner generation
and a distinct fixture instance. Thirty-two additional serialized cycles prove
that earlier instances publish no later events and every successfully retired
image is physically unmapped.

Failure qualification runs irreversible cases in a separate process with one
logical module record per scenario. Invocation timeout, retained owner
resource, active worker, retained typed result, retained deferred callable,
reflected-object rejection, shutdown exception, wrong-thread request, and
recursive owned execution all produce their categorized unload result without
destroying or releasing the affected module image. Successful stress also runs
under Windows Application Verifier with Heaps, Handles, Locks, and TLS checks.

The dedicated native targets are `DynamicDllUnloadQualificationTests` and
`DynamicDllUnloadFailureQualificationTests`; both require explicit
`--mode qualification` admission and are excluded from ordinary aggregates.

## Bounded Exemptions

Owner attribution is unnecessary only when construction proves that no
unloadable code can remain callable or destructible:

- a predicate, visitor, serializer hook, or failure callback is created,
  invoked, and destroyed within one synchronous stack;
- a static C callback is owned by the same library as the external object and
  that object clears the callback during its own destruction;
- the provider code belongs to a process-resident foundation library with no
  dynamic `IModuleInterface` entry;
- a registration or local scope exists only inside a bounded native test; or
- repository search establishes that the service has definitions but no
  production registration, as with the current timer and file-watcher classes.

An exemption must name the storage owner, provider code location, and lifetime
boundary in the owning plan or contract. A load-order assumption, raw pointer,
module-loaded query, or generation invalidation without callable destruction is
not an exemption.
