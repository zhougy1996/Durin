# Reflected Property Editing

Summary: Define detached property validation, stable edit targets, notifications, edit sessions, and history submission.

Modules: DurinEd, CoreDObject

Last reviewed: 2026-11-22

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
| Scoped recording, Undo/Redo ordering, package revisions, and dirty checkpoints | [Transactors](../Transactions/Transactors.md) |
| Property kinds, metadata, and value operations | [Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md) |

`FPropertyView` and its frame-settled controls use the same `FPropertyEdit`
implementation. An active edit roots its exact preview target and releases it
on commit, cancel, or reset. The shared editor transactor owns committed history
and its collector edges.

## Object Notification Contract

`DObject::PreEditChangeProperty(FPropertyEditProposal&)` returns
`std::expected<void, FObjectValidationError>` for synchronous validation and
normalization. The proposal contains the complete mutable detached snapshot root,
a resolved draft leaf, and the member, path, phase, kind, and origin of the edit.
Failure rejects the candidate before live storage changes. Errors own the
object/property identity, reason, and any module-owned typed cause.
`FPropertyEditExtension::PreEdit` uses the same typed result. Lower layers retain
causes; the edit-session boundary formats them into its public result message.

Map removal and key rename may have no leaf in the candidate; in that case the
leaf container is null and the full draft root remains available.

`DObject::PostEditChangeProperty(const FPropertyChangedEvent&)` is the common
notification hook. Its default implementation is a no-op.

After transaction validation and before any Undo/Redo record is applied, the
transactor calls `DObject::PreEditUndo()` once per distinct property-record target.
Its default implementation is a no-op. After all records have been applied, it
calls `DObject::PostEditUndo()` once on each of those targets. On replay failure,
the post callback runs after compensation against the final recovered state,
including targets prepared before replay but not reached; validation failures
before replay emit neither callback. Custom
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

`FPropertyChangedEvent` separates three independent dimensions:

- `Phase`: `Interactive`, `Committed`, or `Cancelled`;
- `Kind`: value assignment or an explicit array/map structural operation; and
- `Origin`: normal editing, Undo, or Redo.

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

The generic path reads and writes the stable reflected snapshot root. It
captures live state, restores the candidate into an internal detached draft,
invokes the pre hook, captures the normalized draft, writes live storage once,
and recaptures the actual value. A failed write or recapture attempts rollback
and emits no post event. Same-target nested edits from a hook are rejected.

Generic mutation returns `std::expected<void, FPropertyMutationError>`.
Execution retains the changed flag and actual/recovered snapshots. Errors own
edit identity and typed draft, snapshot, or validation causes, preserving the
primary failure separately from rollback and recovery-capture failures.
Lower layers retain typed errors; history and edit-session boundaries format
messages for callers. Control flow never depends on diagnostic text.

Canonical values reside in reflected object storage. Object hooks and registered
property-edit extensions supply validation and reactions; custom widgets submit
detached proposals through the same execution path.

Proposal validation, publication, and transaction recording are synchronous.
Domain systems own asynchronous work triggered by a committed edit. Texture2D
build settings, for example, schedule derived-data work after synchronous
validation and publication of the authored values.

With an active `FScopedEditorScriptCall`, interactive publication
rechecks target admission and authoritative editability after proposal hooks,
before restoring the normalized draft into live storage. The native seam has no
Python dependency; the Python adapter owns conversion and exception mapping.

## Edit Session Lifecycle

`Durin::PropertyEditor::IPropertyHandle` provides typed reads and writes for
reflected integers, floats, real vectors and linear colors. Callers compare
the result with `FPropertyAccess::Success`. The reflected handle resolves a
member or nested path on the current object generation. `SetValue` completes
one edit. The view and its frame-settled controls retain an `FPropertyEdit` for
interactive writes. Both paths validate detached drafts and use the active
editor's history. Editor shutdown restores unfinished previews and releases
their scopes before another editor can record.

