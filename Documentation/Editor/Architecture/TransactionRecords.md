# Transaction Record Foundation

Summary: Define exact editor transaction object identity, collector-enumerated record references, and root-free focused property payloads.

Modules: CoreDObject, DurinEd

Last reviewed: 2026-08-30

## Scope

`Durin::Editor::FPersistentObjectRef` and
`FFocusedTransactionObjectRecord` provide the data and lifetime foundation for
collector-integrated editor history. `DTransBuffer` wraps focused and complete
property records together with executable custom changes in the single
application Undo/Redo order. Object creation and deletion remain explicit
custom changes rather than persistent-reference resurrection.

All capture, resolution, collector traversal, and detached restore operations
run on the game thread under CoreDObject's synchronous collection contract.

## Exact Participant Identity

`FPersistentObjectRef` stores only an `FObjectHandle`: one object-array slot and
its generation. A null reference has the invalid slot. A non-null reference
resolves only while that exact generation remains registered and is not
garbage or begin-destroyed.

The reference never falls back to object path, name, Outer path, class lookup,
or a newly allocated object in a reused slot. Rename and reparent therefore
preserve identity, while physical removal invalidates it. An object already
marked as garbage resolves as absent even before physical removal.

Merely storing a persistent reference has no retention effect.
`AddReferencedObjects(FReferenceCollector&)` reports the live object only when
a reachable managed owner explicitly enumerates the reference. Collector
traversal cannot rescue an object already marked as garbage.

## Retention-Neutral Property Payload

`FPropertyValueSnapshotPayload` is the shared property snapshot wire payload.
It owns encoded bytes plus a deduplicated table of exact handles for hard object
references. Weak references remain generation handles encoded in the bytes;
soft references remain asset paths. The payload never calls `AddToRoot`.

`FPropertyValueSnapshot` remains a general detached-value adapter for callers
outside transaction history. Transaction records use the root-free payload and
collector traversal exclusively.

Payload restore validates the requested reflected property shape and resolves
every hard handle. A stale, physically removed, or garbage hard reference makes
restore fail rather than substituting another object. Weak references decode
their original handle and naturally remain invalid when the target is gone.

## Focused Object Records

A focused record owns:

- the target's `FPersistentObjectRef`;
- a top-level member locator containing declaring type, member name, and fixed
  array index;
- one retention-neutral property payload; and
- a deduplicated set of hard value references.

Capture accepts only a live target and a top-level property belonging to its
class hierarchy. The record stores no live value-container address. At restore
time it resolves the exact target, finds the current member, verifies its
declaring type and snapshot compatibility, allocates
`FReflectedValueStorage`, and decodes into that detached storage.

Detached restore does not mutate a live `DObject`, emit editor notifications,
or bypass `PreEditChangeProperty` and `PostEditChangeProperty`. A later property
transaction migration must feed the detached value through the validated
editor mutation pipeline.

`FFocusedTransactionObjectRecord::AddReferencedObjects(...)` reports the target
and every distinct hard payload reference exactly once. It never reports weak
or soft values. A reachable `DObject` owner may enumerate a native vector of
records from its own `AddReferencedObjects(...)` override. Removing or
destroying a record removes all of those strong edges in the same logical
operation; there are no per-record manual roots to unwind.

## Hierarchy Boundary

Transaction references follow the ordinary CoreDObject reachability rules. A
directly referenced child keeps its complete Outer chain alive. Referencing an
Outer does not retain children or siblings. Object creation and deletion need
explicit future records; a persistent reference never resurrects a garbage
object.

## Validation

The lasting contract is covered by `CoreObjectTests`,
`CorePropertyValueSnapshotTests`, and the persistent-reference and focused-
record cases in `EditorOperationTests`. `EditorPropertyTests` protects the
collector-backed property-history path.

## Related Documentation

- [Editor Transactor Core](Transactors.md)
- [Garbage Collection](../../Runtime/Core/GarbageCollection.md)
- [Reflected Property Editing](ReflectedPropertyEditing.md)
- [Editor Transaction System roadmap](../../Roadmaps/EditorTransactionSystem.md)
