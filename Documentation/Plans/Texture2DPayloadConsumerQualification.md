# Texture2D Payload Consumer Qualification Plan

Summary: Qualify Texture2D as the second production consumer of domain-owned payload schemas and close only measured gaps.

Last reviewed: 2026-08-23

Status: Active
Completed:

## Current Status

Stage 0 is active. The tracked corpus contains Texture2D source images up to
4.00 MiB while their `.dasset` packages remain approximately 1.2-1.4 KiB,
demonstrating a real dense-data workload without oversized reflected arrays.
Texture2D already keeps dimensions, usage, color-space, alpha, and build policy
in domain metadata; decoded source pixels are transient and cooked bytes use a
domain descriptor plus `LoadCookedPackagePayload`.

The first task is therefore qualification, not an assumed storage conversion.
It will measure structural, package, decoded-memory, DDC, and cooked behavior,
freeze compatibility evidence, and select either a bounded correction or an
explicit already-conforming disposition.

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

- [ ] Record tracked source/package/companion sizes and representative decoded
  and upload byte counts.
- [ ] Inventory reflected structural nodes and every durable/transient pixel
  representation.
- [ ] Freeze source import, DDC, Cook/runtime, malformed-input, and golden-byte
  evidence.
- [ ] Decide whether a migration is required and state the exact compatibility
  route.

#### Acceptance Gate

- A measured production workload and complete authority map justify either one
  bounded correction or an explicit already-conforming disposition.

### Stage 1: Close measured boundary gaps

- [ ] Implement only the Stage 0-selected change, if any.
- [ ] Keep Texture2D metadata and codecs authoritative for pixel meaning.
- [ ] Preserve transactional publication and last-known-good CPU/GPU resources.
- [ ] Keep ordinary package structure bounded independently of pixel count.

#### Acceptance Gate

- Texture2D uses domain metadata plus opaque authority-owned bytes with no
  oversized ordinary reflected pixel arrays or generic semantic translation.

### Stage 2: Qualify diagnostics and end-to-end behavior

- [ ] Record source, DDC, cooked, decoded, and GPU diagnostic facts without
  exposing backend paths to Texture2D callers.
- [ ] Run focused Texture2D workflows, aggregate tests/build, and docs validators.
- [ ] Complete the plan and update the roadmap with the second-consumer evidence.

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
