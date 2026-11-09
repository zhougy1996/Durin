# Subsystems

Summary: Define shared native subsystem registration, construction, retention, retirement, and Engine ownership.

Modules: Engine, Core, CoreDObject

Last reviewed: 2026-11-09

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

`FSubsystemRegistration` stores scope identity and the common descriptor only;
its snapshots contain Type, Provider, Dependencies and RegistrationIdentity.
World registration owns a separate strongly typed catalog for its Tick policy.
The common kernel receives only common descriptors. Engine and World token
changes affect future collections only. Editor module-load callers and stopping
providers explicitly synchronize at host safe points; there is no per-frame
registration poll or collection reconciliation. See the Editor subsystem contract.

Concrete subsystem classes require a published class default object (CDO).
`ShouldCreateSubsystem(DObject* Outer) const` is a read-only creation query on that
CDO. It runs before instance construction and dependency resolution; returning
false excludes the proposed instance. A selected service requiring an excluded
dependency reports MissingDependency. Existing instances are not reevaluated on
explicit synchronization. Changing a live type's registration identity is
rejected before instance or eligibility callbacks. Recreating a service requires
an earlier synchronization that completed its removal; changing consumer tokens
does not authorize replacement. Callers own consumer stop and reconnect order.
Constructors establish defaults; host resources belong in Initialize/Deinitialize.

## Common Lifecycle

Call registration, lookup and lifecycle operations on the game thread. Declare
same-collection dependencies, and implement `Deinitialize` as non-throwing cleanup
that also tolerates failed initialization. External callbacks that use host
resources enter through `DispatchSubsystemCallback`.

The collection validates descriptors and initializes dependencies before their
users, choosing the first available qualified type name in lexical order. Both
typed and runtime-type lookup return only initialized instances. Dependencies
remain queryable until their own Deinitialize returns; lookup is independent of
permission to execute scripts, tools or new work.

Descriptor ordering rejects the complete set on duplicate types, missing or
wrong-scope dependencies, or cycles; it never returns a valid subset for partial
application. Dynamic synchronization completes descriptor validation before
closing instance gates or changing membership. Invalid updates preserve existing
instances; initialization failures after a valid update still clean the failed
object and roll back newly constructed services.

Initialization is one-shot: Uninitialized becomes Initializing, then Ready or
Failed. Errors and exceptions also clean the failed object. Deinitialization
closes work, reverse-deinitializes constructed services and releases collection
references. It does not mark objects as garbage: external strong references can
retain stopped objects until ordinary GC. Their later destruction does not shut
down the host again. An explicit deinitialization may transition Failed to
Shutdown; neither terminal state admits reinitialization.

The framework protects lifecycle and close-notification callbacks from GC and
reentrant collection cleanup. Shutdown requests close work immediately; cleanup
waits for active callbacks to unwind. Closing work preserves dependency lookup
while rejecting new work. Host shutdown owns the surrounding World, play and
resource teardown; see [World subsystems](../World/WorldSubsystems.md).
External callbacks dispatched through `DispatchSubsystemCallback` defer GC until
they unwind. Host operation state postpones cleanup and host destruction;
subsystem finish-destroy readiness does not delegate to the host.

## Work And Code Lifetime

`GetWorkGate()` returns a shared cancellation identity. Workers capture detached
inputs and this gate, never a subsystem or host object. Game-thread completion
checks IsOpen before resolving endpoints or publishing. Closing a gate does not
join workers, release their captures or stop code already running. Never wait on
the game thread for a completion that needs that same thread.

Work gates only cancel work; they do not pin provider or Engine code. Deinitialize
unregisters external callbacks and releases resources; module-owned tasks still
use the module async-operation protocol and must finish before module shutdown
returns. Providers remain process-resident unless they implement the complete
object, reflected-type and callable cleanup needed for dynamic DLL release.
There is no code-lease accounting in subsystem objects or ModuleManager.
Editor providers remove
tokens and synchronize live collections inside their own ShutdownModule; Engine
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
removed after the World operation unwinds. Init
exceptions and returned failures request the same idempotent cleanup as exit.

Shutdown closes gates, retires editor consumers when present, shuts down the
remaining World while Engine services are available, deinitializes Engine
services, then releases viewport view states. BeginDestroy additionally releases
scene/default-material resources and retains its render fence. Each host's own
BeginDestroy supplies fallback collection cleanup before service objects are
physically destroyed. Subsystem destruction never initiates host shutdown or
waits for host readiness. Uninitialized objects outside a collection and retired
objects retained by external references follow ordinary object destruction.
Launch calls host preparation before Mona, cooked-mesh and task-system teardown;
see [Runtime lifecycle](../Application/RuntimeLifecycle.md).
