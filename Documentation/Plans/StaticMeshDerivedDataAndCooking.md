# Static Mesh Derived Data and Cooking Plan

Last reviewed: 2026-07-26

## Current Status

Stage 0 is in progress. Durin currently imports every static-mesh source file during
`DStaticMesh::PostLoad`, and material previews create separate transient meshes
directly from engine OBJ files. No static-mesh derived-data key, persistent
platform payload, or cooked-runtime path exists.

This plan selects the long-term source/import/derived/cooked boundaries and
provides an implementation sequence. The shared asset-data lifecycle now fixes
the loose `.dbulk` companion naming, package-relative lookup, logical payload
descriptor, DDC separation, and future-archive boundary. Static-mesh-specific
payload fields, fixtures, and source migration details remain to be frozen.

## Goal

Make static-mesh loading consume a versioned Durin-native binary payload instead
of reparsing OBJ, FBX, glTF, or other interchange formats on every asset load.
Keep source models available to editor reimport without making them runtime
dependencies, cache rebuildable payloads in the project DDC, and produce cooked
payloads that load without source files or Assimp.

## Scope

- Optional, editor-facing source import metadata on `DStaticMesh`.
- A deterministic derived-data key for static-mesh build products.
- A versioned Durin static-mesh binary payload and strict reader validation.
- A content-addressed static-mesh object store under `DerivedDataCache`.
- Editor load policy for cooked data, DDC hits, source rebuilds, and failures.
- A minimal static-mesh cooking path and cooked-runtime load contract.
- Project and engine source-model placement outside runtime-mounted Content.
- Migration of built-in material-preview meshes to shared `/Engine` assets.
- Unit, integration, corruption, determinism, and editor/runtime validation.

## Non-Goals

- Creating a new DCC interchange format to replace OBJ, FBX, or glTF.
- Runtime importing of arbitrary user models.
- Mesh streaming, virtualized geometry, meshlets, or GPU-driven cluster data.
- New LOD generation, collision generation, lightmap unwrapping, or mesh
  optimization algorithms beyond preserving the current build result.
- Asynchronous static-mesh loading in the first implementation.
- A repository-wide cooker, patcher, or final archive format for every asset
  class.
- Texture derived data, except for sharing generic cache or bulk-container
  infrastructure where the ownership boundary requires it.
- Deleting source models from version control.

## Design Decisions and Invariants

### Asset, source, and payload identity

- `DStaticMesh` remains the referenced asset. Levels, components, materials, and
  editor tools never reference OBJ files, DDC object paths, or cooked bulk files.
- OBJ, FBX, and glTF remain authoritative authoring inputs. They are not Durin
  runtime formats.
- Source import metadata is optional. A mesh with a valid native payload can
  load without a source model; a procedural or future generated mesh is not
  required to invent a source path.
- Editor assets retain source provenance for reimport. Cooked runtime packages
  may strip that metadata and never require the source file.
- Source paths are normalized project- or engine-relative paths. Absolute
  workstation paths are rejected as persistent metadata.

### Source directory policy

- New project source models live under project-root `SourceAssets/Models/`.
  Engine-owned source models live under `Engine/SourceAssets/Models/`.
- `SourceAssets` is versioned authoring input but is not a runtime content mount
  and is excluded from cooked output.
- `.dasset` packages remain under mounted `Content`.
- Existing colocated Content sources continue to resolve during migration.
  Migration must not make an existing checkout unloadable before its package
  metadata is updated.

### Native payload format

- The native payload is an internal, chunked binary schema, not a public
  `.dmesh` asset type.
- DDC objects use the `.bin` suffix and begin with a `DMSH` magic value.
- The header records schema version, mesh-builder version, target platform,
  payload flags, uncompressed and stored sizes, and a content checksum.
- Payload chunks describe bounds, material-slot/section mapping, LOD metadata,
  vertex streams, and index buffers. Every count, offset, alignment, and byte
  range is validated before allocation or publication.
- Material slot GUIDs and editable default-material references remain
  `DStaticMesh` asset metadata. The payload stores only the stable identifiers
  and section mapping needed by rendering.
- `FStaticMeshRenderData` remains the runtime representation. Encoding and
  decoding use an explicit disk schema rather than serializing C++ object
  memory, STL layouts, padding, or RHI handles.
- Unknown required chunks or unsupported schema versions fail closed. Unknown
  optional chunks may be skipped only when the header marks them optional.
- The first schema preserves current rendering behavior. Future vertex
  compression or platform-specific layouts require a builder-version or schema
  change and must not silently reinterpret old payloads.

### Derived-data key and storage

- The derived-data key includes source content hash, normalized import settings,
  mesh-builder version, payload schema version, and target platform.
