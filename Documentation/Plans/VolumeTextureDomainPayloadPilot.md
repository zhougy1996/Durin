# VolumeTexture Domain Payload Pilot Plan

Summary: Make VolumeTexture metadata and codecs the sole authority for authored and cooked voxel payload meaning.

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

All stages are complete. The source and cooked caller inventory confirms
VolumeTexture is the only production `FAuthoredBulkData` consumer. Source
dimensions and voxel format are already domain fields, while the source schema
version is incorrectly carried only by the authored storage descriptor. Cooked
TXPL already has a reflected domain schema and direct DBLK authority boundary.

The selected compatibility route preserves the physical DAST v4 BulkData and
DABK v1 layouts. Their former 16-byte format id and 4-byte format version slots
become ignored reserved bytes: old nonzero values remain readable, new output
canonicalizes them to zero, and a canonical resave is the supported migration.
Existing Array/Blob-to-Bulk VolumeTexture source migrations remain supported.

Public Archive/authored APIs now carry storage identity only. DAST/DABK readers
ignore historical reserved-slot values and writers emit zeros at the same
offsets. VolumeTexture source schema v1 is reflected and participates in domain
validation, DDC keys, and local build inputs. Focused Archive (9), AssetPackage
(106), VolumeTexture (7), and VolumeTexture source-import (6) tests pass.
The `fast-all` aggregate, Debug Editor `all` build, changed-document validator,
all-plan validator, and all-roadmap validator pass. No separate VolumeTexture
integration target is registered; the focused Texture target covers production
import/reimport, 128-cubed external storage, Cook, and cooked-runtime loading.

## Goal

Make `FVolumeTextureSourceData` and the TXPL cooked descriptor contain every
fact required to validate and interpret their opaque bytes, with no semantic
format translation or schema identity owned by authored bulk storage.

## Scope

- Remove semantic format id/version from `FArchiveBulkDataTransfer`,
  `FAuthoredBulkDataDescriptor`, `FAuthoredBulkData`, and DABK inspection APIs.
- Preserve DAST v4 and DABK v1 physical layouts while accepting old semantic
  slot values and writing canonical zeros.
- Add a reflected VolumeTexture source payload schema version and validate it
  with dimensions, format, bounds, payload id, and exact encoded byte count.
- Keep source voxel bytes as the existing tightly packed portable encoding and
  TXPL as the existing canonical cooked encoding.
- Prove inline/external source save/reload, legacy migration, DDC, Cook/runtime,
  corruption failure, and transactional publication.

## Non-Goals

- Changing DAST, DABK, DBLK, DDC, or TXPL physical framing or payload encoding.
- Adding a generic format registry, typed bulk value, or arbitrary C++ layout
  serialization.
- Migrating other texture, mesh, terrain, animation, or collision consumers.
- Redesigning authored/DDC/Cook services, async IO, or residency.
- Retaining exact obsolete semantic-slot bytes after canonical resave.

## Design Decisions and Invariants

- `FAuthoredBulkData` accepts payload id plus opaque bytes only. Its public
  descriptor owns physical identity, logical/stored byte counts, hashes, and
  placement; it owns no format/schema identity.
- The DAST/DABK slots previously named format id/version remain at the same
  offsets and widths but are reserved. Readers accept any historical values and
  ignore them; writers emit zero. This is a current-only canonicalization route,
  not a new wire version.
- `FVolumeTextureSourceData::PayloadSchemaVersion` is reflected, defaults to
  the current source schema for old packages, and must equal the supported
  version before bytes are interpreted.
- The source codec is tightly packed row-major depth slices with the byte width
  selected by `EVolumeTextureFormat`; checked arithmetic and the texture byte
  ceiling precede exact-size comparison.
- Payload id is storage identity only. VolumeTexture validates the expected
  source/cooked payload id as part of its wrapper consistency, but id equality
  never substitutes for schema metadata validation.
- Cooked post-load continues to validate TXPL schema/target/profile/compression
  before transactional decode and publishes no partial CPU or GPU state.
- Existing source Array and Blob migration routes remain load-only; current save
  emits BulkData plus the reflected domain schema.

## Current Foundations and Gaps

| Area | Foundation | Gap closed here |
| --- | --- | --- |
| Source metadata | Width, height, depth, and portable voxel format are reflected. | Source schema version lives in generic authored storage. |
| Source bytes | Atomic authored bytes, exact-size validation, Array/Blob migrations. | Setter supplies generic format id/version. |
| Authored wire | DAST v4 and DABK v1 are bounded and transactional. | Semantic fields are embedded in generic transfer/descriptors. |
| DDC | Key includes source bytes/metadata and schema constant; corrupt hits rebuild. | Key must use the validated domain schema field. |
| Cooked | Reflected descriptor and TXPL codec already qualify schema/target/profile. | Confirm no generic format translation remains end to end. |

