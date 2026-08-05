# Static Mesh Indexed Material Overrides Plan

Summary: Replace StaticMesh slot GUIDs and orphan overrides with UE-style imported-name reconciliation, stable slot indices, and positional component material overrides.

Last reviewed: 2026-08-05

Status: Completed
Completed: 2026-08-05

## Current Status

All stages are complete from final-stage baseline
`7a03b28085736ad42fae85d38014d72425896589`. `DStaticMeshComponent` now owns a
hidden positional `OverrideMaterials` array, mesh switching preserves that
array, and the material-slot Details customization exposes only current
index-based rows plus Reset and Clear All. The recreated `NewLevel.dasset`
matches the frozen logical manifest with no override, and the repository's only
user material package has been deleted with no compatibility path.

Mesh slots retain stable positional indices through
reimport without GUIDs, section construction consumes the explicit imported-to-
stable map, and DMSH schema 3 stores only a bounded material-slot count. Box,
Sphere, and Teapot were recreated from their checked-in OBJ sources. The final
integration test covers Details assignment through save/reload, reordered
reimport, section remapping, and rendered override retention. Lasting Runtime,
Editor Architecture, roadmap, and Default Material plan assumptions now own the
indexed contract. Repository-wide symbol/package audits, the complete native
aggregate, full `all` build, and hidden-window editor smoke all pass.

The current implementation is the completed outcome of the archived
[Static Mesh Material Slots Plan](Archive/2026-07/StaticMeshMaterialSlots.md):
`DStaticMesh` persists one GUID per material slot, `DStaticMeshComponent`
persists sparse `{SlotId, Material}` overrides, a mesh switch turns unmatched
overrides into explicit orphans, and Details exposes those orphans by GUID.
Reimport still has to identify old and new source slots through exact
`SourceName` and conservative `SourceMaterialIndex` matching before it can
preserve a GUID. The GUID is therefore a downstream reference key, not source
evidence supplied by OBJ, glTF, or Assimp.

The selected replacement follows the simpler UE-style boundary: imported
metadata keeps a mesh's positional slot table stable as far as the source
allows, while a component stores material overrides by slot index. Switching
to another mesh preserves the override array and immediately applies entries
at indices shared by the new mesh. Slot display names remain separate from
exact imported names so user renames do not destroy reimport evidence.

This is an intentional authored-baseline break. The repository contains one
user material asset, `Sandbox/Content/Materials/NewMaterial.dasset`, referenced
by the Box component in `Sandbox/Content/Levels/NewLevel.dasset`. The material
will be removed, the level's logical actor/component state will be captured
before the schema cut, and a fresh `NewLevel.dasset` will be authored at the
same path under the new component schema without that override. The three
Engine StaticMesh packages will likewise be rebuilt from their checked-in OBJ
sources. No legacy slot-GUID field, component override field, loader alias,
upgrade branch, or package migration path will remain.

The active [Default Material and Error Fallback Plan](DefaultMaterialAndErrorFallback.md)
overlaps `DStaticMeshComponent`, the material-slot Details customization, and
their tests. This plan should land before that plan's StaticMesh integration
stage, or that stage must rebase onto the indexed APIs established here. The
assignment precedence itself remains compatible with that plan.

## Goal

Make material-slot behavior predictable when users switch StaticMesh assets,
while retaining sufficient imported metadata to keep slot indices stable
across ordinary reimport.

The completed system must:

- let a component override material slot `N` and keep that override at index
  `N` when its StaticMesh changes;
- preserve matched slot indices through source reorder and unambiguous source
  rename without persisting a slot GUID;
- separate a user-facing, renameable slot `Name` from exact `SourceName` and
  `SourceMaterialIndex` reconciliation metadata;
- retain component override, mesh default, then renderer/default-material
  resolution precedence;
- remove StaticMesh slot-orphan storage, runtime behavior, validation, tests,
  and editor presentation;
- keep render-thread consumption compact and index-only; and
- replace all affected checked-in assets with the new authored schema without
  production compatibility code.

## Scope

- `FStaticMeshMaterialSlotDefinition` ownership, naming, imported metadata,
  default material, ordering, and reimport reconciliation.
- Stable automatic slot-index allocation across reorder, rename, addition,
  removal, duplicate source names, and reappearance.
- Removal of `SlotId` from authored StaticMesh state, runtime render data,
  derived data, cooked payloads, public APIs, and tests.
- A reflected positional `OverrideMaterials` collection owned by
  `DStaticMeshComponent`, with null entries representing inheritance.
- Material lookup, assignment, reset, clearing, duplication, serialization,
  reflection validation, render binding, and mesh-switch behavior by index.
- Name-based convenience lookup and assignment that resolve a unique current
  slot name to its index before using the positional API.
- The StaticMeshComponent fixed-row Details customization and transaction
  identity conversion from GUID to slot index.
- DMSH payload and derived-data-key version changes required when slot GUID
  payloads become a slot count.
- Focused unit, reimport, asset, Details, transaction, rendering, DDC, Cook,
  and end-to-end validation.
