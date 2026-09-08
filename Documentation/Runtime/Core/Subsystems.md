# Subsystems

Summary: Define shared native subsystem registration, construction, retention, retirement, and Engine ownership.

Modules: Engine, Core, CoreDObject

Last reviewed: 2026-09-08

## Scope And Ownership

`DSubsystem` is the transient native service base. `DEngineSubsystem` belongs to
one `DEngine`; `DWorldSubsystem` belongs to one World. The editor module adds
[Editor subsystems](../../Editor/Architecture/EditorSubsystems.md). Select a scope
by the endpoint lifetime the service needs. Engine services cannot require a
current World during initialization. World services are recreated for each PIE
or Preview World; Engine and Editor services survive those World lifetimes.

Every host owns its collection and enumerates its objects in
`AddReferencedObjects`. Outer identifies the host but does not retain services.
Instances are transient, excluded from asset serialization and child duplication.
There is no global service instance, lazy construction, cross-scope dependency
graph, or registration scan over reflected classes.

## Registration And Lookup

A provider retains an explicit registration token from module startup until
module shutdown. `FEngineSubsystemRegistration` accepts an `FSubsystemDescriptor`
with concrete Type, Provider and same-collection Dependencies. Production
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

`FSubsystemRegistration` stores scope identity and optional opaque adapter
policy. The common kernel does not interpret callback or Tick policy. Collection
membership freezes before user factories execute. Token removal and later
registration affect future collections only. World's descriptor retains its
existing aggregate layout and eligibility fields.

## Common Lifecycle

`FSubsystemCollection` validates scope/concreteness, duplicate types, provider
availability, missing dependencies and dependency cycles before construction.
`FSubsystemResult` reports a categorized `ESubsystemError` and a diagnostic.
Topological ordering selects the lexically first available qualified type name.
There is one dependency implementation shared by all scope adapters.

Initialization is one-shot: Uninitialized becomes Initializing, then Ready or
Failed. Returned errors and exceptions clean the failed object too, close all
gates, reverse-deinitialize constructed objects and mark them as garbage. An
explicit later shutdown may transition Failed to Shutdown; neither state admits
reinitialization. Hosts propagate failures through their initialization result.
Cleanup is `noexcept` and must tolerate partially initialized resources.

The kernel defers GC during initialization and retirement. Registration, lookup,
initialization, callbacks and mutation run on the game thread. Closing work does
not remove lookup visibility while dependent consumers retire. Shutdown requests close the Engine, Editor and owned World gates immediately.
DWorld::RequestShutdown records retirement without destructive cleanup; the
current World operation stops later callbacks and unwinds before cleanup.
Host initialization and update scopes defer host cleanup until the operation
returns. External service callbacks use DispatchSubsystemCallback, which keeps
resources alive through callback return and postpones cleanup to the next host
Tick/shutdown boundary. Constructors that request retirement are cleaned without
entering Initialize. Requests during an externally driven World
operation are completed at a later host shutdown/Tick boundary when the World is
idle. World retains its own complete operation and callback admission rules in
[World subsystems](../World/WorldSubsystems.md).

## Work And Code Lifetime

`GetWorkGate()` returns a shared cancellation identity. Workers capture detached
inputs and this gate, never a subsystem or host object. Game-thread completion
checks IsOpen before resolving endpoints or publishing. Closing a gate does not
join workers, release their captures or stop code already running. Never wait on
the game thread for a completion that needs that same thread.

Frozen entries and objects retain provider module code leases; objects keep the
lease through physical destruction. Detached gates retain provider and Engine
code independently. Module shutdown fails with OutstandingCodeLease while those
leases remain. Deinitialize unregisters external callbacks and releases resources;
module-owned tasks still use the module async-operation protocol.

## Engine Host Sequence

Engine loads its providers and initializes cooked-mesh loading, catalog, default
materials and renderer/scene endpoints before initializing Engine services. Only
then does it construct and initialize the initial World and publish it. Worlds
whose Outer is an Engine register a scoped initialization reference with that
host, so retirement also closes unpublished World admission; the reference is
removed after the World operation unwinds. Init
exceptions and returned failures request the same idempotent cleanup as exit.

Shutdown closes gates, retires editor consumers when present, shuts down the
remaining World while Engine services are available, deinitializes Engine
services, then releases viewport view states. BeginDestroy additionally releases
scene/default-material resources and retains its render fence. Scope-specific
service BeginDestroy requests its known host's explicit shutdown and delegates
readiness; the generic base never interprets arbitrary Outers as hosts to destroy.
Launch calls host preparation before Mona, cooked-mesh and task-system teardown;
see [Runtime lifecycle](RuntimeLifecycle.md).
