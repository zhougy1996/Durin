# Reflected Property Editing

This document describes the reflected-property editing system currently
implemented by `CoreDObject` and `DurinEd`. Proposed extensions to the UI API
are documented separately in
[Reflected Property View Evolution](../Reference/ReflectedPropertyViewEvolution.md).

## Scope

The system covers editor-originated changes to reflected `DObject` properties.
It provides:

- synchronous object change notifications;
- stable member-to-leaf property paths;
- focused property snapshots with object-reference lifetime protection;
- interactive preview, commit, and cancel semantics;
- one Undo/Redo entry for one continuous interaction;
- array and map value and structural transactions;
- setter-backed mutation adapters for properties with runtime invariants; and
- an embeddable reflected-property view shared by Details and domain editors.

It does not make every runtime property assignment observable, replace gameplay
setters, or store editor transactions inside `DObject`.

## Ownership Layers

```text
host panel or workspace
  owns FReflectedPropertyView
  supplies FReflectedPropertyViewContext
        |
        v
FReflectedPropertyView
  owns transient widget state and one active edit session
        |
        v
FReflectedPropertyEditSession
  captures, applies, commits, or cancels one logical edit
        |
        +--> IReflectedPropertyMutationAdapter
        |      generic reflected storage or registered semantic setter
        |
        +--> DObject::PostEditChangeProperty()
        |
        +--> externally owned FEditorTransactionManager
                    |
                    v
          FReflectedPropertyTransaction
```

The view and session own only an edit in progress. Committed history belongs to
the transaction manager supplied by the host, so edits from Details, Material
Editor, and other editor surfaces participate in one ordered Undo/Redo history.

## Object Notification Contract

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

Objects use the hook to refresh derived runtime state. For example,
`DMaterialInterface` invalidates material render data for edits to base and
override parameter maps. Validation that may reject a value happens before this
notification, in a setter or mutation adapter.

## Property Snapshots

`FPropertyValueSnapshot` captures one reflected property value rather than a
whole object. The codec supports primitive values, strings, object references,
structs, arrays, and maps.

Snapshots recursively retain referenced `DObject` instances for as long as any
snapshot copy exists. This keeps values reachable while an active session or
committed transaction may restore them. Unsupported or type-mismatched capture
and restore operations fail with an error.

Nested container edits snapshot a stable object-owned ancestor, normally the
top-level member. A vector resize or unordered-map rehash may invalidate a leaf
address, but it cannot invalidate the stored member snapshot used by Undo/Redo.

## Edit Targets

`FReflectedPropertyEditTarget` describes:

- the owning object;
- member and leaf properties;
- the current leaf container and fixed-array index;
- the stable snapshot property and container;
- the owned member-to-leaf path; and
- the mutation kind.

`ForMember()`, `ForStructMember()`, `ForArrayElement()`, and `ForMapEntry()`
build targets without making panels reconstruct path rules themselves. The
active session roots its target object while it retains raw reflected
addresses.

## Mutation Adapters

Every edit uses `IReflectedPropertyMutationAdapter` to capture, apply, and
restore values.

The generic adapter reads and writes the stable reflected snapshot root. A
process-lifetime registry supplies semantic adapters for properties that must
call setters or enforce invariants. Current built-in registrations cover:

- `DSceneComponent.RelativeTransform` through `SetRelativeTransform()`;
- `DCameraComponent.ProjectionSettings` and its nested fields through the
  atomic projection/aspect-ratio setters;
- `DSplineComponent.SplineCurve` and its nested point/setting paths through the
  spline component setters, including curve-cache rebuilds;
- `DStaticMeshComponent.StaticMesh` through `SetStaticMesh()`;
- `DStaticMeshComponent.Material` through `SetMaterial()`;
- `DStaticMeshComponent.Materials[index]` through `SetMaterial(index, value)`; and
- `DMaterialInstance.Parent` through `SetParent()`, including cycle rejection.

Transactions retain the selected adapter for later Undo/Redo, so registered
adapters must have process lifetime. Registration lookup supports base classes;
later and more-derived registrations take precedence. Registrations are keyed by
the object-owned member, allowing a deliberate container adapter to interpret a
nested path while retaining the member snapshot as its stable mutation root.

## Edit Session Lifecycle

`FReflectedPropertyEditSession` implements one logical edit:

```text
Begin(target)
  resolve adapter
  capture original value
  root the target object

Apply(proposal)
  apply through the adapter
  recapture the actual applied value
  notify Interactive when it changed

Commit()
  ignore a no-op
  notify Committed
  mark the package dirty
  CommitApplied() one before/after transaction

Cancel()
  restore the original value through the same adapter
  notify Cancelled
  create no transaction
```

Checkboxes, selections, and structural buttons normally begin and commit in one
frame. Drag, text, color, vector, and transform widgets keep the session active
until deactivation. Repeated `Apply()` calls therefore produce preview events
but only one transaction. Escape restores the original value.

