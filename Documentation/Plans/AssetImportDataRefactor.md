# Asset Import Data Refactor Plan

Summary: Replace asset-family source-import schemas and opaque provenance sidecars with one UE-aligned editor-only model split across AssetCore, Engine, and AssetForge.

Last reviewed: 2026-08-27

Status: Active
Completed:

## Current Status

Stage 0 is complete. Stage 1 proved the common schemas, reflected inner-object
model, validation, clone-to-owner, PostLoad validation, package round trip, and
construct-free inspection in a standalone Runtime `AssetImportCore` prototype.
That prototype placement is now superseded by the reviewed ownership decision:
`FSourceFile`, `FAssetImportInfo`, and the base `DAssetImportData` object belong
to `Engine`, while the concrete AssetForge replay object and DTOs belong to
Editor `AssetForge`. The standalone `AssetImportCore` module will be removed;
`AssetCore` remains the lower-level package/path/inspection module and gains no
import-framework schema.

Stage 2 has realigned the committed Stage 1 prototype to the reviewed boundary.
Engine now owns the common source schema and abstract import-data object;
AssetForge owns the versioned AssetForge replay DTOs and concrete reflected
subclass. The standalone `AssetImportCore` module and its project/module edges
have been removed, and executable CMake closure guards enforce that Engine and
AssetCore do not acquire AssetForge.

Deterministic conversion, bounded legacy decoding, single-output request
reconstruction, and game-thread object creation are implemented and covered.
Stage 2 still must define and prove authored-target/runtime-only loading, replace
the family-specific builder provenance publication hook with preparation of one
complete import-data state, and audit encoded-source translator settings before
the first family migration.

Stage 0 validation on 2026-08-26: `AssetPackageTests` passed 125/125 and
`AssetCookTests` passed 13/13 under `Win64-Debug-DurinEditor`.

Stage 1 prototype validation on 2026-08-26: `AssetImportDataTests` passed 3/3;
standalone `AssetForge` and `Engine` targets built successfully under
`Win64-Debug-DurinEditor`. This evidence remains valid for object/schema
feasibility but does not validate the revised final module ownership.

Stage 2 boundary validation on 2026-08-26: configuration passed with the revised
dependency-closure assertions; standalone `Engine` and `AssetForge` targets
built successfully; `AssetImportDataTests` passed 3/3 and `AssetForgeTests`
passed 19/19 under `Win64-Debug-DurinEditor`. The conversion coverage proves
legacy provenance -> AssetForge state -> provenance preserves canonical bytes
and authored/source/build fingerprints, and rejects import-data object creation
off the game thread.

The concrete replay carrier was renamed on 2026-08-26 from the prototype-era
`Interchange` terminology to `DAssetForgeImportData` and
`FAssetForgeImportState`. Reconfiguration, standalone `AssetForge` build,
`AssetImportDataTests` 3/3, and `AssetForgeTests` 19/19 passed after the reflected
class, source files, module manifest, conversion APIs, fixtures, and plan were
renamed without compatibility aliases.

The publication model was simplified on 2026-08-26 before continuing the
family migration. `FImportOperationSnapshot` is now the authority for
in-progress work; detached candidates do not mutate live assets. Finalization
uses one-way `IAssetBuilder::PublishImportedState` calls on the editor thread
and no longer exposes an AssetForge reversible-exchange token. Persistence is
reported separately through `FImportResult::Persistence`: save failure retains
the newly published in-memory state and Dirty package while leaving the prior
disk/catalog state intact. `AssetForgeTests` passed 19/19 with this contract.

The Texture2D pilot now publishes `DAssetForgeImportData`, reads source
inspection/reimport/recovery state from it with bounded legacy fallback, clears
the old fields on new publication, and preserves DDC recovery across changed
source content. Validation on 2026-08-26: standalone `DurinEd` built;
`AssetImportDataTests` passed 3/3, `AssetForgeTests` passed 19/19,
`AssetImportTests` passed 17/17, and `TextureTests` passed 78/78 under
`Win64-Debug-DurinEditor`. The same one-way publication adapter passed
`StaticMeshTests` 74/74 and `TerrainHeightmapTests` 11/11; the latter now
explicitly proves save failure retains the new payload, revision, and Dirty
package for a later successful Save.

