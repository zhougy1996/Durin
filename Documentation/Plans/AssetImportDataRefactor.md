# Asset Import Data Refactor Plan

Summary: Replace asset-family source-import schemas and opaque provenance sidecars with one UE-aligned editor-only import-data object model.

Last reviewed: 2026-08-26

Status: Active
Completed:

## Current Status

Planning is complete and implementation has not started. The selected direction
keeps import metadata on authored assets, replaces family-specific reflected
source structs with one editor-only inner-object model, and preserves
AssetForge as the only import/reimport execution authority. Stage 0 must first
prove package reachability, Cook stripping, construct-free inspection, and
candidate-to-target ownership transfer before any family schema is migrated.

## Goal

Provide one persistent import-data model for single-output imported assets that:

- represents every mounted source through a common `FSourceFile` and
  `FAssetImportInfo` schema;
- carries complete, versioned framework replay state without a second opaque
  provenance field beside family-specific source data;
- remains available to authored editor recovery, source inspection, and
  reimport while being absent from Cooked packages;
- can be inspected without constructing or loading the asset package;
- participates in the existing failure-atomic candidate publication and
  rollback model; and
- allows new imported asset families to join source-reference indexing without
  modifying a central concrete-type switch.

## Scope

- Add a lightweight Runtime `AssetImportCore` module containing persistent
  import-data schemas and object operations but no importer execution.
- Add `FSourceFile`, `FAssetImportInfo`, `DAssetImportData`, and a concrete
  framework-replay carrier named `DInterchangeAssetImportData`.
- Add the neutral persisted DTOs required to carry translator identity,
  translator settings, planning-pass identity, graph fingerprints, output
  mappings, and authored-output identity without depending on AssetForge.
- Add one `EditorOnly` `TObjectPtr<DAssetImportData>` field named
  `AssetImportData` to standalone imported Engine assets.
- Migrate standalone Texture2D, TerrainHeightmap, StaticMesh, TextureCube, and
  VolumeTexture import metadata, source diagnostics, reimport, repair, and
  uncooked recovery to the common model.
- Reuse `FSourceFile` in import-record source state while retaining
  `DImportRecord` as the authority for multi-output reconciliation.
- Replace family-aware source-reference package inspection with generic
  traversal through the main asset's internal `AssetImportData` reference.
- Provide read-old/write-new compatibility for current family-specific
  `SourceImportData`, `ImportProvenance`, and duplicated source fingerprint
  fields.
- Update the asset lifecycle and AssetForge architecture contracts after the
  implementation and compatibility behavior are validated.

## Non-Goals

- Redesigning source ingestion, mounted-source replacement, relocation, or
  their transaction semantics.
- Changing normalized TextureBuild, StaticMeshBuild, SkeletalBuild, or
  TerrainBuild request and product contracts except where an import-data state
  must accompany publication.
- Redesigning DDC storage, cooked payload schemas, or runtime render/collision
  resources.
- Moving decoder, translator, planning, builder, or reimport execution into
  Runtime Engine or `AssetImportCore`.
- Replacing `DImportRecord` or making one Scene output independently
  authoritative for a multi-output import.
- Adding AssetForge-specific types or dependencies to AssetCore or Engine.
- Copying Unreal Engine field names when they conflict with Durin's complete
  mounted-source-path contract; in particular, Durin does not call a complete
  `FSourcePath` a relative filename.
- Preserving redundant legacy fields through indefinite dual writes.

## Design Decisions and Invariants

### Ownership and module boundary

- The dependency direction is `Core/CoreDObject -> AssetCore ->
  AssetImportCore`, with both Engine and AssetForge depending on
  `AssetImportCore`. `AssetImportCore` depends on neither Engine nor any Editor
  module.
- `AssetImportCore` is available wherever an authored Engine package can be
  loaded. A serialized inner object may not have a class implemented only by an
  optional Editor module.
- AssetCore continues to own packages, mounted paths, inspection, and atomic
  publication. It does not interpret importer identity or replay settings.
- Runtime Engine assets retain only a base `DAssetImportData` reference and do
  not understand AssetForge component registries.
- AssetForge converts between its execution-facing `FImportProvenance` and the
  neutral persistent `DInterchangeAssetImportData` value. AssetForge remains
  the only authority that selects and invokes translators, planning passes,
  and builders.

### Source-file schema

- `FSourceFile` stores a stable source identity, semantic role, display label,
  complete normalized mounted `FSourcePath`, XXH3-128 content hash as reflected
  low/high words, byte count, and optional timestamp diagnostic.
