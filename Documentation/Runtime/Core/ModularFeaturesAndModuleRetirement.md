# Modular Features and Module Retirement

Summary: Define typed feature invocation, owner-bound asynchronous drain, and fail-closed native module shutdown.

Modules: Core

Last reviewed: 2026-08-15

## Feature Contract

A modular feature derives from `IModularFeature` and declares a stable
`FeatureName` plus a non-zero `FeatureVersion`. These declared values form the
cross-DLL interface identity; RTTI, interface addresses, and registration order
are not identities.

Modules register implementations only through the `FModuleContext` passed to
`StartupModule`. The context carries the Core-created owner identity for that
logical module's current load generation. Registration returns a move-only
`FModularFeatureRegistration`. Moving, retiring, or resetting a token affects
only its exact entry. Test code uses `FModuleTestContextFactory`; its isolated
owners cannot retire a production module generation.

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

Shutdown performs this fixed sequence:

1. transition `Active` to `Retiring`;
2. retire feature admission and wait for synchronous visitors;
3. close every owner-bound asynchronous operation group using its declared
   drain or cancel shutdown policy;
4. run the reflected-object drain callback while the library is mapped;
5. call `ShutdownModule(FModuleShutdownContext&)` without Core locks so the
   module can release result handles and inspect or drain its closing groups;
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
through its startup `FModuleContext`. Each group owns one task scope, explicit
cancellation source, stable abort reason, close policy, and diagnostic identity
under the module load generation. A root task explicitly selects the group's
scope and cancellation token; accepted descendants and continuations inherit
the scope under the Task System rules.

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
