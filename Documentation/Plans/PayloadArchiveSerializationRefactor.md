# Payload Archive Serialization Refactor Plan

Summary: Replace whole-buffer payload adapters with UE-style archive serialization, explicit byte boundaries, and caller-owned replacement guarantees while preserving existing formats.

Last reviewed: 2026-09-09

Status: Completed
Completed: 2026-09-09

## Current Status

Post-completion cleanup retires the unused public Volume Build/Parse wrappers
and migrates their regression coverage to Archive boundaries. Explicit target
arguments remain because counting/hashing Archives do not accept target context.
The plan remains completed in place, without archival. Cleanup validation passed
all 82 affected test targets, followed by 102 TextureTests after removal of an
unused Volume error helper. Receipts:
`Build/.agent-state/logs/20260909-163702-637794-30980-ctest.log` and
`Build/.agent-state/logs/20260909-163922-511596-22820-TextureTests.log`.

Stages 0-3 are complete. All six payload entries use bidirectional Archive
schemas and declared byte extents; the generic complete-buffer adapter and
nested mandatory destinations are removed. Production DDC/Cook callers supply
Archive target context and retain their existing publication authority.
Frozen payload bytes, schema/producer versions and DDC keys are unchanged.

Windows Debug validation passed: the 99-test pre-change texture baseline,
100-test Volume pilot, 82-target `test affected`, and final 12-target
`@domain=asset-cook+static-mesh+texture+environment-lighting` selection after
context routing. The final selection includes cooked texture/mesh loading and
process Cook consumers. Exact receipts and compatibility anchors are below.
Documentation validation and all-plan lifecycle validation passed. No GPU
performance or allocation-free decoding claim is made.

Lasting contracts are published in [Serialization](../Runtime/Core/Serialization.md),
[Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md),
[BulkData](../Runtime/Assets/BulkData.md),
[Volume Textures](../Runtime/Assets/VolumeTextures.md), and
[Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md).

## Goal

Make each affected persistent payload use one bidirectional
`Serialize(FArchive&)` customization, with a stable owner/context parameter only
when interpretation requires it. Remove the generic complete-buffer
Encode/Decode bridge and its mandatory temporary destination. Keep canonical
bytes, format validation, bounded allocation, and existing live-resource
replacement guarantees.

## Scope And Dependencies

- Core owns byte transfer, canonical primitives, bounded regions, limits,
  archive context, and structured failures.
- Engine asset families own payload layout, target interpretation, format
  versions, semantic validation, and decoded resource state.
- Package and BulkData infrastructure retains physical placement, asynchronous
  reads, leases, shared ownership, and publication authority.
- The work covers the six adapter users and their direct build, DDC, cooked
  load, and replacement callers. It does not rewrite all serializers.
- Coordinate shared consumer changes with the active
  [Asset Package Reload plan](AssetPackageReload.md). This plan does not take
  ownership of package reload transactions or reference replacement.

Existing contracts remain authoritative until their owning implementation stage
updates them: [Serialization](../Runtime/Core/Serialization.md),
[Package Bulk Data](../Runtime/Assets/BulkData.md), and
[Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md).

## Selected Design

### Public Interface And Format Ownership

Use UE-style `Serialize(FArchive&)`, `Ar << Value`, and
`Ar.Serialize(Data, Size)` at family entry points. Reuse existing Archive
facilities; do not introduce a public codec registry, generic `Result<T>`
framework, or a second byte transport. Existing Build/Parse helpers may remain
temporarily while consumers migrate, but opposite directions alone do not
justify two independent final schemas.

UE alignment means the customization and call pattern, not UE binary-format or
source compatibility. Retain `FCanonicalMemoryReader/Writer` and structured
`HasError`/`Fail` APIs during this bounded refactor. Renaming all Archives or
adding compatibility aliases is not an acceptance requirement. Preserve
canonical little-endian encoding, stable identifiers, and deterministic output.

