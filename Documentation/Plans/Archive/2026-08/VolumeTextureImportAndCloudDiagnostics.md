# Volume Texture Import and Cloud Diagnostics Plan

Summary: Add direct source-backed PNG-atlas volume import and actionable generic Details diagnostics for volumetric clouds.

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-21

## Current Status

Activated on 2026-08-21 after the user selected asset ingestion and generic
Details diagnostics ahead of temporal, lighting, or specialized cloud-editor
work. Volume Texture Foundation already supplies normalized source data,
deterministic mips, DDC, package/cook payloads, texture-reference publication,
and public-RHI `Texture3D` upload. Volumetric Cloud Scene Contract already
supplies a reflected actor/component, stable active selection, generic Details
editing, and a production Renderer handoff.

Stages 0 through 3 are implemented. The texture workflow now imports one
row-major PNG atlas directly as a deterministic `R8_UNORM` or `RGBA8_UNORM`
volume asset, retains its path, hash, and visible atlas settings, and uses detached
TextureBuild candidates for reimport and repair. Contract tests cover strict
atlas-setting rejection, channel assembly, sole-source capture, moved-source
repair, and failed-reimport retention.

The component now derives one ordered eligibility diagnostic for scene
publication and generic Details. Stable texture references are rechecked on
the render thread, so a recovered import becomes selectable without toggling
an unrelated component property. Stage 3 now integrates the existing texture
import dialog and validates imported Base/Detail creation, dependency-aware
reimport, moved-source repair, package reload, generic Details, scene
eligibility, compute/fragment Vulkan paths, offscreen/Present routes, and
last-known-good failure retention.

Validation completed on 2026-08-21: all 75 `TextureTests`, all 29
`EditorPropertyTests`, all 6 `VolumetricCloudSceneContractTests`, both cloud
Vulkan integration targets, the 55-target `fast-all` aggregate, the full Debug
Editor build, documentation lifecycle validation, and an eight-second Debug
Editor startup smoke passed.

The 2026-08-22 source-contract revision removed the intermediate manifest and
made the PNG atlas the sole root source. The asset now serializes and exposes
its source path, `PNG Row-Major Atlas` format, channel selection, slice extent,
depth, and tile grid. Validation passed all 75 `TextureTests`, all 29
`EditorPropertyTests`, the `fast-all` aggregate, a full Debug Editor
build, changed-document validation, and an eight-second Debug Editor startup
smoke.

## Goal

Let a user import, reimport, save, cook, load, and assign deterministic dense
volume textures through the existing asset workflow, then
understand from generic Details exactly why a Volumetric Cloud Component is or
is not eligible to render.

## Scope

- A narrow direct lossless PNG-atlas adapter that
  produces normalized `R8_UNORM` or `RGBA8_UNORM` `FVolumeTextureSourceData`.
- AssetImportCore/AssetForge integration for recognition, dependency capture,
  preview, transactional candidate build, publication, reimport, source repair,
  source relocation, and Content Browser import.
- Serialized source provenance and settings sufficient for deterministic
  reimport while preserving the existing `DVolumeTexture` runtime/cook value.
- Exact source, dimension, memory, path, ordering, and format validation before
  allocating or publishing a candidate.
- A GPU-free cloud eligibility result with stable reason ordering and an
  actionable human-readable message.
- A transient read-only status exposed by `DVolumetricCloudComponent` through
  the existing reflection-driven Details panel.
- Focused import, package, cook, editor, component, and Vulkan end-to-end
  validation using real imported slice fixtures.

## Non-Goals

- A standalone Volume Texture Editor or Volumetric Cloud Editor.
- Volume previews, slice scrubbing, histograms, 3D visualization, cloud debug
  views, presets, or asset-generation UI.
- VDB, DDS, KTX, raw binary, EXR volume, arbitrary channel packing, compressed,
  sparse, streamed, virtual, or bricked volume formats.
- Procedural noise generation or shipping production cloud art.
- Changes to TXPL, public RHI, Vulkan texture topology, the cloud shader,
  temporal reconstruction, quality tiers, lighting, or cloud shadows.
- A generic diagnostics framework for every component. This plan uses existing
  reflected read-only properties for one concrete consumer.

