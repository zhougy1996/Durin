# Ready-to-Use Static Model Import Plan

Summary: Turn supported static glTF and FBX sources into immediately renderable StaticMesh, texture, and material assets through one deterministic import and reimport workflow.

Last reviewed: 2026-07-27

## Current Status

Planning is complete and no implementation stage has started.

Source dependency terminology now follows the unified logical-mount contract
selected by `Documentation/Plans/SourceLibraryReferences.md`: persisted inputs
use `FSourcePath`, and Content/SourceAssets are typed domains of one mount
rather than separate asset and source-library namespaces.

Durin currently imports static geometry through Assimp into one `DStaticMesh`.
The importer traverses the source node hierarchy, bakes node transforms into
vertex data, retains up to four UV channels and vertex color zero, generates
missing smooth normals and tangents, and creates one section per imported mesh
instance. Imported material data stops at `FImportedMaterialSlot`, which carries
only a unique display name, the Assimp material index, and the exact source name
used by material-slot reconciliation. No source material factors, texture
bindings, images, samplers, or surface properties cross the import boundary.

Static meshes already own persistent material-slot GUIDs and optional default
materials. Texture2D assets already support color, normal, and data/mask usage,
sRGB selection, mip generation, desktop compression, DDC restore, cooking, and
render-resource publication. Materials and material instances already support
schema-defined scalar, vector, and texture parameters, inheritance, asset
references, editor mutation, and render-thread updates. The current renderer,
however, consumes only a fixed Blinn-Phong contract with one base-color texture
and does not implement masked or translucent coverage.

The first implementation milestone therefore targets static, opaque,
base-color-textured models using the existing renderer contract. Later stages
expand the same import representation and generated assets to the metallic /
roughness PBR surface contract owned by the Material System plan.

## Goal

- Make one editor import action produce a renderable `DStaticMesh`, all required
  `DTexture2D` assets, one generated `DMaterialInstance` per used source
  material, and populated mesh default-material slots.
- Make the result render with its imported base color immediately when placed
  on a `DStaticMeshComponent`, without manual texture import or slot assignment.
- Preserve a format-neutral source-material representation between Assimp and
  Durin asset creation instead of storing material payload in
  `FImportedMaterialSlot`.
- Support external and embedded model images with portable source provenance,
  deterministic names, deduplication, dependency hashing, and transactional
  publication.
- Make reimport update the same importer-managed assets and slot assignments
  without generating duplicates or transferring materials to unrelated
  surfaces.
- Extend the completed vertical slice to the selected glTF metallic/roughness
  PBR subset when the required material and renderer contracts are available.
- Produce actionable diagnostics for unsupported, ambiguous, missing, unsafe,
  or lossy source data.

## Scope

- Static glTF 2.0 `.gltf` and `.glb` sources as the reference material
  contract.
- Static FBX sources mapped into the supported format-neutral material
  contract where Assimp exposes an unambiguous equivalent.
- A normalized imported material, image, texture-binding, sampler, and
  dependency representation in the editor-only `AssetImport` module.
- Used-material projection into existing `FImportedMaterialSlot` values and
  `DStaticMesh` material-slot definitions.
- External files, glTF data URIs, GLB embedded images, and Assimp embedded
  texture payloads.
- Automatic Texture2D build settings derived from material semantics.
- An engine-owned standard imported-surface parent material and generated
  material instances.
- Deterministic asset naming, collision preflight, multi-package save,
  rollback, reimport, missing-dependency diagnostics, and safe orphan reporting.
- Import-dialog output preview and policy controls required by the selected
  workflow.
- Unit, asset round-trip, transaction, renderer, editor, cook, and hidden-window
  end-to-end validation.

## Non-Goals

- Preserving the source node hierarchy, source mesh instancing, cameras, lights,
  or arbitrary scene metadata. Node transforms continue to be baked into one
  `DStaticMesh`.
- Introducing a `DModel`, prefab, scene-import asset, or actor hierarchy.
- Skeletal meshes, skeletons, skin weights, animations, morph targets, or
  animation retargeting.
- Collision generation, lightmap UV generation, automatic LOD generation, or
  gameplay-specific import presets.
