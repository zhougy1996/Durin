# Transaction Record Foundation

Summary: Define whole-object and array operation records, collector ownership, and replay failure handling.

Modules: CoreDObject, DurinEd

Last reviewed: 2026-11-24

## Scope

CoreDObject supplies `FPersistentObjectRef` identity categories and transaction
serialization. DurinEd's protected `FTransaction::FObjectRecord` coordinates whole-object
before/after payloads, observations and annotations in the same Undo/Redo order
as executable custom changes. Capture precedes the first mutation; later captures
of that object preserve its initial baseline. Editor Actor and instance-component creation and deletion record the owning
membership and participants. The retained object can be published again by replay.

All capture, resolution, collector traversal, and detached restore operations
run on the game thread under CoreDObject's synchronous collection contract.

## Custom Replay Results

Undo calls Revert and Redo calls Apply on already-recorded commands.
Each command decides whether it can still participate through HasExpired. During execution
the buffer reports a command error or exception as a terminal fault. A domain
error's recovery flag does not change the buffer's behavior.
See [replay outcomes](Transactors.md#replay-outcomes).

Bound custom records retain an exact owning object for UE-style per-object
recording and object summaries. Custom changes declare modified objects through
Core's `FCommandChange::GetModifiedObjects`; records freeze and deduplicate exact
identities for summaries, replay hooks, GC retention and package effects. A
sole participant supplies the target for expiry checks on an unbound command.
Only an explicitly bound owner suppresses later whole-object capture. Commands must
report hard dependencies, modified packages, native owned allocations, and the
module whose executable code/deleter they retain. Collector-only dependencies
receive no object completion hooks. See
[runtime transaction contracts](../../../Runtime/ObjectSystem/Transactions.md) for module
ownership and the runtime boundary.

## Exact Participant Identity

`FPersistentObjectRef::EReferenceType` follows UE's RootObject/SubObject split.
Ordinary root references and `FromKey` preserve exact slot/generation identity.
They resolve only while that generation remains registered and is not garbage
or begin-destroyed. Published CDO/default-subobject chains instead retain an exact
root and checked Outer/name steps, as defined by the
[runtime reference contract](../../../Runtime/ObjectSystem/Transactions.md#coredobject-boundary).

An ordinary root reference never falls back to object path, name, Outer path, class lookup,
or a newly allocated object in a reused slot. Rename and reparent therefore
preserve identity, while physical removal invalidates it. An object already
marked as garbage resolves as absent even before physical removal.

Merely storing a persistent reference has no retention effect.
`AddReferencedObjects(FReferenceCollector&)` reports retained root/cached identities only when
a reachable managed owner explicitly enumerates the reference. While enumerating pending and retained records, the undo buffer disables reference elimination on the collector. A reported allocation can survive GC with its garbage flag set. Ordinary resolution continues to reject it. Removing the last retaining entry permits collection on the next sweep.

## Retention-Neutral Property Payload

`FPropertyValueSnapshotPayload` is the shared property snapshot wire payload.
It owns encoded bytes plus a deduplicated table of exact handles for hard object
references. Weak references remain generation handles encoded in the bytes;
soft references remain asset paths. The payload never calls `AddToRoot`.

`FPropertyValueSnapshot` remains a general detached-value adapter for callers
outside transaction history. Domain commands use the root-free property payload
with collector traversal; whole-object records use serialized object payloads and array records retain only
transaction-filtered element payloads.

Payload restore validates the requested reflected property shape and resolves
every hard handle. A garbage target is available to transaction replay only when the same entry recorded its liveness transition. A stale generation or a physically removed target fails validation. Weak references decode
their original handle and naturally remain invalid when the target is gone.
Weak-reference byte offsets accompany the payload for explicit replacement; they
never become hard edges. `CreateReplacement` prepares independent hard tables and
weak slots through the checked map, without scanning arbitrary bytes. Array
records support owner/reference reinstancing with the same class descriptor; a
class-descriptor change safely retires the complete affected history entry.

## Transaction Snapshot Filtering

Whole-object transaction capture selects the runtime Transaction archive purpose.
It includes Transient fields and excludes NonTransactional/Deprecated fields
inside reflected struct payloads as well as object members. The payload retains
this mode for restore. Ordinary property snapshots keep their existing persistence
filtering; the two policies do not share an exclusion flag. A transaction snapshot
retains only hard identities actually serialized by that transaction archive.

## Whole-Object Records

Whole-object recording captures virtual Serialize, metadata and
supplemental annotation before an edit. During recording, a hash index keyed by
persistent object identity stores the position of the first whole-object record.
Nested scopes share this index. Array records establish participation and retain
their individual positions in the ordered record list. Custom commands index each
declared participant by exact generation; only the bound primary object suppresses
ordinary whole-object capture. Snapshot requests use the indexed whole record.
The outermost End captures final state, including the pending-kill bit, and releases the index. Retained
history uses the ordered records for replay, reference collection, membership
queries and controlled replacement. Recorded property edits use virtual Modify. Interactive drafts remain property-editor values;
there is no separate focused history, field locator, promotion or record-update
API. Selected `SnapshotObject` calls affect observation only.

The record's private editor `FSerializedObject` owns an `FDiffableObject` and
annotation. This is distinct from CoreDObject's byte payload of the same name,
following UE's editor/runtime ownership split. The load slot, flip slot and recording-only
snapshot use the same storage type: each retains one identity, immutable payload,
compatibility context and set of tags from one Serialize call. Load/validate uses
a temporary transfer view of that storage. Snapshot presence comes from its
optional state; no separate presence flag or competing undo baseline is retained.
CoreDObject accounts transfer allocations and observation tags at their owner;
the record adds annotation allocations once. Collector traversal follows the same
observation and annotation ownership.

Selected Snapshot properties accumulate, and an unfiltered snapshot makes later
snapshots unfiltered. Metadata and native changes remain observable independently
of the reflected selection. Finalized events merge the original-to-final full
diff with a last-snapshot-to-final partial diff over the accumulated selection.
This closes an observed edit even when the final value returns to the original
and history is pruned as a no-op. Event identity remains the original capture;
transaction and operation IDs come from the live operation. Annotation additional
changes also close against the last snapshot and preserve their `CreatedBy`.
Ordinary replay events use the stored finalized delta. Resurrection also compares
the live object after completion with its retained inverse baseline. Array records
have no finalized property delta; their replay notification requires a restored
annotation.
Actor annotations now supply generated-component additional changes through this
same path; their property-only observations, construction filtering and reference
path matching are owned by
[native construction](../../../Runtime/World/NativeConstruction.md#transaction-reconstruction).

An always-serialized reflected property marked `ValidateTransactionSource=true` opts into Durin's
checked replay preflight. Undo compares the live property's tagged bytes to the
recorded after side and Redo compares them to the before side. The check runs
before replay callbacks and writes. Material and function expression collections
use this for ordered membership, including the case where a different live
object has the same authored GUID. Actor instance-component lists restore the recorded destination, including root replacement.
Both sides still undergo ordinary reference
and schema validation.

Ordinary rename and Outer changes preserve the current identity on a sibling
name collision. During GC a reachable child must leave a destroyed Outer.
If the root namespace already contains its name, forced detachment assigns
an available name and preserves the child's object identity.

Historical load bypasses new-edit proposal validation and normalization.
Reflected member notifications follow the complete load and precede object
completion; native data does not invent reflected members. Whole-object capture
and admission validation return the original diagnostic on rejection.

Undo and Redo first capture every whole-object and array inverse in replay
order, then load the records in that same order. Commands execute in the load pass. The editor record
validates the desired state before loading it through CoreDObject's serializer.
The first Modify fills only the load slot. Finalize writes the end state into
the flip slot. After replay admission, inverse capture replaces the flip slot
with live state while preserving the recorded lifecycle transition. Each
whole-object load immediately swaps the slots, before PostEditUndo. The loaded
annotation remains available in the flip slot for completion callbacks.
Record-level `CaptureInverse` and `Apply` return `void`. A fault in either reports
the transaction ID, replay direction, original record index and cause, then
terminates.

No-op detection applies to the complete transaction, as in UE's `IsTransient`.
If any record or annotation retains an edit, unchanged SaveObject baselines stay
in that ordered transaction too. They can be needed to restore serialized fields
around an opaque native command even when initial and final bytes compare equal.
Only a wholly unchanged transaction is discarded; unrecorded dirtiness is not
implicitly cleared.

Initial, final and inverse captures acquire immutable annotations before virtual
Serialize; schema checks do not acquire another annotation. Replay delivers the
restored side's annotation through annotation-aware PostEditUndo. Completion annotations stay
collector-visible through callbacks and are released at operation end. An
annotation alone retains a record despite equal serialized bytes.

The selected record supplies the completion annotation. An empty annotation normally
invokes ordinary PostEditUndo. Durin components with a selected serialized record
receive the annotation overload even when empty so they can restore their implicit
default lifecycle state. See
[native construction](../../../Runtime/World/NativeConstruction.md#transaction-reconstruction).

Each retained payload enumerates its exact hard edges through the enclosing
record's collector callback. Weak and soft targets remain non-owning. Removing
records removes those hard edges without manual per-record roots. Material graph
commands own property payloads for their structural/domain operations and use
CoreDObject property capture/restore directly; they do not use a parallel generic
transaction snapshot abstraction.

## Array Operation Records

The same protected `FTransaction::FObjectRecord` represents array ranges, whole objects and custom changes. Durin keeps only the active payload in each entry. `ITransaction::SaveArray` identifies the owner, reflected member,
optional struct ancestors, static member index, range and UE operation sign:
Insert (+1) removes on load, Remove (-1) inserts on load, and Replace (0) restores
values. No record retains a vector address or element pointer. Owner/class/path
validation precedes each load, including after allocation relocation.

`EditTransactionArray` explicitly adapts supported `std::vector<T>` mutations.
It copies the incoming range and reserves capacity before capture; typed
`FArrayOps::Splice` requires nonthrowing element moves. Replay decodes only the
retained range into detached typed storage, then splices it into the live vector.
Replacement seeds that range from current elements to preserve omitted fields.
Insert/remove historical payloads scale with changed elements; a zero-payload
insert record captures its inserted values when first undone. Weak slots remain
non-owning and support checked replacement alongside the hard-reference table.
Each element's strong references use `FPersistentObjectRef`: ordinary objects
keep exact identities, while published default graphs resolve through their
checked class/name chains. Replay materializes a temporary exact reference table
for the shared property decoder; general property snapshots keep exact semantics.

Range records use their recorded indices in the array as it stands before the
first load. Independent ranges can share a transaction. For overlapping edits,
index-shifting operations that depend on each other, or changes to other fields
of the same owner, call `Modify()` before the first write. Its whole-object
record covers subsequent range edits. Commands and whole-object loads that
touch the same state must agree on which record restores it.

Each array load flips its range and operation for the next direction. Zero-count
operations and equal-value edits through the mutation adapter record nothing.
Raw SaveArray requires the caller to perform the declared operation.
Unsupported element/container capabilities,
bulk changes and edits that may affect siblings must explicitly capture the whole
object before mutation. Ordinary vector operations do not record themselves.
Maps retain existing snapshot/domain semantics; SaveArray supplies no map policy.

Spline point insertion, removal, duplication, movement and value edits use the
range adapter, including nested reflected `SplineCurve.Points` storage. Bulk curve
replacement and loop changes select whole-object capture. Road spline editing
uses validated RoadNet property proposals with network-wide preview/invariant
updates, so it retains that full-object fallback. Generic property proposals retain full-object history
and detached proposal snapshots because normalization and callbacks can edit other
fields; material graph commands retain semantic rollback and sparse presentation
changes. These paths make no bounded-capture claim. Vector movement, spline
evaluation/rebuild, annotations and full-object fallbacks may still cost O(N),
even when retained range storage and range serialization are bounded by K edits.
Point reordering stores the affected interval; moving across an entire curve can
therefore retain O(N) values.

## Controlled Reinstancing

`CreateTransactorReloadParticipant` is the single reload integration point for
retained history. Like UE's buffer reinstancing hook, it updates references across
records and their supplemental state. Durin prepares detached replacements through
`FObjectReplacementMap` and its existing publication protocol; collector traversal
is observational. It does not install a transaction-private object map.

Whole-object records rebind their target, load and flip slots and any recording snapshot in place.
Immutable serialized payloads use copy-on-write when embedded object keys change.
Weak slots are remapped without adding GC ownership. Commands and ordinary
annotations expose writable references through `AddReferencedObjects`; the
buffer retains their instances. Actor/component annotations additionally remap
typed reconstruction data, including historical paths of absent generated objects.
If a record retains a replaced participant or cannot rebind required state, its
whole transaction is discarded. Cross-package peers keep their edits and are
marked dirty.

Package reload admits only an idle buffer and makes eligible candidate objects
transactional. After graph publication, the participant receives the replacement
map synchronously while old objects remain pinned. It remaps the history present
at that point, including its Redo branch and barriers. The package registry
change notification follows fixup of the published batch. The explicit
`DTransBuffer::OnObjectsReinstanced` entry point uses the same fixup for callers
that coordinate publication themselves.

The cursor and barriers account for discarded entries. Their native commands are
destroyed during retirement while the old objects are pinned.
CDO/default-subobject transactions use
the native admission and logical reference categories above; arbitrary archetypes
and script template mutation remain unsupported.

## Hierarchy Boundary

Transaction references follow the ordinary CoreDObject reachability rules. A
directly referenced child keeps its complete Outer chain alive. Referencing an
Outer does not retain children or siblings. Record ownership changes on the owner and call `Modify()` on the child before marking it as garbage. Replay restores the recorded liveness before loading fields and references. [Component structure history](../LevelEditing/LevelEditorDetails.md#component-structure-history)
uses owner and participant snapshots. History enumerates recorded targets, serialized references and annotation dependencies. Unreferenced children with only an Outer relationship can be collected while history survives. Actor and component completion restore native lifecycle and attachment state on the retained identities.

Record callbacks request GC; the owner collects after application and native completion. A custom replay exception terminates execution with the transaction, direction and record diagnostic.

## Related Documentation

- [Editor Transactor Core](Transactors.md)
- [Garbage Collection](../../../Runtime/ObjectSystem/GarbageCollection.md)
- [Reflected Property Editing](../PropertyEditing/ReflectedPropertyEditing.md)