## Design Decisions and Invariants

### Source contract

- The first source adapter consumes one lossless PNG row-major atlas directly.
  The import settings select slice width/height, depth, tile columns/rows, and
  `red`, `green`, `blue`, `alpha`, `luminance`, or the complete `rgba` tuple.
- Tiles advance left-to-right then top-to-bottom in volume Z order. The atlas
  extent must exactly match the selected tile geometry, and unused tail cells
  after depth are ignored.
- Scalar channel selections produce `R8_UNORM`; `rgba` produces
  `RGBA8_UNORM` without discarding packed cloud-noise octaves. Scalar channel
  extraction is deterministic and luminance uses integer weights 54/183/19
  with round-to-nearest division by 256. Other existing
  `DVolumeTexture` source/output formats remain valid programmatic inputs but
  are not exposed by this importer.
- Width, height, depth, decoded bytes, dependency count, and aggregate source
  bytes are checked with overflow-safe arithmetic against existing texture and
  import limits before candidate storage is allocated. Absolute slice paths,
  missing files, duplicate logical dependencies, `..` escapes, mixed extents,
  unsupported PNG data, atlas extent mismatches, and
  empty volumes fail with source-specific diagnostics.

### Import ownership and failure behavior

- AssetImportCore owns source snapshotting, planning,
  preconditions, cancellation, and publication serialization. AssetForge owns
  PNG-atlas translation, volume-source assembly, TextureBuild
  invocation, and the `DVolumeTexture` import handler. Engine owns serialized
  provenance and the final asset state; TextureBuild remains the only mip/DDC
  builder.
- New single-asset creation follows the repository's existing Texture2D and
  TextureCube pattern: the import dialog prepares mounted sources, builds a
  detached candidate, saves the package, then commits source copies.
  AssetImportCore owns registered capability inspection, immutable dependency
  snapshots, reimport, replacement preconditions, cancellation, and prepared
  exchange for existing assets. This avoids introducing a second generic
  create-execution path that the current single-asset service does not expose.
- The PNG is the root source. Planning and building use immutable captured bytes
  rather than reopening authoring files. Provenance records provider/version,
  source path, content hash, atlas settings, and authored fingerprint so
  reimport and source-reference repair use existing workflows.
- Import and reimport build a detached candidate, validate its normalized
  source and complete platform mip chain, and prepare an exchange before the
  main-thread package mutation. Parse, decode, build, DDC, precondition,
  cancellation, save, or render-resource failure never partially replaces the
  previous asset; existing last-known-good rules remain authoritative.
- Source paths are authoring-only. Cook consumes the existing normalized
  volume asset and TXPL payload, strips source according to current policy, and
  never invokes AssetForge or a decoder at runtime.
- `DVolumeTexture` remains cloud-neutral. Names such as Base, Detail, coverage,
  or erosion do not enter its provenance, build key, or payload.

### Cloud diagnostic contract

- One pure Engine function returns `FVolumetricCloudEligibilityDiagnostic`
  with a stable reason enum, `bEligible`, and an actionable message. The first
  failing reason wins in this order: disabled, owner hidden, missing Base,
  invalid/unbuilt Base, missing Detail, invalid/unbuilt Detail, invalid layer,
  invalid distance, invalid density mapping, invalid optical parameters, then
  ready. Missing optional Weather never makes the cloud ineligible.
- Scene publication and generic Details consume the same validator. Renderer
  still receives only the existing immutable values, counted references, and
  final eligibility bit; diagnostic strings and editor state never cross the
  render-thread boundary.
- The component exposes a nonserialized `DPROPERTY(Edit, ReadOnly, Transient)`
  status string. It refreshes on construction, registration, visibility,
  reflected edits, setters, asset assignment, duplication/load completion, and
  render-state publication. It is derived presentation, not saved intent or an
  independent eligibility authority.
- Import/reimport that preserves a valid texture reference must not require a
  component toggle to recover rendering. Asset publication either retains the
  referenced last-known-good resource or triggers the established dependent
  render-state refresh needed to republish eligibility.

### UI boundary

