# Modular Features and Module Retirement

Summary: Define typed modular-feature invocation, synchronous retirement, and fail-closed native module shutdown.

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
2. retire the module owner and wait for synchronous visitors;
3. run the reflected-object drain callback while the library is mapped;
4. call `ShutdownModule(FModuleShutdownContext&)` without Core locks;
5. audit that no owned entry is published or in flight;
6. transition to `StoppedMapped`;
7. for explicit unload, destroy the module instance and release the library;
8. publish `Unloaded` only after native release.

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

## Deferred Asynchronous Boundary

Returning from a feature visitor does not prove that work submitted by the
implementation has finished. Explicit operation ownership, abort reasons,
Game Thread continuation drain, and callable-destruction proof belong to the
asynchronous operation-drain contract. Until that contract is implemented, a
module remains responsible for draining such work inside its shutdown callback
before the synchronous final audit can authorize native unload.
