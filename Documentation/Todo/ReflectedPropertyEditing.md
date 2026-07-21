# Reflected Property Editing TODO

Last reviewed: 2026-07-22

## Current Status

The Level Editor Details panel can edit reflected scalar, enum, string, object,
math-structure, array, and map properties. A successful edit currently writes
the reflected storage directly and marks the owning package dirty. A few
properties that require immediate side effects bypass the generic write path:

- Actor and scene-component transforms call `SetRelativeTransform()`.
- Static-mesh and material object references use type-specific setters.
- Material-instance parents use `SetParent()` so parent-cycle validation still
  runs.

These special cases preserve important runtime invariants, but they also show
that a raw reflected write is not a complete editor operation. The generic path
does not notify the object, does not distinguish a live drag from a committed
edit, and does not create an editor transaction. Array and map editing further
complicate notification because the changed leaf may be nested below a
top-level member property.

## Implemented Foundation

- [x] Define change phase, mutation kind, and edit/undo/redo origin separately.
- [x] Define a member-to-leaf property path with fixed-array, dynamic-array, and
  stable serialized map-key selectors.
- [x] Add the default no-op `DObject::PostEditChangeProperty()` virtual hook.
- [x] Cover scalar and nested-container event delivery in `CoreDObjectTests`.
- [x] Add focused property-value capture and restore for reflected scalar,
  string, object-reference, struct, array, and map values.
- [x] Keep object references found recursively in a snapshot rooted until every
  snapshot copy releases them.
- [x] Reject unsupported or mismatched property snapshots with an error.
- [x] Add an editor-owned edit session that captures the original value,
  coalesces repeated previews, commits real changes, and restores cancellation.
- [x] Add a generic reflected-storage mutation adapter and leave a stable
  adapter interface for later setter-backed validation.
- [x] Root the edited object while a session owns raw reflected container
  addresses, and deep-copy map-key path bytes used by callbacks.
- [x] Route top-level scalar, enum, string, math-structure, and object Details
  widgets through the shared edit session.
- [x] Preserve continuous activation/deactivation across compound vector and
  transform controls, and cancel an active preview when inspection context is
  lost or becomes read-only.
- [x] Retain setter-backed transform, static-mesh/material, and material-parent
  behavior behind the Details mutation adapter.

Top-level and nested array/map Details edits now emit the common events and
register generic reflected-property transactions. Nested edits notify the exact
leaf path, while their before/after snapshots are rooted at the stable
object-owned member so an array reallocation or map rehash cannot invalidate
Undo/Redo storage. Structural operations commit once per button/key action, and
continuous element widgets still coalesce into one transaction.

Setter-backed transform, static-mesh/material assignment, and material-parent
rules are now process-lifetime registrations in the shared editor mutation
registry. Details resolves them without owning type checks. Material Editor
parent and parameter controls use the same edit session and transaction path;
material parameter notifications invalidate render data for interactive edits,
commit, cancel, undo, and redo. The next optional step is evaluating generated
property-specific callback metadata after these common usage examples have had
time to stabilize.

## Goals

- [x] Give reflected objects one stable post-edit notification contract.
- [x] Preserve both the top-level member property and the exact edited leaf in
  each change event.
- [x] Distinguish interactive preview updates from the final committed change.
- [x] Represent scalar assignment and array/map structural changes explicitly.
- [x] Route commit, cancel, undo, and redo through the same mutation and
  notification path.
- [x] Coalesce a continuous ImGui interaction into one undo transaction.
- [x] Mark packages dirty once a real edit is committed without requiring each
  widget or object callback to do so.
- [x] Keep value validation and object invariants in setters or property
  adapters rather than treating a post-edit callback as validation.
- [x] Remove Details-panel type special cases when an equivalent validated
  property adapter and object notification path exists.

## Non-Goals

- Making every runtime property assignment observable. The first implementation
  covers editor-originated reflected edits.
- Replacing semantic setters used by gameplay or runtime systems.
- Storing editor transactions or ImGui state in `DObject`.
- Rebuilding the reflection code generator solely to support the first version.
- Using string-named metadata callbacks as the primary notification mechanism.

