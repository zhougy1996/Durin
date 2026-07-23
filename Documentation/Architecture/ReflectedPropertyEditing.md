# Reflected Property Editing

This document describes the reflected-property editing system currently
implemented by `CoreDObject` and `DurinEd`. It records adopted behavior only;
unselected UI API extensions remain outside the repository until they become
an issue or an executable plan.

## Scope

The system covers editor-originated changes to reflected `DObject` properties.
It provides:

- synchronous object change notifications;
- stable member-to-leaf property paths;
- focused property snapshots with object-reference lifetime protection;
- interactive preview, commit, and cancel semantics;
- one Undo/Redo entry for one continuous interaction;
- array and map value and structural transactions;
- detached pre-apply validation and normalization for generic mutations;
- object hook implementations for properties with runtime invariants; and
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
        +--> generic detached draft + DObject::PreEditChangeProperty()
        |      or a registered exceptional mutation adapter
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

## Object View Customization

Level Editor extends the shared property view through
`FLevelEditorCustomizationRegistry` and `FObjectPropertyViewBuilder`. The
registry resolves every registered class in base-to-derived order, allowing a
derived object to compose its own rows with inherited customizations.

Each `IObjectDetailsCustomization` describes a layout for the current frame. It
may add a real reflected property from the inspected object or another object,
add a custom row callback, hide a default property, or replace all default
rows. The builder owns search matching for those declarations; Details then
draws the builder rows and appends ordinary properties through `EditObject()`
in the same property table.

Current built-in composition includes:

- Actor `Transform`, bound to the root component's `RelativeTransform`;
- Camera projection rows; and
- Spline transform, curve settings, and point rows.

`DStaticMeshComponent.Materials` uses the ordinary reflected array editor. Its
object hooks validate the detached array and reconcile material dependencies,
the serialized slot-zero mirror, and render state after generic writes.

Details owns only the inspected object, search input, table, and customization
dispatch. It contains no Actor or static-mesh type branches. Real property rows
still enter `EditProperty()`, while custom controls submit through the same
session and mutation pipeline.

## Object Notification Contract

`DObject::PreEditChangeProperty(FPropertyEditProposal&, std::string&)` is the
generic path's synchronous validation and normalization hook. The proposal
contains the complete mutable detached snapshot root, a resolved draft leaf,
and the same member, leaf, path, phase, kind, and origin dimensions used by the
post event. Returning false rejects the candidate before live storage changes.
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

Objects use the post hook to refresh derived runtime state. Scene transforms
refresh their hierarchy, splines rebuild curve caches, static-mesh components
reconcile material dependencies and render state, and materials invalidate
dependent render data. Camera projection and material-parent validation happen
on the detached proposal before the generic write.

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
- the stable snapshot property and container;
- the owned member-to-leaf path; and
- the mutation kind.

`ForMember()`, `ForStructMember()`, `ForArrayElement()`, and `ForMapEntry()`
build targets without making panels reconstruct path rules themselves. Leaf
addresses are construction-time conveniences only; active sessions and
transactions retain stable roots and paths and re-resolve ephemeral storage.

## Generic Mutation and Exceptional Adapters

The generic path reads and writes the stable reflected snapshot root. It
captures live state, restores the candidate into an internal detached draft,
invokes the pre hook, captures the normalized draft, writes live storage once,
and recaptures the actual value. A failed write or recapture attempts rollback
and emits no post event. Same-target nested edits from a hook are rejected.

All current built-in semantic properties use this generic path. Transform
quaternion normalization, camera cross-field clamping, spline step clamping,
material type checks, and material-parent cycle rejection live in pre hooks.
Hierarchy/cache/render/dependency reactions live in post hooks. Static-mesh
material and material-instance parent dependencies keep derived mirrors so a
post hook can compare the newly written canonical storage with the previously
registered dependencies without a setter-plus-direct-storage dual write.

The process-lifetime adapter registry remains temporarily available as an
exception mechanism and for compatibility tests, but has no built-in
registrations. Transactional edits may use an exceptional adapter only through
validated class/member registration. Transactions retain stable target identity
and snapshots, then resolve the adapter again when Undo or Redo executes. Lookup
walks the edited object's class hierarchy from most derived to base, so
selection is independent of registration order.