- Removal/recreation/rebuild of the five affected repository packages and
  updates to lasting Runtime, Editor Architecture, and roadmap documentation.

## Non-Goals

- Removing material-parameter GUIDs from `DMaterial` or `DMaterialInstance`.
- Changing material-instance parameter-orphan behavior; only StaticMesh slot
  orphans are removed.
- Loading, converting, or resaving third-party packages authored with the old
  StaticMesh slot/component schemas.
- Adding reflected aliases, deprecated fields, custom versions, upgrade code,
  or a general asset migration framework.
- Embedding stable Durin identifiers into OBJ/glTF files or writing sidecar
  metadata beside source models.
- Guaranteeing semantic preservation when both imported name and source index
  change in one ambiguous edit.
- A dedicated StaticMesh asset editor, section editor, or material-slot
  compaction UI.
- Automatically compacting unused historical slot indices; compaction would
  renumber positional component data and requires a separately selected
  project-wide operation.
- Changing material parameters, shaders, render passes, fallback appearance,
  texture behavior, or material proxy publication.
- Importing or generating material assets from source-model material records.

## Design Decisions and Invariants

### Mesh-owned slot representation

- `DStaticMesh` remains the authoritative owner of one ordered reflected
  `MaterialSlots` collection. Each `FStaticMeshMaterialSlotDefinition`
  contains only:
  - a non-`None`, mesh-local unique `FName Name` used by users and gameplay;
  - the exact importer-provided `std::string SourceName` used only for source
    reconciliation and diagnostics;
  - the original `uint32 SourceMaterialIndex` used as the positional fallback;
    and
  - an optional `TObjectPtr<DMaterialInterface> DefaultMaterial`.
- `SlotId`, `MaterialSlotsVersion`, GUID lookup, GUID validation, and GUID
  serialization are removed rather than deprecated.
- `Name` and `SourceName` are separate identities. Initial import gives a new
  slot a unique display `Name` derived from the imported material name. A
  matched reimport updates `SourceName` and `SourceMaterialIndex` but preserves
  the existing `Name`, so a user rename survives reimport.
- A slot rename API validates non-`None` and mesh-local uniqueness, marks the
  package dirty, and updates editor-facing metadata without changing section
  indices, defaults, component overrides, or source matching data.
- `FindMaterialSlot(FName)` and `GetMaterialIndex(FName)` remain name-facing
  conveniences. They are not persistent component identities.

### Stable positional reimport reconciliation

- Initial import allocates slots in filtered imported order. After that,
  automatic reimport preserves existing indices rather than adopting a new
  imported order.
- Reconciliation consumes each old slot and new imported slot at most once in
  deterministic passes:
  1. match an exact `SourceName` only when that raw name is unique in both the
     old table and the new imported set;
  2. match remaining entries by an equal, unique `SourceMaterialIndex`, which
     covers a source rename and provides positional handling for duplicate or
     empty names; and
  3. append unmatched new slots in imported order without reusing an existing
     index.
- A matched imported slot updates the source metadata at the old index while
  preserving its user `Name`, `DefaultMaterial`, and index.
- An unmatched old slot remains in `MaterialSlots` as an unused reserved
  position. It is not referenced by rebuilt sections, but it retains its name,
  last imported metadata, and default material so existing positional
  component overrides never silently move to another surface. A later
  unambiguous reappearance may reactivate the same index.
- New slots append after every existing reserved position. Reimport never
  fills a removed slot with unrelated source material merely to reduce the
  array size.
- The builder produces an explicit imported-slot-to-stable-slot map and uses
  that map when assigning each section's compact `MaterialSlotIndex`. It must
  not rediscover the mapping by searching the final table for a source index,
  because retained unused slots can carry historical source indices.
- Duplicate exact source names receive unique display names but do not use
  those presentation suffixes as source identity. The source-index pass is
  the selected best-effort identity for those entries.
- If a rename and reorder remove both usable clues, the old slot remains
  reserved and the new imported material appends. The system accepts an unused
  row rather than silently transferring defaults or component overrides.
- Automatic reimport therefore makes slot indices monotonically stable. An
  explicit future compaction operation may renumber them only with its own
  warning, package policy, and validation plan.

### Component override representation and mesh switching

- `DStaticMeshComponent` persists one reflected
  `std::vector<TObjectPtr<DMaterialInterface>> OverrideMaterials` collection.
  Vector index is the complete persistent override identity.
- A null entry means no component override and resolves through the mesh
  default. Trailing null entries are trimmed after reset and before save;
  interior null entries are retained to preserve later indices.
- `SetMaterial(SlotIndex, Material)` rejects an index not present in the
  current mesh. A non-null assignment expands the vector through `SlotIndex`
  with null holes. A null assignment resets that index and trims trailing nulls.
- `GetMaterialOverride(SlotIndex)`, `HasMaterialOverride(SlotIndex)`, and
  `ResetMaterial(SlotIndex)` are index-based. `SetMaterialByName` and
  `GetMaterialByName` first resolve a unique current `Name` to an index.