- The existing Content Browser texture import flow offers Texture2D or Volume
  Texture for a PNG, previews the asset and mounted source destination, reports exact import
  failures, and publishes the asset. Registered AssetImportCore capabilities
  drive subsequent reimport and repair. No new top-level window or editor
  toolkit is introduced.
- Generic Details continues to render all cloud properties and asset pickers.
  This plan adds only the read-only status row; richer grouping, previews,
  actions, quality presets, and debug controls remain deferred.

## Current Foundations and Gaps

| Area | Existing foundation | Selected gap |
| --- | --- | --- |
| Image input | Core decodes bounded images; Texture2D and TextureCube already use AssetForge translation. | Implemented: strict atlas-setting validation and deterministic captured PNG assembly into R8 or RGBA8 volume source. |
| Import transaction | AssetImportCore owns dependency snapshots, previews, candidate plans, preconditions, and publication. | Register a multi-source volume provider and retain ordered provenance for reimport/repair. |
| Volume asset | `DVolumeTexture` owns normalized source, deterministic TextureBuild/DDC, TXPL cook, and revisioned GPU resources. | Add imported-state/provenance exchange without duplicating its builder or runtime payload. |
| Cloud component | Base/Detail references, eligibility, immutable publication, and generic Details editing are implemented. | Replace the opaque eligibility bit at the authoring surface with one shared reasoned diagnostic. |
| End to end | Real volume sampling and component-driven Vulkan rendering pass using programmatic fixtures. | Prove the same path from captured source files through Content Browser import, package reload, assignment, and visible output. |

## Implementation Stages

### Stage 0: Freeze source schema, provenance, and diagnostics

- [x] Freeze the direct PNG-atlas settings, channel conversion, path resolution,
  dependency ordering, limits, provider identity/version, and reimport
  provenance shape with canonical valid and invalid fixtures.
- [x] Freeze the detached candidate/exchange boundary and enumerate how source
  repair, relocation, cancellation, package conflict, failed rebuild, and
  last-known-good resource publication behave.
- [x] Freeze the cloud eligibility reason order, user-facing messages, refresh
  triggers, and the exact transient read-only Details representation.
- [x] Add GPU-free contract tests for atlas validation, ordered source assembly,
  channel extraction, all rejection paths, diagnostic reason ordering, and
  diagnostic/scene-eligibility agreement.

#### Acceptance Gate

- One unambiguous source/import/diagnostic contract passes without package or
  GPU initialization; later stages have no unresolved format or ownership
  decision.

### Stage 1: Build the source-backed volume importer

- [x] Add serialized volume source provenance and imported-state exchange to
  `DVolumeTexture` without changing normalized source, TXPL, or render-resource
  contracts.
- [x] Implement captured PNG-atlas translation,
  channel extraction, normalized R8/RGBA8 source assembly, TextureBuild invocation,
  and complete candidate validation in AssetForge.
- [x] Register the AssetImportCore provider and single-asset handler for
  recognition, settings, dependency discovery, reimport, repair, cancellation,
  and diagnostics; index every source for the existing relocation workflow.
- [x] Cover create/reimport/no-op reimport, PNG mutation, source
  move/repair, DDC hit/miss/corruption, candidate failure, conflicting target,
  rollback, save/reload, and imported-state duplication.

#### Acceptance Gate

- The normal import service transactionally produces a package-backed,
  reimportable `DVolumeTexture` whose normalized source, complete mip chain,
  provenance, hashes, and last-known-good behavior are deterministic.

### Stage 2: Surface actionable generic cloud diagnostics

- [x] Implement the pure eligibility diagnostic and make scene eligibility use
  it rather than a separate boolean-only validation path.
- [x] Add the transient read-only component status and refresh it across every
  authored, visibility, lifecycle, load/duplicate, and asset mutation path.
- [x] Ensure assigning, importing, reimporting, replacing, unloading, deleting,
  or recovering required textures updates status and scene publication without
  toggling another cloud property.
- [x] Extend generic property/editor tests for status visibility, read-only
  behavior, exact messages, undo/redo, save exclusion, multi-selection/world
  reopen, and valid/invalid asset transitions.

#### Acceptance Gate

- Generic Details always explains the current primary cloud eligibility result,
  and the displayed result agrees with the selected render-thread candidate
  after each supported mutation.

