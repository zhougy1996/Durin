# Reflected Byte Blob Serialization Plan

Summary: Add explicit `std::byte` reflection and atomic Blob serialization, then migrate volume-texture voxel sources away from element-wise `uint8` arrays.

Last reviewed: 2026-08-22

Status: Archived
Completed: 2026-08-22

## Execution Review

The root-cause analysis and semantic correction were necessary and correct:
raising `DefaultDeltaMaxFields` would only hide an Array/Blob modeling error.
Execution made two scope optimizations without changing the contract. Byte and
Blob use explicit runtime kinds plus the existing generic `FProperty` lifecycle
hooks rather than introducing a parallel property-class hierarchy, and
qualification records deterministic node/byte bounds instead of platform-noisy
peak-memory measurements. The mandatory production regression is an actual
uncompressed `16384 x 128` RGBA PNG whose red channel forms the horizontal
`128^3` R8 atlas; it covers import, package save/reload, reimport, and Cook.

## Current Status

Implemented and qualified. Reflection supports non-numeric `std::byte` and a
direct `std::vector<std::byte>` Blob backed by logical `Bytes`; ordinary
`std::vector<uint8>` remains `Array<UInt8>`. Volume source storage and its
import/build/key/cook path use the Blob form, historical Array data has a
versioned load-only migration route, and default-delta diagnostics retain the
reason, path, observed value, and limit. The production `128^3` atlas now
imports, saves, reloads, reimports, and cooks without changing the one-million
field safety limit.

## Goal

Provide an explicit reflected binary-data type whose storage, identity,
copying, comparison, serialization, default planning, package migration, and
diagnostics scale with byte count rather than element count, then use it to
make real `128^3` R8 volume-texture import, save, reload, reimport, and cook
reliable without weakening general Array semantics or planner safety limits.

## Scope

- DurinHeaderTool recognition and generated metadata for `std::byte` and one
  direct `std::vector<std::byte>` Blob property.
- Runtime reflected property kinds and operations for a byte leaf and an owned
  contiguous byte Blob, including construction, destruction, copy, equality,
  snapshots, duplication, and transactional replacement.
- One canonical Archive Blob operation backed by
  `FArchiveLogicalTypeDescriptor::Bytes`, with bounded sizes and identical
  discovery/emission manifests.
- Atomic default-delta capture, identity, planning, authored-override behavior,
  and DAST v4 encode/decode for Blob fields.
- Exact structured diagnostics that preserve the default-delta failure reason,
  logical path, observed size/count, and applicable limit through AssetCore and
  editor import reporting.
- Direct migration of `FVolumeTextureSourceData::Voxels` and its complete
  Engine, TextureBuild, AssetForge, derived-data, cook, and test call chain from
  `std::vector<uint8>` to `std::vector<std::byte>`.
- Versioned load compatibility for previously saved volume assets whose
  `Voxels` field used logical `Array<uint8>`, followed by canonical resave as
  logical `Bytes`.
- Focused generator, CoreDObject, AssetCore, texture, import, cook, editor, and
  aggregate qualification, including the reported horizontal `16384 x 128`
  PNG atlas representing a `128^3` R8 volume.

## Non-Goals

- Automatically treating every `std::vector<uint8>` as a Blob or changing the
  serialized meaning of existing numeric byte arrays.
- A `DPROPERTY(Blob)` escape hatch for `std::vector<uint8>` in this plan; the
  selected production consumer migrates to `std::byte` directly.
- Per-byte Details editing, Array insertion/removal UI, element-level authored
  override paths, or delta patches inside a Blob.
- General typed binary arrays for `uint16`, integers, half/float samples, custom
  allocators, nested Blob containers, Blob Map keys, or sparse/streamed data.
- Raising `DefaultDeltaMaxFields`, bypassing default-relative planning, or
  disabling safety limits to admit the volume source.
- Moving authored volume source into `.dbulk`, changing TXPL/DDC/cooked payload
  formats, adding compression, or changing runtime texture formats.
- Changes to PNG atlas ordering, horizontal/vertical grid support, cloud
  shading, temporal reconstruction, or the specialized cloud editor.

## Design Decisions and Invariants

### Type and reflection semantics

- `std::byte` is a distinct non-numeric reflected leaf. It has one-byte value
  storage and canonical unsigned eight-bit transfer, but generic numeric
  authoring must not expose arithmetic, ranges, or decimal controls for it.
- A direct `std::vector<std::byte>` property is generated as one Blob property,
  not `FArrayProperty<std::byte>`. Its persistent logical descriptor is the
  existing `Bytes` kind. C++ vector capacity and allocator state never enter
  reflection identity or persistent bytes.
