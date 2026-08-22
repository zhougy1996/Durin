# Bounded Payload Serialization Simplification Plan

Summary: Standardize Engine payload decoders on value candidates and reduce bounded Archive adaptation to value, encode, and decode contracts without changing payload bytes.

Last reviewed: 2026-08-23

Status: Active
Completed:

## Current Status

Commit `b01baa61e` centralized nine Engine payload serializers behind
`SerializeBoundedArchivePayload`. The shared helper now owns the bounded-region
capability check, stored-size ceiling, raw byte transfer, decode-failure mapping,
and success-only commit ordering. Texture container and value decoders also use
`FPayloadDecodeResult`, and focused coverage passes for Texture, VolumeTexture,
TerrainHeightmap, StaticMesh/collision, SkeletalMesh/AnimationClip, and
EnvironmentLighting without changing their canonical payload bytes.

The remaining abstraction is intentionally functional but broader than the
selected long-term boundary. It exposes four template parameters and three
callbacks: candidate type, build, parse, and commit. That shape is caused by
historical decoder outputs rather than by the Archive protocol: Texture uses
`unique_ptr`, Terrain and EnvironmentLighting use `shared_ptr<const T>`, while
mesh and animation codecs already decode value candidates. The commit callback
can consequently perform arbitrary publication and the helper cannot express
transactional replacement as a type-level invariant. Terrain and environment
lighting additionally copy successful pointer-owned candidates into their
destinations.

The selected next step is to normalize all complete payload decoders on detached
value candidates, then reduce the Archive adapter to destination value, encode,
and decode. No codec-traits framework or new result container is selected.

## Goal

Make whole-payload serialization expose the smallest contract that preserves
the required behavior: encode the current value when saving; require and bound
one complete remaining Archive region when loading; decode into a detached value;
and move-assign that value only after successful validation.

After completion, every migrated call site supplies only its destination,
policy, encode callable, and decode callable. Candidate construction and commit
ordering are owned by the helper rather than repeated or delegated to callers.

## Scope

- The Engine-private whole-payload Archive adapter and its direct contract tests.
- The nine current serializer entry points for Texture2D, TextureCube,
  VolumeTexture, TerrainHeightmap, StaticMesh render data, StaticMesh collision,
  SkeletalMesh, AnimationClip, and EnvironmentLighting.
- Texture, Terrain, and EnvironmentLighting decoder signatures and ownership
  where pointer-shaped outputs exist only to carry a detached candidate.
- The public VolumeTexture decode function and all repository consumers of its
  candidate-output signature.
- Focused build, decoder, Archive-failure, transaction, and deterministic-byte
  validation for the affected asset families.
- The lasting asset-data-lifecycle contract after implementation is complete.

## Non-Goals

- Changing any payload magic, schema, field order, alignment, checksum, stable
  identifier, builder version, derived-data key, or cooked descriptor.
- Adding a length prefix or replacing the existing bounded Archive region with
  `SerializeByteBlob`.
- Supporting multiple unframed payloads in one parent Archive region, streaming
  decode, partial publication, incremental parsing, or zero-copy specialization.
- Moving the helper into Core or making Core depend on Engine's
  `FPayloadDecodeResult`.
- Introducing codec traits, virtual codec interfaces, type erasure, a custom
  `expected` type, or a generic repository-wide serialization framework.
- Refactoring the asset-specific wire readers, builders, validation rules, DDC
  policy, Cook flow, or runtime resource publication beyond the ownership changes
  required by this plan.
- Broad formatting or naming cleanup in affected payload codecs.

## Design Decisions and Invariants

- One invocation owns exactly one complete payload and, on load, consumes the
  entire remaining bounded Archive region. Callers that need multiple values
  must establish separately framed child regions first.
- Bounded capability and allocation limit remain distinct. Missing
  `EArchiveCapability::RemainingPayload` maps to `UnsupportedCapability`; a
  known size above the payload or container ceiling maps to `LimitExceeded`.
- `Incompatible` continues to map to `UnsupportedVersion`; `Corrupt` continues
  to map to `InvalidData`. A raw Archive transfer failure keeps the Archive's
  original first sticky failure.
