# Authored Package Trailer Foundation Plan

Summary: Implement the bounded DAST v5 trailer and footer codec, construct-free inspection, and exact compatibility fixtures for external DABK v1 placement.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

The bounded trailer/footer foundation is implemented and qualified. It emits
detached canonical bytes, discovers and validates the trailer from EOF without
constructing objects, supports only external DABK v1 placement, and has exact
zero/one/multiple golden fixtures plus structural, integrity, and compatibility
coverage. DAST v4/DABK v1 remains the production reader/writer baseline; no v5
codec or production writer was registered. The next active plan is
[Selected Local Authored Payload Publication](SelectedLocalAuthoredPayloadPublication.md).

## Goal

Implement one bounded, canonical, independently versioned package trailer and
footer codec for DAST v5 external DABK v1 entries, with exact golden bytes,
corruption coverage, EOF discovery, and no production writer activation.

## Scope

- Add a private AssetCore package-trailer logical model, detached builder, EOF
  discovery reader, validator, and construct-free entry inspection API.
- Freeze trailer v1 and footer v1 bytes, limits, hash ownership, canonical
  ordering, unsupported-state behavior, and complete-consumption rules.
- Support exactly one placement: `ExternalDabkV1`.
- Prove exact old DAST v4 rejection of version 5 preambles and preserve all v4
  golden bytes and tests.
- Register a focused contract test target and document the selected foundation
  as implemented without claiming production DAST v5 package support.

## Non-Goals

- Registering a DAST v5 package codec, loading objects from v5, writing v5 from
  live packages, changing the default writer, or saving tracked assets.
- Publishing, moving, copying, deleting, repairing, or canonical-resaving a
  DAST v5/DABK closure.
- Package-local payload bytes, inline placement, compression, referenced or
  virtualized payloads, persistent backends, deduplication, or a DABK v2 wire.
- Tail rewrite, append generations, compaction, source-control migration, or
  legacy DABK retirement.
- Changing `FEditorBulkData`, domain schemas, DDC, Cook, or runtime resources.

## Design Decisions and Invariants

### Composite package layout

- A future DAST v5 package is `ObjectStream || TrailerV1 || FooterV1`. This
  foundation treats `ObjectStream` as an opaque bounded prefix and never
  interprets or constructs objects.
- `ObjectStreamEnd == TrailerOffset`; `TrailerOffset + TrailerSize ==
  FooterOffset`; `FooterOffset + 64 == PhysicalFileSize`. Gaps, overlap,
  overflow, prefix/suffix ambiguity, and trailing bytes fail.
- The overall file ceiling is 1 GiB, the opaque object stream ceiling remains
  256 MiB, and trailer entry count is at most 65,536.

### Trailer v1 header — 64 bytes, little-endian

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `DTRL` (`44 54 52 4c`) |
| 4 | 4 | version `1` |
| 8 | 4 | header size `64` |
| 12 | 4 | entry size `80` |
| 16 | 8 | entry count |
| 24 | 8 | directory offset, exactly `64` |
| 32 | 8 | object-stream end / absolute trailer offset |
| 40 | 16 | XXH3-128 hash of exact directory bytes |
| 56 | 8 | reserved zero |

The trailer size is exactly `64 + EntryCount * 80`; v1 owns no local data or
padding region.

### Trailer v1 entry — 80 bytes, sorted by payload GUID

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 16 | logical `PayloadId` |
| 16 | 4 | placement `1` = `ExternalDabkV1` |
| 20 | 4 | flags, exactly zero |
| 24 | 8 | logical byte count |
| 32 | 8 | stored byte count; equals logical count in v1 |
| 40 | 16 | logical/stored XXH3-128 content integrity |
| 56 | 16 | DABK v1 container hash / generation identity |
| 72 | 8 | reserved zero |

Entries are strictly ascending by GUID and unique. GUID, content hash, and
container hash are nonzero. Unknown placement, nonzero flags/reserved bytes,
size disagreement, or any noncanonical order fails.

### Footer v1 — 64 bytes at physical EOF, little-endian

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `DTRF` (`44 54 52 46`) |
| 4 | 4 | version `1` |
| 8 | 4 | footer size `64` |
| 12 | 4 | flags, exactly zero |
| 16 | 8 | absolute trailer offset |
| 24 | 8 | trailer size |
| 32 | 8 | object-stream end, equal to trailer offset |
| 40 | 16 | XXH3-128 hash of exact trailer header plus directory |
| 56 | 8 | reserved zero |

- Footer discovery reads the last 64 bytes, validates identity/size before
  projecting any range, then validates the full trailer and both hashes.
- The builder produces detached `TrailerV1 || FooterV1` bytes from owned logical
  entries plus an absolute object-stream end. It never writes a file.
- Failed build/read leaves outputs empty. No API accepts or repairs a partial
  candidate.
- Current XXH3-128 remains integrity only. It is not promoted to a persistent
  content key.

## Current Foundations and Gaps