Read target/version facts from Archive context where the mapping is lossless.
Retain explicit stable context where it represents additional family semantics;
reject missing or conflicting target facts instead of consulting global state.
Serialization must not mutate persistent values on save, including discovery,
counting, hashing, and Cook paths that actually invoke these payload entries.

### Loading And Publication

Ordinary payload `Serialize` loads in place. On failure its destination is
destructible but incomplete and must not be consumed; it does not promise to
restore prior values or the Archive cursor. Successful loading replaces the
serialized state completely, including clearing obsolete sequence contents.

Do not unconditionally construct `Candidate`/`LoadedValue` inside the generic
entry or every family serializer. New-resource and cache-miss paths can discard
their unpublished destination on failure. If an operation replaces live data
and must retain the old resource on failure, that operation owns one detached
replacement and publishes it only after complete decode and validation. Reuse
an already detached destination instead of adding another temporary layer.

Audit callers before relaxing the current adapter guarantee. Preserve explicit
transactional contracts of unrelated Core helpers, BinaryEnvelope, package
loading, and BulkData. Local scratch required by a format remains permitted.
This plan introduces no new worker-thread publication rights; existing owner
thread and resource lifecycle rules continue to govern replacement.

### Explicit Regions And Allocation Limits

The owning slot/container supplies the exact payload extent. A payload must not
implicitly consume unrelated bytes remaining in its parent Archive. Use the
existing bounded memory reader and `ReadRegion` first. Add a narrowly scoped
bounded Archive adapter only where a real non-memory caller needs it; propagate
context and failures, check arithmetic, and reject reads across the region.

The complete payload load boundary uses `RequireArchiveEnd` or an equivalent
checked region completion. Nested records consume their own declared extents;
they do not assert the end of their parent. Unsupported archive capabilities
fail explicitly. Do not add a new length prefix or nested envelope to an
existing wire format.

Enforce stored-size limits before input allocation and before output growth.
Reuse BinaryFormat cursor limits where applicable. Preserve family count,
dimension, pitch, offset, alignment, padding, and hash checks. Validate checked
size arithmetic and decoded allocation budgets before allocating from encoded
counts; an input-size cap is not a decoded-memory cap. Stage 0 records existing
limits and the concrete missing checks instead of inventing universal values.

Borrow input regions when their owner remains alive for the complete decode.
Any result retaining byte views must retain an appropriate shared buffer or
resource lease. Do not promise zero-copy decoded resources where their owning
containers require copies. Formats requiring offsets/hashes may compute layout
first or retain bounded staging; single-pass streaming is not mandatory.

### Errors And Compatibility

Keep detailed Archive failure categories such as truncation, overflow, limits,
trailing data, and unsupported versions through internal calls. Distinguish
target incompatibility from format-version incompatibility in diagnostics.
Convert to existing external build/cache result types only at those boundaries,
preserving their recoverable miss/rebuild and cancellation behavior. Do not
expand this into a repository-wide error-enum migration.

Preserve existing payload layouts, byte output, package versions, and DDC key
meaning. Capture representative baseline fixtures before rewriting serializers.
Any discovered need for an incompatible layout change must be recorded as a
plan amendment with explicit version/cache migration before implementation.

BulkData already owns loading, residency, asynchronous range access, and leases.
This plan does not redesign that system, introduce new mip/LOD streaming, or
replace embedded family payloads with a new storage format.

## Caller Audit And Baselines

All six production entries receive memory archives, never a package Archive
directly. Their owning DDC value or BulkData lease provides the exact span.
No non-memory loading adapter is justified. Saving also reaches counting and
hashing Archives in codec tests; those paths must preserve source state.

