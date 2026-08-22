# Canonical Binary Primitive Consolidation Plan

Summary: Consolidate fixed-width canonical binary primitives in Core and remove duplicate Engine and Editor wire writers without changing established formats.

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

All implementation stages are complete. Core now owns the selected fixed-width,
bounded sequential, and bounded random-access operations; Engine and Editor
consumers preserve their established bytes without duplicate primitive writers.
Focused validation and the full `all` build pass. The `fast-all` run passed 57
of 58 targets; its sole `EditorAssetWorkflowTests` failure was reproduced in
isolation as an unrelated source-relocation rejection for an unsupported asset
class and is recorded as non-blocking evidence rather than expanded into this
serialization change.

## Goal

Make `Serialization/BinaryFormat.h` the single small canonical binary primitive
surface used by derived-data containers, cache keys, and compatibility hashes,
then remove `Engine/Private/Serialization/EngineWire.h` and local primitive
writers without changing any established byte format.

## Scope

- Add the missing fixed-width `uint16`, `int32`, and `float32` reader/writer
  operations to Core.
- Expose bounded non-owning sequential regions and bounded random-access
  little-endian unsigned reads through Core.
- Reuse checked Core arithmetic for payload alignment.
- Migrate Engine derived-data codecs, the Editor thumbnail key, and Skeleton
  compatibility hashing.
- Add byte-exact, malformed-input, and signed-zero regression coverage.

## Non-Goals

- Changing any schema version, magic, field order, string-width convention, or
  golden cache/compatibility identity.
- Normalizing floating-point representations globally in Core.
- Replacing domain validation with a generic serializer or admitting native
  object layouts into persistent data.

## Design Decisions and Invariants

- `FBinaryWriter::WriteFloat` and `FBinaryReader::ReadFloat` preserve the exact
  IEEE-754 bit pattern. Canonical means fixed-width little-endian encoding, not
  semantic normalization.
- Skeleton compatibility alone canonicalizes signed zero before hashing, so
  `-0.0f` and `+0.0f` remain compatibility-equivalent.
- Existing Core strings remain `uint64` length-prefixed. Engine payload strings
  that use a `uint32` length retain that layout through small format-local
  composition of Core primitives and bounded regions.
- Random-access reads do not advance reader state and reject an unrepresentable,
  truncated, or overflowing range before touching output.
- Alignment failures are ordinary build/decode failures at external-data
  boundaries; no unchecked duplicate aligner remains in Engine serialization.

## Current Foundations and Gaps

Core already owns canonical integer/float archive encoding, `FBinaryWriter` and
`FBinaryReader`, checked arithmetic, and a memory-reader bounded-region operation.
The convenience binary surface omits several fixed-width primitives and does not
expose its bounded region. Engine consequently owns a duplicate writer/reader,
aligner, and random-access decoder, while Thumbnail and Skeleton each own another
partial writer.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory Core, Engine, Editor, and test consumers.
- [x] Confirm byte compatibility and string-layout differences.
- [x] Select Core/domain ownership for signed-zero normalization.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Complete the Core primitive surface

- [x] Add paired `U16`, `I32`, and `Float` operations.
- [x] Expose bounded sequential regions and random-access unsigned reads.
- [x] Add byte-exact and boundary tests, including signed-zero preservation.

#### Acceptance Gate

- Core tests prove deterministic bytes, transactional bounded reads, and exact
  float bit preservation.

### Stage 2: Remove duplicate consumers

- [x] Migrate Engine derived-data codecs and alignment calls.
- [x] Migrate the Thumbnail cache-key writer without changing its digest.
- [x] Migrate Skeleton compatibility hashing with local signed-zero normalization.
- [x] Delete `EngineWire.h` and verify no duplicate primitive writer remains in
  the selected scope.

#### Acceptance Gate

- Existing golden identities and payload decode behavior remain unchanged, and
  no selected source includes or aliases `EngineWire`.

### Stage 3: Qualify and hand off

- [x] Validate changed documentation and the active plan.
- [x] Run focused Core and affected Engine/Editor tests.
- [x] Run the shared-runtime broad gate required by the Core API change; record
  and isolate any failure unrelated to the changed behavior.
- [x] Record evidence, complete the plan, and commit the isolated change.

#### Acceptance Gate

- All focused validations and the full build pass; any unrelated broad-gate
  failure is isolated and recorded. The commit contains code, tests, and plan
  status with exact provenance.

## Validation Matrix

| Concern | Validation |
| --- | --- |
| Core primitive bytes and bounds | Focused `CoreTests` binary-format cases |
| Skeleton and derived-data compatibility | Registered skeletal/asset contract tests |
| Thumbnail key stability | Registered thumbnail contract tests |
| Shared Core surface | Repository `fast-all` gate |
| Documentation lifecycle | Changed-doc and all-plan validation |

## Definition of Done

- Core is the only selected owner of canonical fixed-width binary primitives.
- Established payload bytes, thumbnail keys, and Skeleton identity goldens remain
  unchanged.
- Skeleton signed-zero equivalence is explicit and regression-tested.
- `EngineWire.h` and the two local writer classes are removed.
- Validation evidence and final status are recorded in this plan.

## Deferred Follow-ups

- Other unrelated module-local alignment helpers are outside this serialization
  consolidation and may be audited independently.

## Related Documentation

- [Serialization](../Runtime/Core/Serialization.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/BinaryFormat.h`
- `Engine/Source/Runtime/Core/Private/Serialization/BinaryFormat.cpp`
- `Engine/Source/Runtime/Core/Public/Templates/CheckedArithmetic.h`
- `Engine/Source/Runtime/Engine/Private/Serialization/EngineWire.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/Skeleton.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/AssetThumbnailKey.cpp`
