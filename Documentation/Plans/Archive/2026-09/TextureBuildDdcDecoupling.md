# Texture Build DDC Decoupling Plan

Summary: Make TextureBuild a pure texture transformation provider while Engine owns editor-only texture DDC orchestration and runtime consumes only cooked PlatformData.

Last reviewed: 2026-09-02

Status: Archived
Completed: 2026-09-02

## Current Status

Implementation and qualification are complete:

- TextureBuild now registers only pure Texture2D, TextureCube, and VolumeTexture
  providers. It has no DerivedDataCache or Build Framework dependency, cache
  policy, key encoding, payload codec, or cache-result vocabulary.
- Engine owns editor-only Texture key construction, typed PlatformData
  serialization/validation, DDC Get/Put fallback, diagnostics, and result
  application. TextureCube normalization remains a distinct pure phase before
  Engine performs the cache lookup.
- DerivedDataCache exposes `DerivedData::GetCache().Get/Put`; caller-visible
  Trim, backend trim support, Build Function cleanup configuration, and
  ShaderBuild per-Put cleanup have been removed. Direct tests prove that Put
  does not evict existing entries.
- Golden Texture2D, six-face/panorama TextureCube, and VolumeTexture identities
  and payload hashes are covered by native tests. Focused and affected tests,
  default Editor and Game builds, and an Editor hidden-window startup/shutdown
  smoke passed on 2026-09-02.
- The configured Game compile and link closure contains neither TextureBuild nor
  DerivedDataCache; cooked PlatformData remains the runtime-only texture source.

## Goal

Make TextureBuild responsible only for deterministic, object-free texture
transformations:

```text
normalized source + build settings + target + cancellation
    -> canonical source when required + PlatformData + recipe metrics
```

Make Engine responsible for the editor-only derived-data pipeline:

```text
build identity -> DDC Get -> PlatformData Serialize/validate
    -> pure TextureBuild call on miss -> PlatformData Serialize -> DDC Put
    -> Engine result application
```

At completion:

- TextureBuild has no source, include, link, registration, or runtime-lifetime
  dependency on DerivedDataCache or its Build Framework;
- TextureBuild provider requests contain no cache persistence policy, and
  provider products contain no derived-data key, hit/miss origin, or cache
  diagnostic;
- Engine computes Texture DDC identities, performs `Get`, validates hits,
  performs pure local builds, serializes complete products, and performs
  best-effort `Put` only when `DURIN_WITH_EDITOR` is enabled;
- authored editor `PostLoad`, import/reimport, property edits, and Cook
  preparation all share the same Engine-owned cache/build orchestration;
- cooked runtime loading never queries DDC or invokes TextureBuild and obtains
  PlatformData only from the required cooked payload;
- DDC and Cook payloads use the PlatformData `Serialize` protocol rather than
  parallel texture codecs;
- existing cache keys and payload bytes remain compatible unless Stage 0 finds
  a concrete defect that requires an explicitly versioned break.

## Scope

- Refine the low-level DDC facade around request/result types and the qualified
  `DerivedData::GetCache().Get/Put` entry point and remove caller-driven Trim.
- Add DerivedDataCache as an optional private Engine dependency that resolves
  only in editor-enabled variants.
- Move Texture2D, TextureCube, and VolumeTexture cache key encoding, bucket and
  budget policy, payload serialization, cache validation, query/store/trim,
  origin classification, and diagnostics into Engine editor-only code.
- Reduce the three TextureBuild providers to pure recipes with copied or
  borrowed Engine-owned value inputs and detached Engine-owned value outputs.
- Split TextureCube panorama normalization from platform construction where
  required to preserve the existing canonical-face DDC identity and avoid
  compression work on a hit.
- Remove the three Texture Build Functions, their registry transaction, local
  input byte envelopes, and TextureBuild's DerivedDataCache dependency.
- Preserve Texture2D asynchronous compilation semantics, provider unload
  safety, completion-application ordering, Cook output, and Game deployment closure.
- Update lasting asset-data, texture-system, module, and build documentation
  after implementation is qualified.

## Non-Goals

- Moving mip generation, compression, pixel-format selection, alpha coverage,
  panorama projection, or volume mip filtering into Engine.
- Making DerivedDataCache understand Texture types, PlatformData schemas,
  build settings, or Cook targets.