- Importing author-authored LOD groups in the first usable milestone.
- Exact emulation of arbitrary FBX/DCC shader graphs, procedural textures, or
  proprietary renderer materials.
- A general material graph compiler. Imported materials target the standard
  parameter schema selected by the Material System plan.
- Automatically deleting generated assets that cease to be referenced after
  reimport.
- Silently overwriting an unrelated asset because its sanitized name collides
  with a proposed generated output.
- Treating OBJ/MTL as a parity target. Existing geometry import remains
  supported, but the first material acceptance fixtures are glTF and FBX.

## Design Decisions and Invariants

### Import representation and ownership

- `FImportedMaterialSlot` remains a lightweight used-surface identity:

  ```cpp
  struct FImportedMaterialSlot
  {
      std::string Name;
      uint32 SourceMaterialIndex = 0;
      std::string SourceName;
  };
  ```

- Material factors and texture bindings are not added to
  `FImportedMaterialSlot`. A slot answers which source material a mesh section
  uses; `FImportedMaterial` describes that source material.
- `FImportedSceneData` gains separate format-neutral collections conceptually
  equivalent to:

  ```cpp
  struct FImportedSceneData
  {
      std::vector<FImportedImage> Images;
      std::vector<FImportedMaterial> Materials;
      std::vector<FImportedMaterialSlot> MaterialSlots;
      std::vector<FImportedMeshData> Meshes;
      std::vector<FImportedDependency> Dependencies;
      std::vector<FImportDiagnostic> Diagnostics;
  };
  ```

- `FImportedMaterial` retains `SourceMaterialIndex` and exact `SourceName`.
  `Materials` may describe unused source entries for diagnostics, while only
  entries referenced by `MaterialSlots` produce Durin material assets.
- `FImportedMeshData::MaterialIndex` is renamed or explicitly documented as a
  source material index. It never becomes a compact material-slot index by
  coincidence.
- Texture bindings reference `Images` by import-local index and carry semantic
  role, UV channel, scale/offset, sampler state, and role-specific factors such
  as normal scale or occlusion strength. They do not contain `DTexture2D`
  pointers.
- Imported images carry a stable source identity, suggested name, encoded
  format, byte count, and either a logical external dependency or owned encoded
  bytes for embedded content. Decoded RGBA pixels are not duplicated in the
  scene representation.
- `AssetImport` owns source parsing and normalized import data. It does not
  create packages, `DTexture2D`, `DMaterialInstance`, RHI resources, or runtime
  objects.
- Editor asset-build orchestration owns conversion from normalized data into
  Durin assets. Runtime-only Engine targets continue to load cooked results
  without Assimp or source-image decoding.

### Format policy

- glTF 2.0 metallic/roughness is the reference semantic model. Required initial
  fields are base-color factor and texture. The PBR expansion adds metallic and
  roughness factors, metallic-roughness texture, normal texture and scale,
  occlusion texture and strength, emissive factor and texture, alpha mode,
  alpha cutoff, and double-sided state.
- The initial importer may use Assimp material keys, but Durin fixtures freeze
  the normalized result rather than Assimp structures or enum values.
- FBX properties map only when their meaning is unambiguous. Missing or
  renderer-specific properties use documented defaults and emit a structured
  warning; they do not receive invented PBR values presented as exact source
  fidelity.
- Unsupported glTF extensions are listed by name in import diagnostics.
  Required unsupported extensions fail before package creation. Optional
  unsupported extensions warn and use the defined core fallback.
- The importer rejects non-finite factors, invalid image references,
  unsupported embedded encodings, invalid UV-set indices, unsafe external
  paths, and resource counts or byte sizes above explicit budgets.

### Source dependencies and provenance

- This plan consumes the logical `FSourcePath`, mounted-source containment, and
  reference-versus-ingest rules selected by
  `Documentation/Plans/SourceLibraryReferences.md`; it does not create a second
  source mounting abstraction.
- A static-model import has one authoritative root source plus an ordered
  dependency manifest. Each entry records its normalized logical role,
  portable source location or embedded identity, exact byte hash, and size.
