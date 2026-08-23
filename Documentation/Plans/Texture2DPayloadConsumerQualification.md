# Texture2D Payload Consumer Qualification Plan

Summary: Qualify Texture2D as the second production consumer of domain-owned payload schemas and close only measured gaps.

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

All stages are complete with an explicit already-conforming disposition. Three
tracked 1024 x 1024 x 32-bit TGA sources are 4,194,322 bytes each and decode to
exactly 4 MiB RGBA8; their Texture2D packages are only 1,300-1,387 bytes. The
asset has 15 bounded reflected domain metadata/descriptor fields, while decoded
source pixels and platform mip vectors are non-reflected transient products.

Texture2D owns TXPL schema 1, every source/build/pixel-format input, canonical
DDC serialization, cooked descriptor qualification, decoded CPU publication,
and RHI upload. No generic semantic descriptor, native-layout persistence, or
oversized ordinary reflected pixel array exists, so changing storage would add
risk without closing a measured gap. The 39 focused Texture2D, derived-data,
Cook, coordination, source, and resource-transaction tests pass; the same code
revision also passed `fast-all` and the Debug Editor `all` build.

## Goal

Prove that Texture2D source, derived, cooked, decoded CPU, and GPU lifecycles
obey the roadmap's domain-schema and opaque-storage boundaries, then fix only
gaps supported by measured production evidence.

## Scope

- Inventory tracked Texture2D source/package sizes and structural node costs.
- Trace source provenance/decode, DDC key and validation, cooked descriptor and
  DBLK load, CPU publication, and RHI upload ownership.
- Freeze current source, DDC, Cook, reload, failure, and compatibility tests.
- Confirm all durable pixel meaning is Texture2D-owned and no native ABI layout
  or generic semantic descriptor is persistent.
- Add a bounded migration only if the inventory finds a concrete violation.
- Provide domain-qualified diagnostic facts needed by the inspection milestone.

## Non-Goals

- Replacing source files with DABK, merging DDC and cooked storage, or adding a
  universal provider.
- Async streaming, virtualization, compression redesign, or package aggregation.
- TextureCube/VolumeTexture changes except shared Texture2D code proven necessary.
- Renaming stable domain formats or changing golden bytes without an explicit
  compatibility decision.

## Implementation Stages

### Stage 0: Measure and freeze the Texture2D baseline

- [x] Record tracked source/package/companion sizes and representative decoded
  and upload byte counts.
- [x] Inventory reflected structural nodes and every durable/transient pixel
  representation.
- [x] Freeze source import, DDC, Cook/runtime, malformed-input, and golden-byte
  evidence.
- [x] Decide whether a migration is required and state the exact compatibility
  route.

#### Acceptance Gate

- A measured production workload and complete authority map justify either one
  bounded correction or an explicit already-conforming disposition.

### Stage 1: Close measured boundary gaps

- [x] Implement only the Stage 0-selected change, if any (no code change selected).
- [x] Keep Texture2D metadata and codecs authoritative for pixel meaning.
- [x] Preserve transactional publication and last-known-good CPU/GPU resources.
- [x] Keep ordinary package structure bounded independently of pixel count.

#### Acceptance Gate

- Texture2D uses domain metadata plus opaque authority-owned bytes with no
  oversized ordinary reflected pixel arrays or generic semantic translation.

### Stage 2: Qualify diagnostics and end-to-end behavior

- [x] Record source, DDC, cooked, decoded, and GPU diagnostic facts without
  exposing backend paths to Texture2D callers.
- [x] Run focused Texture2D workflows, aggregate tests/build, and docs validators.
- [x] Complete the plan and update the roadmap with the second-consumer evidence.

#### Acceptance Gate

- Texture2D becomes a qualified second production consumer and supplies the
  reusable diagnostic questions required to enter the inspection milestone.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Source | Import/reimport, provenance change, malformed image, bounded decode. |
| DDC | Stable hit, input-sensitive miss/rebuild, corruption rejection. |
| Cooked | Target/profile/schema/range/hash validation and no source fallback. |
| Structure | Package/node counts remain bounded as pixel count grows. |
| Publication | Failed candidates preserve last-known-good CPU/GPU state. |
| Aggregate | Focused Texture tests, `fast-all`, Debug Editor `all`, docs validation. |

## Related Documentation

- [Large Asset Payload Architecture](../Roadmaps/LargeAssetPayloadArchitecture.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Testing](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Editor/AssetForge`
- `Engine/Tests/Native/EngineTests/Private/Texture`
