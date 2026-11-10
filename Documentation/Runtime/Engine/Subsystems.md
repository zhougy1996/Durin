# Subsystems

Summary: Define shared native subsystem registration, construction, retention, retirement, and Engine ownership.

Modules: Engine, Core, CoreDObject

Last reviewed: 2026-11-10

## Scope And Ownership

`DSubsystem` is the transient native service base. `DEngineSubsystem` belongs to
one `DEngine`; `DWorldSubsystem` belongs to one World. The editor module adds
[Editor subsystems](../../Editor/Architecture/Framework/EditorSubsystems.md). Select a scope
by the endpoint lifetime the service needs. Engine services cannot require a
current World during initialization. World services are recreated for each PIE
or Preview World; Engine and Editor services survive those World lifetimes.

`DDynamicSubsystem` extends `DSubsystem` for scopes whose live membership can
change. `DEditorSubsystem` derives from this base; Engine and World subsystem
scopes remain direct `DSubsystem` subclasses with frozen membership.
`FSubsystemCollection` owns one-shot initialization and shutdown primitives.
`FDynamicSubsystemCollection` adds protected reconciliation and requires a scope
derived from `DDynamicSubsystem`; only dynamic scope adapters expose updates at
host-controlled safe points. Scope adapters own explicit synchronization
boundaries; dynamic membership does not enable automatic module discovery.

Every host owns its collection and enumerates its objects in
`AddReferencedObjects`. Outer identifies the host but does not retain services.
Instances are transient, excluded from asset serialization and child duplication.
There is no global service instance, lazy construction, cross-scope dependency
graph, or registration scan over reflected classes.

## Registration And Lookup

A provider retains an explicit registration token from module startup until
module shutdown. `FEngineSubsystemRegistration` accepts an `FSubsystemDescriptor`
with concrete Type and Provider. Production
providers use their actual `FModuleStartup::GetModuleName()`; an empty Provider
is reserved for statically linked fixtures. Add new reflected headers to the
owning module's `ReflectHeaders` manifest.

For a concrete reflected `DMetricsSubsystem : DEngineSubsystem`, module startup
and a host consumer use:

```cpp
// Retained as a module member until ShutdownModule.
Registration = std::make_unique<FEngineSubsystemRegistration>(
    FSubsystemDescriptor{
        .Type = DMetricsSubsystem::StaticClass(),
        .Provider = FModuleStartup::GetModuleName()});

if (auto* Metrics = Engine.GetSubsystem<DMetricsSubsystem>())
    Metrics->RecordFrame();
```

`GetSubsystem<T>()` on Engine accepts only Engine subsystem types. On World it
accepts only World subsystem types. Lookup is exact and non-constructing: it
returns null until that concrete object's Initialize succeeds. A service may
query previously initialized dependencies during its own initialization. It
remains queryable during dependent cleanup until its own Deinitialize returns.

`FSubsystemRegistration` stores scope identity, Type and Provider. Its registration
catalog is enumerated for initial population and explicit additions.
Engine, Editor and World typed registration tokens share this catalog and the
common descriptor; World Tick capability belongs to DTickableWorldSubsystem. Engine and World token
changes affect future collections only. Editor module-load callers and stopping
providers explicitly add or remove services at host safe points. There is no
registration diff, per-frame poll or collection reconciliation.

Concrete subsystem classes require a published class default object (CDO).
`ShouldCreateSubsystem(DObject* Outer) const` is a read-only creation query on that
CDO. It runs before instance construction and dependency resolution; returning
false excludes the proposed instance. A consumer checks a null dependency pointer
when the dependency is required. Existing instances are not reevaluated on
repeated addition. Adding an existing type reuses its instance. Recreating a
service requires explicit removal followed by addition. Registration token churn
does not replace a live instance. Callers own consumer stop and reconnect order.
Constructors establish defaults; host resources belong in Initialize/Deinitialize.

