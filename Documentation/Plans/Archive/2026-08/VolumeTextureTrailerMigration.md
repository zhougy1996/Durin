# VolumeTexture Trailer Migration Plan

Summary: Qualify VolumeTexture authored voxels through the explicit DAST v5 trailer route with rollback and lifecycle equivalence.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Completed. `FVolumeTextureAuthoringOptions` carries an operation-local writer
selection through synchronous and asynchronous import, reimport-request, and
repair boundaries; ordinary calls still select v4 and `DVolumeTexture` stores
no format preference. Inline and 2 MiB external fixtures qualified v5
inspection, reload, corruption rejection/recovery, reimport, identical source
and DDC identity, byte-identical cooked `.dasset`/DBLK output, explicit v4
rollback, orphan retention, and delete cleanup. The pilot also corrected
construct-free inline BulkData inspection to decode the canonical bounded byte
blob before checking its hash. Focused VolumeTexture tests passed 6/6, the full
Texture aggregate passed 86/86, and AssetPackage tests passed 108/108. No
tracked content asset was mutated. Milestone 4 continues through
[Authored Trailer Corpus Migration](AuthoredTrailerCorpusMigration.md).

## Goal

Pilot DAST v5 for VolumeTexture authored voxel packages without changing its
reflected schema, payload id, source codec, DDC key, Cook output, runtime data,
or ordinary behavior for any other asset type.

## Scope

- Add an explicit VolumeTexture authoring/pilot selection that reaches package
  save and bundle publication as `DastV5` without global or ambient policy.
- Prove small inline and large external voxel sources save, inspect, reload,
  reimport, rebuild after DDC miss, Cook, and load cooked output equivalently.
- Prove trailer/descriptor/DABK corruption diagnostics, repair ownership,
  orphan handling, relocation, deletion, source control closure, and rollback.
- Qualify canonical v5-to-v4 resave and exact disposition of the v5 companion
  generation before considering corpus migration.

## Non-Goals

- Changing `FVolumeTextureSourceData`, voxel schema/version, payload GUID,
  portable formats, builders, DDC key, TXPL, DBLK, or GPU resource policy.
- Making v5 the ordinary writer for VolumeTexture or any other domain.
- Converting tracked assets, changing `.dasset`/`.dabulk` Git/LFS rules, or
  deleting legacy companions.
- Virtualization, package-local bytes, compression, deduplication, or backend
  work.

## Design Decisions and Invariants

- The pilot selection is explicit at the authoring operation boundary and is
  test-visible. Domain objects never store a package-format preference.
- `VolumeTextureSourcePayloadId` and the reflected source schema are unchanged;
  only the package envelope and mandatory trailer lookup differ.
- Small authored voxel values remain inline with no trailer entry. Values at or
  above 256 KiB remain in generation-named DABK v1 and have exactly one matching
  trailer entry.
- DDC and Cook consume verified resident bytes, so v4 and v5 authored inputs
  must produce identical keys, derived payloads, manifests, and cooked runtime
  results.
- Rollback publishes a complete v4/DABK closure before v5-only bytes become
  cleanup candidates. Inspection and repair never mutate implicitly.

## Implementation Stages

### Stage 0: Freeze pilot entry and fixtures

- [x] Identify the explicit VolumeTexture authoring calls that opt into v5 and
  prove all ordinary calls remain v4.
- [x] Freeze inline/external fixtures, lifecycle matrix, failure points, source
  control closure, and v4 rollback expectations.

#### Acceptance Gate

- The pilot cannot affect another domain or persist ambient writer policy.

### Stage 1: Integrate editor save, reload, and inspection

- [x] Route selected VolumeTexture single/bundle publication through v5 without
  schema or `FEditorBulkData` changes.
- [x] Cover save/reload, reimport, move, delete, construct-free inspection,
  repair diagnostics, orphan discovery, and companion corruption.
- [x] Prove catalog and publication failures preserve the prior complete
  VolumeTexture closure.

#### Acceptance Gate

- Inline and external pilot packages round-trip exactly and every failure keeps
  one loadable closure.

### Stage 2: Prove DDC, Cook, and runtime equivalence

- [x] Compare v4 and v5 source identity, DDC keys, hit/miss/rebuild output, and
  source/import provenance.
- [x] Compare Cook manifests, `.dasset` metadata, DBLK bytes, decoded CPU mips,
  and cooked runtime/GPU handoff.
- [x] Prove no trailer or DABK authoring fact leaks into derived or cooked
  payload schemas.

#### Acceptance Gate

- V4 and v5 authored inputs produce identical domain-owned derived and cooked
  results.

### Stage 3: Qualify rollback and hand off corpus migration

- [x] Canonically resave pilot packages to v4, reload/rebuild/Cook again, and
  classify retained and orphaned DABK generations exactly.
- [x] Record Git/LFS submit closure and verify no tracked asset is changed by
  automated tests.
- [x] Update roadmap Milestone 3 and activate exactly one corpus/default-writer
  plan after focused/regression and documentation validation pass.

#### Acceptance Gate

- The pilot and rollback matrices pass with no schema change, leaked authority,
  tracked mutation, or unresolved companion disposition.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Authoring | Explicit v5 selection; ordinary VolumeTexture and other assets remain v4 |
| Storage | Inline has no entry; external has one exact trailer/descriptor/DABK match |
| Lifecycle | Save/reload, reimport, move, delete, inspection, repair, orphan, and failures |
| Derived | Identical DDC key, hit/miss/rebuild product, and provenance for v4/v5 |
| Cook | Identical cooked package semantics, DBLK, manifest, decoded mips, and runtime load |
| Rollback | Canonical v5-to-v4 resave plus exact companion reachability disposition |
| Source control | `.dasset` Git and `.dabulk` LFS closure unchanged; no tracked mutation |
| Repository | Focused VolumeTexture, package, trailer, DDC, Cook, texture, and documentation validation |

## Definition of Done

- VolumeTexture explicitly exercises v5 across its complete authored/derived/
  cooked lifecycle with no domain schema or output change.
- Every failure and rollback retains a verifiable authored voxel generation.
- Other assets and ordinary VolumeTexture saves remain v4.
- Milestone 3 is complete and exactly one corpus/default-writer plan is active.

## Deferred Follow-ups

- Tracked corpus conversion and default-writer selection.
- Virtualization, optimization, and legacy retirement remain roadmap-owned.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [Selected Local Authored Payload Publication](SelectedLocalAuthoredPayloadPublication.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureBuildOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/VolumeTextureSourceImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
