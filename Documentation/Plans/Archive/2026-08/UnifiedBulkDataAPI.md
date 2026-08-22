# Unified BulkData API Plan

Summary: Unify authored and cooked payload identity, immutable access, and synchronous residency behind provider-owned storage without changing DABK or DBLK formats

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-22

## Current Status

All stages are complete. AssetCore exposes the provider-neutral
`FBulkDataDescriptor`, `EBulkDataStorageDomain`, `IBulkDataProvider`, and
`FBulkData` contracts with centralized descriptor and immutable-buffer
verification. `FAuthoredBulkData` now composes that value and delegates common
identity, residency, failure, immutable buffer, and synchronous loading while
retaining its authored descriptor, replacement, Archive, and DABK boundary.

The cooked adapter maps the reflected DBLK descriptor into the common logical
descriptor and retains package path, target/profile, offsets, compression, and
container lifetime as provider metadata. `DVolumeTexture` defines a TXPL format
GUID and consumes the adapter through the same unloaded/load/resident byte-view
surface while Cook production and reflected serialization remain unchanged.

`AssetPackageTests` passes 108/108 cases, `AssetImportTests` passes 17/17,
`AssetCookTests` passes 13/13, `TextureTests` passes 80/80, and
`TextureCookIntegrationTests` passes 1/1. The full native aggregate and Win64
Debug Editor `all` build pass. Changed/all documentation, plan, and roadmap
validation pass, and lasting contracts now document the common API without
merging lifecycle authority or storage formats.

The selected first end-to-end cooked consumer is `DVolumeTexture`. Its authored
source and cooked TXPL payload already exercise both lifecycle domains in one
asset without requiring a second consumer migration. Existing DAST, DABK, DBLK,
DDC keys, and texture payload bytes are compatibility baselines, not refactor
targets.

## Goal

Give asset consumers one `FBulkData` contract for placement-independent
identity, immutable resident bytes, residency/failure state, and explicit
synchronous loading. Authored and cooked storage remain separate providers with
their current authority, paths, wire formats, verification, and publication
rules.

## Scope

- Add `Asset/BulkData.h` and its AssetCore implementation with a common logical
  descriptor, domain identity, immutable byte ownership, and synchronous
  provider contract.
- Make descriptor validation and content verification common and transactional:
  failed loads retain no candidate bytes and expose one stable failure state.
- Reimplement `FAuthoredBulkData` as the reflected compatibility facade over
  `FBulkData` while retaining the authored descriptor and Archive/DABK codecs.
- Add a cooked DBLK provider adapter that translates one
  `FCookedPayloadDescriptor` plus consumer-supplied semantic format identity
  into `FBulkData`; DBLK offsets, target/profile, compression, and package path
  remain provider metadata.
- Migrate volume texture authored voxel access and cooked TXPL loading to the
  common identity, byte-view, residency, and synchronous load operations.
- Preserve public low-level DABK/DBLK functions for package code, tests, and
  compatibility callers during this milestone.
- Update lasting Asset and volume-texture contracts after implementation.

## Non-Goals

- Changing the DAST BulkData opcode, DABK v1, DBLK, TXPL, Cook manifests, DDC
  keys, package companion naming, or publication ordering.
- Making `FCookedPayloadDescriptor` a reflected `FBulkData` field or persisting
  the provider object in a `.dasset` package.
- Adding a generic mutable lock, common publish/write transaction, or allowing
  cooked payload mutation through `FBulkData`.
- Adding asynchronous requests, cancellation, eviction, mapping, residency
  budgets, compression changes, deduplication, or remote virtualization.
- Building a DDC provider or migrating textures, meshes, terrain, animation,
  collision, or lighting beyond the selected volume-texture cooked path.
- Removing the authored facade or the low-level cooked container APIs before
  all existing callers have bounded follow-up migrations.

## Design Decisions and Invariants

- `Asset::FBulkDataDescriptor` is placement-independent and contains exactly
  `PayloadId`, `FormatId`, `FormatVersion`, `LogicalByteCount`,
  `StoredByteCount`, and `ContentHash`. Logical equality uses all six fields.
- `StoredByteCount` describes the selected encoded payload bytes, not container
  overhead. Container hashes, offsets, alignment, target/profile, compression,
  paths, and cache keys are provider-owned metadata and do not participate in
  logical descriptor equality.
- `Asset::EBulkDataStorageDomain` uses `None` only for the default empty value
  and distinguishes `Authored`, `Derived`, and `Cooked` for payloads. This
  milestone implements authored and cooked providers; `Derived` reserves the
  vocabulary needed by a later DDC adapter without treating a DDC miss as
  authored loss.
- `Asset::IBulkDataProvider` is an immutable, shareable load capability. It
  exposes its storage domain and a synchronous operation that resolves and
  verifies one requested descriptor into `FSharedByteBuffer`. The provider may
  retain typed metadata internally, but consumer code receives no backend path,
  offset, or write operation.
