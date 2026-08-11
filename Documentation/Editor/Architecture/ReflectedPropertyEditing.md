# Reflected Property Editing

Summary: Define reflected property presentation, transactions, multi-object edits, validation, and undo.

Modules: DurinEd, MonaImGui, CoreDObject

This document defines the reflected-property editing system currently
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
  owns Editor::FPropertyView
  supplies Editor::FPropertyViewContext
        |
        v
Editor::FPropertyView
  owns transient widget state and one active edit session
        |
        v
Editor::FPropertyEditSession
  captures, applies, commits, or cancels one logical edit
        |
        +--> detached draft + DObject::PreEditChangeProperty()
        |
        +--> DObject::PostEditChangeProperty()
        |
        +--> externally owned Editor::FTransactionManager
                    |
                    v
          Editor::FPropertyTransaction
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
- Camera projection rows;
- Spline transform, curve settings, selected-point rows, and structural actions; and
- static-mesh material-slot rows derived from the assigned mesh.

The static-mesh customization hides the raw positional `OverrideMaterials`
collection. It emits exactly one fixed row per current mesh-owned slot, labeled
by stable index and user-facing slot name, with resolved source, a material-
interface picker, Reset when an override exists, and Clear All when any visible
or dormant entry is stored. Structural array controls and dormant rows are not
exposed; StaticMesh slot orphans do not exist.

Slot assignment, replacement, and reset snapshot the reflected collection root,
locate the positional entry in draft storage, and use the fixed-width slot index
as the logical transaction identity. Clear All edits the same root as a whole.
The component pre hook validates the bounded array, null holes, and compatible
non-null objects before live storage changes. Its post hook trims trailing nulls
and rebuilds render state from canonical mesh and override storage for edit,
Cancel, Undo, and Redo through the same shared path.

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
refresh their hierarchy, splines publish immutable evaluation snapshots, static-mesh components
rebuild render state from current assignments, and materials run a batched
loaded-object scan to invalidate dependent render data. Camera projection and
material-parent validation happen on the detached proposal before the generic
write.

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

## Generic Mutation

The generic path reads and writes the stable reflected snapshot root. It
captures live state, restores the candidate into an internal detached draft,
invokes the pre hook, captures the normalized draft, writes live storage once,
and recaptures the actual value. A failed write or recapture attempts rollback
and emits no post event. Same-target nested edits from a hook are rejected.

All current built-in semantic properties use this generic path. Transform
quaternion normalization, camera cross-field clamping, spline authoring repair,
material type checks, and material-parent cycle rejection live in pre hooks.
Hierarchy, cache, and render reactions live in post hooks. Material-instance
Parent and static-mesh component assignments are read directly from the newly
written canonical storage: their post hooks need neither the previous
relationship value nor a registered-value mirror. Material inheritance
invalidation computes the affected loaded closure from current Parent chains;
component invalidation resolves current mesh defaults and overrides.

There is no public mutation-adapter or registry surface. No implemented
property requires an exceptional mutation policy: every canonical value is
reflected object storage and every current semantic rule fits detached
validation plus post-write reaction. A future externally owned canonical value
must first demonstrate that this split is impossible before a new policy
contract is introduced.

## Edit Session Lifecycle

`Editor::FPropertyEditSession` implements one logical edit:

```text
Begin(target)
  capture original value
  root the target object

Apply(proposal)
  validate/normalize a detached draft
  atomically apply and recapture the actual value
  notify Interactive when it changed

Commit()
  notify Committed
  if changed, CommitApplied() one transaction with its affected package

Cancel()
  if changed, restore through the same generic hook path
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

`Editor::FPropertyTransaction` retains the stable target, before/after
snapshots, and description. Undo and Redo use the same atomic generic execution
and rollback path as interactive edits and deliver the same post notification
with the corresponding origin.

The session calls `Editor::FTransactionManager::CommitApplied()` because the live
interactive edit has already placed the object in its final state. A no-op or
cancelled edit creates no history entry and does not dirty the package.

Each transaction reports a stable, deduplicated set of affected asset packages.
`Editor::FTransactionManager` owns editor-session revision metadata for those
packages: the current revision, the last successfully saved revision, and
whether that save checkpoint remains trustworthy. Committing a changed edit
allocates a fresh after-revision. Undo moves to the stored before-revision only
after value restoration succeeds, and Redo moves back to the after-revision
only after reapplication succeeds.

For a valid checkpoint, the manager synchronizes the existing
`DPackage::IsDirty()` compatibility boundary from revision equality. Undoing
exactly to the saved revision clears dirty; Redoing away sets it again. Saving
at any history position advances the saved revision only after package save
succeeds, without clearing valid Redo history. A new edit after Undo receives a
new revision identity, so a saved state on the discarded branch cannot alias
the replacement branch.

Persistent edits that bypass transaction history must invalidate the package
checkpoint as well as mark the package dirty. An invalid checkpoint remains
conservatively dirty across Undo and Redo until a successful save or known-clean
package activation establishes a new checkpoint. No-op and cancelled edits
create no transaction or revision and retain their pre-interaction dirty state.
Revision metadata belongs to the editor transaction manager, is not serialized
into `DPackage`, and is forgotten with the document/history lifecycle.

Package revision and dirty-state tracking does not imply mounted-content
discovery invalidation. Reflected edits, including component transforms and
Spline control-point edits, mutate in-memory package state and do not advance
the transaction manager's mounted-content mutation revision. Consequently their
Execute, Undo, and Redo transitions never request an asset-registry scan.

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

## Reflected Property View

`Editor::FPropertyView` is an embeddable immediate-mode view, not a dockable
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
Camera and Spline customizations use it for their semantic layouts and actions.
Material Editor uses it for its parent picker and delegates parameter collection
edits to its schema-driven panel model. Object-details customizations are given
the host-owned view and context so composed rows share the same active session
and transaction history as ordinary Details rows.

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
compose through the same view. Static-mesh material assignments are a domain
customization: the mesh controls visible row count and labels, the property-table
UI presents those slots through shared non-resizable array helpers, and the
shared asset picker reserves a persistent trailing reset action. Edits still
submit the stable reflected override root through the host property view.

When the primary Level Editor Actor changes, Details targets its RootComponent
by default when one exists; a rootless Actor targets the Actor itself. This is a
presentation choice and never creates a component. The component tree retains
separate Actor and component targets, so explicitly selecting the Actor keeps
Actor details and Actor-specific customizations visible until another target or
primary Actor is selected. If an inspected component stops belonging to the
current Actor, Details applies the same RootComponent-or-Actor default. These
target transitions continue through the property view's owner-context handling
so an active preview is committed or restored before its owner changes.

Material Editor owns another property view and a reusable parameter-panel model.
Runtime Engine definitions provide parameter type, labels, ordering,
presentation, ranges, and texture hints. The model snapshots each definition,
resolved value, supplying ancestor, override state, and orphan state into rows.
Edits snapshot the reflected definition or override collection root, then locate
the target entry by GUID in detached draft storage. A parameter GUID is also the
logical transaction identity, so two parameters sharing one collection root
cannot coalesce into the same continuous edit. The specialized scalar, color,
and asset-picker controls remain host-owned; proposal submission, sessions, and
transactions remain delegated to the shared reflected-property infrastructure.

Spline Details now routes transform, loop state, selected-point values, and
point structural actions through the shared view while retaining its custom
layout. Point identity is a stable GUID shared with viewport selection. Numeric
point and tangent edits resolve that GUID against detached spline authoring
storage. Structural append, insert, duplicate, reorder, delete, and loop
operations snapshot the complete authored point collection and loop state so
one action produces one transaction and one coherent evaluation publication.
Viewport point/tangent drags use the generic transform-target transaction
lifecycle, including one history entry for completion and none for Cancel or a
net-zero drag; they do not create an independent transaction system.
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
- package dirty synchronization at initial and middle-history save points across
  Undo and Redo;
- fresh revision identity after history branching, bounded-history eviction,
  invalid checkpoint recovery, failed operations, and multi-package
  transitions;
- Transform Gizmo completed, cancelled, and net-zero interactions; and
- level-document save success/failure, activation, replacement, and discard
  checkpoint handoff;
- pre-apply rejection and retryable cancel failure through object hooks;
- object and snapshot reference lifetime;
- array and map stable paths and structural restoration;
- generic semantic-hook rejection, normalization, reactions, and Undo/Redo;
- material parameter render invalidation;
- fixed static-mesh slot rows, index-scoped root transactions, Reset/Clear All,
  dormant-entry hiding, search, read-only behavior, and material type filtering;
  and
- GUID-resolved material definition and override edits, override
  insertion/removal, orphan removal, and shared transaction history; and
- spline continuous edits, point/tangent target Cancel, structural edits,
  stable GUID resolution, immutable snapshot publication, and Undo/Redo; and
- camera continuous edits, atomic cross-field clamping, Cancel, stable nested
  paths, aspect-ratio edits, and Undo/Redo; and
- object-level `Edit` enumeration, filtering, fixed-array expansion, search,
  and default fixed-array labels.

Host-level automation covers object and document replacement plus read-only
transitions. Interactive checks cover workspace close, PIE, and editor shutdown,
and hidden-window editor startup is part of final validation.

## Related Code

```text
Engine/Source/Runtime/CoreDObject/Public/DObject/PropertyChange.h
Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h
Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h
Engine/Source/Editor/DurinEd/Public/Editor/PropertyEditing.h
Engine/Source/Editor/DurinEd/Public/Editor/PropertyView.h
Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h
Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp
Engine/Source/Editor/LevelEditor/Public/LevelEditorTransformTargets.h
Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp
Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialParameterPanelModel.h
Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp
```