- External URIs resolve relative to the physical root model only after the
  resolved candidate passes its mount's SourceAssets containment. Absolute file
  paths, network URLs, traversal, and nested-link escape are rejected.
- An external image already inside an allowed mounted SourceAssets domain is
  referenced without copying. An external image outside registered source
  domains follows the explicit ingestion workflow and cannot choose a
  destination implicitly from the runtime asset path.
- Data-URI, GLB, and FBX embedded images are extracted transactionally into a
  selected writable SourceAssets domain before Texture2D provenance is
  published.
  Extracted bytes are authoritative editor source data; a temporary file is
  never the only retained source.
- Embedded-image extraction uses deterministic paths derived from the root
  model's chosen source directory, image source identity, sanitized display
  name, and detected extension. Identical existing bytes are reused; different
  existing bytes fail preflight.
- Static-mesh geometry DDC keys continue to depend only on geometry-affecting
  source bytes, importer behavior, mesh settings, builder/schema versions, and
  platform. Texture changes do not invalidate DMSH geometry merely because
  they are in the same model bundle.
- A separate import-manifest fingerprint covers the root source plus ordered
  material and image dependencies. Reimport uses it to detect changes to the
  complete authored model bundle.
- Paths, timestamps, Assimp allocation order, temporary extraction paths, and
  asset destination names never enter semantic content hashes.

### Generated asset layout and identity

- The import dialog selects one root `DStaticMesh` asset path and previews every
  generated package before mutation.
- Generated outputs use deterministic child directories by default:

  ```text
  <Destination>/<ModelName>
  <Destination>/<ModelName>_Materials/<MaterialName>
  <Destination>/<ModelName>_Textures/<ImageName>_<Usage>
  ```

- Sanitization is deterministic, case-insensitive collisions receive stable
  numeric suffixes, and the preview displays final names. An existing unrelated
  asset at any proposed path fails the complete import before source or package
  publication.
- Texture deduplication keys include encoded image identity plus semantic
  build usage and color-space requirements. One source image used as both color
  and linear data produces separate Texture2D assets unless Texture2D later
  supports multiple views over one source/platform payload.
- Sampler and UV-transform differences do not duplicate image assets; they
  remain material-binding state.
- Durin provides one engine-owned standard imported-surface `DMaterial`.
  Generated `DMaterialInstance` assets parent that material and override only
  supported imported parameters.
- Every used static-mesh slot receives the generated instance corresponding to
  its reconciled slot identity as `DefaultMaterial`. Component overrides keep
  their existing precedence.

### Import manifest and reimport

- `DStaticMesh` remains the root referenced asset and owns optional editor
  import-manifest metadata. No runtime `DModel` asset is introduced.
- The manifest records:
  - root and dependency provenance plus the complete fingerprint;
  - importer and material-mapper versions;
  - reconciled slot GUID to generated material-instance reference;
  - stable imported-image identity and semantic usage to generated texture
    reference;
  - the last successfully imported normalized material values needed for
    diagnostics and deterministic update;
  - warnings accepted during the last successful import.
- The manifest is editor provenance and may be stripped from cooked packages.
  Cooked packages retain ordinary StaticMesh-to-material and
  material-to-texture references.
- Reimport parses and validates the entire candidate scene before mutating any
  loaded object. Existing slot reconciliation runs before generated-material
  reconciliation so a preserved slot GUID remains the primary material
  identity.
- Generated material instances are importer-managed. Reimport replaces their
  imported parameter set with the new normalized values. Users who require
  durable customization create a child material instance or assign a component
  override; the editor labels this policy before first manual edit.
- Reimport updates only assets referenced by the prior manifest. It never
  adopts a same-named unrelated asset.
- A generated asset no longer used by the source becomes an explicit orphan in
  the reimport report. It is not deleted automatically.
- A missing, moved, incompatible, or manually deleted generated asset is an
  actionable candidate-validation error unless the user explicitly selects a
  recreate operation before publication.
- Asset moves preserve object references and do not change imported semantic
  identity. Moving the root StaticMesh does not implicitly reorganize generated
  assets or source-library files.

### Transaction, failure, and threading