## Common Lifecycle

Call registration, lookup and lifecycle operations on the game thread. Request
same-collection dependencies in `Initialize(FSubsystemCollection& Collection)`
using `Collection.InitializeDependency<T>()`. Lifecycle methods return void;
single-service addition and dependency initialization return pointers. There is
no subsystem result structure or error enum. Implement `Deinitialize` as non-throwing cleanup
that also tolerates failed initialization. External callbacks that use host
resources use `DispatchSubsystemCallback` unless their concrete endpoint already
enforces equivalent host admission with no intervening user callback. The wrapper
only gates new work after shutdown; it does not own lifecycle or GC scopes.

Dependency initialization runs on demand before returning to the consumer's
Initialize callback. Missing or excluded dependencies return null. Invalid scope
and recursive dependency initialization are assertion failures. Required dependencies
are consumer contracts; optional dependencies may be null. Invalid lifecycle
entry, missing CDOs and inactive providers are also contract violations, not
recoverable initialization failures. Business failures belong to concrete services
and their operations. Both
typed and runtime-type lookup return only initialized instances. Dependencies
remain queryable until their own Deinitialize returns; lookup is independent of
permission to execute scripts, tools or new work.

All external additions enter through the common batch operation, including
single-type requests. The internal single-object creator requires an existing
initialization context and is not an alternate public/protected entry point.
An unregistered single-type request still validates the collection boundary and
returns null, even if an older instance remains live after token removal.

There is no descriptor dependency graph or whole-update validation. Additions
run sequentially; a failed addition cleans its own object and preserves already
initialized services, including dependencies created earlier in that call. A
later explicit addition can retry it. Completion order determines reverse cleanup
order. Removal does not inspect or automatically remove consumers; callers stop
and remove them before removing the services they use.

Initialization is one-shot. Collections expose `IsInitialized()` and
`IsPopulating()`, without a public lifecycle enum. `IsInitialized()` is set when
initialization begins; it is not permission to dispatch work while population
is active. Hosts expose the corresponding `AreSubsystemsInitialized()` or
`AreEditorSubsystemsInitialized()` query and own dispatch admission.
Unexpected user callback exceptions clean the failed object and propagate unchanged;
initial collection setup also cleans previously constructed services and clears
`IsInitialized()`. There is no generic initialization failure
protocol. Hosts do not translate subsystem exceptions into business results.
Unpublished World, PIE package and preview resources still receive exception cleanup.
A shutdown requested by a callback closes admission without destroying resources.
The owning application calls `DEngine::PreExit()` after the complete call chain
returns; initialization helpers, Tick and external callback wrappers do not perform
implicit Engine cleanup. The host checks retirement before publishing its World.
Deinitialization
reverse-deinitializes constructed services and releases collection
references. It does not mark objects as garbage: external strong references can
retain stopped objects until ordinary GC. Their later destruction does not shut
down the host again. Completed collection teardown does not admit reinitialization.

Collections do not broadcast object-reference invalidation when services stop.
Concrete consumers own cancellation, endpoint retirement and stale-reference
checks; actual object invalidation remains with the object/GC lifecycle.
The framework protects lifecycle callbacks from GC. Collection cleanup requires
an idle collection and has no pending or deferred shutdown request. A callback
uses `DEngine::RequestShutdown()` or `DWorld::BeginTearingDown()` to close admission.
Only the owner executes `PreExit()` or `CleanupWorld()` at an idle boundary.
Once population starts, the batch runs to completion unless a callback throws.
There is no initialization cancellation API or cancellation state. A shutdown
request closes host admission immediately but does not interrupt the current
callback or skip later initialization callbacks in that batch. The host waits
for population to finish before clearing the collection and checks retirement
before publishing Worlds or creating consumers.
The host issues its shutdown notification once, including recursive requests.
Engine `RequestShutdown()` is a host-local admission request, not a process exit;
Launch uses the existing `RequestEngineExit()` for process exit. `PreExit()` follows
the UE owner-driven pre-purge shutdown role. World `BeginTearingDown()` closes
admission and `CleanupWorld()` performs synchronous cleanup at an idle boundary.
The complete World operation boundary may service pending World cleanup requests.

