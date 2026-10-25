# Transaction Record Foundation

Summary: Define whole-object and array operation records, collector ownership, and replay failure handling.

Modules: CoreDObject, DurinEd

Last reviewed: 2026-10-21

## Scope

CoreDObject supplies `FPersistentObjectRef` identity categories and transaction
serialization. DurinEd's `FTransactionObjectRecord` coordinates whole-object
before/after payloads, observations and annotations in the same Undo/Redo order
as executable custom changes. Capture precedes the first mutation; later captures
of that object preserve its initial baseline. Object creation and deletion remain
explicit custom changes rather than persistent-reference resurrection.

All capture, resolution, collector traversal, and detached restore operations
run on the game thread under CoreDObject's synchronous collection contract.

## Custom Replay Results

`FCommandChange` exposes synchronous Apply/Revert with a generic owned message
and a recovery-required disposition. It carries no Actor or Material error types,
deferred completion, or synchronous qualification flag. Initial domain execution
is separate from StoreUndo retention; Undo calls Revert and Redo calls Apply.

Actor/Component and LevelEditor changes retain typed `FActorChangeError` in their
domain adapter. MaterialEditor owns its independent `FMaterialChangeError`,
including material diagnostic causes. Domain composition and rollback preserve
these causes until conversion at the generic history boundary. Incomplete rollback
or an exception propagates a recovery disposition, rather than a retryable message.
Descriptions/details are presentation metadata and do not store failure state.

Bound custom records retain an exact owning object for UE-style per-object
recording and object summaries. Custom changes declare modified objects through
Core's `FCommandChange::GetModifiedObjects`; records freeze and deduplicate exact
identities for summaries, replay hooks, GC retention and package effects. A
single declared participant binds an otherwise unbound custom record. Composite
membership does not by itself claim exclusive whole-object capture. Both must
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
a reachable managed owner explicitly enumerates the reference. Collector
traversal cannot rescue an object already marked as garbage.

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
every hard handle. A stale, physically removed, or garbage hard reference makes
restore fail rather than substituting another object. Weak references decode
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

`FTransactionObjectRecord::CaptureWhole` captures virtual Serialize, metadata and
supplemental annotation before an edit. The transactor coalesces records by exact
object identity across nested scopes and captures final live state at outermost
End. Property editing uses the same whole-object path even when Modify does not
capture an ordinary object. Interactive drafts remain property-editor values;
there is no separate focused history, field locator, promotion or record-update
API. Selected `SnapshotObject` calls affect observation only.