| Area | Foundation | Plan gap |
| --- | --- | --- |
| Bounded binary mechanics | `BulkContainerInfrastructure` provides checked arithmetic, bounded readers/writers, range projection, layout validation, and canonical sorted projection | Compose the trailer/footer contract without DABK-specific leakage |
| Integrity | XXH3-128 and exact DABK validation are implemented | Freeze directory and whole-trailer hash ownership |
| Package versioning | DAST preamble dispatch rejects unsupported versions | Prove v4 rejects v5 while not registering incomplete v5 support |
| Tests | Asset package and bulk-container contracts have golden/corruption coverage | Add a focused trailer contract target and independent expected bytes |
| Documentation | Strategic activation selects DAST v5 + DABK v1 | Record only the implemented foundation; production v5 remains future work |

## Implementation Stages

### Stage 0: Freeze wire and API contracts

- [x] Record exact composite, header, entry, footer, endian, size, count, hash,
  ordering, trailing-byte, and unsupported-state rules.
- [x] Define detached logical input/inspection output types and failure-atomic
  builder/reader behavior without file or object ownership.
- [x] Confirm `ExternalDabkV1` is the only placement and DAST v4/DABK v1 bytes
  remain unchanged.

#### Acceptance Gate

- Every byte and semantic field has one owner; no production integration or
  placement branch remains inside foundation scope.

### Stage 1: Implement bounded trailer/footer mechanics

- [x] Add private AssetCore trailer types, constants, builder, EOF discovery,
  parser, hash verification, and checked range validation.
- [x] Reuse bounded container primitives for arithmetic and byte IO; do not
  duplicate filesystem, package, DABK, or domain policy.
- [x] Guarantee deterministic canonical output and unchanged outputs on every
  rejected input.

#### Acceptance Gate

- Valid detached input produces canonical bytes and round-trips exactly;
  malformed inputs fail before exposing entries or prefix ranges.

### Stage 2: Add golden and corruption contracts

- [x] Register `AssetPackageTrailerTests` as a focused `contract` target in the
  `asset-package` domain.
- [x] Add independent golden bytes and exact whole-byte hash for zero, one, and
  multiple canonical entries.
- [x] Cover magic/version/header/entry/footer sizes, counts, overflow, offset,
  order, duplicates, placement, flags, reserved bytes, hashes, truncation,
  overlap, gaps, and trailing bytes.
- [x] Prove EOF inspection constructs no package object and old DAST v4 package
  validation rejects a v5 preamble explicitly.

#### Acceptance Gate

- Focused tests cover every structural and integrity rule and all existing v4
  wire/package tests remain green.

### Stage 3: Record foundation and hand off publication

- [x] Document the implemented trailer/footer foundation in the owning package
  contract, explicitly marking DAST v5 package loading/writing unsupported.
- [x] Update roadmap Milestone 1 state and create the Selected Local Authored
  Payload Publication plan only after all tests and documentation validation.
- [x] Run focused build/tests and changed/all-plan/all-roadmap validation.

#### Acceptance Gate

- Code, tests, package documentation, roadmap, and the next plan agree that the
  codec is foundational only and production DAST v5 remains disabled.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Golden wire | Independent exact bytes and XXH3-128 hashes for canonical fixtures |
| Bounds | Count/size multiplication, absolute offsets, 256 MiB prefix, 1 GiB file, truncation, gap, overlap, and trailing rejection |
| Canonicalization | Strict GUID ordering, duplicate rejection, deterministic build under input permutation |
| Integrity | Directory hash, trailer hash, content/container identity requirements, single-byte corruption across every region |
| Unsupported state | Unknown placement, flags, reserved fields, header/entry/footer versions and sizes |
| Compatibility | Existing DAST v4/DABK v1 goldens unchanged; v4 rejects v5; no v5 codec registration or production write |
| Construction | Parser/inspection operates on byte spans only and creates no `DObject` |
| Native | `AssetPackageTrailerTests`, `AssetBulkContainerTests`, and `AssetPackageTests` selected through the root testing workflow |
| Documentation | Changed validation plus all-plan/all-roadmap validation on lifecycle updates |

## Definition of Done

- Trailer v1 and footer v1 exact bytes, invariants, limits, hashes, and failure
  behavior are implemented and documented.
- Focused and regression tests prove canonical output, bounded EOF discovery,
  corruption rejection, construct-free inspection, and v4 compatibility.
- No production v5 reader/writer, package publication route, default placement,
  tracked asset, or source-control policy changes.
- Milestone 1 is complete and exactly one publication plan is activated.

## Deferred Follow-ups

- DAST v5 logical object-stream codec registration and dual-read loading.
- Companion-first opt-in v5 publication, recovery, package operations, and
  canonical v4 rollback.
- VolumeTexture migration, corpus/default writer, virtualization, optimization,
  and legacy retirement remain roadmap-owned.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [Authored Package Storage Strategic Activation](AuthoredPackageStorageStrategicActivation.md)
- [Authored Package Trailer Qualification](AuthoredPackageTrailerQualification.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [File IO](../../../Runtime/Core/FileIO.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/BulkContainerInfrastructure.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4Reader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp`
- `Engine/Tests/Native/AssetCoreTests/CMakeLists.txt`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkContainerInfrastructureTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4WireContractTests.cpp`
