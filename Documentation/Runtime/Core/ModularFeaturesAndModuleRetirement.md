# Modular Features and Module Shutdown

Summary: Define typed feature invocation, module-owned cleanup, and explicit native library release.

Modules: Core

Last reviewed: 2026-10-15

## Feature Contract

A modular feature derives from `IModularFeature` and declares a stable
`FeatureName` plus a non-zero `FeatureVersion`. These declared values form the
cross-DLL interface identity; RTTI, interface addresses, and registration order
are not identities.

Modules register through `FModularFeatureRegistry::RegisterFeature<T>` and retain
an exact move-only registration token. `Reset()` immediately unregisters that
entry and is idempotent. Token destruction also unregisters. The registry does
not own the implementation, bind registration to module startup, or drain work.
`FModuleStartup` only reports the current startup module name for other systems;
it is not part of feature registration. Tests register features directly;
`FModuleTestHarness` remains separate test support for concrete module lifecycle.

Consumers use `InvokeSingle<T>`, `InvokeAll<T>`, or
`InvokeRegistered<T>(RegistrationIdentity, Visitor)`. Feature references are valid
only within synchronous visitation and must not escape. Single invocation reports
unavailable, invoked, ambiguous, or visitor failure; it does not choose the first
provider. All invocation selects a stable set for the duration of dispatch.
Exact invocation rejects zero, missing, unregistered, replacement, and
wrong-interface identities. Identity changes on every registration and survives
token moves; it is a process-local route, not a persistent key.

`GetPublicationRevision()` is a global monotonic invalidation counter, incremented
on successful registration and actual unregistration. Invocation, token moves,
failed registration, and repeated Reset do not increment it. Unrelated feature
changes may conservatively invalidate a metadata cache. Toolset
providers use these APIs as described in [toolset contracts](Tools.md).

## Main-Thread and Safe-Point Contract

Registration, unregistration, discovery, invocation, and revision reads require
the module-control thread during startup and GameThread once its identity is
installed. Bootstrap establishes the registry on the control thread. Worker
threads submit detached work to a host-owned queue; they never access the feature
registry. These rules are enforced in all build configurations.

Nested synchronous invocation is allowed. A stack scope tracks dispatch depth
and unwinds on success, absence, ambiguity, and visitor exceptions. Any registry
mutation during dispatch is a contract violation, even for another provider, so
an enumerated set cannot be invalidated by reentrant registration or destruction.
The registry has no mutex, condition variable, in-flight counters, retirement
states, waiting, timeout, or self-wait protocol.

`UnloadModule` called during feature dispatch returns false before entering
Shutdown or changing module state. `ShutdownModule` during dispatch is a contract
violation. Schedule these operations at a later safe point after the entire
feature call stack returns. A scope is not a code lease and never waits for a
worker. Modules supporting unload must independently stop producers, synchronize
workers and retained callbacks, unregister features, and destroy their resources
while code is mapped. Unregistration alone does not make unload safe.

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
independent of this capability. MeshBuilder and TextureCompressor explicitly opt in
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

The module-map mutex protects map membership and lookup snapshots. Feature
registration and dispatch are main-thread-only and have no registry mutex.
Module-map locks are released before logging or calling feature, startup, or
shutdown code. Modules synchronize their own worker-facing services separately.

## Asynchronous Operation Boundary

Returning from a feature visitor does not prove that asynchronous work has
finished. Each module stops its own producers during ShutdownModule, cancels or
waits the tasks it owns, and releases callbacks, results and resources. Core
does not register module task scopes, collect descendants, or pump selected
module queues. There is no shared operation-group replacement. Modules without
asynchronous work require no task-specific shutdown boundary.

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
Request the unload and perform it at a later safe point instead. Feature tokens remove registration immediately and provide no wait operation.

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
shutdown directly after external entry points have been stopped.

## External and Deferred Execution

A callback that launches work must leave it under the producing owner's control.
The owner stops submission and completes its actual cleanup before shutdown
returns. Task/event combinations are optional dependency tools, not a mandatory
module retirement protocol. General task completion does not release user-held
results or callbacks; their owner releases them explicitly.

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

Successful qualification proves a synchronous main-thread call returns before
safe-point unload, a module-owned Worker chain and destructor-sensitive capture
drain before module destruction, and the native image is absent after unload.
Worker observations use explicitly obtained process-resident callbacks, not
worker-thread registry access. Reload produces a higher owner generation and a
distinct fixture instance. Thirty-two additional serialized cycles verify that
earlier instances publish no later events and images are physically unmapped.

Failure qualification runs irreversible cleanup failures in a separate process.
Active workers, retained typed results, and injected cleanup exceptions leave
images mapped in ShuttingDown. Wrong-thread and reentrant feature-call unload
are rejected before shutdown and leave the module Active; safe-point unload can
then succeed. Unit death tests enforce wrong-thread registry access and mutation
during dispatch. Feature callbacks never overlap module unload.

The dedicated native targets are `DynamicDllUnloadQualificationTests` and
`DynamicDllUnloadFailureQualificationTests`; both require explicit
`--mode qualification` admission and are excluded from ordinary aggregates.

## Scope of the Guarantees

Synchronous stack-local visitors need no specialized registration lifetime
machinery. Process-resident code also needs no DLL ownership token, but must
still obey object and service lifetimes. Neither an `IsModuleLoaded` query nor
a generation number proves that escaped executable storage has been destroyed.
Explicit unload correctness is the responsibility of the caller and owners;
the registry enforces main-thread dispatch and safe-point mutation only.
