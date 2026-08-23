# std::byte Buffer Migration Plan

Summary: Migrate repository-owned untyped byte buffers from uint8 to std::byte across runtime, asset, rendering, editor, developer, program, and native-test boundaries.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

The repository-wide migration is complete. Repository-owned serialization,
file, hash, asset, build, import, image, thumbnail, shader, and GPU-transfer
payloads now use `std::byte` storage and spans through their complete call
chains. The final audit retains `uint8` only for numeric values such as color
channels, state markers, masks, and typed reflection fields, plus direct
third-party boundaries such as Vulkan pipeline-cache bytes and BC compression.

## Goal

Use `std::byte` consistently for repository-owned untyped byte storage and
views so file, serialization, hash, asset, build, and GPU transfer call paths
compose without reinterpret casts or adapter containers.

## Scope

- Repository-owned `std::vector<uint8>`, `std::span<uint8>`, byte-oriented
  `std::array<uint8, N>`, and `uint8*` payload contracts.
- Public and private call chains across runtime, developer, editor, programs,
  and native tests.
- Explicit adaptation at third-party APIs and genuinely typed pixel/sample
  channels where an external contract requires integer arithmetic.
- Removal of obsolete `uint8` compatibility overloads and reinterpret casts.

## Non-Goals

- Enum underlying types, boolean/flag fields, numeric properties, color
  channels, indices, counts, bit widths, or other arithmetic values.
- Changes to serialized layouts, hashes, package versions, shader binaries, or
  GPU resource contents.
- Editing generated or third-party source.

## Design Decisions and Invariants

- `std::byte` is the canonical element type for untyped storage and views.
- Use `std::as_bytes` and `std::as_writable_bytes` when viewing typed objects;
  use pointer casts only at C and third-party boundaries whose signatures
  cannot be changed.
- Decode or compare a byte numerically with `std::to_integer`; construct it
  with `static_cast<std::byte>`.
- Wire-format tag variables may remain `uint8` while aggregate wire payloads
  are `std::byte`.
- Migration does not change byte counts, endianness, alignment, ownership,
  lifetime, failure behavior, or persistent format identity.

## Current Foundations and Gaps

`FArchive`, `FBinaryWriter`/`FBinaryReader`, `FFileHelper`, `FXxHash`, and
`FSharedByteBuffer` establish the intended `std::byte` contract. Asset package,
build-value, derived-data, import, texture/mesh payload, RHI upload/readback,
and snapshot APIs still carry legacy `uint8` storage and force adapters at
those foundational boundaries.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory repository-owned byte containers, spans, and pointer APIs.
- [x] Separate raw payloads from numeric eight-bit values.
- [x] Record migration invariants and validation requirements.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Migrate foundational runtime contracts

- [x] Migrate remaining Core/CoreDObject serialization and snapshot payloads.
- [x] Migrate AssetCore package, bulk, cook, catalog, mutation, and reference
  payloads through their public contracts.
- [x] Preserve all persistent formats and remove compatibility adapters.

#### Acceptance Gate

- Core, CoreDObject, and AssetCore compile and their focused native tests pass;
  no repository-owned public byte-buffer contract in these modules uses
  `uint8`.

### Stage 2: Migrate consumers and GPU transfer contracts

- [x] Migrate AssetBuildCore, GeometryBuild, TextureBuild, Engine asset types,
  import/editor modules, and programs along complete call chains.
- [x] Migrate RHI/VulkanRHI upload, readback, shader, and transfer payloads,
  retaining typed color/sample arithmetic at the boundary.
- [x] Update affected tests and fixtures to express expected raw data as
  `std::byte`.

#### Acceptance Gate

- Focused build/test selections for all affected module domains pass and
  third-party boundaries contain the only required byte pointer casts.

### Stage 3: Repository audit and qualification

- [x] Audit remaining `uint8` containers, spans, and pointers and classify each
  as numeric/typed or externally constrained.
- [x] Run broad native-test coverage and a full `all` build because public
  contracts cross most repository modules.
- [x] Publish the lasting byte-buffer convention in the C++ coding standard,
  complete this plan, and validate documentation.

#### Acceptance Gate

- No repository-owned untyped byte buffer uses `uint8`; focused and broad
  validation pass; the full build succeeds; documentation records the rule.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Serialization and files | Core/CoreDObject round trips, truncation/failure cases, and exact bytes pass. |
| Assets and builds | Package, bulk, cook, DDC, import, texture, mesh, skeletal, terrain, and thumbnail tests pass. |
| Rendering | RHI command, Vulkan upload/readback, shader, and renderer tests pass without content changes. |
| Repository | Audit has no unclassified raw `uint8` buffer and the full `all` build succeeds. |

Final evidence:

- `DevTool.bat test all` passed all 78 native-test targets on 2026-08-23.
- `DevTool.bat build` completed the `all` target on 2026-08-23.
- The final source audit found no repository-owned untyped `uint8` buffer;
  remaining matches are numeric/typed values or direct third-party boundaries.
- RHI texture command recording now validates that borrowed byte spans cover
  the requested source region before copying their data.

## Definition of Done

- All repository-owned untyped storage and span APIs use `std::byte`.
- Numeric `uint8` uses are retained and distinguishable by their contracts.
- Serialized bytes, hashes, GPU data, and failure behavior are unchanged.
- Focused tests, broad tests, full build, and documentation validation pass.

## Deferred Follow-ups

None. Newly discovered repository-owned raw-byte contracts are part of this
migration rather than deferred compatibility work.

## Related Documentation

- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)
- [Serialization](../../../Runtime/Core/Serialization.md)
- [File I/O](../../../Runtime/Core/FileIO.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core`
- `Engine/Source/Runtime/CoreDObject`
- `Engine/Source/Runtime/AssetCore`
- `Engine/Source/Runtime/RHI`
- `Engine/Source/Runtime/VulkanRHI`
- `Engine/Source/Runtime/Engine`
- `Engine/Source/Developer`
- `Engine/Source/Editor`
- `Engine/Source/Programs`
- `Engine/Tests/Native`
