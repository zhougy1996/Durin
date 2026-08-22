# Unified Texture Import Workflow Plan

Summary: Consolidate Texture2D, TextureCube, and VolumeTexture creation behind one texture-import entry and modal while preserving type-specific source contracts.

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-22

## Current Status

Implementation is complete. The Content Browser exposes one
`Import > Texture...` action, `MLevelEditor` owns one `FTextureImportDialog`, and
the modal explicitly selects Texture2D, Texture Cube, or Volume Texture. Common
popup, destination, mounted-source mode, warning, action, notification, and
unload behavior surround independent typed form state. The existing dedicated
Cube translation unit now supplies embedded panorama and six-face form behavior
without owning a second popup or editor route.

Validation completed on 2026-08-22: both focused
`FTextureImportDialogStateTests`, the `EditorAssetWorkflowTests` target (86
passed, 1 conditionally skipped), all 75 `TextureTests`, all 11 `SkyBoxTests`,
the two-target `asset-workflow` domain,
the 55-target `fast-all` aggregate, the full Debug Editor build, changed-document
and all-plan validation, and an eight-second Debug Editor startup smoke passed.
The startup log reached Vulkan initialization and mounted-asset scanning with
no error, fatal, assertion, or exception diagnostic before the bounded smoke
terminated the process.

## Goal

Give users one predictable texture-import workflow for Texture2D,
TextureCube, and VolumeTexture while retaining the specialized source layouts,
validation, provenance, and transactional publication behavior of each asset
type.

## Scope

- Replace the two Content Browser texture import actions with one
  `Import > Texture...` action.
- Replace the LevelEditor-owned pair of texture import modals with one modal
  and one request route.
- Add an explicit `Texture2D`, `Texture Cube`, and `Volume Texture` asset-type
  selection whose value is never inferred from the selected file extension.
- Consolidate common modal lifecycle, asset destination, mounted-source mode,
  output summary, warnings, cancellation, and completion notification.
- Preserve independent type-specific source state and settings while the user
  switches asset types in one open modal.
- Preserve TextureCube panorama/six-face switching, face orientation guidance,
  HDR exposure, automatic/custom face size, validation summary, and multi-source
  destinations.
- Preserve Texture2D usage selection and VolumeTexture PNG row-major atlas
  channel, slice, depth, and grid settings.
- Add focused state and routing coverage and update the user-facing TextureCube
  workflow documentation.

## Non-Goals

- New texture source formats, including VolumeTexture PNG slice sequences,
  VDB, DDS, KTX, raw volumes, or EXR volumes.
- Automatic asset-type detection. A 2:1 panorama remains a valid Texture2D
  source until the user explicitly selects Texture Cube.
- Changes to AssetForge translation, TextureBuild recipes, DDC keys, package
  schemas, reimport, repair, cooking, RHI resources, or runtime sampling.
- A TextureCube, VolumeTexture, or generic texture preview/editor.
- A framework-wide rewrite of the StaticMesh, Scene, Terrain Heightmap, or
  other import dialogs.
- Preserving the old `Texture Cube...` menu item as a second shortcut.

## Design Decisions and Invariants

### Workflow and state

- The Content Browser sends one texture import request. Opening it resets the
  selected asset type to Texture2D and resets every type-specific state to its
  current defaults; switching type after opening does not discard any fields
  already entered for another type.
- The asset type is a named enum, not multiple Boolean flags. User-facing labels
  are exactly `Texture2D`, `Texture Cube`, and `Volume Texture`.
- One destination asset path is shared across the three modes. Existing
  destination-model behavior remains authoritative: automatic suggestions may
  replace earlier automatic suggestions but never overwrite a manually edited
  path.
- One mounted-source mode is shared by the modal. Each type-specific panel maps
  that mode onto its own one-source or multi-source controls. The selected mode
  does not alter inactive source buffers.
- TextureCube continues to retain panorama and six-face inputs when switching
  between its two source layouts, matching the documented workflow.

### Ownership and decomposition

- `FTextureImportDialog` owns the popup lifecycle, selected asset type, common
  destination model, common source mode, output-summary frame, final action
  row, and import completion callback.
