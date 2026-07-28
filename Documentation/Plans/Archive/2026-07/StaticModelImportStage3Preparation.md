# Static Model Import Stage 3 Preparation Plan

Summary: Refactor the completed static-model import foundations into isolated format adapters and a single-pass resolved transaction plan before the end-to-end asset workflow begins.

Last reviewed: 2026-07-28

Status: Archived
Completed: 2026-07-28

## Current Status

Stages 0 through 4 are complete. The importer now has independent private
glTF/GLB and Assimp-backed FBX adapters, one resolved transaction plan, and
rollback tests at the real decode, candidate-build, DDC, source, package, and
registry boundaries. `ReadyToUseStaticModelImport.md` Stage 3 is unblocked.

The current implementation has the intended external architecture: one
format-neutral `FImportedSceneData` boundary, editor-only source parsing,
runtime asset types that do not depend on Assimp, encoded-byte Texture2D
construction, and reusable atomic package publication. The review before
Stage 3 identified four implementation risks that should not receive more
feature work in their current form:

- `AssetImport.cpp` owns glTF/GLB container parsing, glTF material parsing,
  Assimp material mapping, Assimp image handling, geometry traversal, import
  orchestration, and async handoff in one translation unit.
- glTF metadata and Assimp geometry are correlated through an implicit mesh
  ordering assumption rather than an explicit adapter result and validation
  contract.
- `FMultiAssetImportTransaction::Prepare` repeats package, source, mount,
  collision, and byte-resolution work between preflight and candidate
  construction.
- decode, Texture2D build, and DDC-publication failure injection currently
  occurs before the same composite `BuildFromEncodedBytes` call and therefore
  does not prove rollback from three distinct completed-work boundaries.

Stage 3 of `ReadyToUseStaticModelImport.md` remains blocked until every
acceptance gate in this plan passes. No user-visible import capability is added
by this plan.

### Stage 0 Decision Record

- Private format entrypoints will consume an `FStaticModelImportContext`
  containing the physical root path, logical root source, authoritative root
  bytes, and the shared result/diagnostic sink. `ImportGltfFormat` owns glTF
  metadata normalization; `ImportAssimpFormat` owns non-glTF material
  normalization; `ImportAssimpGeometry` owns node and vertex extraction.
- glTF material identity no longer uses source mesh declaration order or
  Assimp's local material index. The glTF adapter traverses the active scene
  nodes in source order, appends each first-seen source mesh's primitives, and
  produces the source material index for each Assimp mesh allocation. Shared
  geometry indexes that explicit projection with `aiNode::mMeshes`; repeated
  node instances therefore reuse the same source material identity.
- `PrimitiveProjection.gltf` proves why count equality is insufficient. Its
  source primitive order is `2, 0, 1`, its scene visits mesh instances with
  required materials `1, 2, 0, 1`, and the Stage 0 importer incorrectly
  produces `2, 0, 1, 2` because Assimp allocates meshes by first scene use.
- Correcting that projection and replacing the raw FBX diagnostic changes
  normalized output. Stage 1 increments `StaticModelImporterVersion` from 2 to
  3 and updates the frozen expected output in the same commit.
- Initial Stage 1 validation disproved the earlier candidate of using
  `aiMesh::mMaterialIndex` directly: Assimp's glTF lazy dictionaries reorder
  both meshes and materials by first reference, so that index is local to the
  Assimp scene rather than the glTF source material table. The selected
  first-seen mesh projection mirrors the pinned importer allocation contract
  without relying on material names, which may be duplicated.
- Assimp does not provide a stable public DCC-schema contract for the raw
  `3dsMax|main` compound marker. Stage 1 removes the byte scan and emits only a
  generic lossy/unsupported-material warning derived from structured Assimp
  shading and texture properties; it does not promise exporter identification.
- Transaction resolution will produce `FResolvedImportPlan` containing
  `FResolvedPackage`, deduplicated `FResolvedSource`, `FResolvedTexture`, the
  root package, and source filesystem observations. Resolution is pure with
  respect to packages, DDC, registry, loaded objects, and destination files.