- `std::vector<uint8>` remains `Array<UInt8>` by default. The C++ element type
  therefore continues to distinguish binary payload intent from editable
  numeric-array intent without a heuristic based on field size or name.
- Blob values own contiguous bytes and participate in deterministic copy and
  equality. They contain no object references, contribute no GC tokens, and
  cannot be used as an element-addressable authored-override container.
- The first implementation supports a direct reflected Blob field. Nested
  `vector<vector<byte>>`, Map participation, and custom allocators fail in
  DurinHeaderTool with stable source-located diagnostics.

### Archive, delta, and failure semantics

- One shared byte-Blob serialization helper owns count transfer, overflow-safe
  allocation bounds, remaining-payload validation, and raw transfer. Property,
  object-graph, duplicate, snapshot, DAST v4, default-delta, and compatibility
  paths must not invent independent encodings.
- A Blob is captured as one atomic logical node. Identity compares size first
  and then exact bytes; an implementation may use a hash as an accelerator but
  must confirm equality and may not make a collision observable.
- Planner field/node limits count the Blob field once regardless of payload
  length. Independent byte and allocation ceilings remain authoritative, so
  atomic treatment does not create an unbounded allocation path.
- A changed Blob is emitted as one complete value. It has only field-level
  explicit/forced provenance; indexed Array paths are invalid for Blob fields.
- Discovery and authored emission expose the same name, property kind, logical
  `Bytes` type, and version. Failed decode, allocation, migration, planning, or
  package publication leaves the prior object/package/source transaction
  unchanged.
- `FDefaultDeltaDiagnostic` details must survive the AssetCore adapter. The
  editor-facing failure identifies the logical field and whether the failure
  was a field, depth, path, byte, or allocation limit instead of reporting only
  `Default-relative logical planning failed.`

### Volume source and compatibility

- `FVolumeTextureSourceData::Voxels` changes directly to
  `std::vector<std::byte>`. Channel extraction and numeric filtering perform
  explicit `std::to_integer<uint8>` and `std::byte{...}` conversions at their
  arithmetic boundaries; no implicit aliasing or native-memory serialization
  is allowed.
- TextureBuild keys, deterministic mip generation, TXPL encoding, RHI upload,
  hashes, and tests continue to consume the exact same byte sequence. The C++
  type migration must not change DDC identity, mip values, cooked payloads, or
  GPU upload bytes for identical normalized input.
- A volume-source custom-version domain retains a load-only deprecated
  `Array<uint8>` route for the historical serialized name `Voxels`. Detached
  `PostDeserialize` converts it exactly into the current Blob, validates the
  complete source, and commits transactionally. Current saves emit only the
  Blob field and current version.
- The compatibility route remains until the asset compatibility audit proves
  that the supported content baseline contains no old Array form. It is not
  removed merely because newly imported assets resave successfully.
- The authored `.dasset` may retain the normalized source under the current
  volume-texture contract. Moving large authored sources to a separate bulk
  class is a later storage-policy decision and must not block this correctness
  fix.

## Current Foundations and Gaps

| Area | Existing foundation | Selected gap |
| --- | --- | --- |
| Type generation | DurinHeaderTool generates scalar, struct, Array, Map, and reference properties with source-located rejection. | `std::byte` is unsupported and every `std::vector<uint8>` is necessarily an element-wise Array. |
| Logical archives | CoreDObject already defines logical `Bytes`, bounded raw transfer, structured field scopes, and transactional Archives. | Reflected properties have no first-class Blob owner that selects this existing logical value. |
| Default planning | Atomic Bytes comparison already exists for native named fields, while Array planning supports indexed intent and bounded nodes. | Reflected voxel bytes enter the Array path and exceed the one-million-node guard at `128^3`. |
| Schema evolution | DAST v4 custom versions and load-only deprecated reflected fields support exact type-change migration and transactional `PostDeserialize`. | Volume source has no Array-to-Blob version route. |
| Volume textures | PNG import, normalized source, deterministic mips/DDC, cook, TXPL, and Texture3D upload are implemented. | The authored source uses `vector<uint8>` end to end and real `128^3` package save fails after a successful build. |
| Diagnostics | Default-delta planning produces a typed reason and logical path. | AssetCore replaces it with a generic message, obscuring the actionable failure in LevelEditor. |

## Implementation Stages

### Stage 0: Freeze Blob semantics, limits, and migration

- [x] Audit `uint8` and raw-byte reflected consumers, Archive capabilities,
  DAST v4 Bytes framing, process-local object graphs, default planning, and the
  complete volume-source API surface.