- `Asset::FBulkData` owns the common descriptor, optional immutable resident
  buffer, shared provider, residency state, and diagnostic string. It is
  copyable; copies share immutable bytes and provider capability but maintain
  independent state transitions.
- A default `FBulkData` is an empty resident value. Any non-empty value must
  have valid payload/format GUIDs, a nonzero format version, consistent sizes,
  and a content hash matching resident bytes before publication.
- The common API is read-only after construction except for residency:
  `GetDescriptor`, `GetStorageDomain`, `GetResidency`, `GetFailure`,
  `IsResident`, `GetResidentBytes`, and `LoadSynchronous`. Domain-specific
  replacement stays on `FAuthoredBulkData::ReplaceBytes`.
- `LoadSynchronous` is idempotent when resident, rejects missing providers,
  validates sizes and XXH128 before committing, and moves to `Failed` with an
  actionable diagnostic on error. It never exposes partial or unverified bytes.
- `FAuthoredBulkDataDescriptor` remains the DAST/DABK storage descriptor during
  this plan. The facade converts its placement-independent fields to the common
  descriptor and retains `ContainerHash`/`StorageKind` only for authored Archive
  and package publication.
- Authored inline bytes use an authored provider-neutral resident value;
  authored external loads use an authored companion provider. The existing
  loader callback may remain private compatibility machinery during migration,
  but callers consume only the common surface.
- Cooked DBLK adaptation is explicit: it accepts the runtime configuration,
  virtual package path, reflected cooked descriptor, expected target/profile,
  and a consumer-owned `FormatId`. `PayloadSchemaVersion` becomes the common
  format version; uncompressed/stored sizes and payload hash map without wire
  changes.
- `DVolumeTexture` defines a stable TXPL format GUID for the adapter. It keeps
  its reflected `FCookedPayloadDescriptor` so cooked `.dasset` bytes remain
  unchanged, constructs an unloaded `FBulkData` only at the load boundary, then
  decodes the verified resident view transactionally into platform data.
- `FArchiveBulkDataTransfer` remains Core's serialization mechanism. It is not
  promoted into the public asset value because container hash and placement are
  Archive/storage concerns.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| Bytes | Core `FSharedByteBuffer` provides immutable shared ownership. | No asset-level value applies common descriptor and residency rules. |
| Archive | `FArchiveBulkDataTransfer` preserves semantic identity and verified bytes. | Archive residency vocabulary is coupled to serialization rather than a provider contract. |
| Authored | `FAuthoredBulkData` supports replacement, sync load, DAST, and DABK. | Its descriptor, callback loader, and access API are authored-specific. |
| Cooked | DBLK descriptors, container verification, and package payload loading are qualified. | `FCookedPackagePayload` exposes a separate lifetime/view type and no common format GUID. |
| Consumer | Volume source uses authored bulk; volume Cook/runtime uses DBLK/TXPL. | The two paths use different identity, state, and byte-access protocols. |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Compare the authored descriptor/value/load contract with the cooked
  descriptor/container/package-load contract.
- [x] Freeze common logical fields separately from provider metadata.
- [x] Select immutable shared providers and synchronous transactional residency;
  exclude generic mutation and asynchronous policy.
- [x] Select volume TXPL as the first cooked adapter consumer and preserve its
  reflected cooked descriptor.
- [x] Freeze DAST, DABK, DBLK, DDC, and TXPL compatibility baselines.

#### Acceptance Gate

- The common API, provider ownership, conversion rules, compatibility facade,
  first consumer, non-goals, and validation requirements are explicit with no
  unresolved format or transaction decision.

### Stage 1: Add the provider-neutral value contract

Depends on Stage 0.

- [x] Add the common descriptor, domain enum, provider interface, and `FBulkData`
  public API under AssetCore.
- [x] Centralize descriptor validation, resident-buffer size/hash verification,
  and transactional synchronous load behavior.
- [x] Cover empty, resident, unloaded, successful, repeated, corrupt, missing
  provider, failed, copy, and logical-equality behavior in AssetCore tests.
- [x] Keep Core Archive types and physical storage metadata out of the common
  public descriptor.

#### Acceptance Gate

- Focused AssetCore tests prove one immutable value contract independently of
  DABK and DBLK; failed providers cannot publish bytes and repeated loads are
  deterministic.

### Stage 2: Adapt authored bulk behind the common value

Depends on Stage 1.

- [x] Compose `FBulkData` inside `FAuthoredBulkData` and delegate descriptor,
  residency, failure, byte-view, and synchronous-load behavior.
- [x] Preserve detached authored replacement and translate Archive transfers to
  and from the existing authored storage descriptor without changing bytes.