Destroying a session cancels an applied preview as a safety fallback. Hosts
should still explicitly commit or cancel when selection, document, read-only
state, or workspace activity changes so errors can be reported deliberately.

## Transactions and Dirty State

`FReflectedPropertyTransaction` retains the target, before/after snapshots,
description, and adapter. Undo restores the before snapshot; Redo restores the
after snapshot. Both use the adapter and deliver the same object notification
with the corresponding origin.

The session calls `FEditorTransactionManager::CommitApplied()` because the live
interactive edit has already placed the object in its final state. A no-op or
cancelled edit creates no history entry and does not dirty the package.

Dirty-state restoration is intentionally separate from value restoration. The
current policy conservatively leaves a package dirty after Undo. Clearing dirty
state requires a future saved-revision model that can compare the transaction
head with the last saved revision.

## Container Transactions

Arrays distinguish element assignment, add, remove, and resize. Maps distinguish
value assignment, insertion, removal, and key rename. Structural operations
capture enough before/after state to restore removed or resized-away values.

Map key rename rejects collisions before notification. After any structural
operation, the view stops traversing the old container for that frame because
the operation may have invalidated child addresses or changed iteration order.

`FReflectedPropertyView` also provides controlled string-key map helpers used by
Material Editor:

- `SubmitStringMapValueEdit()` edits or creates one logical map value while
  retaining value-set semantics; and
- `SetStringMapEntryEnabled()` records override insertion/removal as structural
  transactions.

## Reflected Property View

`FReflectedPropertyView` is an embeddable immediate-mode view, not a dockable
panel or standalone asset editor. A host stores one instance and supplies a
context containing:

- the externally owned transaction manager;
- an error-reporting callback; and
- read-only state.

`EditObject()` is the default object-level API. It enumerates inherited `Edit`
properties, expands fixed arrays, applies caller-provided search and filtering,
generates default labels, and can either own its property table or compose rows
inside a table owned by the host. It returns the visible row count and whether
any row changed so a composing host can provide its own empty state.

`EditProperty()` remains the controlled composition API. It renders and edits
one top-level reflected property and recursively handles supported structs,
arrays, and maps.
`EditPropertyValue()` and the container-recursion helpers are private so callers
cannot construct unsafe container addresses or edit paths.

Custom widgets can use `SubmitPropertyValueEdit()` to retain their presentation
while sharing proposal capture, session lifecycle, notifications, and history.
Camera and Spline customizations use it for their semantic layouts and actions;
Material Editor uses it for its parent picker and uses the string-map helpers
for parameter values and override presence. Object-details customizations are
given the host-owned view and context so composed rows share the same active
session and transaction history as ordinary Details rows.

`HandleOwnerContext()` cancels an active edit if the object presented by the
view is replaced or the view becomes read-only. The presented owner is tracked
separately from the mutation target so an object-level view can compose real
properties from another object, such as an Actor editing its RootComponent.
`FinishActiveEdit()` lets a host deliberately commit or cancel when a document
or workspace changes.

## Current Host Integration

Level Editor Details owns one property view. It still owns object selection,
the search input state, class display, customization dispatch, and a shared
property table used to compose domain rows. Ordinary reflected-property
enumeration, search matching, fixed-array expansion, and labels are delegated
to `EditObject()`; Actor transform, static-mesh material slots, and registered
customizations continue to compose through the same view.

Material Editor owns another property view but supplies a semantic parameter
layout. It retains inherited-value and override presentation, color and range
controls, and asset-specific pickers. Generic proposals, sessions, and
transactions are delegated to the view.

Spline Details now routes transform, curve settings, point values, and point
structural actions through the shared view while retaining its custom layout.
Camera Details similarly routes FOV, clip planes, aspect-ratio mode, and custom
ratio through a stable `ProjectionSettings -> Leaf` path. Its adapter snapshots
the whole settings structure so clamping one field can safely update another
and Cancel or Undo restores the complete projection state.

## Validation

Automated coverage currently verifies:

- preview/commit/cancel event phases;
- one transaction for a continuous interaction;
- no history for no-op, cancelled, or rejected edits;
- object and snapshot reference lifetime;
- array and map stable paths and structural restoration;
- semantic adapter rejection and Undo/Redo behavior;
- material parameter render invalidation; and
- material override insertion through shared transaction history; and
- spline continuous edits, Cancel, structural edits, stable nested paths, cache
  rebuilds, setter clamping, and Undo/Redo; and
- camera continuous edits, atomic cross-field clamping, Cancel, stable nested
  paths, aspect-ratio edits, and Undo/Redo; and
- object-level `Edit` enumeration, filtering, fixed-array expansion, search,
  and default fixed-array labels.

UI behavior still requires editor smoke and manual interaction coverage for
selection changes, document changes, read-only PIE state, save, and shutdown.

## Related Code

```text
Engine/Source/Runtime/CoreDObject/Public/DObject/PropertyChange.h
Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h
Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h
Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyEditing.h
Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyView.h
Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h
Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp
Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp
```
