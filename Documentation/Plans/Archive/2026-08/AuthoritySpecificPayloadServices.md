# Authority-Specific Payload Services Plan

Summary: Finalize independent authored, derived-cache, and cooked payload service boundaries while retaining only proven shared mechanics.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

All stages are complete. `LoadCookedPackagePayload` remains the proven cooked
service used by texture, mesh, terrain, animation, collision, and
environment-lighting domains, while derived data remains behind domain
build/session APIs. The UE-style `FEditorBulkData` now exposes only verified
storage-neutral bytes and atomic replacement; DAST/DABK placement types and
transactions live in `EditorBulkDataStorage`.

Authored package tests (106), private bulk-container tests (11), focused
VolumeTexture/source-import tests (13), and AssetCook tests (13) pass. Aggregate
and documentation validation are recorded by Stage 3. The next bounded child is
Texture2D payload consumer qualification, selected from tracked multi-megabyte
source images and its already-small descriptor-only packages.

The selected change removes that authored physical descriptor from the
domain-facing value and relocates storage-only types to the authored storage
capability header. Cooked and DDC APIs remain independent; no universal service
or mutation interface is introduced.

## Goal

Make authored mutation/publication, derived-cache rebuild, and cooked deployment
load three explicit authority boundaries whose callers cannot accidentally
inherit another authority's placement, fallback, or failure policy.

## Scope

- Remove authored placement/container descriptor access from
  `FEditorBulkData` and keep its domain surface to opaque verified bytes plus
  atomic replacement/serialization.
- Move editor-package storage kind, physical descriptor, and publication
  payload types into `EditorBulkDataStorageTypes.h`.
- Preserve DAST/DABK wire, package inspection, editor atomic-property summary,
  publication, recovery, relocation, and deletion behavior.
- Record `LoadCookedPackagePayload`/`FCookedPackagePayload` as the synchronous
  immutable cooked authority used by multiple domains.
- Record build/session APIs as the derived authority; retain cache miss as
  rebuildable and corruption as non-publishing failure.
- Prove authored replacement/recovery, DDC miss/rebuild, and cooked hard failure
  independently.

## Non-Goals

- A shared authored/DDC/cooked provider, descriptor, mutation API, or wire
  format.
- Consumer schema changes, broad migrations, async IO, residency budgets, or
  virtualization.
- Hiding cooked descriptors from domains; they contain deployment schema,
  target, compression, range, and integrity facts required for decode.
- Moving private bounded container mechanics or changing DAST/DABK/DBLK bytes.

## Design Decisions and Invariants

- UE-style `FEditorBulkData` stores only storage-neutral verified bytes; Archive
  capture owns physical placement state for save and reconstructs only those
  bytes on load.
- `EEditorBulkDataStorageKind`, `FEditorBulkDataStorageDescriptor`, and
  `FEditorBulkDataStoragePayload` are authority types declared by
  `EditorBulkDataStorageTypes.h`, not by the domain-facing editor byte value header.
- Live property UI reports logical byte count and hash from storage-neutral
  `FBulkData`; package inspection remains the authority for physical placement.
- Authored replacement is allowed only through the value and package
  publication transaction. Cooked package payloads expose no mutation and have
  no source/DDC fallback. Derived misses may rebuild only through their domain
  builder.
- `FCookedPackagePayload` may expose its validated container lifetime because
  multi-payload domains resolve related descriptors from one package; it does
  not expose mutable bytes or a provider abstraction.
- Shared code remains limited to immutable byte buffers, Archive operations,
  hashing, checked arithmetic, bounded readers/writers, and private layout
  validation.

## Current Foundations and Gaps

| Authority | Existing service | Disposition |
| --- | --- | --- |
| Authored | Package Archive plus DABK transaction/recovery functions. | Keep behavior; hide physical descriptor from domain value. |
| Derived | Build definitions/sessions and domain build operations. | Keep; miss/rebuild and corruption policy remain domain-owned. |
| Cooked | Reflected `FCookedPayloadDescriptor` plus `LoadCookedPackagePayload`. | Keep; multiple production consumers prove the boundary. |
| Shared mechanics | Immutable buffers and private bounded container infrastructure. | Keep below every authority. |

