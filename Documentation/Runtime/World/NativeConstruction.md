# Native Actor Construction

Summary: Define deterministic native reconstruction and transient generated-component ownership.

Modules: CoreDObject, Engine

Last reviewed: 2026-08-20

## Component Ownership

Every actor component has one `EComponentCreationMethod`:

- `Native` is created by the actor constructor and persists as authored
  actor state;
- `Instance` is added through `AddInstanceComponent`, persists, participates in
  transactions, and owns its authoring dirty state;
- `Generated` is derived by native construction, transient, retained in the
  runtime ownership collection, and never supplies package or duplication
  authority.

`GetAuthoredComponents()` returns native and instance-authored components.
`GetComponents()` returns a non-allocating const view of
`RuntimeOwnedComponents`, the ordered authority for every live authored and
generated component.
`GetComponents<T>(OutComponents)` and `FindComponentByClass<T>()` preserve the
requested pointer type; `FindComponentByExactClass<T>()` is the Durin-specific
exact-class query. Mutating component ownership invalidates iterators and
references into the live view. Code that can invoke component callbacks uses
`GetComponentsSnapshot()` and revalidates each handle before publication.

The legacy-named persistent `OwnedComponents` field and its
`InstanceComponents` subset retain authored state. Its name is preserved until
the asset corpus can migrate; generic runtime code must not use it as live
ownership authority. `RuntimeOwnedComponents` is the transient reflected
strong-reference collection, while `GeneratedComponents` is the keyed
reconstruction index. Serialization and object-graph duplication use only
authored component state; the destination actor regenerates its own derived
identities.

Generated objects use `EObjectConstructionPurpose::Generated`, carry the
`Transient` object flag, and are retained explicitly through
the reflected transient `RuntimeOwnedComponents` collection. Editor hierarchy diagnostics may display them,
but label them read-only; reflected editing, rename, duplicate, delete, reorder,
reparent, and drag/drop operations do not accept them. Domain-specific details
actions redirect authoring to the owning Actor or authored input component.

## Reconstruction

`RequestNativeReconstruction()` runs synchronously on the game thread. Actor
spawn requests it after construction and before World registration or
BeginPlay. Actor `PostLoad` covers package load and graph duplication;
`DLevel::PostLoad` performs a final request after authored graph validation.
If an authored child completes `PostLoad` first and requests reconstruction,
the Actor rebuild preserves that keyed generated set for final reuse rather
than leaving duplicate derived inners.
Reflected actor edits use the same entry. Consumer components such as Spline
publish their own post-mutation notification to request it after derived input
snapshots are ready.

A request raised while `OnNativeConstruct` is executing is coalesced into one
following pass. Destruction rejects new work. Each pass receives an
`FActorConstructionContext` and declares a complete desired set using a stable
`(namespace, GUID)` key and exact component class.

Equal key and class reuse the committed component. Invalid or duplicate keys,
an unconstructible class, or an equal key with a different exact class fail the
pass. New components remain unregistered and are not actor-owned candidates
until the desired set is valid. Commit attaches candidates to the root, then
publishes authored-first/generated-desired `RuntimeOwnedComponents` and the keyed
registry as one observable membership state. It registers and begins new
components consistently with the actor, then retires unclaimed components in
reverse lifecycle order. Failure destroys unpublished candidates and preserves
the preceding committed registry and ownership collection.

The construction scope suppresses package dirty and edit-revision mutation.
The authored setter or transaction that requested reconstruction remains the
only authoring authority. This suppression is thread-local, nestable, and
limited to the synchronous derived-state pass.
