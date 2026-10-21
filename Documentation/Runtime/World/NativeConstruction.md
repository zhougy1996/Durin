# Native Actor Construction

Summary: Define deterministic native reconstruction and transient generated-component ownership.

Modules: CoreDObject, Engine

Last reviewed: 2026-10-21

## Component Ownership

Every actor component has one `EComponentCreationMethod`:

- `Native` is created by the actor constructor and persists as authored
  actor state;
- `Instance` is added through `AddInstanceComponent`, persists, participates in
  transactions, and owns its authoring dirty state;
- `Generated` is derived by native construction, transient, retained in the
  runtime ownership collection, and never supplies package or duplication
  authority.

`GetComponents()` returns a non-allocating const view of
`OwnedComponents`, the ordered authority for every live authored and
generated component.
`GetComponents<T>(OutComponents)` and `FindComponentByClass<T>()` preserve the
requested pointer type; `FindComponentByExactClass<T>()` is the Durin-specific
exact-class query. Mutating component ownership invalidates iterators and
references into the live view. Code that can invoke component callbacks uses
`GetComponentsSnapshot()` and revalidates each handle before publication.

Native components persist through the object's native subobject graph; the
persistent `InstanceComponents` array records per-instance membership. There is
no separately maintained authored aggregate. `ResetOwnedComponents` rebuilds
runtime ownership from the Outer graph, admitting instance-created components
only while present in `InstanceComponents`. Existing live iteration order is
preserved where possible; relative instance iteration order follows the persistent
instance list, independently of graph allocation order. Removed instances retained for Undo remain outside live
ownership. The generated-component key index serves reconstruction matching only.
Package capture and duplication discover native inner objects through the shared
object graph, while transient generated output is regenerated at its destination.

