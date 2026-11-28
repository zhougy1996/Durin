# Transaction Record Foundation

Summary: Define whole-object and custom records, collector ownership, and replay failure handling.

Modules: CoreDObject, DurinEd

Last reviewed: 2026-11-28

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
Any command reporting expiry through `HasExpired` makes its entire entry ineligible. During execution
the buffer reports a command error or exception as a terminal fault. A domain
error's recovery flag does not change the buffer's behavior.
See [replay outcomes](Transactors.md#replay-outcomes).

Each custom record retains one explicit target by exact identity and passes it to
the command during replay. Summaries, replay hooks and package effects use that
target. A transaction can contain several records for different owners. Commands must
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
with collector traversal. Whole-object records use serialized object payloads.

Payload restore validates the requested reflected property shape and resolves
every hard handle. A garbage target is available to transaction replay only when the same entry recorded its liveness transition. A stale generation or a physically removed target fails validation. Weak references decode
their original handle and naturally remain invalid when the target is gone.
Weak-reference byte offsets accompany the payload for explicit replacement; they
never become hard edges. `CreateReplacement` prepares independent hard tables and
weak slots through the checked map, without scanning arbitrary bytes.

## Transaction Snapshot Filtering

Whole-object transaction capture selects the runtime Transaction archive purpose.
It includes Transient fields and excludes NonTransactional/Deprecated fields
inside reflected struct payloads as well as object members. The payload retains
this mode for restore. Ordinary property snapshots keep their existing persistence
filtering; the two policies do not share an exclusion flag. A transaction snapshot
retains only hard identities actually serialized by that transaction archive.

## Whole-Object Records

Whole-object recording captures virtual Serialize, metadata and
supplemental annotation before an edit. During recording, a membership set tracks captured objects by
persistent identity.
Nested scopes share this index. Custom commands index their
explicit target by exact generation for `ContainsObject()` and suppress later
whole-object capture for that target. An earlier baseline retains its position.
The outermost End captures final state, including the pending-kill bit, and releases the index. Retained
history uses the ordered records for replay, reference collection, membership
queries and controlled replacement. Recorded property edits use virtual Modify. Interactive drafts remain property-editor values;
there is no separate focused history, field locator, promotion or record-update
API.

The record's private editor `FSerializedObject` owns an `FDiffableObject` and
annotation. This is distinct from CoreDObject's byte payload of the same name,
following UE's editor/runtime ownership split. The load and flip slots each retain one identity, immutable payload,
compatibility context and set of tags from one Serialize call. Whole-object replay reads
indices into the capture's hard and weak reference tables. Diff compares the
referenced identities across captures, since each capture assigns its own indices. Load/validate uses
a temporary transfer view of that storage.
CoreDObject accounts transfer allocations and observation tags at their owner;
the record adds annotation allocations once. Collector traversal follows the same
observation and annotation ownership.

Finalize compares the initial and final captures. An edit that returns to its initial
value contributes no field difference. Events carry the original capture identity and
the live transaction and operation IDs.
Ordinary replay events use the stored finalized delta. Resurrection also compares
the live object after completion with its native archetype.
Each whole-object record delivers its event to its own target. Actor annotations supply
component and attachment state for
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

Undo and Redo first capture each whole-object inverse in replay
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

The selected record supplies the completion annotation. An empty annotation invokes ordinary PostEditUndo. Standalone component records
capture authored fields. Actor completion uses normal Level activation after reconstruction. See
[native construction](../../../Runtime/World/NativeConstruction.md#transaction-reconstruction).

Each retained payload enumerates its exact hard edges through the enclosing
record's collector callback. Weak and soft targets remain non-owning. Removing
records removes those hard edges without manual per-record roots. Material graph
commands own property payloads for their structural/domain operations and use
CoreDObject property capture/restore directly; they do not use a parallel generic
transaction snapshot abstraction.

## Array Edits

Call `Modify()` on the object that serializes the array before the first write.
Its first snapshot captures element order, values and other transactional fields
on that owner. Inserts, removals and repeated writes in one scope replay as one
edit, even when later operations shift the indices used by earlier writes.

Spline point edits, curve replacement and loop changes capture the spline component.
The component serializes its curve along with its other transactional state.
Capture cost therefore depends on the serialized component size.

## Controlled Reinstancing

An explicit instance publisher registers `CreateTransactorReplacementParticipant`
with its replacement operation. Package reload registers
`CreateTransactorReloadParticipant` with the packages it will replace. Each
participant checks that the buffer is idle and the map identities are valid
before publication. After publication it forwards the map synchronously to
`DTransBuffer::OnObjectsReinstanced` while the publisher still pins the old objects.

Whole-object records rebind their target and both serialized slots in place.
Immutable serialized payloads use copy-on-write when embedded object keys change.
Weak slots are remapped without adding GC ownership. Commands and ordinary
annotations expose writable references through `AddReferencedObjects`; the
buffer retains their instances. Actor annotations additionally remap
the Actor target, typed instance data and historical root and attachment endpoints.

Explicit instance maps update mapped references on both sides of the cursor.
Unmapped peers keep their existing state. Package reload also makes eligible
prepared objects transactional, validates all records in affected entries, and
checks that the retiring graph has no remaining references in those entries.
When an entry fails, reload marks its surviving peer packages dirty.

Both paths process the history present at publication. Successfully rebound entries
keep their IDs and relative order. A failure removes the whole entry and logs its
ID and cause. The buffer remaps the cursor and Undo barriers for each removal and
broadcasts one queue change after all removals in the batch. Requests carrying
removed IDs are rejected. The participant holds the removed entries until the
retirement phase, when their commands are destroyed while old objects are still pinned.
CDO/default-subobject transactions use
the native admission and logical reference categories above; arbitrary archetypes
and script template mutation remain unsupported.

## Hierarchy Boundary

Transaction references follow the ordinary CoreDObject reachability rules. A
directly referenced child keeps its complete Outer chain alive. Referencing an
Outer does not retain children or siblings. Record ownership changes on the owner and call `Modify()` on the child before marking it as garbage. Replay restores the recorded liveness before loading fields and references. [Component structure history](../LevelEditing/LevelEditorDetails.md#component-structure-history)
uses owner and participant snapshots. History enumerates recorded targets, serialized references and annotation dependencies. Unreferenced children with only an Outer relationship can be collected while history survives. Completion reconciles attachment and registration with restored ownership.

Record callbacks request GC; the owner collects after application and native completion. A custom replay exception terminates execution with the transaction, direction and record diagnostic.

## Related Documentation

- [Editor Transactor Core](Transactors.md)
- [Garbage Collection](../../../Runtime/ObjectSystem/GarbageCollection.md)
- [Reflected Property Editing](../PropertyEditing/ReflectedPropertyEditing.md)