- `ClearMaterialOverrides()` empties the complete collection, including entries
  inactive under the current mesh.
- `SetStaticMesh()` preserves `OverrideMaterials` unchanged. Every non-null
  entry whose index exists in the new mesh immediately applies to that slot.
  Entries beyond the new mesh's current slot count are dormant, serialized,
  excluded from scene-proxy construction, and become active again if a later
  mesh exposes that index. They are positional state, not orphans, and carry no
  old-mesh identity.
- The Details panel shows only current mesh slots and provides an explicit
  Clear All Overrides action when any positional override is stored. It does
  not expose dormant entries as GUID warnings or offer structural array edits.
- The component collection is valid when its length is within the shared
  material-slot bound and every non-null object is a `DMaterialInterface`.
  Duplicate-identity and invalid-GUID validation disappear.
- Old `FStaticMeshMaterialOverride`, `MaterialOverrides`, slot-ID methods, and
  orphan methods are deleted. No empty legacy field remains to suggest support.

### Resolution, rendering, and ordering

- Material resolution remains:

  ```text
  OverrideMaterials[SlotIndex] when non-null
      -> MaterialSlots[SlotIndex].DefaultMaterial
      -> the selected Engine/default renderer path
  ```

- `GetNumMaterials()` remains the current mesh material-slot count, including
  reserved unused slots. Section draw calls reference only active indices.
- Scene-proxy construction walks render-data slot order and requests the
  resolved component material by the same index. Dormant component entries
  beyond the mesh count never create material proxies or dependencies.
- A component material edit continues to publish a binding update for one
  `PendingMaterialSlotIndex`; rapid changes retain the existing component
  revision and stale-update rejection rules.
- A mesh assignment or render-data rebuild recreates component render state.
  Material parameter publication and component binding updates retain their
  current thread boundaries.
- Import, reconciliation, reflected mutation, naming, and asset rebuild occur
  on the object/game thread. The render thread receives no GUID, source name,
  reflected slot definition, or material object.

### Details and transaction semantics

- The Level Editor keeps the fixed-row StaticMeshComponent customization and
  hides raw `OverrideMaterials` storage.
- Each row is identified by `uint32 SlotIndex`, labeled `[Index] Name`, and
  reports component override, mesh default, or renderer/Engine default source.
- Assignment and reset snapshot the reflected `OverrideMaterials` root and use
  the fixed-width serialized slot index as `LogicalIdentity`. Undo/Redo for two
  indices remains independent even though both edits target one collection.
- Detached draft edits resize with null holes, replace one entry, reset one
  entry, or clear the array. No widget mutates live component storage directly.
- `EStaticMeshMaterialSource::Orphan`, `bOrphan`, orphan entry collections,
  GUID search keywords, warning groups, and Remove Orphan actions are deleted.
- A mesh change during an active row edit finishes or cancels the edit through
  the existing reflected-property lifecycle before the Details model rebuilds.

### Derived data and cooked payload

- `FStaticMeshMaterialSlot` in render data drops `SlotId`; its compact array
  position is the only section/material binding identity.
- `FStaticMeshPayloadData::MaterialSlotIds` becomes a bounded
  `MaterialSlotCount`. The required DMSH MaterialSlots chunk stores that count,
  allowing standalone payload validation to bounds-check every section index
  without carrying authoring identities.
- Increment `StaticMeshPayloadSchemaVersion` from 2 to 3 and
  `StaticMeshBuilderVersion` from 1 to 2. The derived-data-key byte schema does
  not change unless its encoding changes independently; the new builder and
  payload versions are already key inputs.
- Source and cooked reconstruction require the payload slot count to equal the
  current package `MaterialSlots.size()`, then restore runtime-only names and
  source indices from the package definitions by index.
- Old DMSH payloads and old `.dasset` field signatures are unsupported. They
  are rejected by existing schema/compatibility boundaries, not converted.

### Authored asset baseline and compatibility

- No compatibility implementation is authorized. The code must not retain
  `SlotId`, `FStaticMeshMaterialOverride`, the old reflected property name,
  version-zero handling, custom upgrade code, or a loader alias.
- The DAST writer serializes every non-transient reflected property, including
  an empty array, so clearing the old Box override cannot remove the old
  `MaterialOverrides` field signature from `NewLevel.dasset`. The old level
  package must not be loaded after that reflected property is removed.
- Before the component schema changes, capture a deterministic logical manifest
  of `NewLevel` sufficient to reproduce its actor classes, names, transforms,
  ownership/attachment, StaticMesh references, camera selection, SkyBox state,
  and other authored values. Reset the Box component material in the captured
  state and exclude `/Game/Materials/NewMaterial`.
- In the same bounded schema-cut change, delete
  `Sandbox/Content/Materials/NewMaterial.dasset` and replace
  `Sandbox/Content/Levels/NewLevel.dasset` with a freshly created package at the
  same virtual path under the new component schema. This is current content
  authoring, not an old-package loader or migration path; no stage may leave a
  checked-in level with a dangling material dependency.
