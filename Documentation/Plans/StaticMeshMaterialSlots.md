# Static Mesh Material Slots Plan

Last reviewed: 2026-07-24

## Current Status

Stages 0 and 1 are complete. The source-slot and compatibility contracts are frozen,
generated importer fixtures cover reorder, rename, duplicate names, addition,
removal, filtering, and exact-name behavior, and characterization tests capture
the version-zero component representation, load ordering, section mapping,
fallback, and live material dependency binding. Static meshes now serialize
ordered material-slot definitions with stable GUID identity, exact source-name
metadata, source indices, and optional default materials. Import and source
rebuild reconcile unambiguous names and conservative index-only renames, while
duplicate-name ambiguity receives new identities and diagnostics. Render data
copies the reconciled order and keeps compact section indices. Version-zero
meshes deterministically derive identities and remain dirty for resave.
`DStaticMeshComponent::GetNumMaterials()` reports the mesh-derived slot count,
but the component nevertheless serializes an ordinary
editable material array, permits growth beyond the mesh slot count, and binds
overrides by array index. Details therefore exposes Add/Remove/Resize controls,
and a source reimport that changes slot ordering can silently assign an
override to the wrong surface.

This plan replaces the index-shaped component array with persistent
mesh-owned slot identities, sparse component overrides, and a fixed-row Details
customization. It is a focused follow-up to the material parameter domain
refactor; it does not change material parameter or shader behavior.

## Goal

Make the imported static mesh authoritative for material-slot identity, names,
order, and visible count, while allowing a component to override the material
assigned to any current slot without exposing container structure editing.

The completed workflow must:

- preserve an override across save/load and unambiguous source reimport
  reordering;
- prevent component APIs, reflection edits, Undo, and Redo from creating
  duplicate or invalid current-slot overrides;
- show exactly one material row per current mesh slot in Details, with no
  Add/Remove/Resize controls;
- resolve each rendered slot through component override, mesh default, then
  renderer fallback;
- retain unmatched overrides as explicit orphans instead of silently applying
  them to another slot; and
- load the existing index-based component representation through a defined
  one-time migration path.

## Scope

- A reflected, serialized material-slot descriptor collection owned by
  `DStaticMesh`.
- Stable per-slot `FGuid` identity, display name, import matching metadata, and
  optional default material.
- Reconciliation of imported slots with previously serialized descriptors.
- A reflected sparse override collection owned by `DStaticMeshComponent`.
- Slot-ID-based runtime lookup, mutation, dependency binding, serialization,
  scene-proxy construction, and material update routing.
- Compatibility conversion from the current `Material`/`Materials` component
  fields.
- A `DStaticMeshComponent` Details customization that draws model-shaped slot
  rows through the shared reflected-property transaction system.
- Orphan visibility and explicit removal in Details.
- Unit, asset round-trip, editor transaction, render-proxy, import/reimport,
  full-build, and hidden-window editor validation.
- Architecture and plan-index updates after the behavior lands.

## Non-Goals

- Importing or generating material assets from source-model materials.
- A general static-mesh asset editor, mesh section editor, or LOD authoring
  workflow.
- User-created, deleted, or reordered mesh material slots.
- Per-section component overrides; sections continue to select a mesh slot.
- Changing material parameters, shader maps, pipeline state, or the surface
  shading model.
- Making dynamic-array length dependencies a general reflection feature.
- Automatically guessing a match when reimport metadata is ambiguous.
- Removing renderer-owned fallback material data or fallback textures.

## Design Decisions and Invariants

### Ownership and representation

- `DStaticMesh` is the persistent owner of slot definitions. Render data is
  rebuilt from source and is not authoritative because it is not serialized.
- A reflected slot definition contains:
  - a valid, unique `FGuid SlotId`;
  - a non-`None`, mesh-local unique display `FName Name`;
  - source matching metadata sufficient to compare an imported slot with a
    previous definition;
  - the latest source material index for diagnostics and runtime build
    mapping; and
  - an optional `TObjectPtr<DMaterialInterface> DefaultMaterial`.
- Imported order determines the current slot order. `FStaticMeshSection`
  continues to carry a compact `MaterialSlotIndex`; render traversal never
  performs GUID lookup.