The import architecture was deliberately narrowed on 2026-08-26. The generic
Import Record subsystem and generic preview execution/cache were removed.
Scene import is now creation-only and publishes ordinary independent outputs;
it has no aggregate record, reverse index, stable reconciliation, whole-scene
reimport, or missing-output repair. This eliminates the former multi-output
authority from this plan: Stage 6 now covers standalone source indexing only,
and later family migrations must not reintroduce a general record or preview
framework.

Removal validation on 2026-08-27: the complete `DurinEditor` target built;
`AssetForgeTests` passed 19/19, `SceneImportTests` 4/4,
`SkeletalSceneLifecycleTests` 1/1, `TextureTests` 77/77,
`SkeletalAssetTests` 34/34, `SkeletalMeshEditorTests` 3/3,
`EditorAssetWorkflowTests` 35/35, and `MaterialTests` 99/99. Changed-document
validation and all-plan validation passed.

## Goal

Provide one persistent import-data model for single-output imported assets that:

- represents every mounted source through a common `FSourceFile` and
  `FAssetImportInfo` schema;
- carries complete, versioned framework replay state without a second opaque
  provenance field beside family-specific source data;
- remains available to authored editor recovery, source inspection, and
  reimport while being absent from Cooked packages;
- can be inspected without constructing or loading the asset package;
- publishes completed imported state once on the editor thread while reporting
  package persistence independently; and
- allows new imported asset families to join source-reference indexing without
  modifying a central concrete-type switch.

## Scope

- Add `FSourceFile`, `FAssetImportInfo`, and the abstract
  `DAssetImportData` base object to Runtime `Engine`; use the base object as the
  only import-data type known by Engine asset classes.
- Add the concrete framework-replay carrier `DAssetForgeImportData` and
  its persisted translator, planning, graph, mapping, and authored-output DTOs
  to Editor `AssetForge`.
- Allow `AssetForge` to depend on `Engine` while preserving the hard inverse
  prohibition: `Engine` and `AssetCore` never depend on `AssetForge`.
- Add one `EditorOnly` `TObjectPtr<DAssetImportData>` field named
  `AssetImportData` to standalone imported Engine assets.
- Migrate standalone Texture2D, TerrainHeightmap, StaticMesh, TextureCube, and
  VolumeTexture import metadata, source diagnostics, reimport, repair, and
  uncooked recovery to the common model.
- Keep Scene multi-output import creation-only; generated outputs become
  ordinary independent assets and add no aggregate import authority.
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
  Runtime Engine or `AssetCore`.
- Preserving an Engine-free standalone `AssetForge` dependency closure;
  authoring tools that execute or fully load AssetForge replay data may depend
  on both `Engine` and `AssetForge`.
- Adding whole-scene reimport, stable multi-output reconciliation, or a general
  preview execution/cache framework.
- Adding AssetForge-specific types or dependencies to AssetCore or Engine.
- Copying Unreal Engine field names when they conflict with Durin's complete
  mounted-source-path contract; in particular, Durin does not call a complete
  `FSourcePath` a relative filename.
- Preserving redundant legacy fields through indefinite dual writes.

## Design Decisions and Invariants

### Ownership and module boundary

- The dependency direction is `Core/CoreDObject -> AssetCore -> Engine ->
  AssetForge -> AssetForgeBuiltins`. The arrows describe increasing ownership;
  each module may depend only on modules to its left.
- Runtime `AssetCore` continues to own mounted paths, packages, atomic
  publication, and raw construct-free package inspection. It does not acquire
  import source, recipe, or framework schemas.
- Runtime `Engine` owns `FSourceFile`, `FAssetImportInfo`, and
  `DAssetImportData` because they form one strongly bound asset import-metadata
  contract used by Engine asset classes.
- Editor `AssetForge` owns `DAssetForgeImportData` and every replay DTO
  that names translators, planning passes, graph state, output mappings, or
  authored-output identity. These are AssetForge protocol, not Runtime-neutral
  asset primitives.