- [x] Adapt external DABK loading through an authored provider while preserving
  companion path resolution, container-hash checks, and package transaction
  ownership.
- [x] Retain reflection registration, default construction, copying,
  `Identical`, old Array/Blob migration, and editor summaries.
- [x] Add golden/current-package, inline/external, corruption, save/reload,
  relocation, deletion, recovery, and reimport regression coverage.

#### Acceptance Gate

- Volume authored source consumes the common read/residency surface through the
  facade; qualified `.dasset` and `.dabulk` bytes and package behaviors remain
  unchanged.

### Stage 3: Adapt one cooked payload and migrate volume runtime loading

Depends on Stages 1-2.

- [x] Add a DBLK provider factory that owns cooked package metadata and maps the
  existing cooked descriptor into a common logical descriptor.
- [x] Preserve target/profile, compression, offset, bounds, container, and hash
  validation by delegating to the qualified cooked loader before common commit.
- [x] Define the volume TXPL format GUID and migrate
  `DVolumeTexture::LoadCookedPlatformData` to `FBulkData::LoadSynchronous` and
  `GetResidentBytes`.
- [x] Keep cook production and reflected `FCookedPayloadDescriptor` serialization
  unchanged; retain low-level `FCookedPackagePayload` APIs for other consumers.
- [x] Add adapter and volume cooked-runtime tests for success, descriptor
  mismatch, wrong format/version/target/profile, missing DBLK, corruption,
  repeated load, and transactional last-known-good publication.

#### Acceptance Gate

- One volume asset uses the same logical identity, immutable byte view,
  residency/failure state, and synchronous load operation for authored DABK and
  cooked DBLK while all persisted and cooked golden bytes remain unchanged.

### Stage 4: Integrate, document, and qualify the milestone

Depends on Stages 1-3.

- [x] Update Asset package/data-lifecycle and volume-texture documentation with
  the common API and explicit provider/authority boundaries.
- [x] Update the parent roadmap milestone state and leave deferred consumer/DDC
  migrations in their owning future milestones.
- [x] Run focused Core, AssetCore, package, texture build/cook/runtime tests.
- [x] Run the full native aggregate and Debug Editor build using repository
  guidance.
- [x] Run changed/all documentation, plan, and roadmap validation and verify a
  clean diff.

#### Acceptance Gate

- All validation is green; lasting contracts name the common API without
  claiming unified storage authority; the roadmap records Milestone 2 complete
  and selects Milestone 3 only if its entry evidence exists.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Logical identity | Unit tests cover all common descriptor fields, invalid identities, equality, and provider metadata exclusion. |
| Immutable access | Resident and loaded byte views are backed by `FSharedByteBuffer`; copies cannot mutate or dangle. |
| Residency | Empty/resident/unloaded/failed, idempotent load, failure diagnostics, and no partial publication are tested. |
| Authored compatibility | DAST/DABK goldens or exact byte comparisons, inline/external save/reload, migration, reimport, move/delete, and recovery stay green. |
| Cooked compatibility | DBLK/TXPL exact bytes, manifest descriptors, target/profile/compression validation, missing/corrupt companion failures, and runtime load stay green. |
| Consumer convergence | Volume authored and cooked tests assert the common descriptor/access/residency vocabulary. |
| Aggregate | Focused native targets, full native aggregate, Debug Editor build, and documentation lifecycle validators pass. |

## Definition of Done

- `FBulkData` is the only new consumer-facing read/residency abstraction and is
  independent of DABK, DBLK, paths, offsets, and write authority.
- `FAuthoredBulkData` remains source-compatible and wire-compatible while
  delegating common behavior.
- A DBLK provider maps cooked metadata into `FBulkData` without changing Cook
  output, and volume cooked runtime loading uses it in production code.
- All failure paths validate before mutation and retain no unverified resident
  bytes or partially decoded platform data.
- Lasting documentation, focused tests, full native tests, the Debug Editor
  build, and documentation validation are green.

## Deferred Follow-ups

- Provider-neutral asynchronous request/cancellation handles, priorities,
  budgets, eviction, mapping, and stale-completion rejection.
- A DDC provider whose cache-miss/rebuild semantics remain derived-data-owned.
- Direct common-value adoption by other cooked textures, static/skeletal mesh,
  terrain, animation, collision, and environment lighting.
- Retirement of `FAuthoredBulkData`, `FCookedPackagePayload`, or other
  compatibility facades after their caller migrations are separately bounded.
- Portable typed atomic buffers over `FBulkData` for consumers that require
  stable element metadata.

## Related Documentation

- [Large Asset Payload Architecture Roadmap](../../../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Authored Asset Bulk Data Foundation](AuthoredAssetBulkDataFoundation.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/SharedByteBuffer.h`
- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/AssetCore/Private/BulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkStorage.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkStorage.cpp`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureCookTests.cpp`