- Content hash is the source-content identity. Path, size, and timestamp may
  accelerate inspection or improve diagnostics but do not substitute for the
  hash in DDC or authored-output identity.
- A valid nonempty source has a nonempty unique stable identity, a valid mounted
  path, and a complete nonzero hash. Partial hashes and duplicate stable
  identities are invalid.
- `FAssetImportInfo` owns a bounded ordered vector of `FSourceFile` values.
  Canonical persistence order is stable-identity byte order. Business logic
  selects a source by stable identity or role rather than by incidental array
  position.
- Standard roles include `source`, `panorama`, `face:+x`, `face:-x`,
  `face:+y`, `face:-y`, and `face:+z`/`face:-z`; dependency translators may
  define stable namespaced roles.
- StaticMesh string hashes, texture/terrain split-word hashes, and
  `FTextureSourceFile` converge on this one representation.

### Import-data object model

- `DAssetImportData` is an abstract reflected inner object containing schema
  version and `FAssetImportInfo`. It exposes const inspection, validation, and
  clone-to-owner operations; assets never expose an unrestricted mutable
  import-data pointer.
- `DInterchangeAssetImportData` is the initial concrete implementation. Its
  name describes a persistent translation/planning/build replay record rather
  than the Editor module that currently consumes it.
- Its persisted DTOs carry component ids and contract versions, bounded opaque
  settings payloads with schema ids/versions and content hashes, planning-pass
  stack entries, source/build graph fingerprints, output mappings, and the
  authored-output fingerprint.
- Source path and content hash occur only in `FAssetImportInfo`. Replay graph
  state references sources by stable identity and does not serialize a second
  copy of path/hash.
- Unknown concrete import-data classes or unsupported required replay schemas
  fail authored recovery and reimport with structured evidence. Basic source
  inspection may still report paths from a valid base source list.

### Asset state versus import recipe

- Settings that continue to define editable asset or Cook behavior remain on
  the asset and in its build-key inputs. Texture usage, compression, SRGB,
  VolumeTexture build settings, material slots, and collision policy do not
  move into import data.
- Settings needed only to reinterpret encoded source bytes live in the
  interchange replay state. These include StaticMesh source-axis conversion,
  VolumeTexture atlas interpretation, source decoder/translator settings, and
  panorama projection policy.
- TextureCube `SourceLayout` remains one authored asset property. Import data
  does not carry a second independently mutable copy; its active source-role
  set must match the authored layout.
- Decoder/importer identity and contract version belong to the translator
  descriptor, not to a family-specific Runtime struct.

### Single-output and multi-output authority

- A standalone imported asset's `AssetImportData` is its complete replay
  authority.
- `DImportRecord` remains the complete replay and reconciliation authority for
  managed multi-output imports. A managed output does not carry a second
  complete planning stack, output mapping, or provider state.
- A managed output may retain only the bounded source/build projection required
  for its independent DDC recovery. Record lookup and Scene reimport continue
  through the rebuildable ImportRecord index.
- `FImportRecordSource` reuses or embeds `FSourceFile` so record and standalone
  imports share one source schema, but this does not merge the two authority
  models.

### Object lifetime and publication

- The asset owns its import-data inner object through a reflected strong
  `TObjectPtr`; `Outer` alone is not lifetime ownership.
- Authored package save reaches the inner object through that reference.
  Cooked save strips the `EditorOnly` reference and must omit the now
  unreachable inner export, its class identity, and its value payload.
- Detached translators/builders produce value-only `FAssetImportDataState`.
  They do not construct `DObject` instances on workers.
- A candidate-owned inner object is never directly installed on an existing
  target. Prepared publication clones the candidate state under the target,
  validates the clone and target `Outer`, then includes its pointer transition
  in the no-fail imported-state exchange.
- Failed candidate creation, preparation, save, or bundle publication restores
  the prior asset state and prior import-data pointer. Successful publication
  leaves displaced unreferenced inner objects eligible for ordinary GC.
- Every family publishes content, import data, DDC identity, diagnostics, and
  render-resource transition as one prepared transaction. VolumeTexture's
  separately callable built-data, source-data, and provenance publication
  seams are removed or made private to that transaction.

### Compatibility

- Migration is read-old/write-new. New publication never updates both legacy
  and new fields.
- Legacy typed source fields and legacy serialized provenance must agree on
  every shared path, hash, translator contract, and import setting before an
  automatic upgrade is accepted.