- Texture phase tests will use a private `FTextureBuildOperations` seam with
  decode, candidate construction, and DDC publication operations. The
  production implementation is the default; tests inject a scoped
  implementation through private test access rather than a workflow-facing
  setter.
- The dependency baseline is `EngineAssetBuild -> AssetImport + AssetCore`
  privately and `EngineAssetBuild -> Engine` publicly. `AssetImport` links
  Assimp privately. Runtime `Engine` references `AssetImport` only as an
  optional editor-capable dependency; runtime-only configurations must continue
  to omit it.

## Goal

- Give glTF/GLB and Assimp-backed FBX material import independent, testable
  editor-only format adapter units.
- Keep one public static-model import entrypoint and one format-neutral result
  so format decisions do not spread into asset construction, the editor
  dialog, manifest handling, reimport, or runtime code.
- Convert portable texture requests into one immutable resolved transaction
  plan before creating assets, publishing DDC objects, staging sources, or
  mutating loaded objects.
- Make each injected failure boundary correspond to the named operation having
  performed the work that rollback is expected to undo.
- Preserve all accepted Stage 1 and Stage 2 behavior unless a separately
  recorded defect requires a versioned normalized-output change.

## Scope

- Private structure of the editor-only `AssetImport` module.
- glTF 2.0 JSON and GLB container metadata parsing, glTF image/material
  normalization, and glTF primitive-to-Assimp-geometry projection.
- Assimp-backed FBX material/image normalization and shared Assimp geometry
  extraction.
- Internal import diagnostics, resource-budget helpers, encoded-image
  validation, dependency resolution, and adapter dispatch needed to establish
  clean ownership.
- `FMultiAssetImportTransaction` request resolution, preflight,
  materialization, publication, rollback, and failure-injection boundaries.
- Narrow Texture2D build seams required to distinguish decode, candidate
  build, and derived-data publication in tests.
- Focused behavior-parity, transaction, and module-dependency validation.

## Non-Goals

- Creating generated Texture2D or material-instance output plans for a model.
- Assigning StaticMesh default materials or adding the standard imported
  surface material.
- Adding the StaticMesh import manifest, reimport reconciliation, repair, or
  orphan handling.
- Expanding the supported glTF extension or FBX material-property set.
- Adding PBR renderer inputs, masked or translucent rendering, sampler
  bindings, or UV-transform rendering.
- Replacing Assimp as the shared geometry reader.
- Changing the public `FImportedSceneData` schema solely to make file
  organization more convenient.
- Refactoring unrelated AssetCore package loading, package moving, package
  deletion, Texture2D import UI, or asynchronous task lifecycle.
- Imposing a source-line target that encourages mechanical file splitting
  without establishing coherent ownership.

## Design Decisions and Invariants

### Format adapter boundary

- `ImportFromFile` and `ImportFromFileAsync` remain the only public
  static-model import entrypoints.
- The orchestrator identifies the source format, reads the authoritative root
  bytes once, records root provenance, selects one private format adapter, asks
  the shared Assimp geometry reader for geometry where applicable, validates
  the combined result, and returns `FImportedSceneData`.
- The glTF/GLB adapter exclusively owns:
  - GLB chunk and glTF JSON parsing;
  - glTF buffers, images, textures, samplers, materials, and extensions;
  - exact glTF source material indices;
  - an explicit projection describing how source primitives correspond to
    Assimp-produced geometry.
- The Assimp material adapter exclusively owns non-glTF material and image
  normalization, including the supported FBX subset and warned lossy
  fallbacks.
- Shared Assimp geometry extraction owns node traversal, transform baking,
  vertex attributes, indices, and source material indices. It does not inspect
  glTF JSON or map FBX material properties.
- Common private helpers may own diagnostics, resource limits, dependency
  containment, hashes, URI normalization, base64 decoding, and encoded-image
  validation. Common helpers do not branch on glTF or FBX material semantics.
- No glTF JSON field name appears in the Assimp adapter or shared geometry
  reader. No `aiMaterial`, `aiTexture`, or Assimp material enum appears in the
  glTF adapter or a public header.