- Runtime-only targets load Cooked packages without `AssetForge`. Authoring,
  Cook, recovery, and canonical-resave targets that fully load authored
  AssetForge replay objects must load `AssetForge` before resolving their concrete
  class. Construct-free source inspection may still read the serialized base
  source field without loading that class.
- Runtime Engine assets retain only a base `DAssetImportData` reference and do
  not understand AssetForge component registries.
- AssetForge converts between its execution-facing `FImportProvenance` and its
  persistent `DAssetForgeImportData` value. AssetForge remains the only
  authority that interprets replay schema support and selects or invokes
  translators, planning passes, and builders.

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

- Engine's `DAssetImportData` is an abstract reflected inner object containing
  schema version and Engine's `FAssetImportInfo`. It exposes const
  inspection, validation, and clone-to-owner operations; assets never expose an
  unrestricted mutable import-data pointer.
- `DAssetForgeImportData` is the initial concrete implementation. Its
  name describes a persistent translation/planning/build replay record rather
  than a new Runtime framework, and its reflected class belongs to AssetForge.
- Its persisted DTOs carry component ids and contract versions, bounded opaque
  settings payloads with schema ids/versions and content hashes, planning-pass
  stack entries, source/build graph fingerprints, output mappings, and the
  authored-output fingerprint.
- Source path and content hash occur only in `FAssetImportInfo`. Replay graph
  state references sources by stable identity and does not serialize a second
  copy of path/hash.
- Unknown concrete import-data classes fail full authored object loading unless
  their owning authoring module is present. Unsupported required replay schemas
  fail AssetForge recovery and reimport with structured evidence. Basic
  construct-free source inspection may still report paths from a structurally
  valid serialized base source list without loading the concrete class.

### Asset state versus import recipe

- Settings that continue to define editable asset or Cook behavior remain on
  the asset and in its build-key inputs. Texture usage, compression, SRGB,
  VolumeTexture build settings, material slots, and collision policy do not
  move into import data.
- Settings needed only to reinterpret encoded source bytes live in the
  AssetForge replay state. These include StaticMesh source-axis conversion,
  VolumeTexture atlas interpretation, source decoder/translator settings, and
  panorama projection policy.
- TextureCube `SourceLayout` remains one authored asset property. Import data
  does not carry a second independently mutable copy; its active source-role
  set must match the authored layout.
- Decoder/importer identity and contract version belong to the translator
  descriptor, not to a family-specific Runtime struct.

### Single-output and Scene output authority

- A standalone imported asset's `AssetImportData` is its complete replay
  authority.
- Scene import has no persistent aggregate replay authority. It creates peer
  assets once; each output thereafter follows its own supported asset workflow.
- Scene outputs do not retain stable reconciliation mappings, reverse ownership,
  tombstones, output fingerprints, or a whole-scene recovery recipe.

### Object lifetime and publication

- The asset owns its import-data inner object through a reflected strong
  `TObjectPtr`; `Outer` alone is not lifetime ownership.
- Authored package save reaches the inner object through that reference.
  Cooked save strips the `EditorOnly` reference and must omit the now
  unreachable inner export, its class identity, and its value payload.
- Detached translators/builders produce value-only `FAssetImportDataState`.
  They do not construct `DObject` instances on workers.
- A candidate-owned inner object is never directly installed on an existing
  target. Finalization applies the completed value to an inner object owned by
  the live target and validates its `Outer` before exposing it.
- Failure or cancellation before finalization leaves the prior asset state and
  import-data pointer unchanged. Finalization is non-cancelable and publishes
  once; it does not retain reverse state in AssetForge.
- Save occurs after live publication and has an independent persistence result.
  Save failure leaves the new state resident and the package Dirty so the user
  can retry Save; it does not reinterpret a completed import as a failed build.
- Every family publishes content, import data, DDC identity, diagnostics, and
  render-resource transition through one narrow editor-thread boundary. VolumeTexture's
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

- The completed Stage 1 prototype currently places all common and interchange
  types in Runtime `AssetImportCore`. Its implementation and tests provide the
  migration source, but the module and its Engine dependency must be removed
  before any asset family adopts the model.
