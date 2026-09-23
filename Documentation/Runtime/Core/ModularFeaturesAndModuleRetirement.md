# Modular Features and Module Shutdown

Summary: Define typed feature invocation, module-owned cleanup, and explicit native library release.

Modules: Core

Last reviewed: 2026-09-23

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
`FModuleTestOwner`; concrete module tests use `FModuleTestHarness`. The harness calls the module cleanup hook and checks feature registration counts;
it does not close registrations or drain tasks on behalf of the module.

Consumers call `FModularFeatureRegistry::InvokeSingle<T>` or `InvokeAll<T>`.
The feature reference exists only during the visitor call and must not be
stored. `InvokeSingle` reports unavailable, invoked, ambiguous, or visitor
failure. `InvokeAll` admits the exact published set as one snapshot and reports
each invocation independently. The registry never selects the first provider
implicitly and exposes no raw lookup or lease API.

Invocation results also report the exact process-local `RegistrationIdentity`.
`InvokeSingle` accepts an optional expected identity and rejects a replacement
before entering its visitor. The identity survives token moves, changes on every
registration (including the same implementation and owner), and is never a
persistent recipe or cache key. `InvokeAll` reports it per admitted entry.

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

The manager schedules lifecycle callbacks and owns native-library handles.
Modules own their services, feature registrations, tasks, and external callbacks.
There is no manager-wide retirement timeout, cancellation policy, resource drain,
or final owner audit.

```text
Registered -> Loading -> Active -> ShuttingDown -> StoppedMapped -> Unloaded
                  |
                  -> LoadFailed
```

Load and shutdown execute on the module-control thread (the Game Thread once its
identity is installed). The startup identity is nested and exception-safe: a
nested module load temporarily installs its own identity and then restores the
outer scope. Identity supplies attribution, not resource ownership.

- `IModuleInterface::ShutdownModule() -> void` closes entry points, waits for work,
  and releases module-owned registrations and resources before returning.
- `FModuleManager::ShutdownModule(Name) -> void` invokes that hook once, then marks
  the module `StoppedMapped`. It retains the module instance and DLL for staged
  process exit. An absent or already stopped module is a no-op. Shutdown requires
  no live code leases and does not consult `SupportsDynamicReloading()`.
- `UnloadModule(Name) -> bool` is explicit runtime DLL release. It rejects wrong
  threads, missing instances, invalid states, live code leases, and modules that
  have not opted into dynamic reloading. Otherwise it shuts down the module,
  destroys the instance while code is mapped, and releases the library. An
  already stopped instance is not shut down twice.
- `ShutdownModulesAtExit(DeferredModules) -> void` calls active modules in reverse
  order of **Startup completion**, skipping explicitly deferred and already stopped
  modules. It leaves libraries mapped for operating-system reclamation.

`SupportsDynamicReloading()` defaults to `false`. Shutdown during process exit is
independent of this capability. MeshBuilder and TextureBuild explicitly opt in
because their external build sessions retain code leases. ShaderBuild opts in
to support switching from the drained compiler provider to cooked shader data.
VulkanRHI opts in because RHI teardown
destroys its backend and joins its threads before releasing the DLL. A reflected
module without a complete native-type/object teardown protocol must not opt in.

Shutdown failure is a lifecycle contract error, not a recoverable retirement
result. Module assertions remain fatal, and thrown cleanup errors propagate out
of the manager with the instance and library still mapped in `ShuttingDown`.
The manager does not retry or resume the failed cleanup, nor release the DLL.
Callers must not catch an error and continue process teardown as if it succeeded.
Failed Startup invokes the module's Shutdown to clean partially initialized state;
Only successful cleanup of a module that supports dynamic reloading permits
destroying the instance and releasing the library. Other failed startups remain
`StoppedMapped`, since process-resident code may already have registered types.
Shutdown implementations must therefore tolerate partial startup.

Stopped and shutting-down instances are never returned by `GetModule` or
`LoadModule`. Only a physically unloaded instance can start a new load generation.

## Locking

The module-map mutex protects map membership and lookup snapshots. The
registry mutex protects feature publication, admission counts, and retirement.
If both are ever needed, module-map order precedes registry order. Current
callback paths release the module-map lock before entering the registry, and
all registry and module-map locks are released before logging or calling
feature, startup, or shutdown code.

## Asynchronous Operation Boundary

Returning from a feature visitor does not prove that work submitted by the
implementation has finished. A module creates `FAsyncOperationGroup` instances
through `FModuleStartup` during its startup callback. Each group owns one task
scope, explicit cancellation source, stable abort reason, and diagnostic identity.
The module chooses when to call `Close(Drain)` or `Close(Cancel)` and then `Drain`.
Its Shutdown must release retained result handles before waiting for quiescence. A root task explicitly
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
states, zero selected deferred callables, and zero Worker wrappers. `Drain`
returns the small `EAsyncOperationDrainStatus` enum; `GetSnapshot()` separately
exposes diagnostics. Registration `Reset` likewise returns
`EModularFeatureRetirementStatus`. There are no aggregate owner drain Result
objects. A module must check cleanup outcomes before returning from Shutdown.

