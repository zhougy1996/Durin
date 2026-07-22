# Reflected Property Editing Plan

Last reviewed: 2026-07-22

The implemented architecture is documented in
[Reflected Property Editing](../Architecture/ReflectedPropertyEditing.md).
Future API design considerations are recorded in
[Reflected Property View Evolution](../Reference/ReflectedPropertyViewEvolution.md).

This file tracks only remaining work. It must not duplicate the implemented
event, snapshot, mutation-adapter, session, transaction, or view contracts.

## Remaining UI Migration

- [x] Route the actor root-transform row through `FReflectedPropertyView` while
  preserving `SetRelativeTransform()` semantics.
- [x] Route static-mesh material-slot rows through shared transactions while
  preserving slot labels and `SetMaterial()` behavior.
- [x] Migrate spline customization value and structural edits through shared
  reflected-property sessions while preserving spline setter/cache semantics.
- [x] Migrate the remaining direct reflected-property writes in Level Editor
  customizations.
- [ ] Ensure every host deliberately commits or cancels an active interaction
  when selection, document, workspace activity, or read-only state changes.

## Object-Level View

- [x] Add `FReflectedPropertyView::EditObject()` so hosts do not manually
  enumerate ordinary `Edit` properties.
- [x] Move default labels, static-array expansion, search, filtering, and
  optional property-table ownership behind that API.
- [x] Keep `EditProperty()` public as the controlled customization/composition
  entry point.
- [x] Make `EditPropertyValue()` and raw container-recursion helpers private.

## Customization and Binding

- [x] Define a minimal object property-view customization registry and builder.
- [x] Express Actor root transform and static-mesh material slots as
  customizations instead of type branches in Details.
- [x] Add a stable reflected-property binding abstraction for logical container
  values without exposing leaf addresses or manual paths.
- [x] Replace the public string-map transition helpers with bindings once the
  binding contract is proven.
- [x] Convert Material Editor parameters to descriptors plus bound property rows
  while retaining inherited values, overrides, ranges, colors, and asset pickers.

## Optional Generated Metadata

- [ ] Evaluate generated property-specific callback or customization metadata
  only after object customization and binding usage has stabilized.
- [ ] Resolve callbacks to validated function pointers during generation; do
  not perform per-edit string lookup.
- [ ] Route generated callbacks through the common event/mutation pipeline so
  phase, path, container, Undo, and Redo semantics remain identical.

## Validation Gaps

- [ ] Add direct coverage for scalar, enum, string, object, math-structure, and
  nested fixed-array event paths through the property view.
- [ ] Verify rejected assignments do not mutate, dirty, notify, or enter history.
- [ ] Verify selection/document changes during an active interaction follow the
  documented commit/cancel policy.
- [ ] Smoke-test Details editing, save, Undo/Redo, PIE read-only behavior, asset
  document switching, and shutdown in `DurinEditor`.

## Recommended Order

1. Add `EditObject()` and migrate Details enumeration.
2. Introduce object customization for composite rows. (Complete)
3. Introduce stable property bindings and simplify Material Editor.
4. Close validation gaps.
5. Re-evaluate generated metadata.