- File timestamps and absolute paths are not part of the content identity.
- Objects are stored at
  `DerivedDataCache/StaticMesh/Objects/<prefix>/<key>.bin`.
- Writes use a same-directory temporary file, flush and close it, then
  atomically replace the final object. Readers never consume a partial write.
- Cache files are rebuildable, ignored output. Missing, stale, truncated,
  corrupt, or unsupported objects are cache misses in the editor.
- Cache cleanup must resolve and verify every deletion target beneath the exact
  static-mesh DDC root.

### Load and failure policy

- Editor load order is:
  1. use an explicitly supplied cooked/native payload;
  2. use a valid DDC object for the computed key;
  3. rebuild from accessible source import data and populate DDC;
  4. fail with an actionable diagnostic.
- A corrupt DDC object is quarantined or safely ignored before a source rebuild;
  it never partially updates the live mesh.
- A valid DDC object permits editor loading when the source file is temporarily
  unavailable, but the editor reports that reimport and cache regeneration are
  unavailable.
- Cooked runtime load order contains no source fallback. Missing, corrupt, or
  incompatible cooked mesh data is a hard asset-load failure.
- Payload decode and render-data replacement are transactional. Bound components
  observe either the previous complete render data or the new complete render
  data, never a partially decoded mesh.

### Cooking and packaging

- The initial cooker writes the same validated native payload used by DDC into
  package-relative cooked bulk data. The first implementation may use a
  `.dbulk` companion beside the cooked `.dasset`; a later archive system may
  absorb that bulk data without changing mesh references or the payload schema.
- Cooked package metadata identifies the bulk object, expected payload hash,
  schema version, and target platform.
- Cooking is deterministic for identical source bytes, settings, builder
  version, and target platform.
- Cook output excludes source OBJ/FBX/glTF files and does not require Assimp in
  a runtime-only build.

### Ownership and thread model

- `Engine` owns static-mesh build settings, key contribution, payload schema,
  codec, and conversion to/from `FStaticMeshRenderData`.
- `AssetCore` owns reusable atomic DDC object-store and cooked bulk-location
  mechanics. It does not interpret mesh payload contents.
- The editor owns import/reimport UI and source-path repair.
- The first implementation performs source import and payload decode
  synchronously on the game thread, matching current `PostLoad` behavior.
  Asynchronous build and load are deferred until the synchronous state machine
  and failure behavior are proven.
- RHI resources and backend-native handles are never serialized. Existing
  render-thread publication and destruction rules remain authoritative.

### Material preview

- Preview sphere and box become engine-owned static-mesh assets with stable
  virtual identities under `/Engine/Editor/MaterialPreview/`.
- Their authoring models live under engine `SourceAssets`; cooked editor/runtime
  data lives through the same native payload path as project meshes.
- All material-preview instances share the loaded mesh assets or immutable
  render data. A preview document does not import or root its own copy.

## Current Foundations and Gaps

### Foundations

- `.dasset` packages already provide stable virtual asset identities and
  serialized `DStaticMesh` metadata.
- Static-mesh import already produces a complete CPU-side
  `FStaticMeshRenderData`, including bounds, material slots, sections, vertex
  streams, indices, normals, tangents, colors, and up to four UV channels.
- Import settings are reflected and persist across package reloads.
- Asset moves and deletes already contribute colocated source-file operations.
- Project-local `DerivedDataCache` exists, is ignored, is outside content
  mounts, and has established atomic cache-writing and safe-cleanup precedent.
- The asset registry and package loader already distinguish virtual assets from
  physical files.

### Gaps

- `DStaticMesh::PostLoad` requires `SourceFile` and calls the source importer on
  every normal load.
- Static-mesh render data has no stable disk codec or schema version.
- No static-mesh derived-data key or persistent object cache exists.
- Current source identity is primarily a colocated filename and cannot represent
  a clean non-mounted `SourceAssets` policy.
- No cooked mesh bulk reference or runtime-only load path exists.
- Runtime targets still inherit source importer assumptions.
- Material previews import `Sphere.obj` and `Box.obj` into per-document transient
  meshes and manually root them.

## Implementation Stages

### Stage 0: Freeze contracts and fixtures

- [ ] Define `FStaticMeshSourceImportData`, including optional normalized source
  path, source content hash, importer identity/version, and import settings.
- [ ] Define the `DMSH` header, chunk table, required chunks, numeric limits,
  alignment, checksum, endianness, and schema-version policy.
- [ ] Define the target-platform identifier and mesh-builder version ownership.
- [ ] Define the exact derived-data key byte encoding; do not hash formatted
  diagnostic strings or native struct memory.
- [x] Define cooked `.dbulk` naming and package-relative lookup rules, including
  how a future archive replaces the loose companion.
- [ ] Add small deterministic fixtures covering one section, multiple material
  slots, multiple UV channels, vertex colors, and malformed payloads.