- Removing the family-neutral Build Framework from DerivedDataCache while
  StaticMeshBuild, SkeletalBuild, TerrainBuild, and other clients still use it.
- Adding asynchronous cache backends, remote DDC, request coalescing, backend
  graphs, multi-value records, or a new generic asset build scheduler.
- Persisting DDC keys or derived PlatformData in authored `.dasset`/`.dbulk`
  packages.
- Changing supported formats, target platforms/profiles, visible import flows,
  transaction behavior, or render-resource readiness.
- Making cooked bulk eager in memory solely for this ownership change. Cooked
  packages must contain PlatformData, while Engine may retain the existing
  bounded lazy materialization from cooked bulk.
- Applying the same ownership migration to mesh, skeletal, terrain, material,
  or shader derived data in this plan.

## Selected Responsibility Split

| Layer | Owns | Must not own |
| --- | --- | --- |
| Engine | Texture DDC key schema and cache policy, editor-only Get/Put, PlatformData serialization and validation, hit/miss/build fallback, object scheduling, diagnostics, Cook serialization, completion result application | Texture compression, mip algorithms, panorama sampling, DDC backend mechanics |
| TextureBuild | Pure normalized-input transformations, recipe validation, algorithm versions, cancellation checkpoints, recipe metrics, provider registration | DDC APIs, Build Framework registration, cache keys/bytes/policy, cache origin, persistence diagnostics, Texture object mutation |
| DerivedDataCache | Backend-neutral request/result types, opaque immutable bytes, synchronous Get/Put, backend concurrency and atomic replacement | Texture schemas, recipes, PlatformData interpretation, object lifecycle, Cook policy, per-build eviction |
| AssetForgeBuiltins | Physical-source capture/decoding, canonical import values, provenance, transaction/save sequencing | DDC operations, Texture recipe implementation, object result application |
| Cooked runtime | Required cooked Texture metadata and bulk payload consumption through Engine serialization | DDC, TextureBuild, authored rebuild inputs, editor diagnostics |

## Design Decisions and Invariants

### PlatformData has one serialization protocol

`FTexturePlatformData`, `FTextureCubePlatformData`, and
`FVolumeTexturePlatformData` remain the schema owners. Engine serializes each
complete value through its existing `Serialize` method:

- DDC store uses `FCanonicalMemoryWriter` with
  `EArchivePurpose::DerivedDataPayload` and the explicit target context;
- DDC hit uses `FCanonicalMemoryReader` with the same purpose and target,
  requires archive end, then requires `PlatformData::IsValid()`;
- Cook serialization uses `EArchivePurpose::CookedPayload`, wraps the resulting
  bytes in the package BulkData field, and retains its existing alignment and
  external-placement policy;
- cooked runtime materialization uses the CookedPayload reader and never
  attempts a DDC fallback.

Archive purpose describes the storage boundary; it must not create a second
field layout. Existing TXPL headers, records, stable pixel formats, checksums,
limits, golden hashes, and target validation remain the compatibility baseline.
TextureBuild must not retain `Encode*PlatformValue` or
`Decode*PlatformValue` wrappers after the migration.

### Engine owns cache orchestration only in editor variants

`Engine.dmodule` gains `DerivedDataCache` as an
`OptionalPrivateDependency`. The Editor variant already enables DDC as a root,
so Engine links it there; the Game variant does not enable it and therefore
retains no DDC closure. Every DDC include, symbol reference, constant, and code
path in Engine is compiled behind `DURIN_WITH_EDITOR`.

An authored package load enters cache/build orchestration from the Texture
asset's existing uncooked `PostLoad`; it does not perform DDC work during
object/module unload. Import/reimport, property edits, async Texture2D
compilation, and Cook preparation call the same family-specific orchestration
entry points. Module unload remains relevant only to retiring TextureBuild
provider admission and draining already admitted pure-build calls.

In a non-editor build, an authored-only build request is unavailable by
construction. Cooked `PostLoad` verifies the required bulk field, clears
authored-only transient state, and lets Engine materialize valid PlatformData
from that field without loading TextureBuild or DerivedDataCache.

### The DDC facade remains opaque and request based

The low-level contract remains synchronous and family neutral:

```cpp
const DerivedData::FCacheGetResult Result =
    DerivedData::GetCache().Get(DerivedData::FCacheGetRequest{
        .Bucket = Bucket,
        .Key = Key,
        .MaximumValueBytes = MaximumBytes});
```

`FCachePutRequest` continues to borrow its byte span only for the synchronous
call, and `FCacheGetResult` continues to own immutable shared bytes. The facade
accessor is renamed from `GetDerivedDataCache()` to `GetCache()` in one atomic
repository migration; callers use the qualified namespace spelling, and no
compatibility alias remains. Status enums and diagnostics remain explicit.

The facade does not validate Texture bytes. Engine treats a hit that fails
PlatformData deserialization, archive-end validation, target validation, or
`IsValid()` as a corrupt disposable cache value and rebuilds from canonical
authored input. Misses and non-fatal cache read failures also fall back to a
local build while preserving a bounded diagnostic. A successful in-memory
build remains usable when best-effort Put fails, including storage exhaustion.

### Build identity is split by authority

TextureBuild owns recipe identity and reports it through immutable provider
descriptors copied while the module invocation gate is held:

- stable producer name;
- builder/algorithm version;
- TextureCube normalization/projection version where applicable.

Engine owns the cache-key encoding because it owns canonical inputs, settings,
target types, payload schema, and the DDC request. The key includes all output
dimensions: canonical imported-data identity, normalized settings, target
platform/profile, Engine payload and key schema versions, and the relevant
TextureBuild recipe versions. Source paths, timestamps, encoded-file
provenance, object paths, request serials, and cache policy never enter the key.

The established bucket strings remain `Textures/Objects`,
`TextureCube/Objects`, and `VolumeTexture/Objects`. Maximum value size and
best-effort store behavior remain unchanged. The current 4 GiB per-bucket
cleanup budget and 16-entry delete limit are removed deliberately: a Texture
build request neither scans nor evicts disk cache entries.

The caller-visible `FCacheTrimRequest`/`Trim` operation is removed in the same
facade migration, together with backend `TrimToBudget`, Build Function cleanup
configuration, ShaderBuild cleanup constants/calls, and their tests. DDC remains
disposable and may grow until explicitly cleared outside the request path. Disk
full, permission, and quota failures are bounded `Put` storage failures and do
not invalidate an already built in-memory product.

### Provider calls are pure and module-lifetime bounded

Provider requests retain normalized source, settings, target, and optional
cancellation observation. `bPersistDerivedData` is removed. Provider outputs
retain only detached PlatformData, recipe metrics, and TextureCube canonical
normalization output where applicable. `DerivedDataKey`, cache origin, and
cache persistence diagnostics are removed from provider products.

Engine runs the complete editor cache/build decision inside the bounded
single-provider visitor. The Engine stack frame may call DDC before or after
calling the provider, but TextureBuild code never sees those calls. Holding the
visitor across the request prevents a descriptor/build-version race with
TextureBuild unload; the provider must not retain inputs, callbacks, or outputs
after returning.

Cache hits do not call the expensive platform builder. Misses call the provider
once, validate the returned PlatformData in Engine, then encode and optionally
store it. Texture2D cancellation/supersession is checked before local build,
before serialization/store, and before result application; accepted terminal
observers still complete exactly once.

### TextureCube normalization precedes its cache lookup

Six-face input is already canonical. Panorama input is not: the current path
projects it into canonical faces before computing the established DDC identity.
To preserve that identity without hiding cache access inside TextureBuild, the
provider boundary becomes two pure phases where necessary:

1. normalize/validate the input and return Engine-owned canonical face data and
   authored panorama metadata;
2. build PlatformData from canonical faces only when Engine reports a cache
   miss or unusable hit.

Engine computes the cube key between those phases. A hit therefore avoids mip
generation and compression but may still require deterministic panorama
projection, matching current behavior. Stage 0 must prove the existing six-face
and panorama keys before this split is implemented.

### Engine applies one derived-data completion result

Family-specific Engine internals combine the pure provider output with DDC
metadata. The completion value carries PlatformData, derived-data key,
`CacheHit` or `Rebuilt` origin, cache query/store diagnostic, provider
descriptor, and metrics. It is not a TextureBuild product and does not escape
the Engine orchestration/completion boundary.