- Initial import and reimport use a reusable multi-asset import transaction
  with explicit prepare, validate, stage, publish, and rollback phases.
- Prepare performs source parsing, dependency resolution, decoding validation,
  output naming, collision checks, resource-budget checks, and candidate asset
  construction without modifying existing packages.
- Publish makes source ingestion/extraction, generated packages, root package,
  manifest, and required DDC objects visible as one logical result. A failure
  leaves previous packages, object state, source files, and registry state
  unchanged and removes only files created by that attempt.
- Rollback never deletes a pre-existing byte-identical source file or package.
- Background work may parse, hash, decode, and build immutable candidate data.
  Package creation, reflected object mutation, asset-registry publication, and
  final save occur on the game/editor thread.
- Async result state uses the task lifecycle and cancellation contract selected
  by `Documentation/Plans/MultithreadingV1.md`. Closing the dialog or project
  cancels or safely abandons unpublished candidates.
- Errors carry stage, source identity, asset path when known, and a stable
  category. Warnings are retained separately and never convert a failed
  required operation into success.

### Surface and texture mapping

- The first usable milestone maps:
  - base-color factor to `BaseColor`;
  - base-color image to `BaseColorTexture`;
  - base-color alpha times material opacity to `Opacity`;
  - color images to `ETextureUsage::Color` with sRGB enabled.
- The first milestone accepts only effective opaque rendering. A source
  material requiring mask or blend is imported with a visible warning and a
  deterministic opaque fallback until the corresponding pipeline stage lands.
- PBR expansion maps normal images to Normal/linear, metallic-roughness and
  occlusion images to DataMask/linear, and emissive images to Color/sRGB.
- The Material System plan owns the generalized PBR parameter schema, static
  material properties, render representation, shader maps, and opaque/masked/
  translucent pipeline policies. This plan owns source-to-schema mapping,
  generated instance values, diagnostics, and end-to-end import acceptance.
- The Texture Support plan owns decoding, mip generation, compression,
  platform data, readiness, and residency. This plan selects usage settings and
  validates that generated textures reach the required ready state.
- Until sampler state becomes material binding data, non-repeat or non-linear
  source samplers warn and use the documented shared renderer sampler. The
  importer does not bake a sampler workaround into UVs.

## Current Foundations and Gaps

### Foundations

- `AssetImport` is an editor-only Assimp adapter and already imports static
  positions, generated or source normals, tangents with handedness, four UV
  channels, vertex color zero, indices, node instances, and material indices.
- `FImportedMaterialSlot` filters unused source materials and carries exact
  source names and indices used by deterministic material-slot reconciliation.
- `DStaticMesh` owns persistent slot GUIDs, optional default
  `DMaterialInterface` references, component update routing, DDC payloads,
  source diagnostics, reimport, and cooking.
- `DTexture2D` imports PNG, JPEG, BMP, and TGA files and has semantic usage,
  explicit sRGB, mip, compression, DDC, cook, and render-resource support.
- `DMaterial` and `DMaterialInstance` provide canonical parameter definitions,
  GUID-based overrides, serialization, dependency tracking, garbage-collection
  reachability, editor mutation, and render-thread snapshots.
- The renderer displays vertex color, base color, and one base-color texture on
  a static mesh and supplies safe fallback textures.
- Asset creation, package save/unload, content-browser import dialogs, asset
  move/delete contributors, and focused import/material/texture tests already
  exist.

### Gaps

- `FImportedSceneData` contains only geometry and material slots; Assimp
  material properties, images, texture bindings, samplers, and dependency
  diagnostics are discarded.
- StaticMesh import creates and saves exactly one asset and assigns no default
  material.
- Only the root model file is copied and hashed. A copied `.gltf` can lose its
  `.bin` and external images, and embedded images have no durable Texture2D
  source representation.
- Texture2D import accepts a physical image file and publishes one asset at a
  time; there is no encoded-image or shared-source entrypoint for a model
  import transaction.
- There is no multi-package asset import transaction, generated-output
  manifest, importer-managed material policy, deterministic reimport mapping,
  or orphan report.
- The current canonical material schema and renderer cannot consume normal,
  metallic, roughness, occlusion, emissive, UV transform, sampler, alpha mode,
  alpha cutoff, or two-sided state.
