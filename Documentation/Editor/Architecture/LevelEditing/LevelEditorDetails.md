# Level Editor Details

Summary: Define Actor/Component inspection targets, component aggregation, generated-component admission, and material-slot editing.

Modules: LevelEditor, DurinEd, Engine

Last reviewed: 2026-11-22

## Host Ownership

Level Editor owns selection, search state, identity controls, and one shared
`FPropertyView`. Its `FLevelDetailLayoutBuilder` adapts the
[shared Details layout](../PropertyEditing/PropertyDetailsViews.md#object-view-customization).
`FLevelDetailHostContext` supplies scene services and per-layout
`ComponentDetailsTargets`; these types remain in LevelEditor.

All reflected and custom rows submit edits through the host-owned view and
transactor. The [property editing pipeline](../PropertyEditing/ReflectedPropertyEditing.md) owns
validation, snapshots, notifications, and history. Hosts handle owner changes
before drawing controls that might use the previous target.

## Actor And Component Targets

Ordinary Level Editor Actor selection targets the Actor itself, including
selecting an already-selected Actor again. The Actor page composes the root's
Transform and categorized editable authored-component properties. A single
component has no redundant name wrapper; multiple components retain named groups.
Actor details customizations can set the Level Editor host context's
`ComponentDetailsTargets` to select primary components or suppress aggregation
with an empty list. Without an explicit selection, Actors aggregate all authored
components. Foreign and duplicate targets are ignored. Transient default
diagnostics are omitted from aggregation; explicit custom diagnostics remain
available.
Component groups reuse registered component customizations, including material
slots and camera projection controls; their hidden and replaced properties remain
respected. Search filters component rows and expands matching groups; a matching
component name or class exposes the whole group. The root transform appears once.
Generated components are excluded from aggregation and remain accessible through
their read-only component pages. Aggregated edits target the actual component
through the host-owned property view and transaction history.
The component tree retains separate Actor and component targets. Explicitly
selecting a component displays that component, and selecting the Actor returns
to the aggregated page. A rootless Actor remains valid; selection never creates
a component. If an inspected component stops belonging to the current Actor,
Details falls back to the Actor. These target transitions continue through
the property view's owner-context handling so an active preview is committed or
restored before its owner changes.

## Component Structure History

The component tree routes component addition, deletion, duplication, renaming,
and reparenting through the host's `DEditorActorSubsystem` and application transactor.
Each operation joins one application scope, with package revision tracking and
preflight rejection before writes. Generated components, read-only targets,
and actors participating in play are rejected. Deletion and duplication require
instance membership; duplication also rejects the root component.

Removal unregisters and uninitializes the component, routes its destruction
notification, removes authored/owned/instance membership, and clears its root
role when applicable. Scene children detach with their world transforms intact.
The record retains the exact component and captured references; absent objects are transient and excluded from package capture.
Undo restores membership order, root role, parent/child attachments and relative
transforms before restoring the previous registration state. Older property
records therefore continue to resolve the same object. Discarding history lets
an absent component be collected normally.

The owner and component are captured before the membership write. The new
component already has the Instance creation method when captured. GC follows the recorded references. Outer-only children may expire. If a callback removes the component or retires its owner, the
subsystem reports that publication failed. Replay suppresses incidental package dirty marks
so rejected operations do not advance package revisions.

Duplication uses the ordinary object-graph duplication pipeline, including owned
subobject remapping and `PostLoad` derived-state rebuilding. Copied fields and
attachment state are prepared before the first creation/registration callbacks.
An unattached source stays unattached with its authored world transform. These
operations share the lifecycle-preserving `AActor::SetInstanceComponentPresence`
primitive; ordinary runtime destruction retains its terminal garbage semantics.

Rename uses the shared whole-object record and metadata replay. Initial name
allocation avoids all live siblings, including inactive components retained by
history. Replay retains exact identity and rejects a destination-name
collision before writing; it never substitutes another name during undo or redo.
Live native components, including the root, can be renamed; template objects
cannot. Name changes preserve root/ownership roles and render registration.
Ordinary historical fields may restore on a retained absent owner, without
reactivating it or rerunning new-edit admission. An unpublished Actor reconciles its restored instance list and native ownership while its tick and components remain inactive.

Reparenting opens a scope and captures the moved component before preserving its
world transform under the destination. Actor attachment captures the moved roots
after validating the batch. Undo restores the saved parent and relative transform
even after an external edit. Replay checks the destination graph for cycles and
reconciles reverse links during completion.
The Actor completion hook validates native instance membership after ordered replay.
No-op renames and reparenting create no history entry. Both commands participate
in package saved revisions without manual saved-state invalidation.

Actor Label uses transactional string storage and the ordinary
Modify/set/property-notification flow. Label-only edits refresh the Outliner
without reconstruction. Creation and duplication use `SetInitialActorLabel`
before publication; internal Name and display Label remain independent. See
[Level naming](../../../Runtime/World/LevelSystem.md) and the
[business-consumer history audit](../Transactions/Transactors.md#structural-history-and-barriers).

## Generated Components

Generated components are inspection-only derived state. Selecting one keeps its
tree row visible but presents a read-only Details view; rename, duplication,
deletion, reordering, reparenting, drag/drop, and direct property proposals are
disabled. Domain owners may expose a redirect action instead. `ASplineMeshActor`
uses `Edit Owning Spline`, returning selection and editing authority to its
persistent Spline root while the generated segment remains non-transactional
and non-serializable.

## Material Slots

The static-mesh customization hides the raw positional `OverrideMaterials`
collection. It emits exactly one fixed row per current mesh-owned slot, labeled
by stable index and user-facing slot name, with resolved source, a material-
interface picker, Reset when an override exists, and Clear All when any visible
or dormant entry is stored. Structural array controls and dormant rows are not
exposed; StaticMesh slot orphans do not exist.

Slot assignment, replacement, and reset snapshot the reflected collection root,
locate the positional entry in draft storage, and use the fixed-width slot index
as the logical transaction identity. Clear All edits the same root as a whole.
Storage validation, dormant entries, and canonical material binding are owned by
[Material system](../../../Runtime/Rendering/Materials/MaterialSystem.md#renderer-boundary).

## Related Documentation

- [Spline authoring and index selection](../../../Runtime/World/SplineSystem.md#level-editor-authoring)
- [Viewport editing](ViewportEditing.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/LevelDetailCustomization.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp`