The DDC Put completes before successful result application when persistence is
requested, preserving the existing Texture2D `OnPersisting` phase and metrics.
Put remains best effort unless an existing caller explicitly requires store
success. There is no standalone Texture publication abstraction or
`Publish*Product` API after the migration. Synchronous paths directly apply valid
PlatformData; asynchronous Texture2D completion performs only the unavoidable
GameThread supersession check, state assignment, and render-resource invalidation
after detached work completes.

## Implementation Stages

### Stage 0: Freeze compatibility and ownership baseline

Dependencies: none.

- [x] Inventory every TextureBuild DDC/Build Framework include, registration,
  key builder, payload codec, provider field, production caller, and direct
  test caller.
- [x] Record golden Texture2D, six-face TextureCube, panorama TextureCube, and
  VolumeTexture key inputs/strings plus representative serialized payload
  hashes and sizes.
- [x] Characterize cold build, warm hit, corrupt/truncated hit, read failure,
  Put/storage-exhaustion failure, persistence disabled, cancellation,
  supersession,
  provider unavailable/ambiguous, and TextureBuild unload/reload behavior.
- [x] Confirm editor and Game module closures, current bucket paths, maximum
  value bounds, cleanup policy, Cook bytes, and cooked-load behavior.
- [x] Freeze the exact provider descriptor fields needed for Engine key
  construction, including cube projection identity, without exposing DDC
  types through provider APIs.

#### Acceptance Gate

- The byte, key, status, policy, lifecycle, and module-closure baseline is
  executable and distinguishes intended compatibility from accidental
  implementation detail.
- There is one selected Engine key input per family and no unresolved owner for
  a value that affects derived output.

### Stage 1: Refine the low-level DDC facade

Dependencies: Stage 0 complete.

- [x] Rename the process facade accessor to
  `Durin::DerivedData::GetCache()` and migrate Build Framework, ShaderBuild,
  tests, and documentation atomically.
- [x] Preserve `FCacheGetRequest` and `FCachePutRequest`, immutable get buffers,
  synchronous put borrowing, bucket locking, filesystem layout, bounds, and
  atomic replacement.
- [x] Remove caller-visible `FCacheTrimRequest`/`Trim`, migrate Build Framework
  and ShaderBuild off per-Put cleanup, and delete backend `TrimToBudget` plus
  Build Function cleanup configuration.
- [x] Add or tighten direct facade tests for request validation, hit/miss,
  corrupt opaque bytes, concurrent Get/Put, storage exhaustion/failure, and
  absence of request-path eviction without adding Texture vocabulary.
- [x] Prove DerivedDataCache still depends only on Core and exposes no backend
  path or asset/build policy.

#### Acceptance Gate

- All direct DDC, ShaderBuild, and existing generic Build Framework callers
  compile and pass through `DerivedData::GetCache().Get/Put` with no storage,
  key, byte, query, or store-policy change other than the selected removal of
  request-path cleanup.

### Stage 2: Establish Engine-owned texture cache primitives

Dependencies: Stage 1 complete.

- [x] Add DerivedDataCache as an optional private Engine dependency and guard
  every include and call site with `DURIN_WITH_EDITOR`.
- [x] Move the three canonical key encodings and family cache policy constants
  to Engine private ownership; consume recipe versions only from validated
  provider descriptors.
- [x] Implement family-typed PlatformData encode/decode helpers exclusively by
  calling `Serialize`, requiring archive end and `IsValid()`, and preserving
  output values on failure.
- [x] Implement small Engine-internal Get/validate and serialize/Put primitives
  with structured cache diagnostics and phase timings; do not
  create a public typeless asset-build framework.
- [x] Add direct Engine tests proving old golden keys and payload bytes survive
  the ownership move and that corrupt bytes become rebuildable misses.
- [x] Configure both Editor and Game variants to prove the optional dependency
  appears only in the Editor closure.

#### Acceptance Gate

- Engine can round-trip each PlatformData family through opaque DDC bytes with
  the same key and payload compatibility, while a Game Engine target contains
  no DDC include, import, or link edge.

### Stage 3: Migrate Texture2D orchestration

Dependencies: Stage 2 complete.

- [x] Reduce `ITexture2DBuildProvider` to descriptor plus pure PlatformData
  construction; remove persistence policy and cache metadata from its request
  and product.
- [x] Move Texture2D Get/decode/build/validate/encode/Put fallback into the
  Engine-owned compilation domain and synchronous orchestration entry point.