- Format adapters produce normalized data only. They do not create packages,
  assets, DDC entries, render resources, manifests, or editor widgets.

### glTF geometry correlation

- Correlation between glTF primitives and Assimp meshes is an explicit adapter
  output with a documented ordering and cardinality invariant.
- The importer fails with a structured diagnostic when Assimp geometry cannot
  be mapped one-to-one to the supported glTF primitive projection. It never
  silently assigns a material by coincidental compact index.
- Tests include multiple meshes, multiple primitives, unused materials, and
  node instances so equality of total counts alone cannot satisfy the
  projection contract.
- If the pinned Assimp behavior cannot support a stable projection, Stage 0
  records and selects a different geometry-correlation strategy before the
  adapter split proceeds.

### FBX and DCC-specific diagnostics

- The importer does not scan raw FBX bytes for exporter- or DCC-specific
  marker strings.
- A DCC-specific warning is emitted only from structured information exposed
  by Assimp or another selected FBX parser. If the property cannot be detected
  reliably, the generic documented lossy-material warning is used instead.
- FBX mappings remain explicit approximations; missing information is not
  presented as exact glTF metallic/roughness data.

### Resolved transaction plan

- Request resolution produces an internal immutable plan containing final
  package paths, physical package destinations, source paths, physical source
  destinations, source bytes or mounted references, hashes, reuse/write
  decisions, staging destinations, Texture2D settings, and the selected root
  package.
- Complete collision, containment, mount-policy, duplicate-source, byte
  identity, and staging-path validation finishes before the first asset is
  created or DDC object is published.
- Candidate construction consumes the resolved plan. It does not resolve the
  same path, reread the same external file, or repeat collision policy.
- A changed external file between resolution and publication fails with a
  source-change diagnostic rather than silently building bytes that differ
  from the resolved plan.
- `Prepare`, `Stage`, `Publish`, and `Rollback` retain their externally visible
  lifecycle. Their implementation delegates to phase-specific private helpers
  rather than containing a second workflow.
- The existing rule remains: dependency packages publish before the designated
  root package, and registry visibility occurs only after all package files are
  in place.

### Failure injection and errors

- Decode failure is injected after encoded bytes and source identity have been
  resolved, at the actual decoder boundary.
- Texture candidate failure is injected after successful decode and before a
  usable Texture2D candidate exists.
- derived-data publication failure is injected after a valid candidate payload
  exists, at the DDC write/publication boundary.
- Package staging, dependency-package publication, root-package publication,
  registry publication, source staging, source publication, and loaded-object
  mutation retain distinct failure coverage.
- Workflow-facing production APIs do not expose a public setter whose only
  purpose is tests. Tests use a private injected-operations seam or the
  existing low-level package-save options where the callback is itself part of
  the reusable transaction contract.
- Internal failures carry a stable phase/category plus a message and relevant
  source or asset identity. Existing public string results may remain as a
  compatibility projection during this refactor.

### Compatibility and versioning

- File moves and responsibility extraction alone do not increment
  `StaticModelImporterVersion`.
- Any intentional change to normalized scene output, diagnostics, dependency
  identity, material-slot identity, hashes, or accepted/rejected source data
  is recorded in this plan before implementation, updates frozen fixture
  expectations, and increments the owning importer version where required.
- Existing geometry-only formats continue to import through the Assimp
  geometry path without gaining material-parity promises.
- Runtime Engine and cooked targets do not acquire dependencies on Assimp,
  JSON parsing, image decoding, or `EngineAssetBuild`.

## Current Foundations and Gaps

### Foundations

- `FImportedSceneData` already separates images, materials, material slots,
  meshes, dependencies, and diagnostics.
- glTF/GLB material metadata is already parsed independently from Assimp
  material objects.
- Assimp remains a functioning geometry reader for supported static formats.
- Stage 0 fixtures freeze normalized glTF and FBX behavior.
- `DTexture2D::BuildFromEncodedBytes`, `Asset::SavePackagesAtomically`, and
  `Asset::DiscardUnpublishedPackage` provide the required behavior, although
  their internal phase boundaries need refinement.
- Focused import, texture-build, package, and injected-rollback tests already
  exist.