- When the mesh slot struct changes, recreate these packages from their
  checked-in OBJ sources at the same virtual asset paths:
  - `Engine/Content/Models/Box.dasset`;
  - `Engine/Content/Models/Sphere.dasset`; and
  - `Engine/Content/Models/Teapot.dasset`.
- Recreate `Sandbox/Content/Levels/NewLevel.dasset` from the captured logical
  manifest under the final reflected component schema and confirm
  `/Engine/Models/Box` and the unaffected SkyBox/texture references resolve.
- The environment-lighting and texture packages are outside the changed
  reflected structures and remain untouched unless the compatibility audit
  produces concrete contrary evidence.
- The final project compatibility audit must classify every checked-in package
  as current. Any external old package remains explicitly incompatible.

## Current Foundations and Gaps

### Foundations

- Import already supplies exact `SourceName`, original
  `SourceMaterialIndex`, filtered slot order, and section source-material
  references.
- `DStaticMesh` already persists ordered slot definitions with separate display
  and source names plus mesh-owned defaults.
- Current reimport code already performs unique source-name matching followed
  by a conservative source-index fallback.
- Sections and renderer code already consume compact material-slot indices;
  GUID lookup is confined to asset/component/editor preparation.
- Component scene-proxy creation and material binding updates already operate
  by `SlotIndex` after resolving the GUID-backed storage.
- The fixed-row Details customization already owns material-only filtering,
  source presentation, reflected root transactions, Dirty state, and Undo/Redo.
- DMSH uses an isolated required MaterialSlots chunk and versions its schema and
  builder in the derived-data key.

### Gaps

- Reimport currently outputs imported order, so removing component GUIDs
  without changing reconciliation would move overrides between surfaces.
- The final slot table does not preserve removed indices and section mapping is
  rediscovered from `SourceMaterialIndex`, which is insufficient once unused
  historical slots remain reserved.
- `Name` is overwritten from imported data on every preserved match and has no
  explicit rename contract independent of `SourceName`.
- Component storage, validation, public API, reflected edits, and Details rows
  are all structured around GUID-keyed sparse overrides and explicit orphans.
- Render data, DMSH payloads, DDC validation, package schema, and many focused
  tests still carry slot GUIDs even though render traversal is index-based.
- The checked-in Box level contains the only user material reference and old
  component override representation. Because empty reflected arrays are still
  serialized, it must be recreated rather than merely cleared and resaved; all
  three Engine model packages contain the old slot struct signature as well.
- The active Default Material plan assumes the same component and Details
  working set and must not be implemented concurrently in this checkout.

## Stage 0 Evidence and Handoff

### Frozen reimport expectations

| Source change | Stable-table result | Section result |
| --- | --- | --- |
| Initial import | Allocate slots in filtered imported order | Each section uses the corresponding new index |
| Reorder with unique names | Match by exact `SourceName`; preserve all old indices | Explicit imported-to-stable map follows the reordered surfaces |
| Rename at the same unique source index | Match in the source-index pass; preserve user `Name`, default, and index; update source metadata | Renamed surface keeps its old stable index |
| Add | Append unmatched imported slots after every existing/reserved slot | New sections reference appended indices |
| Remove | Retain the unmatched old position as unused and never renumber later slots | No rebuilt section references the reserved slot |
| Unambiguous reappearance | Match the retained source evidence and reactivate the prior position | Reappearing sections reuse that stable index |
| Duplicate or empty names | Skip name identity and use unique source-index evidence | Presentation suffixes never participate in mapping |
| Rename plus reorder with neither clue | Retain the old position and append the new imported slot | The new surface uses the appended index; no override/default transfers silently |

Every match consumes one old and one imported entry. New entries never fill an
unrelated reserved position, and section construction consumes the explicit
map produced during reconciliation rather than searching the final table.

### Frozen component and Details expectations

| Operation | Expected positional result |
| --- | --- |
| Assign current slot `N` | Reject an out-of-range index; otherwise expand with null holes and store at `N` |
| Reset slot `N` | Store null at `N`, then trim only trailing nulls |
| Switch to a mesh sharing index `N` | Preserve the array and apply the same non-null entry at `N` immediately |
| Switch to a smaller mesh | Keep higher entries serialized but dormant; do not publish them to the scene proxy |
| Switch to `nullptr` | Preserve every entry dormant; report zero current materials |
| Switch later to a larger mesh | Reactivate every non-null entry whose index is current again |
| Clear All | Empty visible and dormant entries together |
| Lookup by name | Resolve only a unique current user-facing `Name`, then use its index |
| Details assign/reset | Snapshot the one reflected array root and use fixed-width `SlotIndex` logical identity |
| Details Cancel/Undo/Redo | Restore the complete positional snapshot while keeping edits at different indices independent |

The post-cut package must serialize `OverrideMaterials` and must not serialize
component `MaterialOverrides`. StaticMesh packages remain on the old `SlotId`
schema until Stage 2, when they must serialize neither `SlotId` nor
`MaterialSlotsVersion`. No old name remains as an empty compatibility field.