- `DStaticMeshComponent` stores an ordered reflected collection of
  `{SlotId, Material}` overrides. Absence means inheritance. A null assignment
  removes the override rather than storing a null entry.
- The component override collection is serialized and transactional but is not
  exposed through ordinary `DPROPERTY(Edit)` array enumeration.

### Identity and reimport matching

- Slot GUID is the only persistent identity used by component overrides.
  Display name, source index, and vector position are not persistent override
  identities.
- A new imported mesh receives one new GUID per slot. Existing serialized
  definitions retain their GUID when reconciliation finds one unambiguous
  source match.
- Reconciliation runs in deterministic passes from strongest to weakest source
  evidence. The initial implementation must document the exact key produced by
  `AssetCore`; with the current importer this includes the unique imported
  material name and source material index.
- Unique source-name matches are preferred so a pure source-order change
  preserves identity. Source-index fallback is allowed only when it cannot
  conflict with an already matched name and the candidate is unambiguous.
- A rename combined with an order change is not guessed. It creates a new slot
  identity and leaves references to the removed identity orphaned.
- Matching consumes both old and new entries at most once. Duplicate or
  ambiguous candidates produce a diagnostic and new identities rather than
  silent many-to-one reuse.
- The canonical `AssetCore` source key is the pair of the exact source material
  name reported by Assimp and its zero-based index in the Assimp output scene.
  Source names are UTF-8 byte strings and receive no trimming, case folding, or
  Unicode normalization. An empty source name remains empty matching metadata
  even though its display name is `Material_<SourceMaterialIndex>`.
- Display-name uniqueness is presentation-only. Repeated non-empty source names
  are emitted in source order as `Name`, `Name_1`, `Name_2`, and so on, but the
  suffix is not part of source identity. Duplicate raw names are ambiguous for
  name matching and are excluded from source-index fallback; the duplicate
  group receives new identities rather than preserving assignments by position.
- `SourceMaterialIndex` is the index in the Assimp output scene before Durin's
  unused-slot filter. Durin does not compact it to the visible slot position,
  although an individual Assimp format importer may already have omitted or
  compacted unused source-file declarations before Durin receives the scene.
- Legacy static meshes that have no serialized definitions use a deterministic
  mesh-local identity derived from their imported source slot key for the
  initial conversion. This prevents independently saving a migrated component
  from depending on a random GUID that changes until the mesh is also saved.
  Once serialized, the GUID never derives again and survives asset moves.

### Resolution and failure behavior

- Material resolution is:

  ```text
  component override for SlotId
      -> static-mesh slot DefaultMaterial
      -> empty FMaterialRenderData / renderer fallback
  ```

- `SetMaterialBySlotId()` rejects an invalid GUID or a GUID not present in the
  component's current static mesh. An index convenience API may remain, but it
  must bounds-check and immediately translate the current index to a slot GUID.
- Loading may retain an override whose GUID is absent from the current mesh.
  Such an entry is an orphan: it is serialized and visible for removal, but is
  never resolved, bound as a live material dependency, or sent to the scene
  proxy.
- A valid current override collection contains at most one entry per current
  slot GUID and a non-null `DMaterialInterface` value of the correct type.
  Detached reflected proposals reject invalid GUIDs, duplicates, and wrong
  object types before live storage changes.
- Changing `StaticMesh` does not destructively erase unmatched overrides.
  Current overrides are rebound; unmatched entries become orphans and can
  become current again if the previous mesh is restored.
- Default-material dependency changes and component-override dependency
  changes must invalidate the same affected proxy slot without rebuilding
  static-mesh geometry.

### Editor and reflection boundary

- This feature does not add a cross-property dynamic-length concept to Core
  reflection. Slot count and labels are static-mesh domain semantics.
- Level Editor registers an `IObjectDetailsCustomization` for
  `DStaticMeshComponent`, hides the raw override collection, and emits one
  fixed row for every current mesh slot.
- Each current row shows index, slot name, resolved material/source, a material
  picker, and Reset when a component override exists. Rows cannot be added,
  removed, resized, or reordered.
- Orphans appear in a separate warning group with their GUID, last-known label
  when available, assigned material, and an explicit Remove action. Orphans
  are never mixed into the current slot count.