The record's private editor `FSerializedObject` owns an `FDiffableObject` and
annotation. This is distinct from CoreDObject's byte payload of the same name,
following UE's editor/runtime ownership split. Before, final, inverse and snapshot
states use the same storage type: each retains one identity, immutable payload,
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
Replay events capture live state after native completion; component completion
precedes Actor reconciliation and dispatch, as owned by the transactor.
Actor annotations now supply generated-component additional changes through this
same path; their property-only observations, construction filtering and reference
path matching are owned by
[native construction](../../../Runtime/World/NativeConstruction.md#transaction-reconstruction).

Historical load bypasses new-edit proposal validation and normalization.
Reflected member notifications follow the complete load and precede object
completion; native data does not invent reflected members. Capture returns
`std::expected<FTransactionObjectRecord, FTransactionObjectRecordError>`;
validation and application return the corresponding expected void. Errors own
exact target identity, serialization diagnostics and recovery disposition.
`FTransactionRecord` formats diagnostics at the generic history boundary.

Replay prepares all live inverses before ordered loading, matching UE's separate
Save and Load passes. The canonical editor record validates the desired transfer,
loads through CoreDObject's single serializer, and compensates a failed load from
its prepared inverse. Durin's checked coordinator publishes historical flips only
after every record and native completion succeed. Clean compensation loads the
prepared live inverse without replacing either historical side; temporary external
references cannot become new history dependencies after a rejected replay.
A native exception or
failed compensation requires recovery and retains the prepared reference graph;
failed replay does not advance the history cursor or emit a successful UndoRedo
event. Compensation does not flip history from an intermediate state.

No-op detection applies to the complete transaction, as in UE's `IsTransient`.
If any record or annotation retains an edit, unchanged SaveObject baselines stay
in that ordered transaction too. They can be needed to restore serialized fields
around an opaque native command even when initial and final bytes compare equal.
Only a wholly unchanged transaction is discarded; unrecorded dirtiness is not
implicitly cleared.

Initial, final and inverse captures acquire immutable annotations before virtual
Serialize; schema checks do not acquire another annotation. Replay delivers the
restored side's annotation through annotation-aware PostEditUndo. A compensated
load delivers the pre-operation annotation. Completion annotations stay
collector-visible through callbacks and are released at operation end. An
annotation alone retains a record despite equal serialized bytes.

An object record delivers annotation-aware completion even for explicit absence.
Custom-only participants receive ordinary completion. The base DObject overload
forwards safely; Engine components interpret an empty serialized annotation as
their implicit default native state. See
[native construction](../../../Runtime/World/NativeConstruction.md#transaction-reconstruction).

Each retained payload enumerates its exact hard edges through the enclosing
record's collector callback. Weak and soft targets remain non-owning. Removing
records removes those hard edges without manual per-record roots. Material graph
commands own property payloads for their structural/domain operations and use
CoreDObject property capture/restore directly; they do not use a parallel generic
transaction snapshot abstraction.

## Array Operation Records

`FTransactionArrayRecord` is an ordered native record beside whole-object and
custom records. `ITransaction::SaveArray` identifies the owner, reflected member,
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

Multiple range records retain operation order. Each inverse is captured at its
ordered replay step, because later edits may relocate earlier ranges. Flips are
published only after complete successful replay; compensation uses the prepared
inverse without changing history. This checked sequencing is a Durin adaptation.
Native callback failure after a splice requires recovery. Zero-count operations
and equal-value edits through the mutation adapter record nothing; raw SaveArray
requires callers to perform the declared operation and does not infer net no-ops.

A preceding whole-object record owns subsequent array edits on that owner. A
later whole-object capture owns its intermediate baseline and follows preceding
range records in normal replay order. Its inverse is prepared at that ordered
load step too, so repeated Redo cannot replace an intermediate baseline with the
entry's initial state. Other whole-object inverses retain the pre-load pass.
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

Object records prepare their target, serialized states, shared observations,
annotations and any snapshot/completion state together. Relevance includes stored
weak slots in serialized payloads, independently of collector lifetime edges.
Native commands whose dependencies change opt in through `CreateReplacement`;
unaffected records move with their original ownership at commit and need no copy
hook. The editor validates frozen modified participants, affected packages and
the complete retained reference graph. Unsupported required replacement or
incompatible state retires its whole transaction, including otherwise compatible
peer records. Its cross-package peers keep their applied content and lose any
checkpoint that could falsely treat shortened history as a saved baseline.

Preparation retains the source history and its position. Final validation checks
history revision, record snapshots, exact collector edges, package tracking,
events, budget and replacement compatibility. Even a recording that is canceled
before publication invalidates the prepared history. Native data is not loaded
into live targets and construction/lifecycle callbacks do not run during this work.

Commit only moves prepared entries into reserved storage and swaps history,
package tracking and queued events. IDs and Undo/Redo position survive for retained
entries. Old command/annotation destruction occurs during retirement while old
objects remain pinned, outside the non-failing publication. Replaced packages
receive a fresh revision for the newly loaded baseline; corresponding transition
boundaries and save checkpoints rebind together. Candidate objects acquire editor
transactional admission during preparation. Document Discard does not call
`EstablishSavedState` again after publication, which would erase the retained history.

`DTransBuffer::OnObjectsReinstanced` accepts an explicit checked map and reuses
this same participant for synchronous native history replacement. The caller
coordinates actual graph publication and all external owners. Busy/stale
preparation publishes nothing; incompatible records retire their complete entry
under the same package/checkpoint rules. CDO/default-subobject transactions use
the native admission and logical reference categories above; arbitrary archetypes
and script template mutation remain unsupported.

## Hierarchy Boundary

Transaction references follow the ordinary CoreDObject reachability rules. A
directly referenced child keeps its complete Outer chain alive. Referencing an
Outer does not retain children or siblings. Object creation and deletion need
explicit structural records; a persistent reference never resurrects a garbage
object. [Component structure history](../LevelEditing/LevelEditorDetails.md#component-structure-history)
retains the component and its complete Outer subtree while membership is absent,
then restores the same live identity through lifecycle and attachment APIs.

Record application defers garbage collection across callbacks. A custom replay
exception becomes an owned failure with recovery required because its side
effects are unknown. Failed domain execution requiring recovery remains collector-visible in pending
recovery storage, preserving their participant graphs and module references.

## Related Documentation

- [Editor Transactor Core](Transactors.md)
- [Garbage Collection](../../../Runtime/ObjectSystem/GarbageCollection.md)
- [Reflected Property Editing](../PropertyEditing/ReflectedPropertyEditing.md)