## Implementation Stages

### Stage 0: Freeze schema and compatibility decisions

- [x] Inventory VolumeTexture authored, DDC, Cook, runtime, reimport, and
  historical migration paths.
- [x] Confirm VolumeTexture is the sole production authored-bulk consumer.
- [x] Freeze the source byte layout and supported schema version.
- [x] Select backward-read plus canonical-zero-resave for obsolete authored
  semantic slots without changing physical wire versions.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Remove authored storage semantics

- [x] Remove format id/version from public Archive transfer and authored bulk
  descriptor/value APIs.
- [x] Keep DAST/DABK slot offsets and widths, read historical values as ignored,
  and emit zero for current output.
- [x] Update bounded inspection, companion validation, editor diagnostics, and
  AssetCore tests for storage-only descriptors.
- [x] Add compatibility evidence that historical nonzero slots load and current
  canonical output uses zeros.

#### Acceptance Gate

- AssetCore focused tests prove old-slot acceptance, canonical-zero output,
  unchanged framing/bounds, exact immutable bytes, and transactional failures.

### Stage 2: Establish the VolumeTexture domain schema

- [x] Add reflected source payload schema metadata with a backward-compatible
  v1 default.
- [x] Validate schema, expected payload id, dimensions, format, byte arithmetic,
  and exact byte count as one domain object.
- [x] Make source replacement and DDC key generation consume only domain
  metadata plus opaque bytes.
- [x] Extend authored, DDC, Cook/runtime, malformed-schema, and last-known-good
  tests across inline and external placement.

#### Acceptance Gate

- Source save/reload and Cook/runtime results match prior decoded bytes; invalid
  schema or metadata-plus-bytes combinations fail before live/resource mutation.

### Stage 3: Document and qualify the pilot

- [x] Update authored package, lifecycle, serialization, and VolumeTexture
  contracts with the reserved-slot compatibility route and domain codec.
- [x] Run focused AssetCore/Texture tests, required workflow coverage,
  `fast-all`, the Debug Editor `all` build, and documentation validators.
- [x] Complete this plan and select Authority-Specific Payload Services in the
  roadmap.

#### Acceptance Gate

- All validation passes, lasting contracts contain the domain boundary, and no
  generic authored header/API carries payload format or schema meaning.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Historical authored read | DAST/DABK fixtures with nonzero obsolete slots load and verify bytes. |
| Canonical authored output | Current inline/external descriptors emit zero reserved slots with stable framing. |
| Domain source codec | Format-specific exact-size vectors, schema/id mismatch, overflow, and malformed metadata tests. |
| Structural planning | 128-cubed source remains one bulk property node. |
| DDC | Stable key includes domain schema; changes miss/rebuild; corruption does not publish. |
| Cook/runtime | TXPL/DBLK round trip, wrong schema/target/profile, missing/corrupt companion, last-known-good behavior. |
| Workflow | Import, save, reload, reimport, Cook, move/delete/recovery remain transactional. |
| Aggregate | Focused targets, required integration, `fast-all`, Debug Editor build, documentation validation. |

## Definition of Done

- VolumeTexture source/cooked metadata and codecs are sufficient to interpret
  opaque bytes and reject malformed combinations transactionally.
- Generic authored storage/Archive public APIs expose no semantic format id or
  schema version.
- Historical nonzero semantic slots remain readable; current output
  canonicalizes the unchanged physical slots to zero.
- DAST/DABK placement, DDC, TXPL/DBLK decode, and GPU resource publication
  retain their authority-specific behavior and pass required tests.

## Deferred Follow-ups

- Remove reserved authored wire slots only in an explicitly versioned future
  DAST/DABK format change with corpus justification.
- Generalize scalar codec helpers only after another domain proves identical
  durable rules.
- Authority-specific service cleanup belongs to the next roadmap child.

## Related Documentation

- [Large Asset Payload Architecture](../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Bulk Payload Layer Realignment](BulkPayloadLayerRealignment.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Build and Run](../Agents/BuildAndRun.md)
- [Testing](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AuthoredBulkData.h`
- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkData.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AuthoredBulkStorage.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Tests/Native/CoreTests/Private/ArchiveTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
