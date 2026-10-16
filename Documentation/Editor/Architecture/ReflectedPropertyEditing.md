# Reflected Property Editing

Summary: Define detached property validation, stable edit targets, notifications, edit sessions, and history submission.

Modules: DurinEd, CoreDObject

Last reviewed: 2026-10-16

## Scope And Ownership

Editor-originated reflected-property changes follow one pipeline: resolve a
stable member-to-leaf target, validate a detached draft, publish the canonical
value, notify the object, and finalize one logical edit. Runtime assignments
still use their own setters; `DObject` does not own editor history.

| Concern | Owner |
| --- | --- |
| Widgets, shared layouts, custom controls, and host view lifetime | [Property Details views](PropertyDetailsViews.md) |
| Actor/Component targets and aggregation | [Level Editor Details](LevelEditorDetails.md) |
| Payload encoding, exact object identity, and record retention | [Transaction records](TransactionRecords.md) |
| Scoped recording, Undo/Redo ordering, package revisions, and dirty checkpoints | [Transactors](Transactors.md) |
| Property kinds, metadata, and value operations | [Property reflection](../../Runtime/Core/PropertyReflection.md) |

A host owns `FPropertyView`, which owns one `FPropertyEditSession`. A live session
temporarily roots its exact preview target and releases that root on commit,
cancel, or reset. Committed payloads and collector edges belong to the externally
owned transactor shared with domain commands.

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
[Transaction records](TransactionRecords.md#retention-neutral-property-payload).
The detached `FPropertyValueSnapshot` adapter is available outside history;
committed property records use collector enumeration rather than manual roots.

## Edit Targets

`Editor::FPropertyEditTarget` describes:

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

Interactive validation may defer publication while domain work completes.
Completion carries `std::expected<void, FObjectValidationError>` with owned
failure context that does not borrow proposal storage. Cancellation retires the
session owner before a late completion can publish. Committed property records
remain callback-free and synchronous; a domain extension with asynchronous
preview work must also provide a synchronous Undo/Redo path.

## Edit Session Lifecycle

`Editor::FPropertyEditSession` implements one logical edit. Begin, Apply,
Commit and Cancel return `FPropertyEditOperationResult` without string outputs.
`Status` is the sole outcome: failed, no-change, changed or pending.
`GetStatus()` and boolean conversion use that status, never message contents.
`Message` owns presentation text produced inside the session from lower-level
path, snapshot, record, mutation and transactor failures. Additional cleanup,
rollback and deferred-rollback failures are appended without replacing the
primary error. Messages survive session reset and retry. Views and logs consume
the message directly; callers do not inspect nested diagnostic causes.

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
  notify Committed
  end the scoped record

Cancel()
  if changed, restore through the same generic hook path
  cancel the scoped record
  notify Cancelled
  create no transaction
```

Checkboxes, selections, and structural buttons normally begin and commit in one
frame. Drag, text, color, vector, and transform widgets keep the session active
until deactivation. Repeated `Apply()` calls therefore produce preview events
but only one transaction. Escape restores the original value. Every successful
Begin has exactly one terminal Committed or Cancelled event, including an
interaction that never changes or returns to its original snapshot; those no-op
interactions do not dirty the package or enter history.

Destroying a session cancels an applied preview as a safety fallback. Hosts
should still explicitly commit or cancel when selection, document, read-only
state, or workspace activity changes so errors can be reported deliberately. A
failed Apply or Cancel keeps the session active with its stable target and
original snapshot so restoration can be retried. If the destructor's final
safety cancel fails, it emits a fatal editor error instead of silently
discarding the preview state.

## History Submission

The session opens one scoped transaction and updates the pending record's after
payload as previews change the live value. Commit ends that scope after the
Committed notification; it records the already-applied value without a second
write. Cancelled and no-op edits create no history entry or package revision.

`FTransactionObjectRecord` reconstructs the target from its exact object identity,
member locator, path, and before/after payloads. Undo/Redo use the same detached
validation, canonical write, and notification path. Record identity and payload
rules belong to [Transaction records](TransactionRecords.md#focused-object-snapshots);
ordering, rollback, and recovery belong to [Transactors](Transactors.md#structural-history-and-barriers).

Dirty-state and save-checkpoint behavior belongs to
[Package and discovery revisions](Transactors.md#package-and-discovery-revisions).
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