## Implementation Stages

### Stage 0: Inventory and freeze authority boundaries

- [x] Inventory authored storage APIs and domain-facing descriptor access.
- [x] Inventory production cooked service callers and multi-payload use.
- [x] Confirm DDC callers remain behind domain build/session APIs.
- [x] Select the authored descriptor visibility cleanup and explicit keep/remove
  disposition for every current service.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Close the authored physical-metadata leak

- [x] Move authored storage types out of `EditorBulkData.h` and remove public
  physical descriptor access from `FEditorBulkData`.
- [x] Let Archive capture own placement state and retain exact DAST/DABK behavior.
- [x] Change live property summaries to storage-neutral facts while keeping
  construct-free package inspection placement-aware.
- [x] Update focused header/API, authored lifecycle, and physical-container
  tests.

#### Acceptance Gate

- Engine domains cannot access authored placement/container metadata through
  the value header; package workflows and wire goldens remain green.

### Stage 2: Prove independent authority failure policies

- [x] Qualify authored detached replacement and missing/corrupt companion
  rollback/retry behavior.
- [x] Qualify VolumeTexture DDC hit, miss/rebuild, corrupt-result rejection, and
  last-known-good publication.
- [x] Qualify cooked target/profile/schema, missing/corrupt companion, and
  no-source/no-DDC fallback behavior through production services.
- [x] Confirm no shared provider/mutation/fallback abstraction remains.

#### Acceptance Gate

- Each authority's failure fixture passes independently and no failure path
  publishes partial bytes, decoded state, or runtime resources.

### Stage 3: Document and validate the services

- [x] Update lifecycle, package, and VolumeTexture contracts with the final
  authority capability boundaries.
- [x] Run focused tests, `fast-all`, the Debug Editor `all` build, and document
  validators.
- [x] Complete the plan and select the first evidence-backed consumer migration
  or inspection child in the roadmap.

#### Acceptance Gate

- Lasting docs and tests agree on authority ownership, the full build passes,
  and the roadmap records the next bounded child from measured evidence.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Authored API | Compile boundary plus authored bulk/package tests. |
| Authored durability | Inline/external save/reload, corruption, recovery, move/delete fixtures. |
| Derived policy | Volume DDC stable hit, input-sensitive miss/rebuild, corrupt rejection. |
| Cooked policy | AssetCook plus Volume cooked-runtime missing/corrupt/target/schema fixtures. |
| Wire compatibility | Existing DAST/DABK/DBLK golden hashes and round trips. |
| Aggregate | `fast-all`, Debug Editor `all`, changed-doc/all-plan/all-roadmap validation. |

## Definition of Done

- Domain-facing authored bytes expose no placement, container, stored-size, or
  publication descriptor.
- Authored, DDC, and cooked services retain independent mutation, fallback,
  publication, and failure rules with no universal provider.
- Shared physical mechanics remain private and semantic-free.
- Focused/aggregate validation and lasting documentation pass.

## Deferred Follow-ups

- Domain-qualified inspection across authorities belongs to the inspection
  milestone after a second dense consumer is selected.
- Async request/cancel/budget work remains evidence-gated.
- Further public/private header tightening requires measured downstream include
  impact and is not needed to establish the capability boundary.

## Related Documentation

- [Large Asset Payload Architecture](../../../Roadmaps/Archive/2026-08/LargeAssetPayloadArchitecture.md)
- [VolumeTexture Domain Payload Pilot](VolumeTextureDomainPayloadPilot.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [Build and Run](../../../Agents/BuildAndRun.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorageTypes.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CookedAsset.h`
- `Engine/Source/Runtime/AssetCore/Private/CookedAsset.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/VolumeTextureBuildOperations.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
