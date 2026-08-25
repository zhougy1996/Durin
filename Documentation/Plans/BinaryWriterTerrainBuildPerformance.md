# Binary Writer Terrain Build Performance Plan

Summary: Remove per-scalar Archive construction from canonical binary writing and reduce Terrain World integration-test latency without changing bytes or coverage.

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

All stages are complete. `FBinaryWriter` now retains one lifetime-bound
`FCanonicalMemoryWriter`; `TakeBytes()` resets the owned vector to a defined new
sequence without invalidating later writes. No Terrain fixture sharing or
behavioral coverage reduction was required.

The same 15-case Debug executable decreased from 112.220 seconds to 11.996
seconds, an 89.3% reduction. Its former 25.0-second cold/warm/corrupt DDC case
now completes in 2.9 seconds. `CoreUtilityTests` passes 91/91, the complete
`fast-all` profile passes, `TerrainWorldBuildTests` passes 15/15,
`TerrainHeightmapCookTests` passes 1/1, and the full Debug Editor `all` build
passes.

## Goal

Reduce `TerrainWorldBuildTests` Debug runtime substantially while preserving
canonical binary bytes, DDC/build keys, product schemas, validation, cache,
determinism, Cook, and corruption coverage.

## Scope

- Reuse one canonical Archive for each `FBinaryWriter` lifetime.
- Lock scalar, string, byte, and post-`TakeBytes` behavior with Core tests.
- Re-measure every Terrain World case and optimize redundant test construction
  only if the production fix does not meet the target.
- Update lasting serialization documentation if the implementation contract changes.

## Non-Goals

- Change any serialized format, hash, DDC identity, Terrain product size, or Cook layout.
- Remove determinism, cold/warm/corrupt DDC, publication, AssetForge, or Cook coverage.
- Add a Release/performance preset or weaken Debug assertions.

## Design Decisions and Invariants

- `FBinaryWriter` owns one byte vector and one canonical writer bound to that
  vector; it is not copied or moved because the Archive retains a reference.
- `TakeBytes` may be followed by further writes, which append to the valid
  moved-from vector and form a new byte sequence.
- Existing byte-level Core tests and all Terrain World tests are compatibility gates.
- The target is at most 20 seconds for the 15-case Debug executable on this host.

## Current Foundations and Gaps

- The 15-case baseline is 112.220 seconds.
- One complete local Terrain generation costs approximately 8.3 seconds;
  concurrent duplicate generation costs 27.5 seconds under allocator contention.
- Basic coordinate, schema, neighbor, cancellation, and manifest cases total
  less than 0.4 seconds and are not optimization targets.

## Implementation Stages

### Stage 0: Establish attribution and compatibility gates

- [x] Capture per-case Terrain World timings.
- [x] Identify the per-scalar canonical writer construction hotspot.

#### Acceptance Gate

- The runtime attribution and byte-compatibility constraints are explicit.

### Stage 1: Reuse the canonical writer

- [x] Give `FBinaryWriter` one lifetime-bound `FCanonicalMemoryWriter`.
- [x] Preserve scalar, string, byte, header, and `TakeBytes` behavior.
- [x] Build Core and run focused binary/archive tests.

#### Acceptance Gate

- Core serialization tests pass with byte-identical expectations.

### Stage 2: Re-measure and close remaining Terrain overhead

- [x] Capture the complete per-case timing after the production fix.
- [x] Confirm immutable test-product reuse is unnecessary after meeting the target.
- [x] Run all Terrain World, dependent asset serialization, and broad build gates.

#### Acceptance Gate

- The target runtime is met without reducing behavioral coverage; full build and
  selected regression tests pass.

## Validation Matrix

| Change | Validation |
| --- | --- |
| Canonical writer lifetime | Core binary-format and Archive tests |
| Serialized asset compatibility | Focused AssetCore, StaticMesh, Skeletal, Texture, and Terrain tests |
| Terrain latency | Per-case and whole-target `TerrainWorldBuildTests` timing |
| Cross-module API and linkage | Full Debug Editor `all` build |
| Documentation lifecycle | Changed-document and all-plan validation |

## Definition of Done

- `FBinaryWriter` performs no per-scalar Archive construction.
- Existing canonical byte expectations and derived/cooked behavior remain unchanged.
- Terrain World tests meet the bounded Debug runtime target with all 15 cases retained.
- Validation, documentation, and the completed plan are committed together.

## Deferred Follow-ups

- Streaming hash writers and specialized bulk endian encoders require separate
  evidence if buffer materialization later becomes a measured bottleneck.

## Related Documentation

- [Serialization](../Runtime/Core/Serialization.md)
- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`BinaryFormat.h`](../../Engine/Source/Runtime/Core/Public/Serialization/BinaryFormat.h)
- [`BinaryFormat.cpp`](../../Engine/Source/Runtime/Core/Private/Serialization/BinaryFormat.cpp)
- [`TerrainWorldTile.cpp`](../../Engine/Source/Developer/TerrainBuild/Private/Terrain/TerrainWorldTile.cpp)
- [`TerrainWorldBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Terrain/TerrainWorldBuildTests.cpp)