- Authored packages already serialize complete reflected inner-object graphs,
  expose internal object references through construct-free inspection, and do
  not treat `Outer` as a GC strong reference.
- Asset fields already use `DPROPERTY(EditorOnly)` and Cook tests verify that
  family source-import values and provenance are absent from ordinary Cooked
  packages.
- AssetForge already produces a complete `FImportProvenance` with source
  identities, translator/planning identity, graph fingerprints, output
  mappings, and authored-output fingerprint.
- AssetForge candidate construction already prepares detached products. Its
  finalization boundary now performs one-way publication and reports package
  persistence separately from the import outcome.
- Scene outputs deliberately carry no aggregate replay or reconciliation state;
  standalone asset import data is the only persistent replay model in scope.
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

- [x] Add focused fixture-only coverage for an authored asset with a reflected
  hard reference to an inner metadata object.
- [x] Prove authored save/load preserves the strong reference, inner `Outer`,
  object path, reflected subclass, and values.
- [x] Prove clearing the strong reference makes the inner object collectible
  and that `Outer` alone does not retain it.
- [x] Prove construct-free inspection can follow the main object's internal
  reference and read fields from the referenced inner object.
- [x] Prove ordinary Cook strips the EditorOnly reference and omits the inner
  object record, class identity, schema, and value bytes.
- [x] Prototype clone-to-owner and a reversible prepared pointer exchange
  between candidate and existing target objects.
- [x] Verify failed save/publication restores the exact old pointer and values
  without a target-to-candidate ownership edge.
- [x] Record any required object-discovery, Cook reachability, or duplication
  changes before defining production import-data classes.


#### Acceptance Gate

- Authored round trip, Cook pruning, construct-free inspection, GC lifetime,
  clone-to-owner, successful exchange, and rollback are all covered by native
  tests. If any cannot be made reliable without changing the package object
  model, stop this plan and record a revised value-carrier design before
  migrating an asset family.

### Stage 1: Prototype common schemas and operations

- [x] Add a standalone Runtime prototype module, API export header, reflection
  generation, startup registration, and documented prototype dependency edges.
- [x] Implement bounded reflected `FSourceFile` with stable identity, role,
  mounted path, split-word XXH3-128, size, timestamp, and display label.
- [x] Implement canonical `FAssetImportInfo` validation, lookup, ordering, and
  equality/fingerprint helpers.
- [x] Implement schema-bearing bounded payload, component descriptor,
  planning-pass descriptor, source-reference descriptor, and output-mapping
  DTOs without importing AssetForge headers.
- [x] Implement `DAssetImportData` and `DAssetForgeImportData`, including
  validation, state snapshots, clone-to-owner, and PostLoad validation.
- [x] Add construct-free helpers that inspect an `AssetImportData` internal
  reference and decode common `SourceData` without knowing the subclass.
- [x] Add explicit collection, string, and payload byte limits consistent with
  existing AssetForge source and graph limits.
- [x] Add struct/object operation coverage for defaults, copy/move where
  applicable, reflection, package round trip, invalid bounds, malformed
  identities, duplicate source identities, and unsupported schemas.

#### Acceptance Gate

- The prototype has no Engine or Editor dependency; its complete schemas
  round-trip through authored packages; invalid values fail before publication;
  and common construct-free inspection succeeds for the base and interchange
  concrete class. This gate proves feasibility only; Stage 2 owns final module
  placement and dependency validation.

### Stage 2: Establish the final module boundary and persist AssetForge replay

- [x] Move `FSourceFile`, `FAssetImportInfo`, their limits, validation,
  canonical ordering/fingerprinting, and `DAssetImportData` from the prototype
  into `Engine`.
- [x] Keep raw package/reference traversal in `AssetCore`; move schema-aware
  construct-free import-source decoding into `Engine` without making
  `AssetCore` depend on Engine import types.
- [x] Move `DAssetForgeImportData`, replay payload/component/planning/
  source-reference/output DTOs, and replay-specific validation into
  `AssetForge`.