## Proposed Change Contract

Define the event types in `CoreDObject` so an object can react without depending
on `DurinEd`, `LevelEditor`, or ImGui:

```cpp
enum class EPropertyChangePhase : uint8
{
	Interactive,
	Committed,
	Cancelled,
};

enum class EPropertyChangeKind : uint8
{
	ValueSet,
	ArrayAdd,
	ArrayRemove,
	ArrayResize,
	MapInsert,
	MapRemove,
	MapKeyRename,
};

enum class EPropertyChangeOrigin : uint8
{
	Edit,
	Undo,
	Redo,
};

enum class EPropertyPathSelector : uint8
{
	None,
	StaticArrayIndex,
	ArrayIndex,
	MapKey,
};

struct FPropertyPathSegment
{
	const FProperty* Property = nullptr;
	EPropertyPathSelector Selector = EPropertyPathSelector::None;
	uint64 Index = 0;
	std::span<const uint8> MapKeyData;
};

struct FPropertyChangedEvent
{
	const FProperty* MemberProperty = nullptr;
	const FProperty* LeafProperty = nullptr;
	std::span<const FPropertyPathSegment> Path;
	EPropertyChangePhase Phase = EPropertyChangePhase::Committed;
	EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
	EPropertyChangeOrigin Origin = EPropertyChangeOrigin::Edit;
};
```

Add a default no-op virtual hook to `DObject`:

```cpp
virtual auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void;
```

`MemberProperty` is the reflected field directly owned by the edited object.
`LeafProperty` is the scalar, object, struct, array, or map node actually
changed. `Path` disambiguates fixed-array indices and nested array/map entries.
The event owns or borrows its path only for the duration of the synchronous
callback; callbacks must not retain the span.

The representation must not identify map entries only by iteration index. Map
iteration order can change after insertion, removal, or key rename.
`MapKeyData` therefore borrows a stable serialized key snapshot owned by the
caller for the duration of the synchronous notification.

## Edit Session

Introduce an editor-owned reflected-property edit session. The Details panel
should report widget lifecycle and proposed values to this service rather than
combining direct mutation, dirtying, and notification in every draw branch.

```text
Begin(object, property path)
    capture the original property value
    establish the transaction description

Apply(value)
    validate and write through the selected property adapter
    notify PostEditChangeProperty(Interactive)

Commit()
    ignore a no-op edit
    notify PostEditChangeProperty(Committed)
    mark the package dirty
    commit one already-applied editor transaction

Cancel()
    restore the original value through the same adapter
    notify PostEditChangeProperty(Cancelled)
```

Checkboxes, enum selections, object pickers, and container buttons normally
begin and commit in one frame. Drag, color, transform, and text controls remain
interactive until `ImGui::IsItemDeactivatedAfterEdit()` reports the end of the
interaction. Losing the inspected object, closing the document, or entering a
read-only PIE state must commit or cancel the active session deliberately; it
must not silently abandon an already-applied value.

Objects may perform cheap preview work during `Interactive` and defer expensive
resource reconstruction until `Committed`. A callback must tolerate repeated
interactive events and must not create editor transactions recursively.

## Mutation and Validation

The edit service needs a single mutation abstraction with two implementations:

- A generic reflected-storage adapter for values that are safe to copy into
  their reflected address.
- A registered semantic adapter for properties that must call a setter, reject
  a value, preserve ownership, or coordinate external state.

An adapter should support reading a value snapshot, applying a proposed value,
restoring a snapshot, and returning an error. Post-edit notification happens
only after a successful apply. Validation that can fail must happen before the
post-edit callback.

Initial semantic adapters should cover:

- `DSceneComponent::RelativeTransform`.
- Static-mesh component mesh and material assignments.
- Material-instance parent assignment and cycle rejection.

The existing property serialization in `DObject/Archive.cpp` already handles
primitive, struct, object-reference, array, and map values internally. Extract
or expose a focused property-value snapshot codec instead of using a whole
object snapshot for each edit. Snapshots containing object references must keep
those references GC-visible for the lifetime of an active edit or transaction.

## Transactions and Dirty State