- [ ] Record the source-directory migration compatibility window and removal
  criteria for legacy colocated source resolution.

#### Acceptance Gate

- The format and key contract has one selected representation for every field,
  no unresolved alternative layouts, and fixtures sufficient to test the reader
  without invoking Assimp or an RHI.

### Stage 1: Make source provenance optional and relocatable

- [ ] Replace the required `SourceFile`/settings coupling with optional
  `FStaticMeshSourceImportData` while retaining backward load compatibility.
- [ ] Resolve new source paths only beneath project or engine `SourceAssets`.
- [ ] Continue resolving legacy package-relative and mounted source paths during
  migration.
- [ ] Update import to copy source models into the correct `SourceAssets/Models`
  hierarchy while creating `.dasset` packages under Content.
- [ ] Update asset move/delete contributors so moving a `.dasset` does not
  accidentally relocate shared source art, and source deletion requires an
  explicit source operation.
- [ ] Add editor diagnostics and source-path repair for missing or moved sources.
- [ ] Update version-control documentation for the new directory boundary and
  LFS policy.

#### Acceptance Gate

- New assets import with portable source provenance outside mounted Content;
  legacy assets still load; and an asset with no source metadata can exist
  without failing solely because the source field is empty.

### Stage 2: Implement the native static-mesh codec

- [ ] Add explicit encode/decode structures independent of
  `FStaticMeshRenderData` memory layout.
- [ ] Encode and decode bounds, LOD metadata, vertex streams, index buffers,
  sections, and stable material-slot identifiers.
- [ ] Validate magic, versions, platform, checksum, counts, offsets, overlaps,
  alignment, allocation limits, enum values, and cross-chunk references.
- [ ] Reject NaN/Infinity positions and bounds, invalid indices, empty required
  geometry, and sections outside index or vertex ranges.
- [ ] Add deterministic round-trip tests against Stage 0 fixtures.
- [ ] Add truncation, corruption, integer-overflow, decompression-bomb, unknown
  chunk, and unsupported-version tests.
- [ ] Confirm encoding the same render data twice produces byte-identical output.

#### Acceptance Gate

- Valid fixtures round-trip to render-equivalent data deterministically, all
  malformed fixtures fail without unbounded allocation or partial publication,
  and codec tests do not require Assimp, Vulkan, or a window.

### Stage 3: Add static-mesh derived-data caching

- [ ] Add or reuse an `AssetCore` atomic content-addressed object-store API.
- [ ] Implement the static-mesh key builder from source bytes, canonical
  settings, builder version, schema version, and target platform.
- [ ] Read valid cache objects before invoking source import.
- [ ] Write the encoded payload after a successful source build.
- [ ] Treat missing, stale, corrupt, and incompatible objects as safe editor
  misses and rebuild when the source is available.
- [ ] Preserve the last complete live render data when a rebuild or cache write
  fails.
- [ ] Add cache diagnostics for hit, miss reason, key, rebuild, write failure,
  and source-unavailable-but-cached state.
- [ ] Add bounded cleanup and disk-budget accounting for static-mesh objects.

#### Acceptance Gate

- A cold load imports once and publishes a valid cache object; a warm load does
  not invoke the source importer; source/settings/builder/platform changes miss
  deterministically; and corrupt cache data is recovered without escaping the
  DDC root or damaging the asset package.

### Stage 4: Establish cooked static-mesh payloads

- [ ] Add a minimal cook operation that resolves a `DStaticMesh`, obtains or
  builds the target-platform payload, and writes deterministic cooked package
  metadata plus `.dbulk`.
- [ ] Ensure cooked bulk writes are transactional and remove incomplete output
  on failure.
- [ ] Add a cooked/runtime load mode that accepts only matching native payloads.
- [ ] Exclude source import metadata and source files from cooked output unless
  an explicit diagnostic build policy retains metadata.
- [ ] Remove Assimp and source-import code from a runtime-only target dependency
  graph.
- [ ] Reject editor DDC paths as runtime asset references.
- [ ] Add reproducibility tests comparing clean cooks of identical inputs.
- [ ] Add missing, wrong-platform, corrupt, and unsupported cooked-payload tests.

#### Acceptance Gate

- A clean cooked build loads representative static meshes with source models
  removed and without Assimp; repeated cooks are byte-identical; and every
  invalid cooked payload fails with an asset-qualified diagnostic.

### Stage 5: Migrate material-preview meshes

- [ ] Import sphere and box as `/Engine/Editor/MaterialPreview/Sphere` and
  `/Engine/Editor/MaterialPreview/Box`.
- [ ] Move their authoritative model sources to engine `SourceAssets`.
- [ ] Replace per-preview `CreateTransientFromFile` calls with shared asset or
  immutable render-data acquisition.