- Saving validates the encoded byte count before writing. Loading validates the
  remaining byte count, including `vector<uint8>::max_size()`, before allocating.
- The simplified helper has the conceptual contract:

  ```cpp
  template<typename T, typename EncodeFn, typename DecodeFn>
  auto SerializeBoundedArchivePayload(
      FArchive& Ar,
      T& Value,
      FBoundedArchivePayloadPolicy Policy,
      EncodeFn&& Encode,
      DecodeFn&& Decode) -> void;
  ```

- `Encode` receives the source value and produces bytes plus an error message.
  `Decode` receives an immutable byte span and a detached value candidate and
  returns `FPayloadDecodeResult`.
- `T` must be default constructible and move assignable. The helper constructs
  `T Candidate` only on the loading path and performs
  `Value = std::move(Candidate)` only when decode succeeds.
- Decoder APIs with caller-visible output parameters remain transactional on
  their own: they assemble a local value and replace the output only on success.
  The helper's detached candidate is an additional publication boundary, not a
  reason for public decoders to mutate caller state on failure.
- Pointer ownership is not part of complete payload decode identity. No
  `unique_ptr`, `shared_ptr`, or commit callback crosses the adapter contract
  after migration.
- The existing `bool + OutError` encode contract remains in scope because all
  build failures map to `InvalidData`; this plan does not invent an encode-result
  hierarchy without distinct failure semantics.
- The helper remains Engine-private. Its template constraints may improve
  compile diagnostics, but they must not add a codec concept or public module
  surface.
- Canonical encoded bytes and fixed fixture hashes are acceptance evidence.
  Ownership cleanup alone never authorizes a payload schema or builder-version
  change.

## Current Foundations and Gaps

Foundations:

- Core Archive already advertises remaining-region support as an explicit
  capability and provides canonical bounded memory readers and writers.
- `FPayloadDecodeResult` already separates success, incompatibility, and
  corruption across the migrated codec families.
- The current helper contains one implementation of Archive failure mapping,
  size validation, byte transfer, and success-only callback ordering.
- StaticMesh, StaticMeshCollision, SkeletalMesh, and AnimationClip parsers
  already accept value candidates.
- Focused tests cover canonical round trips, malformed payload rejection,
  transactional preservation, and the shared adapter's principal failure paths.

Gaps:

- The helper still exposes `Candidate`, `BuildFn`, `ParseFn`, and `CommitFn`, so
  caller-controlled commit behavior remains outside its invariant.
- Texture2D, TextureCube, and VolumeTexture decoders publish pointer candidates.
- TerrainHeightmap and EnvironmentLighting decoders publish shared immutable
  pointers even though their serializers immediately copy the pointed-to values.
- Existing adapter tests prove callback ordering but do not yet prove the final
  generic value replacement contract or that decode failure preserves a
  non-default destination without a caller-provided commit function.
- The lasting asset lifecycle documentation describes detached publication but
  does not name the Engine-private whole-bounded-region adaptation rule.

## Implementation Stages

### Stage 0: Freeze the value-transaction contract

- [ ] Extend the shared adapter tests with a non-default destination value and
  prove that prefailed Archives, build failure, save-side size failure, missing
  remaining-region capability, load-side size failure, incompatible decode, and
  corrupt decode never replace it.
- [ ] Preserve a success case proving that the final candidate is moved into the
  destination exactly once and that encode sees the current source value.
- [ ] Confirm every target payload value is default constructible and move
  assignable at its serializer implementation boundary; record any exception as
  a plan decision before changing the helper signature.
- [ ] Retain or add family-level assertions that incompatible target/schema facts
  remain distinct from corrupt bytes and that failed public decoder calls leave
  their prior output unchanged.
- [ ] Record the current deterministic fixture hashes or exact byte-equality
  checks used by each affected target so later stages cannot treat ownership
  changes as permission to update fixtures.

#### Acceptance Gate

- The current implementation passes the expanded transaction matrix, every
  affected value satisfies the selected value contract or has an explicit
  recorded exception, and deterministic-byte evidence is identified before any
  decoder ownership signature changes.