- A row edit snapshots the reflected override collection root, locates the
  target by GUID inside detached draft storage, and uses the GUID as logical
  transaction identity. It therefore shares the normal validation,
  notification, Dirty, Cancel, Undo, and Redo path.
- If no static mesh is assigned, Details shows an empty explanatory state and
  the orphan group, not an editable placeholder array.

### Compatibility and versioning

- Add explicit serialized data-version fields for mesh slot definitions and
  component overrides; an empty override collection is a valid authored state
  and cannot itself indicate whether migration ran.
- Version zero means that the version field is absent. The first production
  schema constants are `StaticMeshMaterialSlotsVersion = 1` and
  `StaticMeshMaterialOverridesVersion = 1`.
- Deterministic version-zero mesh GUID derivation hashes a domain separator,
  the mesh package path, the exact source material name bytes, and the original
  source material index in fixed little-endian form. Display names, filtered
  slot positions, source-file absolute paths, and object memory identity are
  excluded. Hash output is converted to `FGuid`; an all-zero result is replaced
  by the fixed non-zero value documented beside the implementation.
- Version-zero static meshes build source data first, derive deterministic
  initial slot definitions, and mark their package dirty for resave.
- Version-zero components convert the legacy `Materials` array by current slot
  index after their referenced mesh has valid definitions. The legacy
  slot-zero `Material` mirror supplies index zero only when the array lacks it.
  Entries beyond the current mesh slot count become migration orphans with
  explicit diagnostics; they are not assigned to a current slot.
- Transitional legacy fields remain serialized but non-editable until
  migration tests and project fixtures prove the conversion. New saves write
  the new version and clear legacy storage.
- Removing the legacy fields is a separate final-stage gate. If repository
  compatibility policy requires keeping them longer, the plan may complete
  with them read-only and deprecated only after Architecture records that
  boundary.

### Thread and ordering boundary

- Import reconciliation, reflected mutation, dependency reconciliation, and
  material resolution occur on the object/game thread.
- The render thread receives only compact slot-indexed
  `FMaterialRenderUpdate`/`FMaterialRenderData` snapshots. It does not receive
  slot GUIDs, reflected slot definitions, or material objects.
- Component revision remains the ordering authority across updates to
  different slots. A mesh assignment or slot-layout change rebuilds the scene
  proxy; a resolved material-data change updates the existing proxy in place.

## Current Foundations and Gaps

### Foundations

- `AssetCore` imports a filtered, ordered `FImportedMaterialSlot` collection
  and assigns source material indices to imported meshes.
- `DStaticMesh::BuildRenderData()` creates material slots and maps sections to
  compact indices.
- `DStaticMeshComponent` already resolves the visible count from mesh render
  data and produces one material snapshot per render slot.
- Material assets already track bound components and propagate live
  render-data changes.
- Reflected property editing already supports stable collection-root
  snapshots, detached validation, custom proposal submission, logical
  identities, and shared Undo/Redo.
- Level Editor already supports inherited object Details customizations,
  hidden default properties, and custom rows.

### Gaps

- Mesh slot metadata currently exists only in non-serialized render data.
- Slot identity is an array index plus a source index; neither protects
  component assignments from reimport reordering.
- The component `Materials` array can outgrow or disagree with the mesh.
- `SetStaticMesh()` does not reconcile material bindings or orphan state.
- Generic Details exposes structural array controls and numeric element labels
  instead of mesh slot names.
- The legacy slot-zero `Material` mirror and the current array create two
  persisted representations.
- There is no mesh default-material ownership or three-level resolution rule.
- Tests cover multi-slot snapshots and generic reflected array editing, but
  not stable reimport identity, sparse overrides, orphan behavior, or a
  fixed-row Details surface.

## Implementation Stages

### Stage 0: Freeze the Slot and Compatibility Contracts

- [x] Specify the canonical source-slot matching key exported by `AssetCore`,
  including normalization, duplicate-name disambiguation, and source-index
  semantics.
- [x] Add checked-in or generated test fixtures covering two named material
  slots, source-order reversal, source rename, duplicate names, addition, and
  removal.
- [x] Capture a version-zero static-mesh/component round-trip fixture using the
  current `Material` and `Materials` fields.