- A complete valid legacy provenance can supply framework-only state, but it
  may not silently override conflicting typed family data. Conflict is an
  actionable reimport/resave diagnostic.
- Successful legacy conversion reports
  `EAssetLoadMutationKind::Upgrade`. The next canonical save writes only
  `AssetImportData`; a second load of that package reports no upgrade mutation.
- Compatibility readers are removed only after representative legacy fixtures
  and the required authored corpus resave have completed under repository
  compatibility policy.

### Construct-free source indexing

- Source-reference indexing inspects the main object's `AssetImportData`
  internal reference, resolves the referenced object through
  `FAssetPackageInspection::FindObject`, reads its common `SourceData`, and
  indexes all nonempty source paths.
- The generic path handles subclasses through the base field schema and does
  not load or construct the asset.
- The index has a temporary read-only legacy fallback during migration. New
  asset families require no central index modification once they use
  `DAssetImportData`.

## Current Foundations and Gaps

- Authored packages already serialize complete reflected inner-object graphs,
  expose internal object references through construct-free inspection, and do
  not treat `Outer` as a GC strong reference.
- Asset fields already use `DPROPERTY(EditorOnly)` and Cook tests verify that
  family source-import values and provenance are absent from ordinary Cooked
  packages.
- AssetForge already produces a complete `FImportProvenance` with source
  identities, translator/planning identity, graph fingerprints, output
  mappings, and authored-output fingerprint.
- AssetForge candidate publication already prepares detached products and
  reverses typed state exchanges on failed package-bundle publication.
- `DImportRecord` already demonstrates bounded persisted settings/provider
  payloads, source identities, output mappings, and validation, but its types
  live in an Editor module and cannot be referenced directly by Runtime asset
  packages.
- Texture2D already has a relatively complete `FTexture2DImportedState`, while
  VolumeTexture still publishes built data, typed source data, and framework
  provenance through separate seams.
- Current family schemas duplicate path/hash/importer concepts with different
  representations. Texture2D additionally retains a separate string content
  hash; VolumeTexture duplicates its source path as `SourceFile`; TextureCube
  duplicates `SourceLayout` between asset and source-import data.
- `SourceReferenceIndex.cpp` currently includes and attempts to deserialize
  every known family-specific `SourceImportData` struct.
- No existing contract proves that stripping an EditorOnly hard reference also
  prunes its otherwise unreachable inner export from Cooked object discovery.
  No generic clone-to-new-owner operation currently protects candidate
  publication from retaining a candidate-owned import-data object.

## Implementation Stages

### Stage 0: Prove inner-object persistence and publication feasibility

- [ ] Add focused fixture-only coverage for an authored asset with a reflected
  hard reference to an inner metadata object.
- [ ] Prove authored save/load preserves the strong reference, inner `Outer`,
  object path, reflected subclass, and values.
- [ ] Prove clearing the strong reference makes the inner object collectible
  and that `Outer` alone does not retain it.
- [ ] Prove construct-free inspection can follow the main object's internal
  reference and read fields from the referenced inner object.
- [ ] Prove ordinary Cook strips the EditorOnly reference and omits the inner
  object record, class identity, schema, and value bytes.
- [ ] Prototype clone-to-owner and a reversible prepared pointer exchange
  between candidate and existing target objects.
- [ ] Verify failed save/publication restores the exact old pointer and values
  without a target-to-candidate ownership edge.
- [ ] Record any required object-discovery, Cook reachability, or duplication
  changes before defining production import-data classes.


#### Acceptance Gate

- Authored round trip, Cook pruning, construct-free inspection, GC lifetime,
  clone-to-owner, successful exchange, and rollback are all covered by native
  tests. If any cannot be made reliable without changing the package object
  model, stop this plan and record a revised value-carrier design before
  migrating an asset family.

### Stage 1: Add AssetImportCore schemas and common operations

- [ ] Add the `AssetImportCore` Runtime module, API export header, reflection
  generation, startup registration, and documented dependency edges.
- [ ] Implement bounded reflected `FSourceFile` with stable identity, role,
  mounted path, split-word XXH3-128, size, timestamp, and display label.
- [ ] Implement canonical `FAssetImportInfo` validation, lookup, ordering, and
  equality/fingerprint helpers.
- [ ] Implement schema-bearing bounded payload, component descriptor,
  planning-pass descriptor, source-reference descriptor, and output-mapping
  DTOs without importing AssetForge headers.