### Stage 1: Normalize decoders on detached values

- [ ] Change Texture2D and TextureCube internal decoders to produce
  `FTexturePlatformData` and `FTextureCubePlatformData` values rather than
  `unique_ptr` candidates.
- [ ] Change `ParseVolumeTextureSerializedValue` and its repository consumers to
  produce a transactional `FVolumeTexturePlatformData` value while retaining its
  `FPayloadDecodeResult` compatibility contract.
- [ ] Change TerrainHeightmap and EnvironmentLighting decoders to produce value
  candidates and move them into successful outputs, eliminating the serializer's
  post-decode copy from `shared_ptr<const T>`.
- [ ] Keep TexturePayloadContainer, StaticMesh, StaticMeshCollision,
  SkeletalMesh, and AnimationClip on their existing value-result contracts; do
  not churn already-conforming signatures.
- [ ] For every modified public or reusable decoder, construct a local candidate
  and assign the caller's output only after all structural, compatibility,
  checksum, and trailing-data validation succeeds.
- [ ] Update decoder and family tests for the new value signatures without
  changing canonical fixtures or expected compatibility classifications.

#### Acceptance Gate

- All nine payload families can decode into detached values, pointer-shaped
  candidate outputs have been removed from this boundary, corrupt and
  incompatible decode preserve prior outputs, and focused family tests retain
  their exact encoded-byte evidence.

### Stage 2: Reduce the Archive adapter to value, encode, and decode

- [ ] Replace the four-parameter/three-callback helper with the selected
  destination-value, policy, encode, and decode signature.
- [ ] Make the helper own loading-path candidate construction and the sole
  success assignment; remove `CommitFn` and explicit candidate-type arguments.
- [ ] Add focused compile-time constraints or assertions for default construction,
  move assignment, and callable return contracts without introducing codec
  traits or a public concept hierarchy.
- [ ] Migrate all nine serializer entry points and pass the source value explicitly
  to encode callables rather than allowing build ownership to remain implicit in
  an arbitrary commit closure.
- [ ] Delete pointer-dereference commits and confirm TerrainHeightmap and
  EnvironmentLighting successful loads move values instead of copying them.
- [ ] Keep all bounded capability, allocation, sticky failure, save-side ceiling,
  decode mapping, and diagnostic-name behavior centralized in the helper.

#### Acceptance Gate

- The helper has no `Candidate` template argument and no commit callback; all nine
  callers use the same value transaction; `GetRemainingPayloadBytes()` appears
  only in the Engine-private adapter for these serializers; and the Stage 0
  transaction matrix passes unchanged.

### Stage 3: Qualify and publish the lasting boundary

- [ ] Build the smallest affected targets and run every row in the validation
  matrix using the repository agent build and testing workflows.
- [ ] Review generated payload bytes or fixed hashes for Texture2D, TextureCube,
  VolumeTexture, TerrainHeightmap, StaticMesh/collision, SkeletalMesh,
  AnimationClip, and EnvironmentLighting; reject any unexplained difference.
- [ ] Search the Engine payload serializers for manual remaining-byte allocation,
  `Incompatible`/`Corrupt` Archive mapping, and pointer-shaped complete-payload
  candidates that should have migrated.
- [ ] Update the authoritative asset-data-lifecycle documentation with the
  implemented Engine-private whole-bounded-region and value-transaction contract;
  keep Archive capability ownership in the Core serialization document.
- [ ] Update this plan's status, checklists, validation evidence, and completion
  date only after all required gates pass.

#### Acceptance Gate

- All validation rows pass, canonical bytes are unchanged, no migrated serializer
  retains the old candidate/commit adaptation, the lasting contract is documented
  in its owning domain, and the plan is ready to mark Completed.

## Validation Matrix