The default top-level integer and float controls and the reflected color popup record
input during drawing. At frame end the host applies the active control's final
value and settles its edit before starting a different control's edit. A color
popup keeps one edit through successive picker gestures and commits it on
dismissal. Escape or disappearance restores the active preview. A failed
terminal action blocks the handoff. Drawing order does not change the values
recorded for Undo. Other controls continue to use the legacy widget submission
path.

`FPropertyEdit` implements one logical edit. `Begin`, `Apply`, `Commit`,
and `Cancel` return success as a boolean. Applying the current value succeeds
and a no-op commit adds no undo record. The caller can supply a string output
for diagnostics on failure. The view can surface a rejection as a short,
non-modal notification and suppress repeated copies while it is visible.
A proxy target can report a backing-object write failure after its committed notification.
The session restores its original property value and returns the diagnostic. A failed
restoration keeps the edit available for retry. Other recoverable edit failures keep their reason visible
in the Details panel until retry succeeds. Python uses the failure text in its
exceptions.

A completed property write closes its session even when the buffer subsequently
discards the entry or reports a publication failure. The buffer owns that
diagnostic. Callers can independently check availability before starting another edit.

The lifecycle is:

```text
Begin(target)
  pin the exact live target generation with TStrongObjectPtr
  capture original value
  begin one scoped transactor entry with before == after

Apply(proposal)
  validate/normalize a detached draft
  atomically apply and recapture the actual value
  replace the pending record's after payload
  notify Interactive when it changed

Commit()
  check the object's CanEditChange for the edited member
  notify Committed
  end the scoped record

Cancel()
  if changed, restore through the same generic hook path
  discard an owned recording or close the nested scope
  notify Cancelled
  create no transaction
```

Checkboxes, selections, and structural buttons normally begin and commit in one
frame. Drag, text, vector, and transform widgets keep the session active
until deactivation. Repeated `Apply()` calls therefore produce preview events
but only one transaction. Escape restores the original value. A successful commit emits one Committed notification even when the last
interactive update already supplied the final value. Cancellation emits Cancelled.
If a proxy rejects its committed write, restoration emits Cancelled and the
caller receives the write failure. An unchanged edit leaves history and package
dirtiness untouched.

Reflected color values use the picker popup as the edit boundary. Opening the
popup does not begin a transaction. The first proposal begins a normal property
edit session; subsequent picker gestures update the same preview until the popup
closes. Normal dismissal commits the session, while Escape or losing the
editable target cancels it. The picker reports its lifetime separately from
the activation state of its nested controls; it does not write reflected
storage or own history. Map keys remain structural edits and retain their widget
identity across renames, with uniqueness validation on the structural write path.

Legacy controls use `FPropertyEditInteraction` to
observe `FPropertyInteractionState` independently of the writer. Numeric and
vector gestures and text confirmation use control activation. Color gestures
use popup lifetime. This follows UE's separation of interactive updates from
the enclosing transaction boundary. Escape takes precedence over deactivation
and new proposals. Hosts call `EndInteractionFrame` after drawing all rows to
cancel abandoned previews. Legacy per-row reconciliation also detects missing
controls on the next frame. Failed restoration retains the session and cancel
request. Material graph sessions keep their domain setter and likewise remain
active until restoration succeeds.

The legacy UI entry point `UpdatePropertyValueEdit` accepts one interaction description
and an optional detached-value assignment. Missing assignments only observe the
control lifetime. Its boolean result reports successful processing of the update.
`SubmitPropertyValueEdit` remains the explicit non-widget assignment entry point
and no longer accepts a second, overriding interaction policy. Both use the same
snapshot preparation and property-session writer. Widget adapters reduce raw
activation or popup state to one identity, boundary kind, and terminal request;
the graph canvas does not maintain a second widget identity or visibility clock.