- [x] Freeze generated/runtime property kinds for `std::byte` and
  `std::vector<std::byte>`, including supported positions and stable rejection
  of nested/custom forms.
- [x] Freeze Blob count encoding, byte/allocation ceilings, logical identity,
  copy/equality, authored intent, Details presentation, and diagnostic fields.
- [x] Freeze the volume custom-version GUID, deprecated `Array<uint8>` route,
  conversion ordering, DDC/TXPL byte-identity expectation, and retirement gate.
- [x] Add contract fixtures for direct/fixed `std::byte`, a Blob above
  one million bytes, malformed/truncated Blob input, Array-versus-Blob type
  distinction, and the reported `128^3` volume save.

#### Acceptance Gate

- Type semantics, wire shape, bounds, compatibility, failure behavior, and the
  exact production regression are explicit and fail for the expected reason
  before implementation.

### Stage 1: Add reflected byte and Blob properties

- [x] Teach DurinHeaderTool to recognize `std::byte` and a direct
  `std::vector<std::byte>`, emit distinct leaf/Blob metadata, and reject
  unsupported positions with stable source locations.
- [x] Add runtime byte/Blob kinds and generated parameter records using the
  generic property lifecycle hooks for exact storage lifecycle, copy, equality,
  snapshot, and transaction operations.
- [x] Integrate non-editable Blob summaries into generic inspection while
  rejecting numeric metadata, per-element editing, and Array operations.
- [x] Prove direct/fixed byte values, Blob construction/destruction, generated
  determinism, unsupported forms, metadata applicability, and GC neutrality.

#### Acceptance Gate

- Generated and runtime reflection distinguish `std::byte`, Blob, `uint8`, and
  `Array<uint8>` without changing existing numeric-array schemas.

### Stage 2: Make Blob serialization and default planning atomic

- [x] Add the shared bounded Blob Archive operation and use it across reflected
  serialization, object graph, duplication, property snapshots, editable copy,
  DAST v4 capture/load, and unknown-field handling.
- [x] Capture logical Bytes in one node, compare complete values exactly, emit
  one field-level delta, and reject element-level authored paths.
- [x] Preserve typed default-delta diagnostics through AssetCore and the editor
  import result, including logical path and applicable count/byte limit.
- [x] Prove deterministic save/load/resave, empty and boundary-rejected values,
  truncation/oversize rollback, default omission, forced emission, changed
  value publication, more-than-one-million-byte capture, and both delta modes.

#### Acceptance Gate

- Blob planning cost is independent of element count, all Archive paths share
  one bounded encoding, and failures remain transactional and actionable.

### Stage 3: Migrate volume-texture source bytes

- [x] Convert `FVolumeTextureSourceData::Voxels` and all Engine, TextureBuild,
  AssetForge, DDC, cook, upload, hashing, and test APIs to
  `std::vector<std::byte>` with explicit numeric conversions only where needed.
- [x] Add the versioned deprecated `Array<uint8>` field and detached
  `PostDeserialize` conversion; retain authored intent and canonical-resave
  evidence through the established compatibility system.
- [x] Prove identical normalized bytes, DDC keys, mip chains, TXPL values,
  cooked payloads, upload footprints, and last-known-good behavior before and
  after the C++ type migration.
- [x] Add package save/reload/reimport/cook coverage for horizontal `128 x 1`,
  vertical `1 x 128`, and compact row-major atlas layouts, with the horizontal
  `16384 x 128` R8 case as the mandatory regression.

#### Acceptance Gate

- Current volume assets save Blob bytes, supported historical Array assets
  migrate transactionally, and a real `128^3` R8 import completes every
  authored and cooked lifecycle step.

### Stage 4: Qualify and publish the lasting contract

- [x] Run focused DurinHeaderTool, CoreDObject, AssetCore package, texture
  build/import, editor property, and volume/cloud integration targets under the
  repository testing workflow.
- [x] Run the native aggregate, full Debug Editor build, documentation
  validation, automated editor-service import/save/reopen coverage, and a
  validation-enabled editor startup smoke.
- [x] Record deterministic default-plan node and byte bounds for the `128^3`
  R8 production fixture; use explicit Archive/package ceilings rather than
  platform-noisy peak-memory samples.
- [x] Publish lasting `std::byte`/Blob reflection and serialization rules in
  the Core contracts and update the volume-texture authoring contract.
- [x] Record compatibility evidence, complete every passed gate, and mark the
  plan completed only after the reported user workflow succeeds.

#### Acceptance Gate

- All focused, aggregate, build, documentation, compatibility, and real-editor
  gates pass with no Array-schema regression or hidden generic planner error.