| Entry | Direct producers and consumers | Destination and failure owner |
| --- | --- | --- |
| Texture2D | `Texture2DBuildProvider.cpp` uses `TextureDerivedDataCache::Load/Store`; `TextureCookedData.h` captures and loads cooked PlatformData | Provider has unpublished platform data; cache load currently adds a candidate. Cook load owns a unique candidate and calls SetPlatformData/UpdateResource only after exact completion and releasing the lease. Miss rebuilds; failed Store remains diagnostic. |
| TextureCube | `TextureCubeBuildProvider.cpp`, same cache and cooked templates | Same boundaries; six ordered faces and complete mip chains must replace previous sequences. |
| VolumeTexture | `VolumeTextureBuildProvider.cpp`, same cache and cooked templates; public Build/Parse helpers used by TextureBuildTests | Same boundaries. At the Stage 0 audit, public Parse promised preservation; its compatibility wrapper was retired in post-completion cleanup after test migration. |
| Environment lighting | `DEnvironmentLighting::PostLoad`, `GetData`, `SerializeCooked`, `ContributeToCook`; AssetForgeBuiltins produces the authored file through Serialize | PostLoad and Cook validation own shared candidates; lazy cooked load retains its bulk lease through decoding and publishes after validation. No DDC. Authored file loading currently lacks a pre-read family size check. |
| StaticMesh render | `StaticMeshBuildProvider.cpp` EncodeRenderData/DecodeRenderData, `StaticMeshCook.cpp`, `DecodeStaticMeshCookedProduct` | Build decode owns a local payload then constructs unpublished render data. Cooked product owns the joint render/collision candidate and publishes only after metadata validation. Cancellation is recoverable and checked during codec work. |
| StaticMesh collision | Same provider EncodeCollision/DecodeCollision, StaticMeshCook, DecodeStaticMeshCookedProduct | Local payload and detached geometry; source/query policy validated before publication. Joint cooked-product publication belongs to the caller. |

Texture target enums map losslessly to Archive Platform `Win64` and Profile
`Game`/`EditorValidation`. Production inner archives now carry these facts and
family customizations read them directly. Optional explicit target parameters
remain for source compatibility and context-free counting/hashing callers;
missing facts and conflicts are rejected. StaticMesh likewise reads Win64 from
Archive context and retains its borrowed cancellation callback, which is not
archive context. No target or producer identity is read from global state.
Environment payload layout carries no platform field; Cook checks Win64/Game
at its owner. Package reload's detached product and joint publication contract
must remain unchanged.

Existing compatibility anchors (captured before implementation):

- TextureDerivedDataTests fixes Texture2D format-specific byte hashes and DDC
  keys, plus Cube size 1376 and hash `8e22a84dac1195860e4e3860199b8dda`.
- TextureBuildTests fixes Volume size 177, hash
  `3653410e7207268f7089e69dfa0f3d38`, and DDC key
  `f912c280977e4486722e8f7bb22a5277`; also exercises corrupt-cache rebuilding.
- StaticMeshPayloadCodecTests fixes render sizes 556/824 and hashes
  `38eca74fe55840b7496a5a7ce640a9c8`/`22b719d486a6e9c288ede84204ab5ab6`.
- StaticMeshCollisionStage0Tests.inl fixes production collision bytes with hash
  `b43434e49ae8c9c62ea8ef4024b54159` and production key bytes with hash
  `01a75c9d6203686e307cc52a38543a74`.
- The checked-in `Engine/Content/Renderer/DefaultStudioEnvironment.iblbulk`
  baseline SHA-256 is
  `e696f55fb5da842983e3d2dd2bb6f4f4403fbf15c2331a8996fd0179f893c1ab`.
  EnvironmentLightingTests exercises this file, deterministic fixture output,
  corruption rejection, and authored-to-Cook consumption.

Concrete limits and required work:

- Texture stored bytes are capped at 2 GiB; dimensions are 16384 (2D), 4096
  (Cube), 2048 (Volume), with 32 mips. Record-table allocation currently occurs
  before family count validation; cap it to the dimension's slice/mip count
  before reserve. Disjoint record ranges bound aggregate decoded voxel bytes;
  retain exact pitch checks before allocating each mip.