Add a generic reflected-property transaction to `DurinEd`. It should retain a
safe object reference, the property path, the before/after snapshots, the
change kind, and a human-readable description.

- [x] Use `FEditorTransactionManager::CommitApplied()` after an interactive
  edit has already placed the object in its final state.
- [x] Undo restores the before snapshot through the mutation adapter.
- [x] Redo restores the after snapshot through the mutation adapter.
- [x] Undo and redo issue the same object notification used by a normal commit.
- [x] A failed restore leaves transaction history coherent and reports the
  adapter error through the existing transaction event path.
- [x] A no-op comparison produces no transaction and does not dirty the
  package.
- [x] Continuous editing produces one entry per activation/deactivation cycle,
  not one entry per frame.

Dirty-state restoration is a separate policy from value restoration. The first
version may conservatively leave a package dirty after undo, matching the fact
that the in-memory document has participated in editing. A later saved-revision
model can clear dirty state only when the transaction head exactly matches the
last saved revision.

## Container Changes

Array and map operations need explicit change kinds because they cannot always
be described as assignment to an existing leaf.

- [x] Capture array add, remove, resize, and element assignment separately.
- [x] Preserve sufficient before/after data to undo removed or resized-away
  elements.
- [x] Capture map insert, remove, value assignment, and key rename separately.
- [x] Reject a map key rename that would collide with an existing key before
  notifying the object.
- [x] Build the event path from the outer member through every nested container
  so the object can invalidate the correct derived state.
- [x] Ensure a structural mutation cannot leave an active child widget holding
  a stale element address.

## Optional Property-Specific Sugar

After the object-level hook and edit service are stable, reflection metadata may
offer a concise property-specific callback such as `OnChanged`, provided the
code generator resolves it to a validated function pointer and checks the
signature. Do not perform a string lookup on every edit. The generated callback
should be invoked by the common object hook or mutation pipeline so it receives
the same phase, path, undo/redo, and container semantics.

This is optional. An object-level override with a property comparison is enough
for the first implementation and avoids expanding reflection metadata before
the event contract is proven.

## Recommended Implementation Order

1. Add the change event and default `DObject::PostEditChangeProperty()` hook,
   plus focused CoreDObject tests for event paths.
2. Add a property-value snapshot codec with object-reference lifetime handling.
3. Add the reflected-property edit session and generic mutation adapter in
   shared editor code.
4. Migrate scalar, enum, string, math-struct, and object edits in Details.
5. Add the generic reflected-property transaction and route undo/redo through
   the edit service.
6. Add array and map structural operations with stable paths and snapshots.
7. [x] Register semantic adapters and remove equivalent Details-panel type checks.
8. [x] Migrate Material Editor parameter controls to the same continuous-edit and
   transaction behavior where their setter semantics are compatible.
9. Evaluate generated property-specific callback metadata only after the common
   contract has stable usage examples.

Each step should keep the existing editor behavior usable. Do not combine the
event contract, all container operations, code-generation changes, and every
editor migration into one change.

## Validation

- [ ] Verify scalar, enum, string, object, and math-structure edits issue the
  expected interactive and committed events.
- [x] Verify a drag issues multiple preview events but creates one transaction.
- [x] Verify clicking without changing a value creates no event, dirty state,
  or transaction.
- [x] Verify cancel restores the original value and derived runtime state.
- [x] Verify undo and redo call object notification and refresh scene/render
  state.
- [ ] Verify nested fixed arrays, dynamic arrays, structs, and maps produce the
  correct member, leaf, and path.
- [x] Verify map paths remain meaningful after insertion, removal, and key
  rename.
- [ ] Verify rejected assignments do not mutate, dirty, notify, or enter the
  transaction history.
- [x] Verify object references held only by a transaction remain valid across
  garbage collection, or that an expired target fails safely.
- [ ] Verify switching selection or documents during an active edit follows the
  documented commit/cancel policy.
- [x] Build the full `all` target through `BuildTool` using the active Agent
  profile.
- [ ] Run `DurinEditor` from the same full build and smoke-test Details editing,
  save, undo, redo, PIE read-only behavior, and shutdown.

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPropertyEditing.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPropertyEditing.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
