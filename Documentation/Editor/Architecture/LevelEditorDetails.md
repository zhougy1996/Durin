# Level Editor Details

Summary: Define Actor/Component inspection targets, component aggregation, generated-component admission, and material-slot editing.

Modules: LevelEditor, DurinEd, Engine

Last reviewed: 2026-10-16

## Host Ownership

Level Editor owns selection, search state, identity controls, and one shared
`FPropertyView`. Its `FLevelDetailLayoutBuilder` adapts the
[shared Details layout](PropertyDetailsViews.md#object-view-customization).
`FLevelDetailHostContext` supplies scene services and per-layout
`ComponentDetailsTargets`; these types remain in LevelEditor.

All reflected and custom rows submit edits through the host-owned view and
transactor. The [property editing pipeline](ReflectedPropertyEditing.md) owns
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
[Material system](../../Runtime/Rendering/MaterialSystem.md#renderer-boundary).

## Related Documentation

- [Spline authoring and stable point identity](../../Runtime/World/SplineSystem.md#level-editor-authoring)
- [Viewport editing](ViewportEditing.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/LevelDetailCustomization.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp`