- Type-specific state is represented by cohesive Texture2D, TextureCube, and
  VolumeTexture form values or models. Drawing, source browsing, suggestions,
  validation, and submission dispatch through the selected type without
  accumulating unrelated Cube and Volume fields in the common shell.
- The implementation may retain a dedicated TextureCube translation unit, but
  it must no longer own a second popup, destination lifecycle, completion
  callback, or LevelEditor member. No generic virtual importer-panel framework
  is introduced for only three closed built-in variants; an enum plus explicit
  typed state or `std::variant` is sufficient.
- Existing `FImportDialogDestinationModel`, mounted-source inspection helpers,
  AssetForge entry points, and per-type validation remain the authorities. UI
  consolidation must not duplicate source decoding or asset validation.

### Validation and failure behavior

- The import button is enabled only when the selected type's complete source,
  destination, mounted-source, and settings validation succeeds. Invalid
  inactive type state never blocks the selected type.
- Type switching, source-layout switching, browsing cancellation, and modal
  cancellation perform no filesystem or package mutation.
- Successful publication produces the same asset class, source provenance,
  mounted-source copies, package path, notification, unload behavior, and
  failure-atomic guarantees as the existing per-type workflow.
- Errors remain source-specific. Six-face TextureCube diagnostics continue to
  identify the affected face rather than collapsing into a generic modal error.
- VolumeTexture remains limited to one lossless row-major PNG atlas. A richer
  adapter requires a named production asset that demonstrates atlas repacking
  cost, 2D extent pressure, or an upstream tool that only emits sequences.

## Current Foundations and Gaps

- `FTextureImportDialog` already provides the common popup, destination,
  mounted-source, output-summary, warning, and action structure for Texture2D
  and VolumeTexture, but `bImportVolume` encodes a two-type assumption.
- `FTextureCubeImportDialog` already has complete panorama and six-face form
  behavior, strict pre-publication validation, source suggestions, and
  transactional AssetForge submission. Its duplicated popup shell and
  destination lifecycle are the primary consolidation target.
- `MLevelEditor` constructs, routes, draws, and owns both dialogs, and
  `EContentBrowserImportType` exposes separate Texture and TextureCube request
  values.
- `FMountedSourceImportFormModel` covers the ordinary single-source case.
  TextureCube's multiple sources require its existing explicit diagnostic set;
  forcing six faces through the single-source model would lose information.
- `ImportDialogStateTests.cpp` covers shared destination and mounted-source
  state, but there is no pure coverage for three-type selection, inactive-state
  retention, or texture request routing.
- The TextureCube guide names the old menu action and must move to the unified
  workflow. Runtime Texture2D, Cube, and VolumeTexture contracts do not require
  a behavioral update because source and asset semantics are unchanged.

## Implementation Stages

### Stage 0: Freeze the workflow and ownership boundary

- [x] Inventory the existing Content Browser routes, LevelEditor ownership,
  shared import state, and per-type source contracts.
- [x] Select one explicit three-type workflow without file-based type inference.
- [x] Keep VolumeTexture PNG sequence import deferred until justified by a
  named production source.
- [x] Define common-shell and type-specific-state ownership and failure
  invariants.

#### Acceptance Gate

- The plan distinguishes the UI consolidation from AssetForge and runtime
  texture contracts, and no source-format decision remains open.

### Stage 1: Introduce the unified texture-import state

- [x] Replace the Boolean Texture2D/VolumeTexture choice with a named three-type
  asset selection.
- [x] Separate the existing Texture2D, VolumeTexture, and TextureCube form
  values so common modal code does not directly own unrelated per-type fields.
- [x] Move common destination, source mode, validation-result presentation, and
  action-row ownership into the unified dialog.
- [x] Preserve inactive type state and both TextureCube source layouts during
  in-modal switching.
- [x] Add pure state tests for default selection, reset behavior, manual
  destination preservation, and inactive-state retention without requiring an
  ImGui application host.

#### Acceptance Gate

- Focused tests prove deterministic open/reset/switch behavior, and each type
  can produce an independent validation/submission request through the common
  shell without changing AssetForge APIs.