### Gaps

- The existing logical format separation is expressed as private functions in
  one large translation unit rather than module-level ownership.
- glTF parsing and Assimp geometry correlation depend on an under-specified
  ordering assumption.
- The FBX-specific `3dsMax|main` warning is detected by scanning raw source
  bytes.
- Transaction preflight stores only partial results, causing the
  materialization loop to redo resolution and validation.
- Three named Texture2D failure points are checked before one composite
  operation and therefore do not validate distinct rollback states.
- The transaction returns mainly free-form error strings, which will not scale
  cleanly to Stage 3 output planning and Stage 4 repair diagnostics.

## Implementation Stages

### Stage 0: Freeze seams and behavior parity

Dependencies: completed Stages 0 through 2 of
`ReadyToUseStaticModelImport.md`.

- [x] Record the current file-level dependency graph for `AssetImport`,
  `EngineAssetBuild`, Texture2D build, and AssetCore bundle save.
- [x] Freeze synchronous and asynchronous normalized outputs for every current
  static-model fixture before moving implementation.
- [x] Add glTF fixtures whose multiple meshes, primitives, node instances, and
  unused materials disprove count-only material correlation.
- [x] Characterize the pinned Assimp mesh ordering and select the explicit glTF
  primitive projection contract.
- [x] Freeze the supported FBX warning output after removing raw-byte
  DCC-marker detection; record whether structured detection or the generic
  lossy warning is selected.
- [x] Define the private adapter interfaces, shared import context, resolved
  transaction-plan representation, typed internal failure record, and
  Texture2D phase seam before moving code.
- [x] Prove current runtime module dependencies contain no source importer or
  image-decoder dependency.

#### Acceptance Gate

- Adapter ownership, glTF geometry correlation, FBX diagnostic policy,
  transaction plan fields, true failure boundaries, behavior-parity fixtures,
  and versioning consequences are selected with no unresolved interface
  decision.

#### Stage 0 Handoff

- Baseline commit: `d1c9fe26`.
- Working set: this plan, `PrimitiveProjection.gltf`,
  `ExpectedNormalized.json`, and `AssetImportTests.cpp`.
- Key decisions: the glTF adapter produces an explicit source-material
  projection in Assimp's first-seen mesh allocation order; shared geometry
  consumes it through Assimp node mesh references. Neither source declaration
  order nor Assimp's locally reordered material index is treated as source
  identity. Stage 1 introduces the three private format/geometry entrypoints
  and increments the importer version for the corrected output.
- FBX decision: delete the `3dsMax|main` byte scan and retain only warnings
  supported by structured Assimp material data.
- Transaction decision: resolve once into `FResolvedImportPlan`; real texture
  phase injection is private and phase-specific.
- Validation: the new fixture reproduced the Stage 0 defect as
  `2, 0, 1, 2` versus required `1, 2, 0, 1`; the frozen characterization and
  complete `AssetImportTests` suite passed. Module metadata inspection
  confirmed Assimp is private to `AssetImport` and no required runtime Engine
  dependency points to `AssetImport` or `EngineAssetBuild`.

### Stage 1: Extract independent format adapters

Dependencies: Stage 0.

- [x] Extract common diagnostics, dependency, URI, hash, budget, base64, and
  encoded-image validation into private format-neutral support.
- [x] Extract GLB container and glTF JSON/image/material parsing into the
  glTF/GLB adapter.
- [x] Extract Assimp material/image normalization into the Assimp-backed
  format adapter and remove the raw `3dsMax|main` byte scan.
- [x] Extract Assimp node and geometry traversal into the shared geometry
  reader without material-format policy.
- [x] Reduce `AssetImport.cpp` to root-byte acquisition, format dispatch,
  adapter/geometry composition, final normalized validation, and async
  lifecycle.
- [x] Implement and validate the selected explicit glTF
  primitive-to-geometry projection.
- [x] Keep the public `AssetImport.h` normalized types and entrypoints stable
  unless Stage 0 recorded an approved semantic correction.
- [x] Run frozen fixture, geometry, material-slot, malformed-input,
  synchronous/asynchronous equivalence, and module-dependency tests.