- [x] Preserve memory admission, cancellation checkpoints, `OnPersisting`,
  phase metrics, latest-wins request serials, completion mailbox behavior,
  GameThread completion application, and exact-once terminal callbacks.
- [x] Route uncooked `DTexture2D::PostLoad`, import/reimport, property edits,
  Scene import, and Cook preparation through the same Engine path.
- [x] Remove the Texture2D Build Function, local-input envelope, DDC codec, and
  registration while leaving the other two families operational until their
  stages complete.

#### Acceptance Gate

- Texture2D cold, warm, corrupt, no-persist, cancellation, supersession,
  provider-retirement, import, PostLoad, Cook, and resource-readiness tests pass
  with unchanged keys/payloads and no Texture2D DDC symbol in TextureBuild.

### Stage 4: Migrate TextureCube and VolumeTexture orchestration

Dependencies: Stage 3 complete.

- [x] Split TextureCube canonical normalization from platform construction,
  then place Engine DDC lookup between the pure phases.
- [x] Preserve six-face and LDR/HDR panorama authored metadata, canonical-face
  identity, projection versioning, cache-hit work avoidance, and result application.
- [x] Reduce `IVolumeTextureBuildProvider` to pure PlatformData construction
  and move its lazy-source-aware cache/build policy into Engine without forcing
  external authored bulk reads on a valid hit.
- [x] Move cube and volume Get/decode/build/validate/encode/Put,
  diagnostics, uncooked PostLoad, import/reimport, Cook preparation, and
  result application through their Engine-owned paths.
- [x] Remove their Build Functions, local-input envelopes, DDC codecs, and
  registrations after each family cutover.

#### Acceptance Gate

- Six-face, panorama, volume, lazy authored bulk, cold/warm/corrupt cache,
  import, PostLoad, Cook, thumbnail, and GPU resource tests pass with unchanged
  keys and payload bytes.
- No family performs expensive platform construction on a valid DDC hit.

### Stage 5: Remove TextureBuild's DDC boundary and qualify runtime closure

Dependencies: Stage 4 complete.

- [x] Delete Texture Build Function registry state, callbacks, definitions,
  local byte envelopes, cache codecs, Build Framework tests, and obsolete
  product cache fields.
- [x] Remove DerivedDataCache from `TextureBuild.dmodule`; audit public/private
  headers and binaries for DDC or Build Framework references.
- [x] Simplify TextureBuild startup/shutdown to atomic provider registration and
  reverse-order retirement, preserving the module-owned callback gate and
  draining admitted pure calls before code unload.
- [x] Prove cooked Texture2D, TextureCube, and VolumeTexture load required
  PlatformData solely from cooked bulk through Engine serialization and cannot
  query DDC or invoke a provider.
- [x] Build the default Editor target, run focused and affected native tests,
  run the Editor startup/shutdown smoke, and qualify the Game build/deployment
  closure using the repository build and testing workflows.
- [x] Update lasting architecture/module/build documents only after code and
  closure evidence passes, then run changed/all documentation, all-plan, and
  all-roadmap validation.

#### Acceptance Gate

- TextureBuild is a pure recipe/provider module with no DDC dependency or
  cache vocabulary.
- Editor Engine owns all Texture DDC operations, Game Engine owns none, and
  cooked runtime Texture loading succeeds without developer modules.
- Focused/affected tests, default Editor build, Editor smoke, Game closure, and
  documentation validators pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| DDC facade | Direct Get/Put validation, status, bounds, concurrency, atomicity, and storage-failure tests pass through `DerivedData::GetCache()`; no Trim API or request-path eviction remains |