- The import dialog previews only one `.dasset` plus one copied model source
  and cannot show generated materials, textures, dependencies, warnings, or
  collisions.
- Async mesh import reports one monolithic result and does not expose
  cancellation, progress, or structured failure state.

## Implementation Stages

### Stage 0: Freeze fixtures, schemas, and cross-plan boundaries

Dependencies: none.

- [ ] Add checked-in glTF fixtures covering base-color factor, external image,
  data URI, GLB embedded image, duplicate image use, multiple materials,
  duplicate material names, unused materials, UV set selection, sampler modes,
  alpha modes, double-sided state, and required/optional extensions.
- [ ] Add focused FBX fixtures for the material properties Assimp exposes
  consistently in the pinned version, plus one unsupported DCC-specific
  material fixture.
- [ ] Freeze allocation limits for material, image, texture binding,
  dependency, encoded-byte, decoded-pixel, and diagnostic counts.
- [ ] Freeze `FImportedMaterial`, `FImportedImage`,
  `FImportedTextureBinding`, `FImportedSampler`,
  `FImportedDependency`, and structured diagnostic fields.
- [ ] Freeze exact default values and glTF/FBX-to-normalized-property mapping,
  including every lossy fallback and its warning category.
- [ ] Freeze the editor-only StaticMesh import-manifest schema, cook stripping
  policy, compatibility behavior for existing StaticMesh packages, and mapper
  version contribution.
- [ ] Freeze deterministic asset naming, case-insensitive collision handling,
  texture deduplication, generated-asset ownership, reimport update, recreation,
  and orphan policies.
- [ ] Record the exact dependency on unified mount stages and the temporary
  sequencing rule if this plan begins before mounted source paths land.
- [ ] Characterize current package/registry behavior under multi-package save
  failure before selecting transaction primitives.

#### Acceptance Gate

- Normalized fixture outputs, allocation limits, schema/version ownership,
  generated-output identity, source dependency policy, and rollback semantics
  are executable and contain no unresolved representation or ownership
  decision.

### Stage 1: Add normalized material and dependency import data

Dependencies: Stage 0.

- [ ] Add the selected import-only material, image, binding, sampler,
  dependency, and diagnostic types to `AssetImport`.
- [ ] Parse Assimp materials into the format-neutral representation without
  exposing `aiMaterial`, `aiTexture`, or Assimp enums in public headers.
- [ ] Resolve material texture references to deduplicated imported-image
  indices and retain source material indices independently from compact used
  slots.
- [ ] Preserve existing material-slot filtering and duplicate-name behavior
  while proving that every mesh source material index resolves to exactly one
  slot or a defined default.
- [ ] Parse external image metadata, glTF data URIs, GLB embedded images, and
  Assimp embedded texture payloads without decoding them in the geometry loop.
- [ ] Enumerate root and sidecar dependencies with normalized identities,
  hashes, sizes, and containment diagnostics.
- [ ] Validate all indices, factors, transforms, byte ranges, MIME/extension
  agreement, and allocation limits before returning a successful scene.
- [ ] Increment the importer version for every normalized-output behavior
  change and add exact synchronous/asynchronous result-equivalence tests.

#### Acceptance Gate

- Every Stage 0 source fixture produces the frozen format-neutral scene and
  diagnostic result, malformed or over-budget inputs fail without partial
  output, and no runtime module depends on Assimp material types.

### Stage 2: Build portable image-source and multi-asset transaction primitives

Dependencies: Stages 0 and 1; unified mount registry, provenance, and
reference-or-ingest semantics from its owning plan.

- [ ] Add an editor asset-build entrypoint that validates encoded image bytes
  and builds a `DTexture2D` candidate with explicit usage and sRGB settings
  without requiring an unrelated temporary authoritative source path.
- [ ] Reference external image dependencies already inside an allowed
  SourceAssets domain and transactionally ingest external dependencies only
  through the selected writable-mount workflow.
- [ ] Extract embedded images to deterministic writable SourceAssets
  locations and reuse only byte-identical existing sources.