### Stage 2: Integrate TextureCube and collapse editor routing

- [x] Render all three type-specific source/settings panels inside the single
  `Import Texture` modal.
- [x] Integrate TextureCube panorama validation, six-face diagnostics, output
  summary, source-destination suggestions, and submission into the shared
  destination and action lifecycle.
- [x] Replace the two Content Browser texture menu entries and request enum
  values with one `Import > Texture...` action.
- [x] Remove the second LevelEditor dialog owner, route, draw call, and obsolete
  popup-only code while retaining a cohesive Cube form implementation.
- [x] Verify browsing cancellation, invalid sources, type switching, import
  success, and import failure do not leak state or mutate inactive sources.

#### Acceptance Gate

- The Content Browser exposes exactly one texture import action; that action can
  create valid Texture2D, TextureCube panorama, TextureCube six-face, and
  VolumeTexture atlas assets with the same source provenance and package output
  as before.

### Stage 3: Publish the workflow and complete validation

- [x] Update the TextureCube user guide to route through `Import > Texture...`
  and select `Texture Cube` without restating runtime projection contracts.
- [x] Update any implemented Editor architecture contract whose description of
  import routing becomes stale.
- [x] Run focused import-dialog state and affected LevelEditor tests discovered
  through the registered test list.
- [x] Run the appropriate bounded editor/import domain, `fast-all`, a full
  Debug Editor build, and an Editor startup smoke according to the repository
  build and test guides.
- [x] Record exact validation evidence, publish any lasting editor-workflow
  invariant in its owning document, and complete the plan.

#### Acceptance Gate

- Documentation validation, focused tests, bounded aggregate coverage, full
  editor compilation, and startup smoke pass; the active guide and architecture
  contracts describe the shipped unified workflow.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Selection and state | Pure tests cover Texture2D default, three-type switching, reset-on-open, manual destination retention, inactive state retention, and Cube layout retention. |
| Common validation | Tests prove only selected-type diagnostics gate submission and mounted-source/destination failures remain actionable. |
| Texture2D | Existing image formats, usage selection, source ingestion/reference, asset class, and package publication are unchanged. |
| TextureCube | Panorama/HDR and six-face paths preserve orientation, validation summaries, source destinations, provenance, and failure-atomic publication. |
| VolumeTexture | One PNG row-major atlas preserves channel and grid settings, R8/RGBA8 result selection, provenance, and reimport compatibility. |
| Editor routing | Content Browser and LevelEditor expose and own one texture-import request and modal. |
| Regression | Registered focused targets, the bounded editor/import domain, `fast-all`, full Debug Editor build, and startup smoke pass. |
| Documentation | Changed-document and all-plan validation pass; user guidance names the unified entry and explicit Cube type selection. |

## Definition of Done

- One Content Browser action opens one texture import modal with three explicit
  asset types.
- Each type retains all previous supported settings, source layouts,
  validation, provenance, transactional source handling, and publication
  results.
- Switching types or Cube layouts preserves inactive input state and performs
  no mutation.
- No duplicate TextureCube popup, LevelEditor owner, or import request route
  remains.
- VolumeTexture accepts the existing single PNG atlas and no speculative new
  source form.
- Focused tests, bounded regression gates, editor build/smoke, and documentation
  validation pass, and lasting workflow documentation is current.

## Deferred Follow-ups

- Add a `PNG Slice Sequence` VolumeTexture source layout only after a named
  production workflow demonstrates that atlas conversion is a recurring cost
  or violates supported 2D image limits. That follow-up must define ordering,
  missing indices, mixed formats/extents, dependency bounds, mounted-source
  storage, reimport, relocation, and repair before implementation.
- Consider richer texture previews or drag-and-drop type suggestions separately;
  neither should weaken explicit asset-type selection.

## Related Documentation

- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Texture Cube Workflow](../../../Editor/Guides/TextureCubeWorkflow.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Agent build and run workflow](../../../Agents/BuildAndRun.md)
- [Agent testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialogCube.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/ImportDialogState.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/ImportDialogState.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Public/Widgets/MLevelEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ImportDialogStateTests.cpp`