- [x] Add characterization tests for current section-to-slot mapping, fallback
  behavior, package load ordering, and material dependency binding.
- [x] Record the selected data-version constants and deterministic legacy GUID
  derivation inputs in the plan before production storage changes begin.

The reconciliation fixtures establish these expected identity results:

| Source change | Expected identity result |
| --- | --- |
| Initial `Red`, `Blue` import | Allocate one new persistent GUID for each slot. |
| Reverse uniquely named `Red`, `Blue` | Preserve both GUIDs by unique raw source name and reverse their runtime order. |
| Rename `Red` to `Crimson` without moving it | Preserve the GUID by unambiguous source-index fallback. |
| Rename and reorder the same slot | Preserve any independently unique unchanged-name match; allocate a new GUID for the renamed unmatched slot. |
| Add `Green` | Preserve existing uniquely named GUIDs and allocate one new GUID for `Green`. |
| Remove `Red` | Preserve `Blue`; references to the removed `Red` GUID become orphans. |
| Duplicate raw source names | Do not use display suffixes or source-index fallback for the duplicate group; emit an ambiguity diagnostic and allocate new GUIDs. |

#### Acceptance Gate

- The test inputs distinguish reorder, rename, addition, removal, and
  ambiguity, and the expected identity result for each case is explicit.
- A legacy fixture can be loaded by the pre-change code and is suitable for
  proving migration after the new fields land.

### Stage 1: Add Persistent Mesh-Owned Slot Definitions

- [x] Introduce the reflected static-mesh slot-definition structure and a
  serialized ordered collection on `DStaticMesh`.
- [x] Add read-only lookup APIs by slot index, GUID, and name without exposing
  mutable collection storage to components or renderer code.
- [x] Extend `AssetCore` source metadata only as far as required by the Stage 0
  matching contract.
- [x] Reconcile imported slots against previous definitions before building
  render data, preserving GUIDs and default-material references for
  unambiguous matches.
- [x] Generate deterministic initial identities for version-zero assets and
  ordinary persistent identities for genuinely new unmatched slots.
- [x] Copy the reconciled order/name/source-index data into
  `FStaticMeshRenderData`; keep section mapping compact and validate every
  section index.
- [x] Define how transient/debug meshes create valid slot definitions without
  requiring package serialization.
- [x] Mark changed persistent slot metadata dirty only during import/reimport
  or migration, not on every normal `PostLoad()` rebuild.
- [x] Add tests for GUID validity/uniqueness, save/load stability, default
  material reference reachability, reorder preservation, conservative
  rename/order handling, additions, removals, duplicate names, and
  deterministic legacy conversion.

#### Acceptance Gate

- A saved mesh reloads with the same ordered slot GUIDs and defaults.
- Reversing two uniquely named source slots reverses runtime order but
  preserves each slot's GUID and default material.
- An ambiguous change never reuses one old GUID for multiple new slots and
  emits a diagnostic instead of silently guessing.
- All existing static-mesh import and render-resource tests pass.

### Stage 2: Replace Index Arrays with Sparse Component Overrides

- [ ] Introduce the reflected `{SlotId, Material}` override structure,
  collection, and component data version.
- [ ] Implement lookup and mutation by GUID, plus a bounds-checked index
  convenience API that translates through the current mesh definition.
- [ ] Implement current/orphan classification and the three-level material
  resolution rule.
- [ ] Update `SetStaticMesh()`, `PostLoad()`, `PreEditChangeProperty()`,
  `PostEditChangeProperty()`, `BeginDestroy()`, and dependency reconciliation
  around sparse current overrides.
- [ ] Bind only resolved current component overrides and mesh defaults needed
  by the component; orphan materials must not trigger render updates.
- [ ] Convert version-zero `Materials`/`Material` storage after mesh slot
  definitions are available, clear migrated legacy values on new saves, and
  produce diagnostics for excess entries.
- [ ] Ensure invalid GUIDs, duplicate current GUIDs, null entries, and
  incompatible objects are rejected consistently by API and detached
  reflected edits.
- [ ] Update garbage-collection reachability and asset dependency/reference
  reporting for mesh defaults, current overrides, and serialized orphans.
- [ ] Add tests for API bounds, sparse resolution, reset-to-default, mesh
  switching, orphan preservation/reactivation/removal, migration, load/save,
  copy/duplicate, Undo/Redo hooks, dependency binding, and destruction.