## Validation Evidence

- DurinHeaderTool: 192 tests passed.
- `CoreObjectTests`: 79 passed; `CorePropertyValueSnapshotTests`: 16 passed;
  `AssetPackageTests`: 98 passed; `TextureTests`: 78 passed.
- The production atlas fixture uses an actual `16384 x 128` PNG and completes
  import, authored save/reload, reimport, and Win64/Game Cook. The normalized
  source contains exactly 2,097,152 bytes and both enabled/no-delta plans remain
  below 100 logical fields.
- Full `Win64-Debug-DurinEditor` `all` build passed. The native aggregate passed
  all 77 targets on rerun; the first run's only failure was the unrelated
  renderer deferred-deletion race, which passed immediately in isolation.
- The built validation-enabled editor remained healthy through the bounded
  startup smoke; the automated AssetForge fixture owns the reproducible
  import/save/reopen portion.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Type distinction | `std::byte`, Blob, `uint8`, and `Array<uint8>` retain explicit non-heuristic identities | DurinHeaderTool and CoreDObject tests |
| Property lifecycle | Empty/nonempty Blob values copy, compare, snapshot, duplicate, replace, and destroy exactly | CoreDObject tests |
| Default delta | A Blob above one million bytes is one atomic value; unchanged omits and changed/forced emits completely | Default-delta tests |
| Archive safety | Size overflow, truncation, unsupported capabilities, and excessive allocation reject before commit | Core/CoreDObject tests |
| DAST v4 | Current Bytes packages round-trip deterministically and preserve unknown-field behavior | AssetCore package tests |
| Compatibility | Historical volume `Array<uint8>` loads through its exact version route and canonical resave emits only Bytes | Asset compatibility fixtures/audit |
| Volume byte identity | Source, mip, key, TXPL, cook, and upload byte sequences remain unchanged | Engine/TextureBuild/RHI tests |
| Real import | `16384 x 128` PNG with `128 x 1` tiles imports as `128^3` R8, saves, reloads, reimports, and cooks | AssetForge/editor integration tests |
| Layout regression | Horizontal, vertical, and compact row-major grids reconstruct identical Z ordering | Volume importer tests |
| Diagnostics | A forced planner limit failure reports reason, path, observed value, and limit in LevelEditor | AssetCore/editor tests |
| Aggregate | Focused targets, native aggregate, full build, docs, and editor smoke stay green | Repository validation |

## Definition of Done

- Reflection accepts `std::byte` and treats a direct
  `std::vector<std::byte>` as an owned atomic Blob while preserving ordinary
  `std::vector<uint8>` Array behavior.
- Every reflected persistence and process-local value path handles Blob through
  one canonical bounded operation with transactional failure.
- Default-delta planning represents a multi-megabyte Blob without per-byte
  nodes and surfaces complete diagnostics when any independent bound fails.
- Volume-texture source APIs use `std::byte` end to end, retain exact output
  identity, and migrate supported old `Array<uint8>` packages.
- The user's `128^3` horizontal PNG workflow imports, saves, reloads,
  reimports, cooks, and drives the existing cloud asset path successfully.
- Lasting reflection, serialization, asset compatibility, and volume-texture
  contracts are documented and all required validation gates pass.

## Deferred Follow-ups

- Typed atomic buffers for `uint16`, half, float, and other portable element
  formats once a concrete authored consumer freezes endianness and editing
  semantics.
- Nested Blob containers or Blob Map values after an authored schema requires
  them.
- Content-addressed authoring bulk storage, compression, streaming, chunking,
  and partial loading for source payloads materially larger than current dense
  volume assets.
- Hash caching for repeated very-large Blob equality checks if profiling shows
  exact comparison is a meaningful cost.

## Related Documentation

- [Serialization](../../../Runtime/Core/Serialization.md)
- [Generated reflection system](../../../Runtime/Core/ReflectionSystem.md)
- [Asset packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset data lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Volume textures](../../../Runtime/Assets/VolumeTextures.md)
- [Volume Texture Import and Cloud Diagnostics plan](VolumeTextureImportAndCloudDiagnostics.md)
- [Testing](../../../Agents/Testing.md)
- [Build and run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool`
- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DefaultDeltaPlan.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DefaultDeltaPlan.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureDerivedData.cpp`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Editor/AssetForge/Private/VolumeTextureSourceTranslation.cpp`
- `Engine/Tests/Native/CoreDObjectTests`
- `Engine/Tests/Native/AssetCoreTests`
- `Engine/Tests/Native/EngineTests/Private/Texture`