Following UE's initialization mechanism, `DActorComponent::PostInitProperties`
adds native component ownership after the full constructor, before graph loading
or component `PostLoad` callbacks. A Spline load callback can therefore request
Actor reconstruction while all copied native components already have ownership;
it need not wait for Actor `PostLoad`. Explicit instance-component adoption stays
with the Actor APIs. Editor presence commands remove initial native ownership
from prepared additions before their recorded membership transition.
`OnComponentCreated`, registration and `InitializeComponent` callbacks do not run
merely because ownership is added.
The shared hook and failure lifetime are defined by
[object initialization](../Core/GarbageCollection.md#object-initialization).

Generated objects use `EObjectConstructionPurpose::Generated`, carry the
`Transient` object flag, and are retained explicitly through
the reflected transient `OwnedComponents` collection. Editor hierarchy diagnostics may display them,
but label them read-only; reflected editing, rename, duplicate, delete, reorder,
reparent, and drag/drop operations do not accept them. Domain-specific details
actions redirect authoring to the owning Actor or authored input component.

## Reconstruction

`RerunConstructionScripts()` runs synchronously on the game thread. Spawn,
PostLoad, duplication, authored input notifications and editor completion use
this same entry point. It captures and validates component instance/root data,
calls `DestroyConstructedComponents()`, invokes `ExecuteConstruction()`, then
restores root and external attachment relationships. Native and instance-authored
components survive; generated components are destroyed and recreated on every
pass. Consumers resolve generated components again after a request, using a
stable source key rather than retaining a pointer to the previous generation.

`ExecuteConstruction(const FTransform&, const FComponentInstanceDataCache*)`
invokes the virtual `OnConstruction(const FTransform&)` notification and applies
post-construction instance data. Durin currently has no Blueprint/SCS execution.
The native hook is the supported producer of generated components; future
script/template producers belong in the same execution pipeline. It does not
rerun the Actor's C++ constructor.

`AddComponentByClass` publishes generated ownership immediately. Its first four
arguments follow UE's class/manual-attachment/relative-transform/deferred-finish
shape; optional source key and requested name are Durin additions. Without an
explicit key, exact class and per-class creation order provide source
identity, analogous to UE UCS serialized-index matching. Domain producers can
supply stable keys across reorderings until template/archetype matching exists.
`FinishAddComponent` performs
automatic root attachment and ordinary creation, registration and play completion.
Deferred finish lets a producer configure mesh/material/physics inputs first.
Creation callbacks can observe both Actor ownership and the generated index.
There is no unpublished candidate set, component reuse, set commit or rollback
to the previous generation. Duplicate/invalid keys and unconstructible classes
fail explicitly. A failure after teardown cleans newly generated output and
reports the error; a failed capture/owner preflight leaves current output intact.

A request raised inside a reconstruction pass coalesces into at most one following
pass. Requests during Actor BeginPlay/EndPlay wait for that lifecycle boundary
to settle before generated components can be destroyed.
Destruction rejects new work. GC remains deferred across the whole operation.
`RerunConstructionScripts` returns success and accepts optional captured cache/root
inputs; these are explicit Durin extensions to UE's no-argument void entry point.

The construction scope suppresses package dirty and edit-revision mutation.
The authored setter or transaction that requested reconstruction remains the
only authoring authority. This suppression is thread-local, nestable, and
limited to the synchronous derived-state pass.

## Component Instance Preservation

`Engine/ComponentInstanceDataCache.h` supplies `FInstanceCacheDataBase`,
`FActorComponentInstanceSourceInfo`, `FActorComponentInstanceData` and
`FComponentInstanceDataCache`. Component `GetComponentInstanceData` hooks supply
supplemental instance overrides; data `ApplyToComponent` hooks restore those
values through component APIs. The cache does not serialize authored component
properties a second time. The initial built-in data is Generated scene relative
pose. Equal class-default poses are omitted using the existing default graph;
classes explicitly lacking a CDO retain native pose data without an inferred
archetype. Fresh components start from their constructor defaults; omitted cache
entries do not overwrite that initialization. Creation and registration occur
before post-construction instance restoration, rather than exposing a cached pose
as a new component's creation-time default. Domains may extend the hook for
their own
reconstruction-owned data.

`FActorComponentInstanceSourceInfo` replaces transaction-only endpoint matching.
Authored components use exact generation and owner identity; Generated components
use owner, existing construction key and exact class. An absent Generated key is
admitted for reconstruction, while a same-key different class in retained cache
data is rejected before
creation callbacks. No retired Generated object is retained for identity reuse.
Cache owners enumerate data and source references; unmanaged standalone caches
must be exposed through their managed owner's collector when retained across GC.

`FindAndReplaceInstances` follows UE's instance-cache replacement responsibility
on source information, lifecycle data, typed entries, root recovery data and the
cache. It uses the existing checked `FObjectReplacementMap`. Source matching must
agree with that map: exact class, owner and construction key cannot be replaced
by a same-name authored component or an assignable but different class. An
ambiguous generated owner or incomplete serialized source is invalid. An expired
exact component reference never acquires a new generation by Name.

Cache replacement prepares independent typed entries through the shared
`CreateReplacement` implementation and publishes only after every reference check
succeeds. `GetDataType` supplies a native descriptor registered with a stable name,
construction and exact-type copy factories. Their coallocated native storage
reuses existing `FDStructOps` construction, copy and destruction operations;
no parallel struct lifetime table is maintained. This adapts UE's typed struct-storage
facility; domain structs register through `RegisterType<T>` and override their
normal Serialize, reference enumeration and reference replacement hooks as needed.
The registered copy constructor must make mutable supplemental state independent;
sharing immutable values is permitted. Registration must occur before archive loading (normally during module/owner
initialization). Built-in descriptors are always available. A derived type that
inherits an incompatible descriptor is rejected rather than sliced; duplicate
type names and unknown archived types are rejected. No central list of concrete
component data types or per-derived replacement clone is required. Native owners
must retain the code module while data or its descriptor is in use.

Every ordinary request captures the cache and shared
`FActorRootComponentReconstructionData` before destroying generated components.
A caller may supply an earlier capture to recreate a removed generated set;
Undo/Redo supplies the same data from `FActorTransactionAnnotationData`. Supplied
captures remain fixed for the synchronous request and its coalesced pass.

`ECacheApplyPhase` uses UE's `PostSimpleConstructionScript`,
`PostUserConstructionScript` and `NonConstructionScript` names. The first phase
is reserved for future template/SCS execution and is not emitted today. Since
Durin currently generates components in native `OnConstruction`, the second
phase follows that hook. This differs from UE's Blueprint path, whose SCS/UCS
cache phases precede its final native OnConstruction notification. The third
phase is available to domain supplemental data outside script construction.
Lifecycle history belongs to transaction annotations, not the reconstruction cache. A scene pose explicitly
assigned during the native hook takes precedence over cached data, including
same-value writes. Construction-owned fields are also omitted during cache capture.
Tracking belongs to the component, with Actor's execution flag delimiting construction; automatic attachment and cache application do not
acquire construction authority. Root and external attachment recovery follows
the instance cache. No TLS construction-context stack remains.

The cache's explicit archive path uses registered type names and each type's
serializer; an unknown domain type reports an error rather than being omitted.
It is in-process supplemental data, not a new package or generic undo format.

## Lifecycle And Reregistration

`UnregisterComponent` removes Tick admission and render/physics registration. It
retains creation, initialization and Play state, matching UE's unregistration
boundary. Repeated unregister/register calls are idempotent. Terminal destruction,
nonterminal membership removal and historical completion explicitly request the
needed EndPlay, uninitialization and creation/destruction notifications through
the component owner. Unregistration alone does not clear play-bound timers;
EndPlay and terminal teardown still clear them.

`DActorComponent::CaptureLifecycleState`, `ValidateLifecycleState` and
`ApplyLifecycleState` own the supported native checkpoints. Their data is a
Durin adaptation because these native flags are not reflected properties. Actor and component
transaction annotation data retain identities and these checkpoints; they do not execute
a separate transition sequence. Restoring a checkpoint checks ownership and
retirement after callbacks and reports unsettled results rather than resurrecting
an object. Registered-but-uninitialized and unregistered-but-initialized/playing
states are distinct from terminal destruction.

`FComponentReregisterContext` temporarily unregisters a component and restores
registration when its scope ends. Ordinary World render-scene replacement and
`ReregisterComponent` use it. It preserves initialization, Play, Tick settings and
bound timers, while preserving explicit Tick configuration edits inside the scope.
It does not rerun creation/initialization/BeginPlay just to rebuild
resources. A nested context on an already-unregistered component is a no-op.
Retired components still release old registration/resources but never re-register.
`ReRegister` explicitly reports callback failures; automatic scope completion logs
a failure without throwing from the destructor. GC is deferred across the scope.

`FTickFunction` owns native scheduling capture, validation, restoration and explicit
archive transfer through `FTickFunctionState`. This plain checkpoint is shared by
ordinary reregistration and historical recovery; no transaction Tick executor
exists in the component cache or annotation.

`FScopedSuspendRerunConstructionScripts` coalesces requests until its outer scope
ends. Durin scopes one Actor and supports native runtime callers, whereas UE's
editor helper suspends globally. This adaptation lets each Actor's historical
completion consume its own cache before other Actor completions finish. World
scene replacement uses the same scope around resource changes and resumes after
the World operation gate settles. Exception unwinding discards the pending rerun
rather than constructing from partial inputs. Historical completion holds it through full-object notifications and consumes those requests
without scheduling a second reconstruction. The remaining guards describe active
construction/callback work; no transaction-only replay/completion booleans remain.

## Transaction Reconstruction

Engine-owned `FActorTransactionAnnotation` and
`FComponentTransactionAnnotation` supplement authored object serialization with
native lifecycle and Tick state. Actor-wide component checkpoints are retained
by the Actor annotation; independently recorded component checkpoints stay with
the component annotation. Both delegate transitions exclusively to the component.
Reconstruction caches have no lifecycle capture mode or lifecycle application
branch. They retain exact authored components and use
the owning Actor, construction key and exact class for generated components.
An annotation never retains an obsolete generated instance solely to replay it.
Participant validation rejects expired exact dependencies before historical
fields load; generated keys may be absent until reconstruction publishes them.

Replay completes components before their owning Actor. Actor PreEditUndo holds
the shared construction suspension until component state settles. Scene completion repairs authored attachment reverse links and
world transforms before restoring registration, so callbacks see that graph
consistently. Actor completion reconciles authored ownership, performs one native
reconstruction request,
restores supplemental component state through lifecycle APIs, and publishes the
full-object completion. Already settled registration/creation/initialization
states do not run their callbacks again. A component whose entire supplemental
state is default needs no allocated annotation; its serialized record's empty
annotation denotes that default state. Custom-only records keep ordinary
completion and continue to own their lifecycle changes.

`FActorTransactionAnnotation::CreateIfRequired` and `HasInstanceData` omit an
Actor annotation when its instance/root recovery data and supplemental Tick
state are default. Default Tick state is disabled in PrePhysics, registered only
for an Actor admitted to the current World Level. An empty annotation restores
that baseline through Tick's existing API during historical completion. This
keeps rejected or unchanged property edits out of history without replaying them
merely to discard an empty capture. Component/root caches expose HasInstanceData;
non-default native lifecycle and root recovery data still require annotations.
Previous annotation snapshots retain their external-child capture scope.

Actor annotations compose the shared component cache and root reconstruction
data. The shared root data reconciles root identity and conditionally captures a
generated root's world/relative transforms, parent, and external children. A replacement
root receives its captured pose during post-construction recovery, which restores
attachment reverse links and dependent world transforms.
The annotation refresh adapter passes its prior shared root scope to capture.
Final and inverse snapshots keep authored children found beneath generated
parents in that recovery scope. They capture each child's current parent
and relative pose even after root removal. Consequently, a child with its own
serialized property record receives the correct final pose after parent teardown;
temporary world-transform changes during reconstruction do not become redo state.
Construction failure releases the execution guard and cleans generated output.
Uncertain lifecycle effects or reconstruction failure require transaction recovery;
no terminally destroyed component is resurrected. GC requests made by construction
or registration callbacks remain deferred until the synchronous operation ends.
