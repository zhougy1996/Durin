# Native Actor Construction

Summary: Define deterministic native reconstruction and transient generated-component ownership.

Modules: CoreDObject, Engine

## Component Ownership

Every actor component has one `EComponentCreationMethod`:

- `NativeDefault` is created by the actor constructor and persists as authored
  actor state;
- `Instance` is added through `AddInstanceComponent`, persists, participates in
  transactions, and owns its authoring dirty state;
- `Generated` is derived by native construction, transient, retained outside
  reflected actor storage, and never supplies package or duplication authority.

`GetAuthoredComponents()` returns native-default and instance components.
`GetOwnedComponents()` returns a snapshot of every live authored and generated
component. Registration, visibility, ticking, play, destruction, picking, and
component lookup use the all-live view. Serialization and object-graph
duplication reach only the reflected authored collections; the destination
actor regenerates its own derived identities.

Generated objects use `EObjectConstructionPurpose::Generated`, carry the
`Transient` object flag, and are retained explicitly through
`AActor::AddReferencedObjects`. Editor hierarchy diagnostics may display them,
but label them read-only; reflected editing, rename, duplicate, delete, reorder,
reparent, and drag/drop operations do not accept them. Domain-specific details
actions redirect authoring to the owning Actor or authored input component.

## Reconstruction

`RequestNativeReconstruction()` runs synchronously on the game thread. Actor
spawn requests it after construction and before World registration or
BeginPlay. Actor `PostLoad` covers package load and graph duplication;
`DLevel::PostLoad` performs a final request after authored graph validation.
Reflected actor edits use the same entry. Consumer components such as Spline
publish their own post-mutation notification to request it after derived input
snapshots are ready.

A request raised while `OnNativeConstruct` is executing is coalesced into one
following pass. Destruction rejects new work. Each pass receives an
`FActorConstructionContext` and declares a complete desired set using a stable
`(namespace, GUID)` key and exact component class.

Equal key and class reuse the committed component. Invalid or duplicate keys,
an unconstructible class, or an equal key with a different exact class fail the
pass. New components remain unregistered candidates until the desired set is
valid. Commit attaches candidates to the root, publishes the new registry,
registers and begins new components consistently with the actor, then retires
unclaimed components in reverse lifecycle order. Failure destroys candidates
and preserves the preceding committed registry.

The construction scope suppresses package dirty and edit-revision mutation.
The authored setter or transaction that requested reconstruction remains the
only authoring authority. This suppression is thread-local, nestable, and
limited to the synchronous derived-state pass.