- [x] Remove the `AssetImportCore` module, descriptor/project registration,
  Engine dependency, API surface, and obsolete prototype paths after all users
  and tests have moved.
- [x] Change `AssetForge` to depend publicly on `Engine`; prove `Engine` and
  `AssetCore` have no source, descriptor, link, startup, or reflection
  dependency on `AssetForge`.
- [ ] Define the authored-target loading contract for the concrete AssetForge
  subclass and prove runtime-only Cooked loading neither deploys nor resolves
  `AssetForge`.
- [x] Define deterministic conversions between `FImportProvenance` and a
  value-only AssetForge `FAssetForgeImportState`.
- [x] Version the new provenance encoding so source path/hash live only in
  `FAssetImportInfo`; graph source descriptors reference stable source
  identities.
- [x] Retain bounded decoding for the current provenance bytes and convert them
  into the new value without writing the legacy encoding again.
- [x] Centralize creation and validation of candidate-owned
  `DAssetForgeImportData` on the main/editor thread.
- [ ] Change asset-builder provenance application to prepare one complete
  import-data state rather than call family-specific `PublishImportProvenance`
  methods.
- [ ] Ensure translator settings that reinterpret encoded sources are complete
  and versioned in replay state before deleting equivalent family fields.
- [x] Verify unchanged provenance produces byte-identical canonical new state
  and stable authored-output fingerprints across runs.

#### Acceptance Gate

- The standalone `AssetImportCore` module no longer exists; common source and
  base import-data types are owned by Engine, while interchange persistence is
  owned by AssetForge. Initial import and reimport can reconstruct an equivalent
  `FImportRequest` from the AssetForge state plus explicitly retained authored
  asset settings; new persistence contains no duplicated source path/hash;
  legacy provenance remains read-only compatible; runtime-only targets do not
  deploy AssetForge; and Engine and AssetCore have no AssetForge dependency.

### Stage 3: Migrate Texture2D as the pilot family

- [ ] Add `AssetImportData` to `DTexture2D` and carry value-only import state in
  `FTexture2DImportedState`.
- [ ] Move mounted path, source hash, source size, and last-write diagnostic
  into one common source with stable identity and role `source`.
- [ ] Move decoder identity/version to the persisted translator descriptor and
  remove Engine-side knowledge of AssetForge translator selection.
- [ ] Convert source inspection to common mounted-path/existence/hash logic.
- [ ] Convert import, reimport, repair, source mutation, thumbnail inspection,
  PostLoad recovery, DDC key construction, semantic no-op, and one-way
  publication to the new authority.
- [ ] Add read-old conversion for `FTexture2DSourceImportData`, legacy
  `ImportProvenance`, string `SourceContentHash`, source size, and timestamp.
- [ ] Stop writing `FTexture2DSourceImportData`, `FTextureSourceFile`, legacy
  provenance, and duplicate string source hash.
- [ ] Confirm source and build identity remain equivalent or deliberately bump
  the affected builder/key version with recorded rationale.

#### Acceptance Gate

- Texture2D initial import, authored round trip, DDC hit/miss/corruption,
  changed/missing source diagnostics, repair, source replacement, reimport,
  no-op reimport, failed-save Dirty retention, legacy upgrade, second-load
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
  render-resource publication, and DDC behavior.
- [ ] Define and implement the minimal per-output source/build projection for a
  Scene-managed StaticMesh without creating a second complete Scene replay
  authority.
- [ ] Add read-old/write-new compatibility and conflict tests for both families.

#### Acceptance Gate

- Terrain async recovery/publication and StaticMesh standalone/Scene paths pass
  import, reimport, DDC, source mutation, legacy upgrade, Cook stripping, and
  persistence-failure tests without family-specific source/importer fields being written.

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
  provenance publication with one imported-state publication boundary.
- [ ] Add read-old/write-new compatibility and conflict tests for both families.

#### Acceptance Gate

- Six-face and panorama Cube plus direct-atlas VolumeTexture pass import,
  reimport, source repair, DDC, legacy upgrade, semantic no-op, failed-save retention,
  authored round trip, and Cook tests with exactly one source/layout/provenance
  authority.