## Edit Session Lifecycle

`FReflectedPropertyEditSession` implements one logical edit:

```text
Begin(target)
  resolve adapter
  capture original value
  root the target object

Apply(proposal)
  validate/normalize a detached draft on the generic path
  atomically apply and recapture the actual value, or use an exceptional adapter
  notify Interactive when it changed

Commit()
  notify Committed
  if changed, mark the package dirty and CommitApplied() one transaction

Cancel()
  if changed, restore through the same generic hook path or selected adapter
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

## Transactions and Dirty State

`FReflectedPropertyTransaction` retains the stable target, before/after
snapshots, and description. It resolves the generic implementation or registered
exceptional adapter when Undo or Redo executes. Both operations use the same
atomic execution and rollback path as interactive edits and deliver the same
post notification with the corresponding origin.

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
Map insertion is drafted before mutation: key and supported leaf-value widgets
edit temporary proposals, and `MapInsert` is submitted only when the user
confirms the new entry. A default-key collision therefore does not require
inserting and then renaming a live entry.

`FReflectedPropertyBinding` represents a logical container value without retaining
its current leaf address. The current factory binds a string-key map value by
capturing its reflected key snapshot and stable event-path bytes. `IsPresent()`
and every edit re-scan the live map, so a resize or rehash between frames cannot
invalidate the binding.

`SubmitBoundPropertyValueEdit()` gives custom UI only a temporary value container,
then submits the resulting member snapshot with value-set semantics.
`SetBoundPropertyEnabled()` records insertion/removal as structural transactions.
Panels therefore neither retain nor mutate a live map leaf address and never
construct reflected paths themselves.

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

Leaf widgets are separated from submission. A widget reads the displayed value
into ordinary temporary state and returns a detached assignment proposal that
contains no object or edit target. Ordinary properties apply that proposal only
to an internal property-draft leaf before submitting the stable member snapshot. Map
key widgets apply the same proposal to a temporary key and then submit an
explicit `MapKeyRename`; they never write key storage inside the live map.

Custom widgets can use `SubmitPropertyValueEdit()` to retain their presentation
while sharing proposal capture, session lifecycle, notifications, and history.
Proposal callbacks receive a leaf resolved inside generated draft storage;
they never mutate the live object while constructing the proposed snapshot. Runtime
setters, cache rebuilds, and other semantic side effects therefore run only through
the object hooks when the proposal is applied.
Camera and Spline customizations use it for their semantic layouts and actions;
Material Editor uses it for its parent picker and uses stable string-map bindings
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
to `EditObject()`; Actor transform and registered customizations continue to
compose through the same view. Static-mesh materials remain in ordinary
reflected-property enumeration.

Material Editor owns another property view but supplies a semantic parameter
layout. One descriptor table defines parameter name, label, reflected value type,
presentation, defaults, and ranges. A shared dispatch maps those descriptors to
scalar drag, color, or texture-asset rows and derives the corresponding base or
override map from the value type. All parameter values and override presence use
stable bindings, while inherited-value lookup and the specialized `ColorEdit3`
and asset-picker controls remain host-owned. Generic proposals, sessions, and
transactions are delegated to the view.

Spline Details now routes transform, curve settings, point values, and point
structural actions through the shared view while retaining its custom layout.
Camera Details similarly routes FOV, clip planes, aspect-ratio mode, and custom
ratio through a stable `ProjectionSettings -> Leaf` path. The draft snapshots
the whole settings structure so its pre hook can clamp one field and safely
update another; Cancel and Undo restore the complete projection state.

## Validation

Automated coverage currently verifies:

- preview/commit/cancel event phases;
- terminal events for no-op and edit-away-and-back interactions;
- one transaction for a continuous interaction;
- no history for no-op, cancelled, or rejected edits;
- exceptional-adapter partial-apply rollback and cancel retry;
- execution-time transaction adapter resolution;
- object and snapshot reference lifetime;
- array and map stable paths and structural restoration;
- binding re-resolution after map rehash and presence across Undo/Redo;
- generic semantic-hook rejection, normalization, reactions, and Undo/Redo;
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
Engine/Source/Editor/MaterialEditor/Private/MaterialParameterDescriptors.h
Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp
```