### Captured `NewLevel` reconstruction manifest

`FNewLevelBaselineTests.CapturesAuthoredLogicalManifestBeforeSchemaCut`
loads the checked-in package through `/Game/`, asserts this manifest, and keeps
component material state deliberately outside the reconstruction contract:

| Actor order/name | Class and root component | Authored state retained by reconstruction |
| --- | --- | --- |
| 0 `DirectionalLightActor` | `ADirectionalLightActor` / `DirectionalLightComponent` | Rotation `(w=0.91030458659254632, x=0, y=0, z=-0.41393907719442657)`; translation `(0, 2.1259323682006928, 0.86950246609238957)`; unit scale; white color; intensity `1`; ambient `0.08` |
| 1 `CameraActor` | `ACameraActor` / `DCameraComponent` | Primary camera; identity rotation; translation `(-1.6669377762075972, 0, 0)`; unit scale; FOV `60`; near `0.1`; far `1000`; viewport aspect mode; custom ratio value `16/9` |
| 2 `SkyBoxActor` | `ASkyBoxActor` / `SkyBoxComponent` | Identity transform; `/Game/Textures/TEXCUBE_PureSky_512x512`; white tint; intensity `1`; scene ID `8dcc0ca9-e5c5-42c7-91e2-03716ab9ec56` |
| 3 `Box` | `AStaticMeshActor` / `DStaticMeshComponent` | Identity transform; `/Engine/Models/Box`; no reconstructed component material override |

All actors are visible, unattached, own exactly their one default root
component, and have no instance components. The old package additionally has
exactly one GUID override (`2323d1ae-605b-48d4-90d6-987eda2df6ff`) referencing
`/Game/Materials/NewMaterial`; that evidence is asserted only to prove what
Stage 1 removes.

### Asset and coordination audit

- `git ls-files '*.dasset'` identifies eight checked-in packages. The only
  material package is `Sandbox/Content/Materials/NewMaterial.dasset`.
- A tracked-package byte search finds `/Game/Materials/NewMaterial` only in
  `Sandbox/Content/Levels/NewLevel.dasset`.
- The pre-cut JSON compatibility audit classified all eight packages as
  `Ready`, `Compatible`, and `Current`. The tool's existing `--fail-on` argparse
  path raises `AttributeError: 'tuple' object has no attribute 'append'` before
  scanning, so Stage 0 used the successful no-policy JSON result and recorded
  the limitation rather than expanding this plan into tooling repair.
- The focused level load and manifest assertion pass. Loading emits only the
  known stopped-render-command diagnostics because WorldTests owns no live
  renderer; the loaded logical state is complete.
- WorldTests now deploys its required `RHI` dependency explicitly, avoiding a
  stale test-bin DLL when the checked-in level loads render-resource asset
  classes.
- The Default Material plan remains at its initial status with no completed
  stage or overlapping implementation diff.

Stage 1 may rely on the table and manifest above without loading the old level
after removing `MaterialOverrides`. Its atomic asset step must create the fresh
level first in detached/test output, validate this same logical contract with
an empty positional override collection, then install the new package and
delete `NewMaterial.dasset` in one bounded change.

## Implementation Stages

### Stage 0: Freeze Indexed Semantics and Clean the Authored Asset Input

Dependencies: baseline `f4fa25353c4c55264b0128a01b6fac30c02703c7` and
the user's explicit decision to discard the only user material asset instead
of maintaining compatibility code.

- [x] Record focused before-change expectations for mesh switching, slot
  reorder, rename, add, remove, duplicate names, component resolution,
  Details transactions, and serialized field presence.
- [x] Freeze the stable-index reconciliation table from this plan, including
  reserved removed slots, append-only new indices, and explicit section-map
  production.
- [x] Freeze dormant override behavior across smaller/larger/no-mesh switches,
  trailing-null trimming, Clear All semantics, and name lookup uniqueness.
- [x] Capture a deterministic manifest of `NewLevel` with the current code,
  including actor/component topology and authored values, and produce a
  reconstruction fixture or test that excludes the Box material override.
- [x] Confirm `NewMaterial.dasset` is the only user material package and
  `NewLevel.dasset` is its only checked-in dependency. Schedule its deletion
  atomically with the Stage 1 replacement level; do not leave the Stage 0
  checkout with a dangling dependency.
- [x] Run the repository package compatibility audit and targeted level load
  before any reflected schema removal; do not proceed until the logical
  replacement has enough evidence to preserve the intended level without
  loading the old package after the cut.
- [x] Record whether the Default Material plan has begun implementation. If it
  has, rebase this plan's working set and tests before Stage 1; do not merge two
  independent component/Details edits.

#### Acceptance Gate

- The desired behavior matrix has one expected result for every reorder,
  rename, addition, removal, duplicate-name, and cross-mesh case.
- The captured `NewLevel` manifest contains no `NewMaterial` dependency or
  authored GUID override, preserves every other intended actor/component value,
  and the existing checkout remains internally valid until the atomic Stage 1
  asset replacement.
- No compatibility or migration requirement remains open for the production
  code stages.