All property writers prepare the write before copying the root into a detached
proposal. Preparation checks read-only state, pending cancellation, logical
target, and (for widget writes) control identity and boundary. A different
control or target must finish the preceding interaction successfully first;
restoration failure rejects the new assignment without invoking it. Input from
the cancelled gesture cannot restart it. Container mutations use the same gate
and retain detached map keys across restoration. Prebuilt root snapshots are
private to the prepared write path, so callers cannot submit a stale snapshot
that resurrects another field's cancelled preview.

Material canvas cancellation reports whether the Toolkit may proceed.
Deactivate, close, discard, and working-copy reset must preserve the canvas and
working object if restoration fails; retrying the operation retries cancellation.
An affected material Toolkit rejects object replacement before publication if
its interaction cannot be restored.
This failure propagation is a Durin requirement for rejectable restoration, not
a claim that UE's color-picker reset callbacks provide a retry protocol.

This division follows UE 5.8's `IPropertyHandle::SetValue`,
`SPropertyEditorNumeric::OnBeginSliderMovement`/`OnEndSliderMovement`, and
`FColorStructCustomization` callbacks: interactive updates are distinct from
the transaction owner's terminal action. Property handles accept per-write flags. `NotTransactable` skips creating a
transaction. An enclosing recording can still capture the property owner.
`InteractiveChange` sends an interactive notification and leaves the final
notification to the caller's terminal write. Reuse the handle through the
gesture. The final write emits one committed notification even when its value
matches the last interactive update. A subsequent identical write is silent. A failed terminal action is
retried at most once per ImGui frame, and an unchanged error is reported once per
interaction. Cancellation remains pending across failed retries.

`CancelUnchanged()` closes an active unchanged session, emits Cancelled for a
live target, and ends its transaction scope. It preserves an enclosing
recording and earlier edits when a proposal is rejected. Sessions with applied
changes use the ordinary restoration and cancellation path.

Destroying a session cancels an applied preview as a safety fallback. Hosts
should still explicitly commit or cancel when selection, document, read-only
state, or workspace activity changes so errors can be reported deliberately. A
failed Apply or Cancel keeps the session active with its stable target and
original snapshot so restoration can be retried. If the destructor's final
safety cancel fails, it emits a fatal editor error instead of silently
discarding the preview state.

## History Submission

Begin requests a transaction by default. Passing `bShouldTransact = false`
skips opening a scope. An existing recording still captures the target. The explicit
non-null DTransactor reference overload remains for low-level native composition.
NonTransactional property metadata continues to exclude that property from
history, including when ordinary recorded admission is selected. Local preview
snapshots, validation and cancellation restoration remain available without history.

The session opens one scope and calls `Modify(false)` before the first write.
Recorded sessions enroll ordinary targets with the Transactional object flag.
Template targets follow runtime eligibility. The first whole-object baseline
includes edits made by change reactions. Repeated Modify calls reuse it.
Commit ends the scope after the Committed notification and records the applied
value. An unchanged nested commit ends its action without canceling an enclosing
recording. Cancellation discards the shared native recording; preview restoration
remains the edit session's separate responsibility. No-op/canceled Modify can
leave a dirty package without a history entry.

`FTransaction::FObjectRecord` reconstructs the target from its exact object identity,
whole-object serialized payloads and annotations. Undo/Redo check structural/type/identity validity and restore historical
payloads directly, bypassing new-edit proposal validators and normalization.
New edits continue to use detached validation; replay emits the normal
property notifications followed by balanced object completion hooks. Record identity and payload
rules belong to [Transaction records](../Transactions/TransactionRecords.md);
ordering, rollback, and recovery belong to [Transactors](../Transactions/Transactors.md#structural-history-and-barriers).

Editor object creation supplies `EObjectFlags::Transactional` for automatic
whole-object capture. A snapshot member marked
`EPropertyFlags::NonTransactional` disables session history rather than selecting
a different history backend. Object eligibility and property exclusion are distinct.

Dirty-state and save-checkpoint behavior belongs to
[Package revisions](../Transactions/Transactors.md#package-revisions).
Persistent changes outside history must invalidate the checkpoint and mark the
package dirty. Reflected property edits do not request asset-registry scans.

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