| Key compatibility | Golden key bytes and strings for Texture2D, six-face/panorama TextureCube, and VolumeTexture are unchanged |
| Payload compatibility | DDC and Cook golden bytes/hashes round-trip only through PlatformData `Serialize`; corrupt/trailing/oversized values rebuild or fail at the correct boundary |
| Pure provider contract | Requests contain no persistence policy; products contain no key/origin/cache diagnostic; TextureBuild has no DDC or Build Framework references |
| Cache policy | Warm hit skips expensive build; miss/read failure/corruption rebuild; Put/storage failure is best effort; persistence-disabled builds do not write |
| Async Texture2D | Admission, priority, cancellation, supersession, metrics, completion pumping/application, and exact-once observers remain unchanged |
| TextureCube | Six-face and panorama normalization, canonical identity, projection version, cache behavior, result application, thumbnails, and Cook pass |
| VolumeTexture | Cache hits preserve lazy authored-source behavior; cold build, volume mips, result application, Cook, and runtime load pass |
| Lifecycle | Uncooked editor PostLoad owns DDC orchestration; module unload only retires providers; cooked PostLoad never reaches DDC/build code |
| Module closure | Editor Engine optionally links DDC; TextureBuild and Game closures exclude DDC as specified; cooked deployment excludes TextureBuild and developer codecs |
| Documentation | Changed/all document, all-plan, and all-roadmap validators pass with lasting ownership updated after implementation |

## Definition of Done

- TextureBuild receives normalized values and returns canonical/PlatformData
  values without importing or naming any DerivedDataCache type.
- Engine editor code is the sole Texture DDC client and uses explicit
  `FCache*Request` values through `DerivedData::GetCache()`.
- PlatformData `Serialize` is the only DDC/Cook payload codec and retains exact
  compatibility or an explicitly versioned, documented migration.
- Uncooked editor load/build workflows share one Engine cache pipeline;
  runtime loads only required cooked PlatformData.
- Existing Texture diagnostics, cancellation, completion application, Cook, module unload,
  and render-resource semantics remain qualified.
- TextureBuild's Build Functions and DDC dependency are removed, Editor/Game
  closure checks pass, and lasting documentation states the implemented split.

## Deferred Follow-ups

- Add configurable process-wide disk quotas, background/global garbage
  collection, age/LRU policy, cache statistics, or a user-facing Clear DDC
  command only when measured cache growth requires them. None belongs in an
  individual build request.
- Generalize the Engine-side pattern to other asset families only after each
  family proves that its runtime module owns the payload type/codec and can
  retain editor-only DDC dependencies without polluting Game closure.
- Reassess async DDC, remote backends, request coalescing, and shared cache
  policy only from measured workload requirements.
- Consider a family-neutral editor derived-data orchestrator only after at
  least two Engine-owned families exhibit the same complete lifecycle; this
  plan intentionally keeps typed Texture paths visible.
- Decide whether cooked PlatformData should be eagerly materialized at PostLoad
  in a separate performance/lifetime change; this plan guarantees its cooked
  presence and DDC independence, not eager memory residency.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Build System](../../../Development/Build/BuildSystem.md)
- [Texture Build Object Boundary Completion](TextureBuildObjectBoundaryCompletion.md)
- [Agent Build And Run](../../../Agents/BuildAndRun.md)
- [Agent Testing](../../../Agents/Testing.md)

## Related Code

- [`DerivedDataCache.h`](../../../../Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataCache.h)
- [`DerivedDataBuild.cpp`](../../../../Engine/Source/Developer/DerivedDataCache/Private/DerivedDataBuild.cpp)
- [`TextureBuild.dmodule`](../../../../Engine/Source/Developer/TextureBuild/TextureBuild.dmodule)
- [`TextureBuildModule.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp)
- [`TextureBuildOperations.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildOperations.cpp)
- [`TextureCubeBuildOperations.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuildOperations.cpp)
- [`VolumeTextureBuildOperations.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/VolumeTextureBuildOperations.cpp)
- [`Engine.dmodule`](../../../../Engine/Source/Runtime/Engine/Engine.dmodule)
- [`Texture2DBuildProvider.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/Texture2DBuildProvider.cpp)
- [`Texture2DCompilation.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilation.cpp)
- [`TextureCubeBuildProvider.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/TextureCubeBuildProvider.cpp)
- [`VolumeTextureBuildProvider.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureBuildProvider.cpp)
- [`TextureDerivedData.h`](../../../../Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h)
- [`TextureDerivedDataCache.h`](../../../../Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedDataCache.h)
- [`TextureDerivedDataKey.h`](../../../../Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedDataKey.h)
- [`Texture2D.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp)
- [`TextureCube.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp)
- [`VolumeTexture.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp)
- [`TextureBuildTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp)
- [`TextureDerivedDataTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureDerivedDataTests.cpp)
- [`TextureImportAndCacheTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureImportAndCacheTests.cpp)
- [`TextureCookTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureCookTests.cpp)