- [ ] Implement `DAssetImportData` and `DInterchangeAssetImportData`, including
  validation, state snapshots, clone-to-owner, and PostLoad validation.
- [ ] Add construct-free helpers that inspect an `AssetImportData` internal
  reference and decode common `SourceData` without knowing the subclass.
- [ ] Add explicit collection, string, and payload byte limits consistent with
  existing AssetForge source/import-record limits.
- [ ] Add struct/object operation coverage for defaults, copy/move where
  applicable, reflection, package round trip, invalid bounds, malformed
  identities, duplicate source identities, and unsupported schemas.

#### Acceptance Gate

- `AssetImportCore` has no Engine or Editor dependency; its complete schemas
  round-trip through authored packages; invalid values fail before
  publication; and common construct-free inspection succeeds for the base and
  interchange concrete class.

### Stage 2: Persist AssetForge replay state through the common model

- [ ] Define deterministic conversions between `FImportProvenance` and a
  value-only `FAssetImportDataState`/`FInterchangeImportState`.
- [ ] Version the new provenance encoding so source path/hash live only in
  `FAssetImportInfo`; graph source descriptors reference stable source
  identities.
- [ ] Retain bounded decoding for the current provenance bytes and convert them
  into the new value without writing the legacy encoding again.
- [ ] Centralize creation and validation of candidate-owned
  `DInterchangeAssetImportData` on the main/editor thread.
- [ ] Change asset-builder provenance application to prepare one complete
  import-data state rather than call family-specific `PublishImportProvenance`
  methods.
- [ ] Ensure translator settings that reinterpret encoded sources are complete
  and versioned in replay state before deleting equivalent family fields.
- [ ] Verify unchanged provenance produces byte-identical canonical new state
  and stable authored-output fingerprints across runs.

#### Acceptance Gate

- Initial import and reimport can reconstruct an equivalent `FImportRequest`
  solely from the new common state; new persistence contains no duplicated
  source path/hash; legacy provenance remains read-only compatible; and Engine
  has no AssetForge dependency.

### Stage 3: Migrate Texture2D as the pilot family

- [ ] Add `AssetImportData` to `DTexture2D` and carry value-only import state in
  `FTexture2DImportedState`.
- [ ] Move mounted path, source hash, source size, and last-write diagnostic
  into one common source with stable identity and role `source`.
- [ ] Move decoder identity/version to the persisted translator descriptor and
  remove Engine-side knowledge of AssetForge translator selection.
- [ ] Convert source inspection to common mounted-path/existence/hash logic.
- [ ] Convert import, reimport, repair, source mutation, thumbnail inspection,
  PostLoad recovery, DDC key construction, semantic no-op, state exchange, and
  rollback to the new authority.
- [ ] Add read-old conversion for `FTexture2DSourceImportData`, legacy
  `ImportProvenance`, string `SourceContentHash`, source size, and timestamp.
- [ ] Stop writing `FTexture2DSourceImportData`, `FTextureSourceFile`, legacy
  provenance, and duplicate string source hash.
- [ ] Confirm source and build identity remain equivalent or deliberately bump
  the affected builder/key version with recorded rationale.

#### Acceptance Gate

- Texture2D initial import, authored round trip, DDC hit/miss/corruption,
  changed/missing source diagnostics, repair, source replacement, reimport,
  no-op reimport, failed publication rollback, legacy upgrade, second-load
  cleanliness, thumbnail inspection, and Cook stripping pass with only the new
  schema written.

### Stage 4: Migrate TerrainHeightmap and standalone StaticMesh

- [ ] Move Terrain source path/hash/size/timestamp to common source role
  `source`; move decoder/format/profile replay policy to interchange state.
- [ ] Preserve asynchronous Terrain derived-data generation, cancellation,
  supersession, revision publication, and source-mutation transaction behavior.
- [ ] Move standalone StaticMesh path and string source hash to common source
  role `source`; move importer identity and source-axis settings to interchange
  replay state.
- [ ] Preserve StaticMesh material reconciliation, collision production,
  prepared render-resource exchange, DDC behavior, and rollback.
- [ ] Define and implement the minimal per-output source/build projection for a
  Scene-managed StaticMesh without creating a second complete Scene replay
  authority.
- [ ] Add read-old/write-new compatibility and conflict tests for both families.

#### Acceptance Gate

- Terrain async recovery/publication and StaticMesh standalone/Scene paths pass
  import, reimport, DDC, source mutation, legacy upgrade, Cook stripping, and
  rollback tests without family-specific source/importer fields being written.