### Stage 1: Cut the Component and Details Surface Over to Positional Overrides

Dependencies: Stage 0.

- [x] Replace `FStaticMeshMaterialOverride` and `MaterialOverrides` with the
  reflected positional `OverrideMaterials` collection; add bounds, type,
  null-hole, length, trailing-trim, and Clear All behavior.
- [x] Remove every slot-ID component API and implement index/name assignment,
  lookup, reset, and clear APIs with the resolution precedence defined above.
- [x] Preserve the override collection through `SetStaticMesh()` and prove
  shared indices apply immediately while out-of-range entries remain dormant.
- [x] Update `PostLoad`, reflected pre/post edit hooks, duplication, package
  Dirty state, render-state invalidation, and pending binding updates for the
  new property and validation rules.
- [x] Convert the Details model and widget to index entries, index-scoped root
  transactions, current rows, Reset, and Clear All; remove all StaticMesh slot
  orphan UI and search terms.
- [x] Update component, material rendering, material-instance interaction,
  schema/editing, and Details tests without changing material-parameter orphan
  coverage.
- [x] Create a fresh `NewLevel` package at the original virtual path from the
  Stage 0 manifest under the new component schema; compare its logical scene
  snapshot with the captured reference and run package field checks proving the
  old component symbols are gone.
- [x] Delete `NewMaterial.dasset` in the same change that installs the fresh
  level package, and prove no checked-in dependency or package byte string still
  names `/Game/Materials/NewMaterial`.

#### Acceptance Gate

- A one-slot override survives `MeshA -> MeshB -> nullptr -> MeshA` and always
  applies at index zero when that index exists.
- A higher override becomes dormant on a smaller mesh, never reaches the scene
  proxy, and reactivates at the same index on a later larger mesh; Clear All
  removes both visible and dormant entries.
- Assignment, reset, Cancel, Undo, Redo, duplication, save/reload, and Dirty
  state operate through one hidden positional collection.
- No production `DStaticMeshComponent` or Level Editor material-slot symbol
  mentions slot GUIDs or StaticMesh slot orphans.
- Focused component, Details, material binding, world, and rendering tests pass.

#### Stage 1 Handoff

- Baseline: `d1392ff4b1b20094d05094f47e3ef6ae98bbd7c2`.
- Working set: component positional storage/APIs, material-slot Details,
  material/rendering/world regression tests, `NewLevel.dasset`, and removal of
  `NewMaterial.dasset`.
- Decision: overrides are preserved strictly by array index; null resets a
  position, trailing nulls are trimmed, out-of-range entries remain dormant,
  and Clear All removes visible and dormant entries.
- Asset result: the recreated level is 9,836 bytes, contains
  `OverrideMaterials`, contains neither the old component field nor
  `/Game/Materials/NewMaterial`, and all seven remaining packages audit as
  ready, compatible, and current.
- Validation: `MaterialTests` 66/66, `StaticMeshTests` 44/44, and `WorldTests`
  62/62 pass. Stage 2 must remove mesh-side GUID identity and rebuild the three
  Engine model packages without loading/resaving their incompatible old slot
  structs.

### Stage 2: Remove Mesh Slot GUIDs and Establish Stable Reimport Indices

Dependencies: Stage 1.

- [x] Remove `SlotId` and `MaterialSlotsVersion` from authored slot
  definitions; remove GUID lookup and all GUID validity/uniqueness logic from
  import, load, debug mesh creation, render-data restoration, and tests.
- [x] Preserve user `Name` on matched reimport, update only source metadata,
  add validated rename/name-index APIs, and keep newly imported display names
  unique and non-`None`.
- [x] Replace imported-order reconciliation with the selected stable-index
  algorithm, retain unmatched old positions, append unmatched new positions,
  and emit an explicit imported-to-stable slot map.
- [x] Route section construction through that explicit map and validate that
  every section references one active stable slot even when historical source
  indices collide with retained unused entries.
- [x] Remove `SlotId` from `FStaticMeshMaterialSlot`, replace DMSH
  `MaterialSlotIds` with `MaterialSlotCount`, bump payload schema/builder
  versions, and update DDC key, codec, corruption, Cook, and restore tests.
- [x] Recreate Box, Sphere, and Teapot packages from their checked-in OBJ
  sources at the existing Engine asset paths; do not deserialize or resave the
  incompatible old slot struct.
- [x] Rewrite the StaticMesh material tests around index stability, source
  metadata, user rename persistence, unused reserved slots, section remapping,
  and append-only additions rather than GUID preservation.

#### Acceptance Gate

- Reordering uniquely named imported materials leaves existing slot indices and
  component assignments unchanged while sections follow the correct surfaces.
- Renaming at the same source index preserves the old index, user slot name,
  and default material; ambiguous rename-plus-reorder retains the old unused
  position and appends a new slot.
- Removing a source material does not renumber later slots, and reintroducing
  an unambiguous material can reactivate its prior position.
- DMSH schema 3 round-trips only a bounded slot count, rejects schema 2, and
  validates section indices against the package slot table.
