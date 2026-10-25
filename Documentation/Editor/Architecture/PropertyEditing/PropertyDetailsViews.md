# Property Details Views

Summary: Define shared reflected-property views, declarative Details layouts, custom controls, and host lifetime obligations.

Modules: DurinEd, MonaImGui, CoreDObject

Last reviewed: 2026-10-16

The [property editing pipeline](ReflectedPropertyEditing.md) owns detached
validation, notifications, sessions, and history submission. This document owns
presentation and composition; [Level Editor Details](../LevelEditing/LevelEditorDetails.md)
owns Actor/Component aggregation.

## View Ownership And Submission

`Editor::FPropertyView` is an embeddable immediate-mode view, not a dockable
panel or standalone asset editor. A host stores one instance and supplies a
context containing:

- transaction mode (recorded by default, or explicit NotTransactable);
- an error-reporting callback;
- optional asset reveal/open callbacks; and
- read-only state.

The context does not select a transaction service. A new interaction resolves
the active editor once; its session retains the exact scope until completion.
Unavailable recorded admission rejects the edit rather than silently disabling
history. CallInEditor actions require recorded mode. Custom spline commands use
the same admission policy as ordinary property writes.

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

The view boundary reports owned display messages for failed proposal capture
and submission. It preserves the original failure and stops before further
validation or snapshot work can replace it. Lower-level draft and transaction
APIs retain typed causes; presentation callers consume the formatted result.

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
Composed rows share the host-owned view and context, so custom and default
controls participate in the same active session and history.

`HandleOwnerContext()` cancels an active edit if the object presented by the
view is replaced or the view becomes read-only. The presented owner is tracked
separately from the mutation target so an object-level view can compose real
properties from another object, such as an Actor editing its RootComponent.
`FinishActiveEdit()` lets a host deliberately commit or cancel when a document
or workspace changes.

## Object View Customization

DurinEd provides the shared property view, `FDetailCustomizationRegistry`, and
`FDetailLayoutBuilder` under `PropertyEditor/` in the `Durin::Editor` namespace.
Asset editors and Level Editor use the same layout and registration contracts. The
registry resolves every registered class in base-to-derived order, allowing a
derived object to compose its own rows with inherited customizations.

Each `IDetailCustomization::CustomizeDetails(IDetailLayoutBuilder&)` describes a
layout for the current frame. `GetObject()` supplies the inspected object and
optional `GetHostContext()` supplies an `IDetailHostContext` extension. No host
context is required for ordinary asset layouts. The concrete
`FDetailLayoutBuilder` binds those before dispatch and does not permit a layout
to be rebound to a different owner. Customizations depend on the layout interface,
while the host owns its concrete renderer and transaction-aware property view. It
may add a real reflected property from the inspected object or another object,
add a custom row callback, hide a default property, or replace all default
rows. `AddDefaultProperties()` appends remaining reflected properties to the
same layout before drawing. Declared and default rows share category grouping,
sorting, and search, so a category is drawn once even when both contribute rows.

`EditCategory()` returns a stable `IDetailCategoryBuilder` for a named category.
Categories can contain reflected properties and custom rows, expose a display
label, tooltip, initial collapsed state, and sort order. Explicit property
declarations record placement by object, property, and fixed-array index.
Default enumeration omits only that exact row; other objects
and array indices remain eligible. Placement is independent of search and
category visibility, so a filtered or hidden declared row does not reappear as a
default row. `HideProperty()` remains an explicit whole-property default
suppression rule and never serves as implicit placement bookkeeping. Category
names and labels participate in search; matching categories expand during search.
`HideCategory()` suppresses both declared category rows and ordinary reflected
properties in that category. `RegisterDetailCustomization()` and
`FindDetailCustomizations()` retain base-to-derived composition and scoped
unregistration. `ApplyRegisteredCustomizations()` binds the inspected object and
dispatches the shared registry. `DrawDetailPanel()` is the shared host entrypoint:
it resolves owner context, dispatches registered customizations, applies an
optional host layout extension, appends filtered default rows to the layout,
and draws that layout in one property table. `ShouldDrawDefaultProperty()` applies
replacement, explicit hiding, category hiding, and exact placement rules.