- Texture body offsets and hashes require layout preparation, but input may be
  borrowed through ReadRegion for the complete synchronous decode. Output
  layout is already checked before body construction. Share header/record
  schemas and preserve the 80-byte header and 40-byte records.
- StaticMesh render permits 8 GiB, 64 chunks, 8 LODs, 65536 sections per LOD,
  100 million vertices and 300 million indices per LOD. Existing chunk decoding
  caps compression ratio at 64 (compressed chunks currently fail explicitly as
  unavailable) and validates stream extents before vector
  allocation. Audit aggregate native vector storage separately from encoded
  stream sizes, including aligned vector element sizes and absent optional
  streams when loading over old values.
- DCOL permits 256 MiB and 8 chunks (current schema requires four), checks
  count-times-element-size overflow, exact disjoint extents, and two million
  triangles. Geometry construction is semantic validation, not publication;
  retain it and cancellation checks while removing redundant payload candidates.
- Environment has fixed dimensions and element counts. Its current input body
  copy and generic input copy are unnecessary. Preserve canonical half-value
  bytes explicitly rather than relying on host-endian memcpy.
- Existing generic-adapter tests assert transactional loads. Replace those
  implementation-specific expectations with ordinary-load failure/discard,
  explicit region isolation, save immutability, and caller-owned replacement
  tests. Do not weaken unrelated BinaryEnvelope, BulkData or package guarantees.

Implementation qualification has passed the full affected selection (82 targets),
including TextureCookIntegrationTests, CookedMeshLoadingTests, AssetPackageTests,
StaticMesh cancellation/DDC tests and EnvironmentLightingTests. Receipt:
`Build/.agent-state/logs/20260909-154446-381661-32044-ctest.log`.
The subsequent five-target family feature selection also passed, including the
DCOL overflow category and raw-without-borrowing regression; receipt:
`Build/.agent-state/logs/20260909-154758-738284-32560-ctest.log`.
Final context-routing changes passed the 12-target bounded Cook/family domain
selection, including cooked mesh and texture consumers. Receipt:
`Build/.agent-state/logs/20260909-155113-213171-31852-ctest.log`.

## Implementation Stages

### Stage 0: Audit Payload Contracts And Capture Baselines

- [x] Trace all six entries through build, DDC, cooked loading, and replacement;
  record destination ownership, byte extent source, target context, and failure
  handling for each direct caller in this plan.
- [x] Identify existing tests and capture deterministic baseline payload bytes
  and relevant DDC-key expectations before changing implementation.
- [x] Record required bounded-reader/writer changes, allocation checks, and
  callers that require preservation of old values; resolve Archive context
  mappings and non-memory region needs from those concrete callers.

Completion condition: every affected entry has an explicit boundary and failure
owner, with baseline compatibility evidence and a bounded implementation scope.

### Stage 1: Establish Archive Boundaries And Migrate VolumeTexture

- [x] Implement only the Core boundary/limit facilities justified by Stage 0,
  reusing existing readers, writers, and BinaryFormat cursors.
- [x] Migrate VolumeTexture to the single bidirectional family customization;
  put full-region completion and live replacement protection at their owners.
- [x] Verify unchanged baseline bytes, successful load equivalence, exact
  bounds, early allocation rejection, and failure publication behavior.

Completion condition: the pilot no longer uses the generic adapter and proves
the selected contracts without changing its wire format.

### Stage 2: Migrate Remaining Payload Families And Retire The Adapter

- [x] Migrate Texture2D, TextureCube, environment lighting, and both StaticMesh
  entries and their callers to the pilot contract.
- [x] Remove duplicate directional schemas and unnecessary whole-input copies;
  retain documented bounded staging where the existing format needs it.