- [ ] Implement the reusable prepare/stage/publish/rollback transaction for
  several packages, source files, DDC objects, and loaded-object mutations.
- [ ] Make package and source collision preflight complete before the first
  write.
- [ ] Add injected-failure coverage for directory creation, source write,
  decode, Texture2D build, DDC publication, package save, registry publication,
  and final root-package save.
- [ ] Prove rollback preserves every pre-existing file and restores loaded
  object state and registry visibility exactly.

#### Acceptance Gate

- A synthetic import can publish several Texture2D packages and extracted or
  referenced source images atomically, and every injected failure leaves no
  new visible asset, source file, dirty package, registry row, or mutated
  pre-existing object.

### Stage 3: Deliver the opaque base-color end-to-end workflow

Dependencies: Stages 1 and 2; current Texture2D and material vertical slices.

- [ ] Add the engine-owned standard imported-surface material with stable
  asset identity and canonical current parameter definitions.
- [ ] Add deterministic output planning for the root StaticMesh, used material
  instances, and semantically deduplicated textures.
- [ ] Create one Texture2D per planned color image with
  `ETextureUsage::Color`, explicit sRGB, and the selected compression settings.
- [ ] Create one generated material instance per used imported material, set
  its parent, and map base-color factor, base-color texture, and effective
  opacity through canonical parameter GUIDs.
- [ ] Assign generated instances to reconciled StaticMesh slot
  `DefaultMaterial` references before root-package publication.
- [ ] Persist the initial import manifest and complete dependency fingerprint
  on the StaticMesh asset while retaining ordinary runtime asset references.
- [ ] Extend the import dialog to preview all assets and source operations,
  show warnings/collisions, and require no second manual import or assignment
  action.
- [ ] Add an editor integration test that imports an opaque textured GLB,
  saves/unloads/reloads all outputs, places the mesh on a component, and
  resolves the imported texture through the default slot.
- [ ] Add a rendered Vulkan fixture proving the generated material samples the
  expected sRGB texels and factor after package reload.

#### Acceptance Gate

- Importing the canonical opaque GLB once creates a saved StaticMesh, generated
  texture and material-instance assets, populated default slots, and a valid
  manifest; the reloaded mesh renders the expected base-color result without
  manual authoring, fallback orange, or source decoding at runtime.

### Stage 4: Make generated assets reimportable and manageable

Dependencies: Stage 3.

- [ ] Reimport the complete dependency graph into immutable candidates before
  changing the root mesh or any generated asset.
- [ ] Reconcile source materials through preserved slot GUIDs and update the
  same manifest-referenced material instances after source reorder and rename.
- [ ] Reconcile imported images by stable source identity and semantic usage,
  update the same Texture2D assets when bytes change, and avoid duplicates when
  bindings reorder.
- [ ] Preserve component overrides and the existing conservative static-mesh
  slot reconciliation rules.
- [ ] Detect user moves, missing assets, incompatible replacements, changed
  ownership, and source dependency changes with explicit recreate or repair
  actions.
- [ ] Report removed generated assets as orphans and provide selection/reveal;
  do not delete them as part of ordinary reimport.
- [ ] Label generated material instances as importer-managed and explain that
  reimport replaces imported values.
- [ ] Add package round-trip and transaction tests for reorder, rename, add,
  remove, shared image, changed image, missing sidecar, moved generated asset,
  manually deleted generated asset, collision, and injected save failure.

#### Acceptance Gate

- Two consecutive unchanged imports are idempotent, a changed source updates
  only the expected existing assets and slot assignments, removed outputs are
  reported without deletion, and every failed reimport preserves the complete
  previously renderable asset graph.

### Stage 5: Expand generated materials to the PBR surface contract

Dependencies: Stage 4; the Material System plan's PBR inputs, static material
properties, versioned render representation, normal mapping, and required
opaque/masked pipeline support.

- [ ] Extend the standard imported-surface material schema with the stable
  imported PBR parameter and static-property identities selected by the
  Material System plan.
- [ ] Map metallic and roughness factors and texture channels, normal texture
  and scale, occlusion texture and strength, emissive factor and texture, UV
  selection/transform, alpha mode/cutoff, and double-sided state.