#### Acceptance Gate

- Component serialization contains no index-dependent new assignment data.
- Changing to a mesh with a different slot layout cannot apply an old override
  to an unrelated current slot.
- Existing component assets retain all in-range legacy assignments after
  migration, including the slot-zero mirror fallback.
- Current overrides update dependencies and render state; orphan-only material
  changes do not.

### Stage 3: Build the Fixed-Row Details Customization

- [ ] Add and register a `DStaticMeshComponent` Details customization in Level
  Editor, with module-lifetime registration/unregistration.
- [ ] Remove `Edit` exposure from the raw legacy/new collections and hide any
  transitional property that would otherwise be enumerated.
- [ ] Draw one searchable row per current mesh slot using imported order and
  names, with resolved source, override state, picker, and Reset action.
- [ ] Filter the asset picker to `DMaterialInterface` and report rejected
  references through the host Details error channel.
- [ ] Submit assignment and reset through the override collection root in
  detached draft storage; use `SlotId` as the logical transaction identity.
- [ ] Draw a separate orphan warning group with deterministic ordering and
  explicit Remove actions.
- [ ] Handle no mesh, unloaded/build-failed mesh, read-only/PIE, selection
  replacement, component removal, and mesh changes during an active edit.
- [ ] Add model/view tests for row construction, labels, search keywords,
  inheritance/override/orphan state, and material type filtering.
- [ ] Add editor transaction tests for assign, replace, reset, orphan removal,
  Cancel, Undo, Redo, package Dirty state, and edits to two different slot
  GUIDs sharing one collection root.

#### Acceptance Gate

- Details shows exactly the current mesh slot count and offers no structural
  container controls.
- Every edit follows shared validation and transaction behavior; no custom
  widget mutates live component storage directly.
- Search, read-only mode, selection changes, and Undo/Redo preserve consistent
  row and component state.

### Stage 4: Complete Render and Reimport Integration

- [ ] Build scene-proxy material snapshots by current mesh slot order while
  resolving component override, mesh default, and fallback.
- [ ] Route live material changes to every current slot that resolves to the
  changed material, including one material used by multiple slots.
- [ ] Rebuild the proxy when mesh assignment or reconciled slot layout changes;
  keep parameter-only material changes on the in-place render command path.
- [ ] Confirm component revisions order rapid updates across independent slots
  and that stale updates cannot overwrite newer resolved data.
- [ ] Add proxy tests for mesh defaults, component precedence, fallback,
  multiple slots sharing a material, reimport reorder, orphan exclusion, and
  rapid cross-slot updates.
- [ ] Add an asset/editor integration test covering import, assignment through
  the fixed-row model, save/reload, source reorder, reimport, and preserved
  rendered assignment.

#### Acceptance Gate

- Reimport reordering changes section/runtime slot indices as needed without
  changing which surface receives each preserved override.
- The render thread consumes only compact slot-indexed snapshots.
- Live updates affect all and only the slots whose resolved material changed.
- Focused Engine and editor tests pass.

### Stage 5: Retire Legacy Paths and Record the Architecture

- [ ] Search production code and tests for direct component `Material`/
  `Materials` storage access, unchecked index growth, generic-array Details
  assumptions, and render-side GUID lookup.
- [ ] Remove the legacy slot-zero mirror and index array when the compatibility
  gate permits; otherwise leave them private, deprecated, read-only migration
  inputs with an explicit removal condition.
- [ ] Remove obsolete generic-array material-slot tests and replace them with
  fixed-row and GUID-override coverage.
- [ ] Update `Documentation/Architecture/MaterialSystem.md` with mesh slot
  ownership, identity, reconciliation, resolution, orphan, dependency, and
  renderer-boundary rules.
- [ ] Update `Documentation/Architecture/ReflectedPropertyEditing.md` to
  replace the ordinary `Materials` array description with the Details
  customization and collection-root transaction behavior.
- [ ] Update `Documentation/Plans/MaterialSystem.md` to reference the landed
  slot architecture and remove any stale index-array claims.
- [ ] Run focused native tests, the complete affected test suites, the full
  `all` build, and the hidden-window `DurinEditor` smoke procedure documented
  by repository setup guidance.