### Stage 5: Migrate TextureCube and VolumeTexture

- [ ] Represent six-face Cube sources with the six canonical face roles and a
  panorama source with the `panorama` role.
- [ ] Validate exact active source-role sets against the single authored
  TextureCube `SourceLayout` property and reject inactive or duplicate roles.
- [ ] Move decoder and projection replay versions into interchange state and
  remove the duplicated import-data `SourceLayout`.
- [ ] Move VolumeTexture source path/hash/diagnostics to common role `source`.
- [ ] Move Volume atlas import format, channel selection, slice dimensions,
  depth, and tile layout into versioned replay settings while retaining runtime
  `FVolumeTextureBuildSettings` on the asset.
- [ ] Replace VolumeTexture's duplicated `SourceFile` reflected string with a
  derived read-only UI accessor.
- [ ] Replace separately callable built-data, source-import-data, and
  provenance publication with one prepared imported-state transaction.
- [ ] Add read-old/write-new compatibility and conflict tests for both families.

#### Acceptance Gate

- Six-face and panorama Cube plus direct-atlas VolumeTexture pass import,
  reimport, source repair, DDC, legacy upgrade, semantic no-op, rollback,
  authored round trip, and Cook tests with exactly one source/layout/provenance
  authority.

### Stage 6: Generalize source indexing and import-record sources

- [ ] Replace concrete family deserialization in `SourceReferenceIndex.cpp`
  with traversal through the main object's internal `AssetImportData`
  reference and common `FAssetImportInfo` fields.
- [ ] Preserve a bounded read-only fallback for unresaved legacy packages until
  the compatibility retirement gate is met.
- [ ] Remove family header dependencies and class-name filtering from the new
  index path; unknown subclasses with valid base source data remain indexable.
- [ ] Rework `FImportRecordSource` to reuse/embed `FSourceFile` while retaining
  record-only reconciliation state.
- [ ] Convert record creation, validation, fingerprinting, indexing,
  relocation/Fix Up participation, and legacy record loading to the common
  source representation.
- [ ] Verify index refresh remains construct-free, bounded, cancelable, and
  stable across catalog revisions.

#### Acceptance Gate

- Source indexing discovers every new standalone and record source without
  constructing assets or naming concrete asset classes; legacy fallback is
  tested; and ImportRecord remains the only complete authority for managed
  multi-output reconciliation.

### Stage 7: Remove legacy production schemas and update contracts

- [ ] Remove family production use of `FTextureSourceFile`,
  `FTexture2DSourceImportData`, `FTextureCubeSourceImportData`,
  `FVolumeTextureSourceImportData`, `FTerrainHeightmapSourceImportData`, and
  `FStaticMeshSourceImportData` after compatibility readers no longer require
  their live types.
- [ ] Remove asset-level opaque `ImportProvenance` strings and family-specific
  publish/getter APIs that exposed the old authority.
- [ ] Remove duplicate Texture2D string source hash, VolumeTexture `SourceFile`,
  and Cube import-data `SourceLayout`.
- [ ] Remove concrete-family source-index fallbacks only after the required
  corpus resave and compatibility policy permit it.
- [ ] Update Asset Data Lifecycle, Asset Import Framework, mounted-source,
  VolumeTexture, Terrain, and relevant family contracts with the implemented
  ownership and migration result.
- [ ] Update `Last reviewed`, plan status, stage handoffs, and exact validation
  evidence throughout implementation.

#### Acceptance Gate

- Repository search finds no production writer or competing runtime authority
  for the removed fields; lasting contracts describe the new ownership; and
  all required authored fixtures have a canonical new-format path.

### Stage 8: Complete repository-wide validation and migration evidence

- [ ] Run the smallest native suites during each family stage and the required
  aggregates only after all family migrations are integrated, following the
  repository testing workflow.
- [ ] Validate authored and Cooked package inspection for every migrated class,
  including absence of import-data object/class/schema/value bytes in Cook.
- [ ] Validate failure injection at candidate creation, clone-to-owner,
  prepared exchange, package save, bundle publication, and rollback.
- [ ] Validate deterministic source/import fingerprints and any deliberate DDC
  key/version transitions on clean and warm caches.
- [ ] Validate canonical resave on representative legacy packages and confirm
  the second authored load has no upgrade mutation.
- [ ] Validate source-reference indexing across standalone, six-source,
  panorama, Scene record, missing-source, corrupt-reference, and unknown
  subclass fixtures.