- [ ] Create generated textures with Color/sRGB, Normal/linear, or
  DataMask/linear settings according to binding semantics.
- [ ] Define and implement the selected policy for glTF packed
  metallic-roughness and occlusion channels without silently treating color
  data as sRGB.
- [ ] Honor material sampler and UV-transform state through renderer material
  bindings once available; retain explicit warnings for any still-unsupported
  state.
- [ ] Implement opaque and masked import parity and the deterministic fallback
  for blend materials until translucent rendering reaches its Material System
  acceptance gate.
- [ ] Add rendered-image fixtures for dielectric, metallic, roughness
  extremes, normal mapping including mirrored transforms, AO, emissive,
  multiple UV sets, alpha mask, double-sided state, and fallback values.
- [ ] Add FBX mapping fixtures that distinguish exact mappings from warned
  approximations.

#### Acceptance Gate

- The canonical supported glTF PBR fixture renders within defined image
  tolerances after import and reload, every texture has the correct
  usage/color-space build settings, masked and double-sided policy is effective,
  and FBX approximations are never silent.

### Stage 6: Add production editor feedback and async lifecycle

Dependencies: Stages 3 and 4; Stage 5 for PBR-specific presentation; applicable
Multithreading V1 task-state stages.

- [ ] Show parse, dependency, texture-build, material-build, mesh-build, save,
  and rollback phases with structured progress.
- [ ] Add cancellation before publication and safe result abandonment during
  dialog close, project close, or editor shutdown.
- [ ] Present source dependency locations, reference-versus-ingest actions,
  generated output paths, texture usage/color space, material mappings,
  unsupported features, fallbacks, and total resource estimates before import.
- [ ] Persist accepted non-fatal warnings in the import manifest and surface
  changed warnings on reimport.
- [ ] Add content-browser relationships from the root StaticMesh to generated
  assets and from generated assets back to their importer-managed root without
  making a runtime reverse index authoritative.
- [ ] Add repair/recreate commands that run through the same candidate and
  transaction path as ordinary reimport.
- [ ] Add editor tests for cancellation, project shutdown, warning review,
  collision navigation, dependency repair, generated-asset moves, and orphan
  reveal.

#### Acceptance Gate

- Large imports remain responsive, cancellation never publishes a partial
  bundle, every failure identifies its phase and source/output, and users can
  inspect and repair the complete generated graph without filesystem guesswork.

### Stage 7: Close compatibility, cooking, and lasting documentation

Dependencies: all required preceding stages for the selected shipped surface
subset.

- [ ] Verify existing geometry-only StaticMesh packages load unchanged with an
  absent import manifest and retain their current material slots and DDC keys.
- [ ] Cook imported StaticMesh, material-instance, material, and Texture2D
  dependencies and prove cooked runtime loading requires neither source
  libraries, source files, Assimp, nor image decoders.
- [ ] Inspect runtime target dependency graphs and deployment output for
  forbidden editor/import libraries.
- [ ] Add import-manifest and generated-asset diagnostics to asset-registry
  inspection without retaining editor provenance in default cooked packages.
- [ ] Run focused native suites, complete Engine tests, a successful full
  `all` build, hidden-window editor import/reload smoke coverage, and cooked
  runtime smoke coverage through the repository DurinDevTool workflow.
- [ ] Move lasting import representation, material mapping, source dependency,
  reimport, generated-asset, and runtime/cook contracts into their owning
  Runtime or Editor Architecture documents.
- [ ] Update the Material System, Texture Support, Unified Mount Source
  References, and Multithreading V1 plans only for work whose owning acceptance
  evidence actually passed.
- [ ] Archive this plan only after every required acceptance gate and lasting
  documentation handoff is complete.

#### Acceptance Gate

- Existing assets remain compatible, the complete generated model graph cooks
  and runs without authoring dependencies, runtime-only builds exclude import
  code, all required validation passes, and no lasting implemented contract
  remains owned only by this active plan.

## Validation Matrix