The result reports owner readiness, table drawing, visible rows, and changes;
search, table identity, host context, and empty-state presentation are options.

A host may supply its own `IDetailHostContext` extension or no extension.
The extension must outlive its layout and row callbacks. Host-specific types
remain in their owning editor module; see [Level Editor Details](../LevelEditing/LevelEditorDetails.md).

Details owns only the inspected object, search input, table, and customization
dispatch. It contains no Actor or static-mesh type branches. Real property rows
still enter `EditProperty()`, while custom controls submit through the same
session and mutation pipeline.

The shared layout also hosts
[reflected editor function actions](EditorFunctionActions.md), which owns their
admission, execution ordering, and transaction contract.

The shared property view renders an ordinary reflected struct as a default-open
tree containing each nested field marked `Edit`. Nested rows retain the
object-owned outer member as their snapshot root and append a struct-member
path segment for notification and transaction identity. Math structs such as
vectors, quaternions, transforms, and linear colors keep their compact inline
widgets instead of expanding into member rows.

## Typed Metadata Presentation

Authoring syntax, applicability, and runtime metadata ownership are defined by
[Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md#typed-authoring-metadata).

The generic property view reads `FProperty::GetTypedMetadata()` for first-party
presentation. `DisplayName` replaces the humanized field label, `ToolTip` is
shown together with the reflected type, and `Category` groups visible fields
with a stable category-first ordering. Numeric rows use `Step` as drag speed,
`Precision` as their decimal format, `UIMin`/`UIMax` as presentation bounds,
and render the selected unit beside the value. Fixed-array elements retain
their index in the display label. Raw `DisplayName`, `ToolTip`, and `Category`
remain a compatibility view, but new code does not parse numeric behavior from
raw strings.

Hard-bound checks run against the complete detached draft before live mutation
or session capture. Their applicability and exact numeric behavior are owned by
[Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md#typed-authoring-metadata).

## Container Controls

After a structural operation, the view stops traversing the old container for that frame because
the operation may have invalidated child addresses or changed iteration order.
Map insertion is drafted before mutation: key and supported leaf-value widgets
edit temporary proposals, and `MapInsert` is submitted only when the user
confirms the new entry. A default-key collision therefore does not require
inserting and then renaming a live entry.

## Reference Controls

The generic weak-reference row reports null, live, expired, or type-mismatched
state and permits an explicit clear. It does not offer a durable asset picker;
weak-reference storage and retention follow [Property reflection](../../../Runtime/ObjectSystem/PropertyReflection.md#soft-and-weak-references).

`LoadSoftObject` returns `FPropertySoftLoadResult` with owned location, path,
and typed storage/accessor/asset failure context. Failure clears the output
object; the picker formats the result when reporting it. `InspectSoftObject`
does not load: its state and original/resolved paths describe redirection
independently of typed errors.

## Host Lifecycle

`DrawDetailPanel()` calls `HandleOwnerContext()` before building or drawing custom
properties. Hosts that draw identity or selection controls first must guard that
work as well. A failed terminal operation must be reported before abandoning the
active edit. The host owns document replacement, save/discard, and workspace
transitions; closing or hiding a panel does not transfer its edit to another view.

Editor modules unregister customization handles before their implementations unload. Optional host contexts and row
callbacks remain valid through layout traversal.

## Related Code

- `Engine/Source/Editor/DurinEd/Public/PropertyEditor/PropertyView.h`
- `Engine/Source/Editor/DurinEd/Public/PropertyEditor/DetailCustomization.h`
- `Engine/Source/Editor/DurinEd/Public/PropertyEditor/DetailLayoutBuilder.h`
- `Engine/Source/Editor/DurinEd/Public/PropertyEditor/DetailCustomizationRegistry.h`