- [ ] Remove preview-owned mesh rooting and retain only ownership required for
  the preview scene, viewport, and light.
- [ ] Preserve sphere/box switching, material-slot mapping, rotation, zoom, GC
  safety, and preview teardown behavior.
- [ ] Update tests from transient OBJ import expectations to shared native
  asset/payload expectations.

#### Acceptance Gate

- Opening multiple material documents imports no OBJ per preview, all previews
  render using shared engine mesh data, garbage collection does not invalidate
  an active preview, and cooked editor operation does not require the preview
  source models.

### Stage 6: Complete validation and architecture handoff

- [ ] Run focused codec, cache, package, static-mesh, material, viewport, and
  editor tests using the repository test workflow.
- [ ] Build the required full editor target through `BuildTool`.
- [ ] Run the hidden-window DurinEditor smoke test after the successful full
  build.
- [ ] Inspect source-control output to confirm DDC and Cooked products remain
  ignored while `.dasset`, source art, and required metadata are tracked.
- [ ] Move lasting source, key, format, load, cooking, and failure contracts into
  Architecture documentation.
- [ ] Remove legacy colocated source resolution only after repository assets are
  migrated and compatibility coverage proves the removal deliberate.

#### Acceptance Gate

- All validation rows below pass, the editor and runtime boundaries are
  documented as current architecture, no source importer is required by cooked
  static-mesh loading, and no required behavior remains specified only in this
  plan.

## Validation Matrix

| Area | Validation | Required result |
| --- | --- | --- |
| Codec unit | Encode/decode every Stage 0 fixture | Render-equivalent deterministic round trip |
| Codec robustness | Truncated, corrupt, oversized, overlapping, invalid-index, and wrong-version inputs | Bounded failure with no partial publication |
| Key unit | Change source bytes, each import setting, builder version, schema, and platform independently | Every semantic change changes the key |
| Cache integration | Cold load followed by warm load | First load builds once; second load invokes no importer |
| Cache recovery | Delete or corrupt the cached object | Editor rebuilds from source and replaces it safely |
| Missing source | Remove source after a valid cache exists | Editor loads cached mesh and reports reimport unavailable |
| Missing all inputs | Remove source and cache | Asset-qualified load failure |
| Legacy migration | Load existing colocated-source packages | Compatibility succeeds before explicit migration |
| Cook determinism | Cook identical input in clean output roots twice | Byte-identical package metadata and bulk payload |
| Cook isolation | Remove source models and Assimp from runtime environment | Cooked meshes still load and render |
| Cook rejection | Wrong platform, checksum, schema, missing bulk | Hard failure before render-data publication |
| Material slots | Multi-section fixture and default-material references | Stable slot GUID and section mapping survives cache/cook |
| Editor preview | Open multiple materials and switch sphere/box | Shared mesh data, correct material, no per-preview import |
| GC/lifetime | Collect during and after active previews | Active data survives; teardown releases ownership safely |
| Rendering | Full editor build and hidden-window smoke test | Meshes and material previews render without validation errors |
| Repository | Inspect tracked and ignored files | Source and packages tracked; DDC/Cooked products ignored |

## Definition of Done

- Static-mesh normal editor loads use valid DDC data without reparsing source
  models.
- Cooked static meshes load without source files or Assimp.
- Native payloads are versioned, deterministic, checksummed, bounds-checked, and
  platform-qualified.
- Source provenance is optional, portable, and sufficient for editor reimport.
- Project and engine source models have a documented non-mounted
  `SourceAssets` home.
- Material-preview meshes are stable shared `/Engine` assets and no longer
  create transient meshes from OBJ per preview.
- Required tests, full build, and hidden-window editor smoke validation pass.
- Lasting contracts are present in Architecture and storage documentation.

## Deferred Follow-ups

- Asynchronous source import, DDC read, payload decode, and GPU upload.
- Mesh streaming and residency budgets.
- Automatic LOD generation and platform-specific mesh optimization.
- Meshlet or clustered geometry payload chunks.
- Collision and ray-tracing acceleration data in the cooked payload.
- Remote/shared DDC and distributed asset building.
- A unified cooker and archive container that absorbs loose `.dbulk` files.
- Source-control locking or asset-oriented VCS workflows for large teams.
- Runtime-generated mesh persistence.

## Related Documentation

- `Documentation/Architecture/AssetPackages.md`
- `Documentation/Architecture/AssetDataLifecycle.md`
- `Documentation/Architecture/LevelSystem.md`
- `Documentation/Architecture/MaterialSystem.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Architecture/TextureSystem.md`
- `Documentation/Git/ContentVersionControl.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/DerivedDataCache.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp`
- `Engine/Source/Programs/Tests/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Source/Programs/Tests/EngineTests/Private/MaterialTests.cpp`