- Rebuilt Engine model packages load, build render resources, and expose their
  expected slot names without any serialized `SlotId` field.
- Focused StaticMesh import, payload, DDC, Cook, resource, and renderer tests
  pass.

#### Stage 2 Handoff

- Baseline: `4de1d51516f0b77591ffadbafa65a63e16341332`.
- Working set: `DStaticMesh`, runtime render slots, DMSH/DDC/Cook paths,
  StaticMesh material/payload/cache tests, shared test fixtures, and the three
  Engine model packages.
- Decision: exact non-empty unique `SourceName` matches first, unique source
  index matches second, unmatched imports append, and unmatched old positions
  remain reserved. Matched slots preserve user `Name`, default material, and
  index while updating source evidence.
- Payload result: DMSH schema 3 and builder version 2 store one bounded
  `MaterialSlotCount`; schema 2 is rejected, section indices validate against
  the count, and package metadata restores runtime names/source indices.
- Asset result: Box is 3,303 bytes; Sphere and Teapot are 3,309 bytes each.
  All were rebuilt from checked-in OBJ sources, contain neither `SlotId` nor
  `MaterialSlotsVersion`, and all seven repository packages audit as ready,
  compatible, and current.
- Validation: `StaticMeshTests` 44/44, `MaterialTests` 66/66, and `WorldTests`
  62/62 pass. Stage 3 must add/confirm the end-to-end integration contract,
  update lasting documentation, run full native/full `all`, and smoke the
  verified editor executable.

### Stage 3: Close Integration, Assets, Documentation, and Full Validation

Dependencies: Stage 2 and no concurrent writer in the overlapping Default
Material plan working set.

- [x] Add or rewrite an end-to-end test covering import, positional Details
  assignment, save/reload, source reorder, reimport, section remapping, and the
  same rendered surface retaining its override by stable index.
- [x] Cover cross-mesh switching among one-slot, fewer-slot, and larger-slot
  meshes through both API and reflected editor property changes.
- [x] Audit production code, generated metadata, tests, and checked-in package
  bytes for `FStaticMeshMaterialOverride`, component `MaterialOverrides`, mesh
  `SlotId`, DMSH `MaterialSlotIds`, and StaticMesh slot-orphan behavior; exclude
  unrelated material-parameter GUID/orphan symbols from removal.
- [x] Confirm `NewMaterial.dasset` remains deleted, `NewLevel.dasset` has no
  stale reference, all Engine models use the new slot schema, and every
  checked-in package is current under the project compatibility audit.
- [x] Update `Documentation/Runtime/Rendering/MaterialSystem.md` with dual-name
  slot ownership, stable reimport indices, positional override resolution,
  mesh-switch behavior, dormant entries, and index-only render boundaries.
- [x] Update
  `Documentation/Editor/Architecture/ReflectedPropertyEditing.md` with
  index-scoped fixed-row transactions and removal of the StaticMesh orphan
  workflow.
- [x] Update `Documentation/Roadmaps/MaterialSystem.md` and, if still active,
  the Default Material plan's affected API assumptions without duplicating
  implementation contracts.
- [x] Run focused tests, the complete affected native suites, the full `all`
  build, and the hidden-window `DurinEditor` smoke procedure through the root
  workflow documented by the repository.
- [x] Record the final baseline, working set, decisions, open questions, asset
  audit, and validation evidence; move lasting rules to their owning documents
  and complete this plan.

#### Acceptance Gate

- StaticMeshComponent Details supports ordinary mesh switching without `None +
  Orphan <GUID>` presentation; shared slot indices retain their overrides.
- No production or serialized StaticMesh slot/component state contains a slot
  GUID, GUID-keyed override, compatibility alias, migration branch, or
  StaticMesh slot orphan.
- Reimport, source/cooked load, render proxy creation, live material updates,
  Undo/Redo, and package save/reload agree on the same positional slot table.
- Every checked-in package passes the compatibility audit, all required tests
  and the full `all` build succeed, and the verified editor executable remains
  alive through the smoke interval.

#### Final Handoff

- Baseline: `7a03b28085736ad42fae85d38014d72425896589`.
- Working set: end-to-end StaticMesh material coverage; Runtime material and
  StaticMesh rendering contracts; reflected editing architecture; material
  roadmap; active Default Material integration assumptions; and this plan.
- Decisions: component assignment identity is only the positional slot index;
  mesh import preserves indices monotonically using source evidence; renderer
  consumption remains index-only; material parameter GUID/orphan behavior is
  intentionally unaffected. No compatibility or migration code was added.
- Open questions: none. Optional compaction, stronger source identifiers, and
  cross-mesh semantic mapping remain the explicit deferred follow-ups below.
- Assets: `NewMaterial.dasset` is deleted, `NewLevel.dasset` contains no stale
  reference, the three Engine models use the current slot schema, package-byte
  searches find no retired StaticMesh symbols, and all seven checked-in
  packages audit as ready, compatible, and current.
