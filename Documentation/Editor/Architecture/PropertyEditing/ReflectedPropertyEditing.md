# Reflected Property Editing

Summary: Define detached property validation, stable edit targets, notifications, edit sessions, and history submission.

Modules: DurinEd, CoreDObject

Last reviewed: 2026-11-28

## Scope And Ownership

Editor-originated reflected-property changes follow one pipeline: resolve a
stable member-to-leaf target, validate a detached draft, publish the canonical
value, notify the object, and finalize one logical edit. Runtime assignments
still use their own setters; `DObject` does not own editor history.

| Concern | Owner |
| --- | --- |
| Widgets, shared layouts, custom controls, and host view lifetime | [Property Details views](PropertyDetailsViews.md) |
| Actor/Component targets and aggregation | [Level Editor Details](../LevelEditing/LevelEditorDetails.md) |
| Payload encoding, exact object identity, and record retention | [Transaction records](../Transactions/TransactionRecords.md) |
| Scoped recording, Undo/Redo ordering, and package dirty state | [Transactors](../Transactions/Transactors.md) |
| Property kinds, metadata, and value operations | [Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md) |

Details and material graph parameter controls retain a property handle for the
active gesture. The control pins its target until the gesture ends. The editor
transactor owns committed history and its collector edges.

## Object Notification Contract

`DObject::PreEditChangeProperty(FPropertyEditProposal&)` returns
`std::expected<void, FObjectValidationError>` for synchronous validation and
normalization. The proposal contains the complete mutable detached snapshot root,
the edited member, the write flags, and the leaf when it resolves. The post-change
event carries the path, change type, and origin.
Failure rejects the candidate before live storage changes. Errors own the
object/property identity, reason, and any module-owned typed cause.
The write path formats the original failure for the caller.

Map removal and key rename may have no leaf in the candidate; in that case the
leaf container is null and the full draft root remains available.

`DObject::PostEditChangeProperty(const FPropertyChangedEvent&)` is the common
notification hook. Its default implementation is a no-op.

After transaction validation and before any Undo/Redo record is applied, the
transactor calls `DObject::PreEditUndo()` once per distinct property-record target.
Its default implementation is a no-op. After all records have been applied, it
calls `DObject::PostEditUndo()` once on each of those targets. On replay failure,
started targets receive completion against the state reached at failure, including
targets whose records were not reached. Admission refusals emit neither callback. Custom
changes continue to own their domain synchronization. The default implementation
calls `PostEditChange()`, producing a full-object property notification with no
member or leaf. Per-property replay notifications retain their Undo/Redo origin
and precede this completion hook. Overrides may use `PostEditUndo()` to rebuild
state that depends on multiple restored fields or objects.

Scene components remember their pre-replay attachment parent without retaining
it. During replay they defer reflected transform updates; completion removes the
old parent's child entry, restores unique membership in the current parent,
notifies the Outliner when root reparenting changes the Actor hierarchy, and recalculates world
transforms through the normal render/physics notification path. Children and
world transforms remain derived state rather than transaction snapshots.

Actor visibility follows the UE setter split: `SetHidden()` only assigns
`bHidden`, while `SetActorHiddenInGame()` synchronizes owned components and marks
the package dirty. Actor property edits and full-object completion notifications
refresh component visibility from canonical storage after native reconstruction;
they never overwrite a primitive component's independent `bVisible` state.

`FPropertyChangedEvent::ChangeType` reports `ValueSet` for assignment, including
a map key rename. Container insertions and removals use `ArrayAdd` and
`ArrayRemove` for both arrays and maps. Interactive scalar writes report
`Interactive`; interactive structural writes combine that bit with the container
operation. `Origin` identifies direct editing, Undo, or Redo.

The event carries both:

- `MemberProperty`, the top-level reflected field owned by the object; and
- `LeafProperty`, the exact scalar, struct, object, array, or map node edited.

`Path` connects the member to the leaf. Its selectors distinguish fixed-array
indices, dynamic-array indices, and map keys. Map entries use serialized key
data instead of iteration indices because rehashing can reorder entries.
Path spans and key bytes are valid only for the synchronous callback.

Objects use the post hook to refresh derived runtime state from canonical
storage. Validation and normalization belong in the pre hook, including
cross-field checks against the full detached root; post hooks handle hierarchy,
cache, and render reactions.

## Property Snapshots

Edits capture one stable object-owned property root, normally the top-level
member. Nested Array/Map operations may invalidate leaf addresses but cannot
invalidate the member payload used by history. Capture or restore fails on an
unsupported or incompatible property shape.