### Stage 6: Generalize standalone source indexing

- [ ] Replace concrete family deserialization in `SourceReferenceIndex.cpp`
  with traversal through the main object's internal `AssetImportData`
  reference and common `FAssetImportInfo` fields.
- [ ] Preserve a bounded read-only fallback for unresaved legacy packages until
  the compatibility retirement gate is met.
- [ ] Remove family header dependencies and class-name filtering from the new
  index path; unknown subclasses with valid base source data remain indexable.
- [ ] Verify index refresh remains construct-free, bounded, cancelable, and
  stable across catalog revisions.

#### Acceptance Gate

- Source indexing discovers every new standalone source without constructing
  assets or naming concrete asset classes, and legacy fallback is tested.

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
- [ ] Validate failure injection at candidate creation, owner validation,
  one-way publication, package save, and bundle persistence.
- [ ] Validate deterministic source/import fingerprints and any deliberate DDC
  key/version transitions on clean and warm caches.
- [ ] Validate canonical resave on representative legacy packages and confirm
  the second authored load has no upgrade mutation.
- [ ] Validate source-reference indexing across standalone, six-source,
  panorama, missing-source, corrupt-reference, and unknown
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
| Inner objects | Authored save/load, subclass preservation, strong-reference GC lifetime, correct Outer/path, one-way publication, failed-save Dirty retention |
| Cook | No `AssetImportData` field, inner export, concrete class identity, replay schema string, source path, source hash, or replay payload in ordinary Cooked packages |
| Compatibility | Every family legacy fixture, consistent typed/provenance values, missing half, conflicting halves, unsupported contract, read-old/write-new, clean second load |
| Asset behavior | Initial import, save/load, reimport, repair, replace, relocate, changed/missing source, semantic no-op, DDC hit/miss/corruption, failed-save Dirty retention |
| TextureCube | Exact six-face roles, panorama-only role, duplicate/missing/inactive role rejection, one authored `SourceLayout` authority |
| VolumeTexture | Atlas recipe persistence, no duplicated path string, one-way content/import publication, payload and render-resource coherence |
| Terrain | Async admission, cancellation, supersession, revision coherence, source mutation, DDC recovery |
| StaticMesh | Standalone authority, Scene initial creation, axis recipe, material mapping, collision/render publication |
| Scene | Initial multi-output creation, deterministic paths, collision rejection, no aggregate record or reimport |
| Source index | Construct-free generic inspection, internal-reference corruption, unknown subclass, legacy fallback, bounded/cancelable refresh |
| Module boundary | No `AssetImportCore`; Engine owns common import metadata, AssetForge owns interchange replay, AssetCore and Engine have no AssetForge dependency, runtime-only closure does not deploy AssetForge |
| Determinism | Stable canonical import-data bytes, graph/authored fingerprints, DDC input equivalence or documented version bump |
| Documentation | Changed/all docs, all active plans, and updated long-lived ownership contracts |

## Definition of Done

- Every standalone imported Engine asset persists one EditorOnly
  `DAssetImportData` authority and no family-specific source/importer schema.
- The standalone `AssetImportCore` prototype has been removed; Engine owns the
  common import metadata contract and AssetForge owns the concrete interchange
  replay contract.
- Every mounted import source uses `FSourceFile`; every source content hash uses
  the same reflected XXH3-128 representation.
- Source path/hash are serialized once per authority and framework graph state
  references sources by stable identity.
- Engine assets and AssetCore have no AssetForge/AssetForgeBuiltins dependency.
- AssetForge can reconstruct initial recovery/reimport requests from the common
  import data and performs all translator/planning/builder selection.
- Asset content and import data publish once during editor-thread finalization;
  no live target references a candidate-owned inner object, and save failure
  leaves the published package Dirty with an explicit persistence result.
- Cooked packages contain no import-data reference, inner object, class/schema
  identity, or source/replay bytes.
- Scene import publishes independent assets and persists no aggregate replay or
  reconciliation authority.
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
- `Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/Persistence/ImportProvenance.h`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/Persistence/AssetForgeImportData.h`
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