- Validation: focused StaticMesh, Material, and World suites pass; the complete
  native aggregate and full Editor/Game `all` builds pass under the agent
  profile; regenerated metadata in both variants contains no retired slot
  symbols; the resulting `DurinEditor.exe` loads `Sandbox.dproject` hidden and
  exits normally after 30 engine ticks.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Source metadata | Exact raw `SourceName`, unique display `Name`, source-index fallback, empty and duplicate-name fixtures |
| Reimport ordering | Reorder preserves indices; rename preserves by source index; additions append; removals reserve; reappearance reactivates |
| Section mapping | Every imported section uses the explicit imported-to-stable map and never resolves through a historical duplicate source index |
| Slot naming | Valid unique rename, rejection of `None`/duplicate names, persistence, and survival across reimport |
| Component API | Index bounds, null reset, interior holes, trailing trim, name lookup, Clear All, and no-mesh behavior |
| Mesh switching | Shared indices apply to a new mesh; out-of-range entries are dormant; later meshes reactivate the same positions |
| Resolution | Component override, mesh default, then selected renderer/Engine default path for every current slot |
| Reflection | Detached array edits validate type/length; Cancel/Undo/Redo and two index identities share one root safely |
| Details | Fixed current rows, slot labels/source state, Reset/Clear All, no structural controls, no GUID/orphan presentation |
| Rendering | Scene proxies receive slot-ordered material proxies; dormant entries are absent; revision ordering rejects stale updates |
| Payload/DDC | DMSH schema 3 count encoding, bounds/corruption rejection, derived-data miss from version bumps, source/cooked restore |
| Assets | User material deleted atomically with a recreated level, three Engine model assets rebuilt, all packages current |
| Regression | StaticMesh, materials, AssetCore, reflection, world, viewport, Cook, renderer reload, and full native suites pass |
| End to end | Import -> assign -> save/reload -> reorder/reimport -> render retains the intended surface by stable index |
| Product validation | Full `all` build and hidden-window editor smoke succeed through the documented root workflow |

Build, test, package-audit, and smoke commands must use the current root
instructions in `Documentation/Development/Build/BuildAndRun.md` and
`Documentation/Development/Build/NativeTests.md`; this plan does not copy
commands that may become stale.

## Definition of Done

- StaticMesh material slots persist user names, exact import names, source
  indices, and defaults without any slot GUID or versioned GUID schema.
- Automatic reimport keeps existing slot indices stable, reserves removed
  positions, appends new slots, and maps sections explicitly.
- Components persist positional `OverrideMaterials`, retain them across mesh
  switches, resolve them by index, and expose optional name-based convenience
  APIs.
- StaticMesh slot orphans and their Details UI no longer exist; material
  parameter orphan behavior remains intact.
- Render data and DMSH payloads carry only the material-slot count/order needed
  for compact section indices.
- No legacy compatibility or migration code is present, the one user material
  is deleted, the level is recreated, affected mesh assets are rebuilt, and all
  repository packages match the new authored baseline.
- Lasting Runtime, Editor Architecture, and roadmap documents describe the
  indexed contract.
- Focused and full validation, the full build, and editor smoke all pass, and
  the plan contains a compact final handoff before completion.

## Deferred Follow-ups

- A dedicated StaticMesh asset editor for renaming slots, inspecting imported
  names/source indices, showing unused reserved slots, and explicitly compacting
  them.
- Project-wide positional override remapping if explicit slot compaction is
  ever selected.
- Strong source-format material identifiers or sidecar metadata when a format
  supplies identity better than exact name and source index.
- Per-mesh override profiles when one component intentionally switches among
  unrelated multi-material meshes but needs different assignments per mesh.
- Name-based or user-authored cross-mesh mapping beyond the selected shared
  positional behavior.
- A real external package migration plan if Durin later promises compatibility
  across authored schema baselines.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Static Mesh Material Slots Plan](Archive/2026-07/StaticMeshMaterialSlots.md)
- [Default Material and Error Fallback Plan](DefaultMaterialAndErrorFallback.md)
- [Project Asset Compatibility Audit Plan](ProjectAssetCompatibilityAudit.md)

## Related Code

```text
Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h
Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp
Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h
Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h
Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp
Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h
Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp
Engine/Source/Editor/LevelEditor/Public/StaticMeshMaterialSlotDetails.h
Engine/Source/Editor/LevelEditor/Private/Customizations/StaticMeshMaterialSlotDetails.cpp
Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshMaterialTests.cpp
Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp
Engine/Tests/Native/EngineTests/Private/Materials/MaterialInstanceTests.cpp
Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp
Engine/Tests/Native/EngineTests/Private/StaticMeshMaterialSlotDetailsTests.cpp
Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp
Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp
Engine/Content/Sources/Models/Box.obj
Engine/Content/Sources/Models/Sphere.obj
Engine/Content/Sources/Models/Teapot.obj
Engine/Content/Models/Box.dasset
Engine/Content/Models/Sphere.dasset
Engine/Content/Models/Teapot.dasset
Sandbox/Content/Levels/NewLevel.dasset
Sandbox/Content/Materials/NewMaterial.dasset
```