Sessions and records use `FPropertyValueSnapshotPayload`. Encoding, exact hard
handles, soft/weak semantics, and collector retention follow
[Transaction records](../Transactions/TransactionRecords.md#retention-neutral-property-payload).
The detached `FPropertyValueSnapshot` adapter is available outside history;
committed property records use collector enumeration rather than manual roots.

## Edit Targets

`FPropertyEditTarget` describes:

- the owning object;
- member and leaf properties;
- the stable snapshot property and container;
- the owned member-to-leaf path; and
- the mutation kind.

`ForMember()`, `ForStructMember()`, `ForArrayElement()`, and `ForMapEntry()`
build targets without making panels reconstruct path rules themselves. Targets
contain no resolved leaf address. Draft construction resolves temporary draft
storage internally, while sessions and transactions retain only the stable
snapshot root and owned path.

Target validation and path resolution return `FPropertyEditPathError` with
owned location and nested failure context. Failed resolution leaves its output
unchanged. `FPropertyValueDraft` returns `FPropertyValueDraftError`, preserving
initialization, storage, snapshot, and path causes without formatting. Invalid
drafts keep their initialization cause; failed restore remains retryable and
never publishes to the live object. Successful retries do not alter errors
already returned to callers.

## Generic Mutation

Typed setters pass their prepared draft directly to the shared write path.
Applying a proposed snapshot initializes a draft from the supplied value.
The shared write compares the normalized draft with the live
value captured for this write. A synchronous listener can close the recording or preview during
`Modify()`. Shutdown requests block new edit sessions and handle writes. A pending write can finish
while its target remains live. Retiring the target in a proposal or dirty notification
rejects publication.
Canceling the recording discards its undo entry. A request to finish a gesture
during a property write is settled after publication. A failed write returns its
diagnostic. Same-target nested writes from a proposal hook are rejected.
Subsequent writes compare against live storage, including corrections made by
post-edit hooks.

Texture2D corrects invalid usage, compression quality, alpha mip mode, and
alpha coverage threshold to their build defaults after a reflected write.
Interactive updates defer the build. A final settings notification schedules
one build, and a direct usage change selects its default sRGB setting.
Undo and Redo rebuild once from the complete restored object, including its
recorded sRGB override. Explicit build and import requests validate their input
at admission.

With an active `FScopedEditorScriptCall`, interactive publication
rechecks target admission and authoritative editability after proposal hooks,
before restoring the normalized draft into live storage. The native seam has no
Python dependency; the Python adapter owns conversion and exception mapping.

## Edit Session Lifecycle

`IPropertyHandle` resolves the reflected member or nested path on the bound
object generation. Typed setters and `SetValueFromSnapshot` publish a single
change synchronously. The snapshot setter accepts complete detached values,
including structures and containers. Access and validation failures carry
diagnostics for the caller.

An active control holds the same handle throughout a drag. Each update uses
`InteractiveChange | NotTransactable` inside the gesture transaction. On
release, the handle sends one final `ValueSet` if an interactive change was
published. It can finish with the same value as the last frame. Further
identical writes are silent. Escape, a hidden control and owner changes end
ordinary drags at the last published value. Undo reverses the whole drag.

Details settles the outgoing control before starting an incoming assignment.
`UpdatePropertyValueEdit` can observe a widget without supplying a value.
`SubmitPropertyValueEdit` accepts a detached assignment for explicit edits.
Map keys retain widget identity across renames and validate uniqueness before
publication.

A color picker keeps its original color through multiple drags in the popup.
Normal dismissal accepts the result. The Cancel button restores the original
color. A failed restoration reports its diagnostic and ends the owned
transaction normally so an outstanding change can be undone. A picker that
joins an outer transaction leaves it open.

Gesture state belongs to the control. A stale recording is checked against
both its transactor and transaction ID, so late input cannot close an
independent recording. Finishing from within a protected transaction operation
is invalid.

## History Submission

The first proposal opens a gesture transaction when requested. With
`bShouldTransact = false`, an existing recording still captures eligible
writes. NonTransactional property metadata excludes that member from history.

A normalized change calls `Modify()` before publication, or marks the package
directly for an excluded property. Rejected and identical proposals leave the
package alone. Returning to the original value during a gesture can leave its
package dirty. Ordinary targets are enrolled with the Transactional flag.
The first recorded write captures the object and subsequent writes reuse that
record. The gesture finishes its own transaction after the final notification.

`FTransaction::FObjectRecord` reconstructs the target from its exact object identity,
whole-object serialized payloads and annotations. Undo/Redo check structural/type/identity validity and restore historical
payloads directly, bypassing new-edit proposal validators and normalization.
New edits continue to use detached validation; replay emits the normal
property notifications followed by balanced object completion hooks. Record identity and payload
rules belong to [Transaction records](../Transactions/TransactionRecords.md);
ordering and history failure handling belong to [Transactors](../Transactions/Transactors.md#structural-history-and-barriers).

Editor object creation supplies `EObjectFlags::Transactional` for automatic
whole-object capture. A snapshot member marked
`EPropertyFlags::NonTransactional` disables session history rather than selecting
a different history backend. Object eligibility and property exclusion are distinct.

The package dirty-state contract is defined in
[Package dirty state](../Transactions/Transactors.md#package-dirty-state).
Persistent changes outside history mark the package dirty. Reflected property edits do not request asset-registry scans.

## Container Edits

Arrays distinguish assignment, add, remove, and resize. Maps distinguish value
assignment, insertion, removal, and key rename. Structural operations preserve
removed or resized-away values in their stable root snapshots. Key rename rejects
collisions before notification. Draft insertion and traversal invalidation rules
belong to [Container controls](PropertyDetailsViews.md#container-controls).

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/PropertyChange.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Editor/DurinEd/Public/PropertyEditor/PropertyEditing.h`
- `Engine/Source/Editor/DurinEd/Public/Transactions/TransactionObjectRecord.h`