Host dispatch and public service entries enforce admission. Private synchronous
execution relies on the caller's lifetime contract. Callbacks must not invoke
Engine cleanup, mutate collection membership, unload providers, or run immediate
GC. They use `RequestGarbageCollection()`; Launch executes collection at its GC
safe point after callbacks return. Independent hosts and tests own the same
ordering explicitly. There is no host-operation counter or GC scope around every
Engine call, and finish-destroy readiness does not represent callback activity.
Collection population and World operations retain their own local invariants.
World lifecycle notifications complete their current batch before the host
applies retirement or transitions. An admitted generic Tickable batch similarly
completes before owner cleanup; explicit Tickable unregister/disable still takes
effect immediately. Actor and Timer callback admission remains independently
World-owned. The World owns play and attachment pairing, not individual collection entries.
See [Tickable objects](TickableObjects.md) for phase admission and the stricter
TickableWorldSubsystem initialization/deinitialization pairing contract.

Dependency lookup remains available during cleanup. Asynchronous completions and
retained endpoints still validate cancellation and lifetime. Host shutdown owns
World, play and resource teardown; see
[World subsystems](../World/WorldSubsystems.md).

## Work And Code Lifetime

The subsystem base and collection do not own task cancellation. Each consumer
owns its task handles, cancellation sources and publication checks. Workers
capture detached inputs and consumer-owned cancellation state. Game-thread
completion checks cancellation and the intended live service before publishing.
Durin task and tool cancellation are cooperative; cancellation does not join
workers, release captures or stop code already running. A task cancellation token
copy is a snapshot; detached work that must observe later cancellation shares the
same token object. Tool cancellation sources already supply shared token state.
Never wait on the game thread for a completion that needs that same thread.

Deinitialize cancels/drains consumer-owned work, unregisters external callbacks
and releases resources. Module-owned tasks still use the module async-operation
protocol and must finish before module shutdown returns.
Providers remain process-resident unless they implement the complete
object, reflected-type and callable cleanup needed for dynamic DLL release.
There is no code-lease accounting in subsystem objects or ModuleManager.
Editor providers stop their consumers, remove live instances through their host,
then release registration tokens inside their own ShutdownModule; Engine
and World providers still require their frozen host collections to be shut down
by their owners.

## Engine Host Sequence

Minimum host initialization establishes cooked-mesh and renderer endpoints.
Project admission then loads its providers, requires a successful catalog,
initializes default materials and the complete Engine subsystem set, and only
then constructs and publishes the initial World with its frozen provider set.
The Editor performs this admission after shell presentation; Project Browser has
no initial project World or project-scoped Engine services. Worlds
whose Outer is an Engine register a scoped initialization reference with that
host, so retirement also closes unpublished World admission; the reference is
removed after the World operation unwinds. Returned host initialization failures
request ordinary shutdown. Unexpected subsystem callback exceptions clean partial
collection and unpublished World state before propagating to the outer execution boundary.

Shutdown records retirement, stops concrete editor consumers when present, shuts down the
remaining World while Engine services are available, deinitializes Engine
services, then releases viewport view states. BeginDestroy additionally releases
scene/default-material resources and retains its render fence. Each host's own
BeginDestroy supplies fallback collection cleanup before service objects are
physically destroyed. Subsystem destruction never initiates host shutdown or
waits for host readiness. Uninitialized objects outside a collection and retired
objects retained by external references follow ordinary object destruction.
Launch calls host preparation before Mona, cooked-mesh and task-system teardown;
see [Runtime lifecycle](../Application/RuntimeLifecycle.md).