#### Acceptance Gate

- glTF/GLB and Assimp-backed FBX normalization compile as independent private
  units, all existing accepted outputs remain frozen, count-only glTF
  correlation is impossible, the raw FBX marker scan is absent, and runtime
  targets remain free of importer dependencies.

#### Stage 1 Handoff

- Baseline commit: `f211678b`.
- Working set: `AssetImport.h/.cpp`, new
  `StaticModelImportInternal.h`, `StaticModelImportCommon.cpp`,
  `GltfStaticModelAdapter.cpp`, `AssimpStaticModelAdapter.cpp`,
  `AssimpStaticModelGeometry.cpp`, `StaticMesh.cpp`,
  `AssetImportTests.cpp`, `StaticMeshMaterialTests.cpp`, frozen normalized
  expectations, and this plan.
- Key symbols and decisions: `ImportGltfFormat`, `ImportAssimpFormat`, and
  `ImportAssimpGeometry` are private unit boundaries behind the unchanged
  public entrypoints. The glTF adapter builds its Assimp mesh projection by
  active-scene node traversal and first-seen source mesh order; duplicate
  material names are irrelevant to the mapping.
- Corrected behavior: `PrimitiveProjection.gltf` now produces source material
  indices `1, 2, 0, 1` across node instances. Because normalized output and the
  FBX diagnostic contract changed intentionally, `StaticModelImporterVersion`
  and the StaticMesh Assimp importer version are 3.
- FBX behavior: unsupported structured texture/shading properties emit
  `unmapped-material-properties` or the existing `Phong` warning. No raw FBX
  byte marker is inspected.
- Validation: all 18 `AssetImportTests` passed; the affected StaticMesh source
  provenance test passed; the `Win64-Debug-DurinGame` runtime-only `Engine`
  target built successfully. Targeted source inspection found no glTF schema
  strings outside the glTF adapter, no Assimp material types in the glTF
  adapter or public header, and no remaining `3dsMax|main` code probe.

### Stage 2: Replace repeated preparation with one resolved plan

Dependencies: Stage 1.

- [x] Add the immutable resolved package, source, texture, mutation, and root
  publication records selected in Stage 0.
- [x] Resolve and read every external input once, then retain the exact bytes,
  hash, identity, and filesystem observation required for later
  source-change detection.
- [x] Perform all package/source identity, collision, containment, mount,
  byte-reuse, and staging-path checks while building the resolved plan.
- [x] Make candidate construction consume only resolved records and reject
  changed external inputs without repeating policy resolution.
- [x] Split the current monolithic `Prepare` implementation into named
  resolution, candidate-construction, and final-consistency helpers.
- [x] Preserve mounted reference, explicit ingestion, deterministic embedded
  extraction, byte-identical reuse, root-last publication, registry ordering,
  and caller mutation rollback behavior.
- [x] Add focused tests proving no package creation or DDC write occurs before
  complete preflight and no request is resolved twice.

#### Acceptance Gate

- One resolved transaction plan is the sole input to candidate construction
  and publication, complete preflight precedes every mutation, changed inputs
  fail deterministically, and all successful and rollback Stage 2 fixtures
  retain their previous externally visible results.

#### Stage 2 Handoff

- Baseline commit: `10770985`.
- Working set: `StaticModelImportBuild.cpp`,
  `StaticModelImportBuildTests.cpp`, and this plan.
- Key symbols and decisions: `FResolvedImportPlan` owns resolved package,
  source, texture, mutation, and root records. `ResolvePlan` performs all
  filesystem and policy resolution; `BuildCandidates` uses retained bytes and
  hashes; `ValidatePreparedPlan` checks the materialized package set. Runtime
  publication state is stored separately so the resolved records remain
  unchanged.
- External-input decision: resolution observes size and last-write time both
  before and after its single read. Candidate construction, staging, and
  publication reject a later observation mismatch and never reread the input.
- Validation: all 9 `FStaticModelImportBuildTests` passed, including complete
  collision preflight and changed-input rollback. Targeted source inspection
  confirmed external `LoadBytes` is confined to `ResolvePlan`; candidate
  construction performs no path classification, mount-policy check, or source
  read.