| Area | Required cases | Evidence |
| --- | --- | --- |
| Normalized import | glTF external/data URI/GLB images, multiple/unused/duplicate-name materials, FBX exact and lossy mappings | AssetImport unit tests with frozen normalized values and diagnostics |
| Geometry compatibility | transforms, mirrors, normals/tangents, four UVs, vertex colors, sections, material indices | Existing plus extended AssetImport and StaticMesh tests |
| Dependency safety | relative sidecars, mounted source references, ingestion, embedded extraction, traversal, absolute/network URI, missing bytes, link escape | Mount/source and import dependency tests |
| Resource budgets | material/image/binding counts, encoded and decoded byte limits, dimensions, malformed payloads | Importer and Texture2D rejection tests |
| Naming and deduplication | invalid names, case collisions, duplicate images, same image with color/data uses, existing identical source, unrelated asset collision | Output-planner unit and asset integration tests |
| Transactionality | failure at every source, DDC, package, registry, object mutation, and root-save boundary | Injected-failure filesystem/package tests |
| Initial import | one action creates mesh, textures, instances, defaults, manifest, and saves all packages | Editor integration and package reload test |
| Reimport | unchanged, reorder, rename, add/remove material, image change, moved/missing output, orphan, failed save | Manifest/reconciliation integration tests |
| Material mapping | factors, color space, normal/data usage, packed channels, UV set/transform, sampler, alpha, double sided | Material/Texture unit tests and rendered-image fixtures |
| Render thread | ready/fallback texture transitions, imported material snapshots, stale update rejection | Renderer and Vulkan tests |
| Serialization | asset references, manifest versions, generated outputs, move/rename, legacy absent manifest | Package round-trip and compatibility fixtures |
| Cooking | full dependency closure, stripped editor provenance, packaged payloads, no source/import modules | Cook integration and runtime-only dependency tests |
| Editor lifecycle | progress, cancellation, shutdown, diagnostics, repair, reveal | Editor smoke and focused UI-model tests |

## Definition of Done

- A user can select a supported static glTF/GLB or supported-subset FBX source,
  choose one StaticMesh destination, review all planned source and asset
  outputs, and complete one import action.
- The action produces one saved StaticMesh, every required Texture2D, one
  generated material instance per used source material, assigned mesh default
  slots, and a versioned editor import manifest.
- Reloading the packages and assigning only the StaticMesh to a component
  renders the supported imported appearance without fallback orange or manual
  material/texture work.
- External and embedded dependencies remain portable, safe, hash-verified, and
  reimportable after moving the workspace to another absolute path.
- Unchanged reimport is idempotent; changed reimport updates the same generated
  assets transactionally; failed reimport leaves the previous complete graph
  intact.
- Existing geometry-only meshes remain loadable and render as before.
- Cooked output loads without source models, source images, mounted SourceAssets
  registration, Assimp, or editor image decoders.
- Required focused tests, full build, editor smoke, rendered validation, and
  cooked runtime validation succeed through the repository DurinDevTool.
- Long-lived implemented contracts are documented in their owning domains and
  this plan is archived with completion evidence.

## Deferred Follow-ups

- A `DModel` or prefab asset preserving node hierarchy, transforms, and mesh
  instancing.
- Skeletal meshes, skeletons, animation clips, morph targets, and character
  import.
- Collision and lightmap generation, authored and generated LOD workflows, and
  model optimization.
- Full translucent rendering and sorting beyond the Material System plan's
  initial policy.
- Arbitrary DCC shader graph translation and material graph generation.
- Source-image payload sharing across different Texture2D usage views.
- Automatic deletion of generated orphans after a dedicated impact,
  reference-safety, undo, and recovery design.
- OBJ/MTL material parity and additional interchange formats after glTF and FBX
  acceptance coverage is stable.

## Related Documentation

- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Plans/SourceLibraryReferences.md`
- `Documentation/Plans/MultithreadingV1.md`
- `Documentation/Development/Build/BuildAndRun.md`

## Related Code

- `Engine/Source/Editor/AssetImport/Public/AssetImport.h`
- `Engine/Source/Editor/AssetImport/Private/AssetImport.cpp`
- `Engine/Source/Editor/EngineAssetBuild/`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInstance.h`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/`
- `Engine/Tests/Native/EngineTests/Private/Texture/`