- [x] Preserve cancellation, cache rebuild, save immutability, and replacement
  ordering; remove mandatory nested temporary destinations.
- [x] Remove `BoundedPayloadSerialization.h` and obsolete helpers only after
  all consumers migrate; verify no stale includes or adapter references remain.

Completion condition: all six entries use the selected Archive interface, and
every retained temporary or compatibility wrapper has a concrete owner/reason.

### Stage 3: Qualify Consumers And Publish Lasting Contracts

- [x] Validate baseline byte equality and load equivalence for all affected
  families, including DDC and cooked payload consumers selected in Stage 0.
- [x] Cover truncated input, overflow/oversize declarations, malformed records,
  unsupported context/version, trailing bytes, adjacent payload isolation, and
  successful loading over a destination with pre-existing sequence contents.
- [x] Verify ordinary failed destinations are discarded and replacement paths
  retain old live data; include StaticMesh cancellation and save immutability.
- [x] Verify borrowed-region lifetimes and the removal of identified input
  copies; report any remaining staging without claiming allocation-free loads.
- [x] Update owning Serialization and affected asset contracts, record exact
  validation evidence, and complete the plan only after all gates pass.

Completion condition: compatibility, failure handling, and all directly affected
consumer paths are validated and documented.

## Final Qualification Evidence

| Contract | Evidence |
| --- | --- |
| Canonical bytes and keys | Existing TextureDerivedDataTests, Volume DDC fixtures, render/DCOL frozen hashes, and byte-for-byte re-emission of the checked-in studio environment all pass. |
| Declared boundaries | New texture, Volume, render and DCOL adjacent-payload tests leave the next value unread; complete boundaries reject trailing bytes. Environment fixed extent has equivalent coverage. |
| Failure and allocation | Truncation sweeps, existing malformed/hash/record/LOD tests, preallocation stored-size/count limits, DCOL multiplication overflow and raw-reader-without-borrowing rejection pass. |
| Target/version interpretation | Existing incompatible schema/producer tests and new Archive-only, missing and conflicting target cases pass; target failures have their own Archive category. |
| Replacement and cancellation | Existing DDC rebuild, cooked-product and resource tests pass. Ordinary cancellation candidates are explicitly discarded and successfully retried. Old optional mesh streams and texture sequences are replaced completely. |
| Save immutability | Texture, Volume, environment, render and collision counting/hashing tests pass; DCOL source index/ordinal/leaf arrays remain unchanged through save. |
| Borrowed ownership | Production decode retains DDC shared buffers or BulkData leases through synchronous interpretation. Typed results retain no input views; region pointer equality is tested by the Volume pilot. |

Retained staging is format-owned: textures and environment retain one bounded
body for their leading hash; render data retains bounded logical chunks and
physical envelope packing; DCOL retains bounded streams/body and local leaf-order
indices. Geometry validation may construct detached scratch. These are not
live replacements. The public Volume Build/Parse wrappers retained at initial
completion were removed during post-completion cleanup after their test callers moved
to Archive boundaries. Optional explicit target arguments support
legacy/context-free counting and hashing callers, while production uses Archive
facts. No schema, cache-key or package-version migration was necessary.

## Validation And Handoff

Before builds read [Agent Build And Run](../Agents/BuildAndRun.md); before
selecting native tests read [Agent Testing](../Agents/Testing.md). Choose focused
existing suites from the Stage 0 caller inventory; add regression coverage for
changed contracts rather than tests that only mirror helper implementation.
Documentation-only planning requires documentation validation, not a native build.
Keep one source/build writer per checkout and record stage evidence in the same
commit as implementation. Stage checkboxes remain open until their gates pass.

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Core/Public/Serialization/BinaryFormat.h`
- `Engine/Source/Runtime/Core/Public/Serialization/SerializationDefinitions.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedDataCache.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TexturePayloadContainer.h`
- `Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLighting.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageArchive.h`
