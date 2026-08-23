# Bulk Payload Layer Realignment Plan

Summary: Realign common bulk payload code around opaque immutable bytes and authority-specific storage services.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

All stages are complete. The inventory found one production `IBulkDataProvider`,
the cooked DBLK adapter; authored payloads are always resident and derived data
does not use the common provider. The selected implementation therefore removes
the unproven cross-authority residency API while preserving all DAST, DABK,
DBLK, Cook manifest, and reflected package bytes.

`FBulkData` now owns only verified immutable resident bytes and their
storage-neutral identity. Authored data retains its current wire descriptor;
Cooked VolumeTexture loads through `LoadCookedPackagePayload` without a common
descriptor translation. AssetBulkContainer (11), AssetPackage (106), AssetCook
(13), and focused VolumeTexture (7) tests pass. The `fast-all` aggregate,
Win64 Debug Editor `all` build, changed-document validator, all-plan validator,
and all-roadmap validator also pass.

## Goal

Make the common payload layer express only opaque immutable byte ownership and
integrity, while authored packages and cooked deployment retain independent
descriptors, loading, publication, and failure behavior.

## Scope

- Inventory every common bulk descriptor, provider, residency, and domain
  caller and record its disposition.
- Remove semantic format/version, stored-size, and authority identity from the
  common descriptor.
- Remove the common provider and unloaded/failed residency state because only
  one authority uses it.
- Keep authored DAST/DABK descriptors and publication byte-compatible while
  composing them with the narrowed common resident-byte owner.
- Remove the cooked-to-common adapter and load VolumeTexture cooked bytes
  through the existing cooked package service.
- Update focused tests and lasting runtime documentation.

## Non-Goals

- Changing DAST, DABK, DBLK, Cook manifest, DDC, or reflected package formats.
- Removing the authority-specific authored descriptor fields required for
  current wire compatibility; their domain-schema migration belongs to the
  VolumeTexture pilot.
- Adding async IO, cancellation, range loading, eviction, compression,
  deduplication, or virtualization.
- Migrating dense consumers other than the minimal VolumeTexture cooked caller
  needed to retire the adapter.

## Design Decisions and Invariants

- `FBulkDataDescriptor` becomes a storage-neutral byte identity containing only
  payload id, logical byte count, and content hash.
- `FBulkData` is a verified immutable resident byte owner. Empty default values
  are valid; non-default creation is transactional.
- Authority-specific stored sizes, compression, placement, format/schema, and
  failure policy remain on authored or cooked descriptors and services.
- `IBulkDataProvider`, `EBulkDataStorageDomain`, `EBulkDataResidency`, and
  `CreateCookedPackageBulkData` are removed rather than renamed because only the
  cooked adapter uses unloaded common state.
- Cooked VolumeTexture validates the reflected cooked descriptor's domain
  schema and target before calling `LoadCookedPackagePayload`, then decodes the
  returned opaque bytes transactionally.
- Existing container and package bytes remain exact. This plan changes C++ APIs
  only; no historical route or resave is introduced.

## Current Foundations and Gaps

| Area | Foundation | Selected disposition |
| --- | --- | --- |
| Shared bytes | `FSharedByteBuffer` provides immutable shared ownership. | Keep. |
| Common descriptor | Carries id, format, version, logical/stored sizes, hash. | Keep only id, logical size, hash. |
| Common provider | One cooked implementation; no authored/DDC implementation. | Remove. |
| Authored data | Resident `FEditorBulkData` plus transactional DAST/DABK lifecycle. | Preserve wire and publication behavior; use narrowed common bytes. |
| Cooked data | DBLK descriptor/service plus a synthetic common adapter. | Keep DBLK service; remove adapter. |
| VolumeTexture | Only production adapter caller. | Load opaque cooked bytes directly and retain domain validation. |

## Implementation Stages

### Stage 0: Freeze the boundary and compatibility matrix

- [x] Inventory common descriptor/provider/domain/residency callers.
- [x] Confirm authored payloads are resident and derived data has no common
  provider dependency.
- [x] Select current-wire preservation for DAST, DABK, DBLK, Cook manifests,
  and reflected packages.
- [x] Record the removal and migration disposition of every experimental API.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Narrow common bytes and preserve authored behavior

- [x] Reduce the common descriptor to payload id, logical byte count, and hash.
- [x] Replace common provider/residency construction with transactional resident
  construction.
- [x] Migrate the value now named `FEditorBulkData` to the narrowed common owner without changing
  archive transfers or authored descriptors.
- [x] Rewrite focused common-byte tests around immutable sharing, integrity,
  invalid identity, and transactional replacement.

#### Acceptance Gate

- AssetCore compiles; focused common and authored package tests prove exact
  byte sharing, corruption rejection, and unchanged transactional behavior.

### Stage 2: Restore the cooked authority boundary

- [x] Remove the cooked provider adapter and its public factory.
- [x] Change VolumeTexture cooked loading to use `LoadCookedPackagePayload`
  directly and validate its domain schema before decode.
- [x] Replace adapter tests with authority-specific load and failure tests.

#### Acceptance Gate

- Cooked descriptor selection, target mismatch, missing/corrupt companions, and
  VolumeTexture cooked decode retain their existing failure behavior without a
  common descriptor translation.

### Stage 3: Document and validate the realigned layer

- [x] Update Asset lifecycle, package, reflection, and VolumeTexture contracts
  to describe opaque common bytes and authority-specific services.
- [x] Run focused AssetCore and VolumeTexture tests, then the registered shared
  runtime aggregate required by the roadmap.
- [x] Validate all active plans/roadmaps and changed documentation.
- [x] Update roadmap status and completion evidence.

#### Acceptance Gate

- Focused and aggregate validation pass, documentation contains no stale common
  provider contract, and the roadmap selects the VolumeTexture pilot next.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Common boundary | Compile-time removal plus focused `FBulkDataTests`. |
| Authored compatibility | Existing authored package/companion golden and transaction tests. |
| Cooked compatibility | Existing DBLK golden, manifest, path, corruption, and runtime tests. |
| Structural planning | Existing reflected authored bulk atomic-node tests. |
| Domain decode | Existing VolumeTexture source/cook/runtime round trips. |
| Aggregate | Registered AssetCore/Engine domains and `fast-all` as required by failures/risk. |
| Documentation | Changed-doc, all-plan, and all-roadmap validation. |

## Definition of Done

- Common bulk headers contain no semantic format/version, stored-size,
  authority enum, provider, or cross-authority residency API.
- Authored and cooked paths use their own descriptors and retain exact current
  wire formats and transactional failure behavior.
- VolumeTexture contains no cooked-to-common descriptor translation.
- Lasting documentation describes the implemented boundary and required tests
  pass.

## Deferred Follow-ups

- Move VolumeTexture source schema identity fully out of the authored byte
  descriptor in the VolumeTexture Domain Payload Pilot.
- Reconsider a shared request primitive only after two authorities demonstrate
  the same async or range-IO contract.
- Migrate additional dense domains through separate consumer plans.

## Related Documentation

- [Large Asset Payload Architecture](../../../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Generated Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Build and Run](../../../Agents/BuildAndRun.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/AssetCore/Private/BulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkDataTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