### Stage 3: Integrate the user workflow and qualify real output

- [x] Integrate direct PNG volume mode into the existing Content Browser texture import
  dialog, mounted output preview, error presentation, and source-reference
  workflows without a new editor window.
- [x] Import Base and Detail fixtures through the public workflow, assign them
  through generic asset pickers, save/reload/reopen the level, and prove the
  real component drives compute and fragment fallback in offscreen and Present
  routes.
- [x] Prove reimport changes visible density while preserving asset/component
  identity; malformed reimport retains the previous visible cloud and reports
  the failure; deletion/missing source produces the frozen diagnostic behavior.
- [x] Publish lasting volume-import and cloud-diagnostic contracts, reconcile
  the roadmap/P5 boundary, and run focused import/asset/editor/cloud/Vulkan
  targets, native aggregate, full build, documentation validation, and a
  validation-enabled Debug Editor smoke.

#### Acceptance Gate

- A user can import two volume assets, add one cloud actor, assign the assets,
  see either a rendered cloud or an exact corrective status, and retain that
  workflow across reimport, package reload, cook, and clean shutdown without a
  specialized editor.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Manifest and slices | Explicit order, identical extents, bounded R8/RGBA8 extraction, stable hashes; malformed/missing/escaping input rejected before publication | Parser and source-translation tests |
| Import transaction | Preview, candidate build, exchange, cancellation, conflicts, and failure are atomic | AssetImportCore/AssetForge tests |
| Reimport and source management | No-op and changed reimport, relocation, repair, dependency mutation, and corrupt source preserve identity and last-known-good state | Import lifecycle tests |
| Package/DDC/cook | Authored provenance/source round-trips; DDC is deterministic; cooked runtime needs no authoring module or source file | Asset/package/cook tests |
| GPU resource | Imported mip topology uploads and samples through public Texture3D paths; replacement/recovery releases cleanly | Vulkan texture tests |
| Eligibility diagnostics | Every primary reason and ready state is deterministic and agrees with scene eligibility | Engine contract tests |
| Generic Details | Status is visible, transient, read-only, searchable, updated after edits/imports, and compatible with undo/redo | Editor property tests |
| User vertical slice | Import Base/Detail, assign, save/reopen, render, reimport, fail/recover, Present/offscreen, and shutdown | Editor/cloud Vulkan integration and smoke |

## Definition of Done

- The existing Content Browser imports one row-major PNG atlas directly into a
  valid, saved, reimportable, cookable `DVolumeTexture` using the shared import
  transaction and TextureBuild pipeline.
- Import failures and reimport failures are atomic, source-specific, and retain
  the previous valid asset/resource.
- A Volumetric Cloud Actor assigned imported Base and Detail textures renders
  through the production scene path; optional Weather remains optional.
- Generic Details reports one exact actionable status that shares the runtime
  eligibility authority and updates without unrelated property toggles.
- No specialized volume/cloud editor, cloud semantics in `DVolumeTexture`, new
  runtime decoder dependency, RHI change, or premature P3/P4 work is introduced.
- Focused tests, aggregate tests, full Debug build, documentation validation,
  and Editor smoke pass; lasting contracts are published.

## Deferred Follow-ups

- P3 owns temporal reconstruction and named quality tiers.
- P4 owns production scattering, self-transmittance, and cloud shadows.
- P5 owns volume/cloud previews, slice inspection, procedural generation,
  presets, debug views, richer source adapters justified by production assets,
  and the standalone authoring experience.
- Broader generic component validation presentation may be extracted only when
  a second concrete component needs the same protocol.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)
- [Volume textures](../../../Runtime/Assets/VolumeTextures.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Asset data lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Build and run](../../../Agents/BuildAndRun.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Image/ImageDecoder.h`
- `Engine/Source/Editor/AssetImportCore/Public/AssetImportCore.h`
- `Engine/Source/Editor/AssetForge/Public/Texture2DSourceTranslation.h`
- `Engine/Source/Editor/AssetForge/Private/AssetForgeProviders.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Developer/TextureBuild/Public/Texture/VolumeTextureBuildOperations.h`
- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/VolumetricCloudSceneProxy.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