## Specialized Registries and Explicit Unload

Domain registries retain their own class, identity, ranking, route, and generation
rules. Registration takes the provider or callback and returns an exact removal
handle; no module callback gate or module resource lease is propagated through
business interfaces. The module manager does not discover or drain stored callbacks, virtual objects,
custom deleters, copied function wrappers, feature entries, or tasks.

Explicit module shutdown and physical unload require a caller-established safe
point on the module-control thread (Game Thread after its identity is installed).
The caller stops dependent consumers and external dispatch before shutdown;
specialized callbacks must have returned and cannot race the unload. A callback
must not unload its own module, including through a reentrant UI or event path.
Request the unload and perform it at a later safe point instead. Registration tokens and operation groups provide local wait operations that
the module explicitly invokes as part of its cleanup.

Each owner is responsible for these boundaries, in dependency order:

- stop request producers, UI/event dispatch, timers, and external entry points;
- unregister exact handles to remove future lookup;
- cancel or finish accepted work and wait for callbacks and queued continuations;
- detach consumers and destroy retained plans, sessions, previews, providers,
  callable copies, and custom deleters while their code is mapped;
- release services only after their consumers have finished using them.

Registration removal does not invalidate an already copied callable or shared
pointer. A registry mutex serializes registry access, not execution after a
callback has been copied outside the lock. Consumers must release those copies
before DLL release. Metadata-only enumeration remains preferable when callers
do not need executable state. Specialized generation checks and thumbnail
session invalidation continue to govern their local resources.

`ShutdownModule()` completes the module-owned portion of this cleanup before
returning. Cleanup failure must throw so the manager leaves the library mapped;
logging an error and returning success is insufficient. Failed startup must
roll back any external registrations it published before propagating failure.
Modules whose consumers cannot establish this boundary must remain mapped.
Normal process exit stops modules in reverse load order and leaves libraries
mapped for operating-system reclamation.

Workspace registration stores the original workspace directly. The host closes
documents, releases integrations, and destroys external workspace references
before unloading the concrete editor module. Asset compiler handle reset removes
routes, stops admission, finishes accepted compilation, and calls provider
shutdown directly, including after typed-feature retirement has started.

## External and Deferred Execution

A callback invocation that launches work must attach the root task to an
owner-created `FAsyncOperationGroup` before returning. The module may keep task
result handles during normal operation, but `ShutdownModule` must cancel or
wait them and destroy those handles before completing its own group drain.
Local task scopes remain valid for native tests and objects whose code is not
unloadable; they are not a production substitute for module attribution.

Render commands are bounded submissions rather than registry entries, but
their callable storage still contains producer code. Every unloadable producer
must stop admission and call `FlushRenderingCommands` while its DLL and the RHI
backend remain mapped. If an object owns worker-produced GPU uploads, its
shutdown first cancels and waits the worker handles, then flushes accepted
render commands, then destroys publication state.

Process shutdown preserves this ordering mechanically. The general reverse-load
pass shuts down render-command producers while the task executors, rendering
thread, and RHI thread are alive. Launch stops the task system after this pass. `VulkanRHI` is explicitly deferred from that pass. After all
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

Successful qualification also registers a fixture-owned console callback and
checks that explicit shutdown removes it and destroys its capture before the
module instance is destroyed and its DLL unmapped.

Successful qualification proves that an admitted synchronous call completes
while late admission is rejected, a Worker-to-Game-Thread chain and its
destructor-sensitive capture drain before module destruction, and the native
image is absent after `UnloadModule`. Reload produces a higher owner generation
and a distinct fixture instance. Thirty-two additional serialized cycles prove
that earlier instances publish no later events and every successfully retired
image is physically unmapped.

Failure qualification runs irreversible cases in a separate process with one
logical module record per scenario. The fixture owns feature and asynchronous cleanup. Invocation timeout, active
worker, retained typed result, retained deferred callable, and recursive feature
cleanup throw from its Shutdown. These exceptions propagate without releasing
the affected image. Wrong-thread unload is rejected before entering Shutdown.

The dedicated native targets are `DynamicDllUnloadQualificationTests` and
`DynamicDllUnloadFailureQualificationTests`; both require explicit
`--mode qualification` admission and are excluded from ordinary aggregates.

## Scope of the Guarantees

Synchronous stack-local visitors need no specialized registration lifetime
machinery. Process-resident code also needs no DLL ownership token, but must
still obey object and service lifetimes. Neither an `IsModuleLoaded` query nor
a generation number proves that escaped executable storage has been destroyed.
Explicit unload correctness is the responsibility of the caller and owners;
Core registration/group waits prove only their declared local boundaries.