- [ ] Run changed/all documentation and all-plan validation after the final
  contract updates.

#### Acceptance Gate

- All Definition of Done conditions have evidence in `Current Status`, every
  required native and documentation validator passes, and no incomplete
  compatibility or non-standard validation remains unrecorded.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Source values | Empty/default behavior, one and many sources, canonical ordering, duplicate stable identity, invalid mounted path, partial/zero hash, bounds, reflected round trip |
| Inner objects | Authored save/load, subclass preservation, strong-reference GC lifetime, clone-to-owner, correct Outer/path, successful exchange, reversible rollback |
| Cook | No `AssetImportData` field, inner export, concrete class identity, replay schema string, source path, source hash, or replay payload in ordinary Cooked packages |
| Compatibility | Every family legacy fixture, consistent typed/provenance values, missing half, conflicting halves, unsupported contract, read-old/write-new, clean second load |
| Asset behavior | Initial import, save/load, reimport, repair, replace, relocate, changed/missing source, semantic no-op, DDC hit/miss/corruption, failed publication rollback |
| TextureCube | Exact six-face roles, panorama-only role, duplicate/missing/inactive role rejection, one authored `SourceLayout` authority |
| VolumeTexture | Atlas recipe persistence, no duplicated path string, atomic content/import publication, payload and render-resource rollback |
| Terrain | Async admission, cancellation, supersession, revision coherence, source mutation, DDC recovery |
| StaticMesh | Standalone and Scene-managed authority, axis recipe, material reconciliation, collision/render prepared exchange |
| Import records | Shared `FSourceFile`, stable output reconciliation, relocation/Fix Up, no second complete output authority |
| Source index | Construct-free generic inspection, internal-reference corruption, unknown subclass, legacy fallback, bounded/cancelable refresh |
| Determinism | Stable canonical import-data bytes, graph/authored fingerprints, DDC input equivalence or documented version bump |
| Documentation | Changed/all docs, all active plans, and updated long-lived ownership contracts |

## Definition of Done

- Every standalone imported Engine asset persists one EditorOnly
  `DAssetImportData` authority and no family-specific source/importer schema.
- Every mounted import source uses `FSourceFile`; every source content hash uses
  the same reflected XXH3-128 representation.
- Source path/hash are serialized once per authority and framework graph state
  references sources by stable identity.
- Engine assets and AssetCore have no AssetForge/AssetForgeBuiltins dependency.
- AssetForge can reconstruct initial recovery/reimport requests from the common
  import data and performs all translator/planning/builder selection.
- Asset content and import-data pointer transition in one prepared reversible
  publication; no live target references a candidate-owned inner object.
- Cooked packages contain no import-data reference, inner object, class/schema
  identity, or source/replay bytes.
- `DImportRecord` remains the single complete authority for managed
  multi-output imports while sharing the common source-file schema.
- Source-reference indexing is construct-free and independent of concrete
  asset family types.
- Legacy family packages either upgrade deterministically and resave to the new
  schema or fail with an explicit conflict/reimport diagnostic; successful
  resaves load without another upgrade mutation.
- DDC and cooked runtime behavior remain equivalent except for explicitly
  documented producer/key version transitions.
- Lasting ownership and lifecycle rules are present in their authoritative
  Runtime and Editor contracts, and this plan contains final validation
  evidence before completion.

## Deferred Follow-ups

- Adding Asset Registry summary tags for source files. The selected first
  implementation uses full construct-free package inspection because the
  current catalog has no generic tag store.
- Generalizing `DAssetImportData` to non-AssetForge import frameworks beyond
  the subclass and conversion seams required here.
- Selective reimport of one source role. The schema preserves stable identities
  needed for it, but this plan retains existing whole-request semantics.
- Exposing editable import settings through a generic details panel. Initial
  migration preserves existing family UI and read-only source diagnostics.
- Removing legacy compatibility readers before the repository's authored
  package resave and compatibility policy explicitly permit it.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/SourcePath.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageInspection.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/Persistence/ImportProvenance.h`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/Persistence/ImportRecord.h`
- `Engine/Source/Editor/AssetForge/Private/ImportExecution.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImportProvider.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImportProvider.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImportProvider.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainHeightmapImportProvider.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImportProvider.cpp`
- `Engine/Source/Editor/DurinEd/Private/Source/SourceReferenceIndex.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureImportAndCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/VolumeTextureSourceImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SourceLibraryReferenceContractTests.cpp`