| Boundary | Target and selection | Required evidence |
| --- | --- | --- |
| Shared adapter | `StaticMeshTests`, `FBoundedPayloadSerializationTests.*` | Save/load limits, capability rejection, sticky failure preservation, error mapping, source encode, and success-only value replacement pass. |
| Static mesh values | `StaticMeshTests`, `FStaticMeshPayloadCodecTests.*` | Render and collision payload round trips, incompatibility, corruption, truncation, transactional preservation, and deterministic fixtures pass. |
| Texture values | `TextureTests`, `FTextureDerivedDataTests.*:FVolumeTextureTests.*` | Texture2D, TextureCube, and VolumeTexture value decode, target/schema classification, prior-output preservation, and stable bytes pass. |
| Terrain value | `TerrainHeightmapTests`, `FTerrainHeightmapPayloadTests.*:FTerrainHeightmapDerivedDataTests.*` | Value round trip, hierarchy validation, corruption rejection, and deterministic payload evidence pass. |
| Skeletal and animation values | `SkeletalAssetTests`, `FSkeletalAssetTests.*` | SkeletalMesh and AnimationClip round trips, malformed payload transactions, DDC/Cook-facing adapters, and deterministic bytes pass. |
| Environment value | `EnvironmentLightingTests`, `FEnvironmentLightingTests.*` | Value round trip, producer compatibility, corrupt-input preservation, checked-in payload validation, and direct Cook bytes pass. |
| Static review | Engine payload source search plus diff review | Nine migrated calls, one remaining-byte adaptation, no commit callback, no pointer candidate at the complete-payload boundary, and no schema/version edits. |
| Documentation | Documentation validators | Changed documentation and all active plans validate with no new diagnostics. |

## Definition of Done

- `SerializeBoundedArchivePayload` accepts a destination value, policy, encode,
  and decode; it constructs and publishes the loading candidate itself.
- All nine current payload serializers use the simplified contract without
  manual bounded-region, allocation, error-mapping, or commit logic.
- Complete-payload decoders use detached value candidates and preserve prior
  outputs on every failure.
- Missing bounds, excessive sizes, incompatible payloads, corrupt payloads, and
  raw Archive failures retain their selected structured failure codes.
- Existing payload bytes, schemas, builders, DDC keys, and Cook descriptors are
  unchanged.
- Every validation-matrix row passes and evidence is recorded in Current Status.
- The lasting runtime contract is updated, the plan is marked Completed, and
  each implementation-stage commit carries exact plan/stage provenance required
  by repository policy.

## Deferred Follow-ups

- A length-delimited child-region API for composing multiple payloads inside one
  parent Archive.
- Zero-copy decode specialization for memory-backed Archives after profiling
  demonstrates that the current transfer allocation is material.
- A common encode-result type if future builders acquire failure categories that
  cannot correctly map to `InvalidData`.
- Cross-module extraction only if another runtime module independently needs the
  same complete-payload transaction without depending on Engine semantics.
- Codec traits or generated codec registration only if the family count and
  repeated call-site context grow enough to justify the added type machinery.

## Related Documentation

- [Serialization](../Runtime/Core/Serialization.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`BoundedPayloadSerialization.h`](../../Engine/Source/Runtime/Engine/Private/Serialization/BoundedPayloadSerialization.h)
- [`PayloadDecodeResult.h`](../../Engine/Source/Runtime/Engine/Public/PayloadDecodeResult.h)
- [`TextureDerivedData.h`](../../Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h)
- [`TextureDerivedData.cpp`](../../Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp)
- [`VolumeTextureDerivedData.cpp`](../../Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureDerivedData.cpp)
- [`TexturePayloadContainer.h`](../../Engine/Source/Runtime/Engine/Private/Texture/TexturePayloadContainer.h)
- [`TerrainHeightmapDerivedData.cpp`](../../Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapDerivedData.cpp)
- [`StaticMeshDerivedData.cpp`](../../Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp)
- [`SkeletalDerivedData.cpp`](../../Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalDerivedData.cpp)
- [`EnvironmentLighting.cpp`](../../Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLighting.cpp)
- [`StaticMeshPayloadCodecTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp)
- [`TextureDerivedDataTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Texture/TextureDerivedDataTests.cpp)
- [`TextureBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp)
- [`TerrainHeightmapTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp)
- [`SkeletalAssetTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp)
- [`EnvironmentLightingTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/EnvironmentLightingTests.cpp)