- [ ] Record completion evidence, archive this plan, update active/archive
  indexes, and repair direct links.

#### Acceptance Gate

- No production editor surface exposes component material overrides as a
  structurally editable array.
- No current component assignment is persisted or resolved solely by vector
  position.
- Lasting behavior is documented in Architecture and all required validation
  passes from a clean task diff.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Import metadata | AssetCore tests cover unique names, duplicate names, source indices, reorder, rename, add, and remove |
| Mesh persistence | Engine tests cover GUID/default save-load stability and deterministic version-zero initialization |
| Reimport | Tests prove unambiguous preservation and conservative orphaning under ambiguous changes |
| Component API | Tests cover GUID lookup, index bounds, sparse assignment, reset, mesh switch, and duplicate rejection |
| Compatibility | A version-zero fixture migrates `Materials` and the slot-zero mirror without wrong-slot assignment |
| Reflection | Detached proposals reject invalid collections; Cancel/Undo/Redo restore the collection root |
| Details model | Rows equal current slots, use slot labels, expose no structural controls, and separate orphans |
| Dependencies | Current overrides/defaults bind and invalidate correctly; orphans do not participate |
| Rendering | Proxy snapshots respect precedence and slot order; live updates and stale-revision handling pass |
| GC/assets | Mesh defaults, current overrides, and retained orphans have defined reachability/reference reporting |
| Regression | Existing AssetCoreTests, EngineTests, reflected-property, material, world, and viewport coverage passes |
| End to end | Full `all` build succeeds and `DurinEditor --hidden-window` remains alive for the timed smoke interval |

All build, test, and smoke operations must use the root workflow documented in
`Documentation/Setup/BuildAndRun.md` and
`Documentation/Setup/NativeTests.md`.

## Definition of Done

- Static-mesh assets persist valid unique material-slot GUIDs and reconcile
  them deterministically with imported source slots.
- Components persist sparse GUID-keyed overrides and cannot create invalid
  current assignments through API, reflection, loading, Undo, or Redo.
- Reimport ordering changes do not silently move preserved assignments between
  surfaces; ambiguous and removed identities become explicit orphans.
- Material resolution follows component override, mesh default, then renderer
  fallback for every current slot.
- Details displays model-shaped fixed rows with assignment, reset, search,
  inherited source, and orphan removal, without generic array structure
  controls.
- Existing index-based assets follow the documented migration path.
- The render thread remains independent of reflected objects and slot GUIDs.
- Automated tests, the full build, and hidden-window editor smoke pass.
- Architecture documentation is updated and this plan is archived according to
  `Documentation/Plans/AGENTS.md`.

## Deferred Follow-ups

- A dedicated static-mesh asset editor for authoring default materials,
  inspecting sections/LODs, and manually resolving ambiguous reimport matches.
- Source-format-specific persistent material identifiers when import libraries
  expose stronger keys than name/index metadata.
- Bulk assignment, copy/paste, drag/drop, and multi-component editing in
  Details.
- User-authored mesh slot aliases or explicit remapping during reimport.
- Per-platform or per-LOD material overrides.
- A generic `NoStructuralEdit` reflected container presentation hint, if a
  second domain demonstrates the same UI-only need.
- Automatic source-material asset creation and import rules.

## Related Documentation

- `Documentation/Architecture/MaterialSystem.md`
- `Documentation/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Architecture/LevelSystem.md`
- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/Archive/2026-07/MaterialParameterDomainRefactor.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`

## Related Code

```text
Engine/Source/Runtime/AssetCore/Public/AssetCore.h
Engine/Source/Runtime/AssetCore/Private/AssetCore.cpp
Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h
Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h
Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp
Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h
Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp
Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h
Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp
Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h
Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp
Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyView.h
Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h
Engine/Source/Editor/LevelEditor/Private/Customizations/
Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp
Engine/Source/Programs/Tests/AssetCoreTests/Private/AssetImportTests.cpp
Engine/Source/Programs/Tests/EngineTests/Private/MaterialTests.cpp
Engine/Source/Programs/Tests/EngineTests/Private/ReflectedPropertyEditingTests.cpp
Engine/Source/Programs/Tests/EngineTests/Private/ViewportTests.cpp
```