### Stage 3: Make rollback tests exercise real work boundaries

Dependencies: Stage 2.

- [x] Separate or instrument encoded-image decode, Texture2D candidate build,
  and DDC publication so each boundary can fail after all preceding work has
  succeeded.
- [x] Move transaction-only failure controls behind the selected private
  injected-operations seam and remove the workflow-facing public
  `SetFailurePoint` API.
- [x] Retain reusable AssetCore package-save phase injection where it validates
  the generic atomic bundle contract rather than static-model policy.
- [x] Assert the exact filesystem, DDC, loaded-package, dirty-state, registry,
  source-file, and caller-owned mutation state after every failure.
- [x] Add occurrence coverage for multiple textures and packages so failures
  after an earlier dependency succeeded exercise reverse-order rollback.
- [x] Verify destructor rollback, explicit rollback, rejected repeated
  execution, and successful publication ownership.

#### Acceptance Gate

- Every named failure point corresponds to a distinct observed execution
  boundary, the test suite proves rollback after preceding work occurred, and
  test-only controls no longer expand the public model-import workflow API.

#### Stage 3 Handoff

- Baseline commit: `60da30df`.
- Working set: `Texture2D.h/.cpp`, `StaticModelImportBuild.h/.cpp`,
  `StaticModelImportBuildInternal.h`,
  `StaticModelImportBuildTests.cpp`, the Engine native-test CMake file, and
  this plan.
- Key symbols and decisions: the public `BuildFromEncodedBytes` behavior is
  unchanged and delegates to a private hook-aware overload.
  `FTextureBuildOperations` invokes decode, candidate-build, and DDC
  publication controls immediately before their real operations. The
  transaction's public `SetFailurePoint` and failure enum were removed;
  `FMultiAssetImportTransactionTestAccess` is available only through the
  module's private include path.
- Failure-boundary decision: source staging, source publication, package
  staging, dependency publication, root publication, and registry publication
  are distinct categories. Injected messages use stable phase names and carry
  the affected source or texture identity where the transaction owns it.
- Validation: all 12 `FStaticModelImportBuildTests` and all 55 `TextureTests`
  passed. Later-occurrence coverage proves rollback after an earlier texture
  wrote DDC and after an earlier dependency package was published. Explicit
  rollback, destructor rollback, repeated `Prepare`, caller mutation
  restoration, and successful publication ownership are covered.

### Stage 4: Validate the Stage 3-ready foundation

Dependencies: Stages 1 through 3.

- [x] Run all affected AssetCore and Engine focused native suites through the
  repository DurinDevTool workflow.
- [x] Run the complete native suites required by the affected modules and
  distinguish any pre-existing failure from a regression with a focused
  reproduction.
- [x] Build the Runtime Engine target and the affected editor/test targets
  through DurinDevTool.
- [x] Inspect the final module dependency graph for editor/runtime boundary
  regressions.
- [x] Inspect the diff for duplicated format policy, duplicated path
  resolution, raw FBX marker scans, public test-only controls, and unrelated
  refactoring.
- [x] Update `ReadyToUseStaticModelImport.md` with the completed handoff and
  unblock its Stage 3 only after all required validation succeeds.

#### Acceptance Gate

- Frozen import behavior and transaction guarantees pass, the affected targets
  build, module boundaries remain valid, each identified review risk is
  removed, and the original plan can begin Stage 3 without adding features to
  the former monolithic paths.

#### Stage 4 Handoff

- Baseline commit: `9652b327`.
- Working set: this plan and `ReadyToUseStaticModelImport.md`; Stage 4 made no
  functional code changes.
- Validation: the complete `Win64-Debug-DurinEditor-Tests` preset built and all
  726 registered tests passed; three isolation/link cases were reported as
  intentionally skipped. This includes 19 `AssetImportTests`, 28
  `AssetPackageTests`, 43 `StaticMeshTests`, and 56 `TextureTests`. The
  `Win64-Debug-DurinGame` Runtime `Engine` target also built successfully.
- Boundary audit: runtime-only compilation retained the editor guard around
  `AssetImport`; no runtime module gained an `EngineAssetBuild` dependency.
  Public `StaticModelImportBuild.h` contains no failure enum or setter.
  Targeted searches found no raw `3dsMax|main` code probe and no candidate
  construction path that rereads or reclassifies external source input.
- Outcome: every acceptance gate and definition-of-done item passed. The
  preparation plan is completed and the owning workflow may begin Stage 3.

## Validation Matrix

| Area | Required cases | Evidence |
| --- | --- | --- |
| glTF adapter | `.gltf`, `.glb`, external image, data URI, buffer-view image, samplers, transforms, required/optional extensions | Frozen `AssetImport` normalized-output tests |
| glTF correlation | multiple meshes, multiple primitives, node instances, unused and reordered materials | Explicit projection tests that cannot pass by total-count equality |
| Assimp/FBX adapter | exact supported properties, Phong/Lambert fallback, embedded/external texture, unsupported DCC material | Frozen FBX adapter tests and structured warnings |
| Shared geometry | transforms, mirrors, normals, tangents, UVs, colors, sections, source material indices | Existing and focused geometry tests |
| Format isolation | JSON strings, Assimp types, and material policy remain in their owning private units | Dependency/include inspection and targeted source search |
| Resolved plan | mounted reference, ingestion, embedded extraction, identical reuse, collisions, changed external input | Transaction resolution tests |
| Mutation ordering | complete preflight, source staging, DDC, dependency packages, root package, registry, loaded objects | Ordered event trace in focused tests |
| Rollback | real decode/build/DDC/source/package/registry/root/mutation boundaries and later occurrences | Injected-operation state assertions |
| Compatibility | synchronous/asynchronous equality, unchanged importer version when semantic output is unchanged | Frozen fixtures and version assertion |
| Boundaries | runtime Engine and cooked-facing targets exclude importer/decoder/editor modules | Module graph inspection and target build |

## Definition of Done

- glTF/GLB and Assimp-backed FBX import are independent private functional
  units behind the existing unified public entrypoint.
- Shared geometry extraction contains no glTF JSON or FBX material mapping
  policy.
- The glTF primitive-to-geometry mapping is explicit, validated, and covered
  by fixtures stronger than count equality.
- No importer diagnostic depends on scanning raw FBX bytes for a DCC marker.
- A single immutable resolved plan replaces both passes of transaction request
  resolution and collision policy.
- Decode, Texture2D candidate, DDC, source, package, registry, root, and loaded
  mutation failures exercise distinct completed-work states and restore the
  complete prior state.
- Test-only transaction control is private to the implementation/test seam.
- Frozen normalized outputs, focused and required complete tests, affected
  builds, and module-boundary validation pass.
- `ReadyToUseStaticModelImport.md` records the handoff and Stage 3 is explicitly
  unblocked.

## Deferred Follow-ups

- Generated output naming, standard imported material creation, StaticMesh
  defaults, and initial manifest persistence remain Stage 3 of
  `ReadyToUseStaticModelImport.md`.
- Generated-asset reconciliation, repair, and orphan reporting remain Stage 4
  of the owning plan.
- PBR mapping and renderer expansion remain Stage 5 of the owning plan and the
  Material System plan.
- General asynchronous progress, cancellation, and shutdown behavior remain
  the Multithreading V1 integration owned by the later workflow stage.
- Replacing Assimp geometry import or supporting additional interchange
  formats requires a separate plan.

## Related Documentation

- `Documentation/Plans/ReadyToUseStaticModelImport.md`
- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Plans/Archive/2026-07/SourceLibraryReferences.md`
- `Documentation/Development/Build/BuildAndRun.md`

## Related Code

- `Engine/Source/Editor/AssetImport/Public/AssetImport.h`
- `Engine/Source/Editor/AssetImport/Private/AssetImport.cpp`
- `Engine/Source/Editor/EngineAssetBuild/Public/StaticModelImportBuild.h`
- `Engine/Source/Editor/EngineAssetBuild/Private/StaticModelImportBuild.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureBuild.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureBuild.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/StaticModelImportBuildTests.cpp`
