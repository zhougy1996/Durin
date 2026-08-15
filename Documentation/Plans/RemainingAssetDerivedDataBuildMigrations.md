# Remaining Asset Derived Data Build Migrations Plan

Summary: Migrate TextureCube, SkeletalMesh, AnimationClip, and TerrainHeightmap production DDC paths to the local derived-data Build request model without changing their keys, payloads, Cook output, or publication ownership.

Last reviewed: 2026-08-16

Status: Completed
Completed: 2026-08-16

## Current Status

Implementation and validation are complete. The completed
[Local Derived Data Build Requests Plan](Archive/2026-08/LocalDerivedDataBuildRequests.md)
provides the synchronous `FBuildSession`, immutable definitions, function
registration, cache policy, cancellation, and structured output now used by
all six production families. TextureCube, SkeletalMesh, AnimationClip, and
TerrainHeightmap query, validate, build, and store only through registered
functions; no in-scope production code retains a direct ObjectStore or
`FBuildCacheClient` state machine. `Skeleton` remains the authored relationship
authority and contributes compatibility identity rather than becoming a recipe.

## Goal

Make the local Build request model the sole production DDC query, validation,
local-build, and store path for TextureCube, SkeletalMesh, AnimationClip, and
TerrainHeightmap while preserving:

- every existing canonical key byte and 32-character key;
- the TXPL, DSKM, DANM, and Terrain payload byte formats and cache roots;
- authored source provenance, hard Skeleton relationships, Cook companions,
  cooked runtime behavior, and reflected publication transactions;
- family-owned source decode, scene capture, asynchronous coordination,
  cancellation, coalescing, and GameThread publication boundaries; and
- current required versus best-effort persistence behavior unless Stage 0
  evidence proves an existing caller contract is different.

Completion requires production consumers, not registrations or tests alone.
Every in-scope family operation that touches DDC must construct an immutable
definition and execute it through `FBuildSession`; no migrated operation may
retain a parallel direct ObjectStore or manual `FBuildCacheClient` sequence.

## Scope

- One registered TextureBuild function for TextureCube platform payloads,
  covering six-face and equirectangular-panorama imports and cache-only authored
  loads.
- Two registered GeometryBuild functions for SkeletalMesh and AnimationClip,
  migrated together with their shared Skeleton compatibility contract and
  scene-import publication transaction.
- One registered GeometryBuild function for TerrainHeightmap, covering direct
  builds, non-persisting builds, cache-only loads, synchronous authored
  fallback, and the existing asynchronous/coalesced authored-load worker.
- Canonical local-input codecs, complete cached/built-value validation, typed
  adapters, status/diagnostic mapping, module startup/shutdown registration,
  and direct-linked test/tool fallback matching the existing StaticMesh and
  Texture2D convention.
- The smallest family-neutral `FBuildOutput` timing additions needed to replace
  Terrain's direct filesystem timing without exposing cache paths or stores.
- Focused identity, payload, cache, cancellation, module-lifetime, import,
  authored-load, Cook, cooked-runtime, and integration validation.
- Lasting Asset Data Lifecycle and Code Modules documentation after the
  migrations are implemented.

## Non-Goals

- Remote execution, worker processes, RPC, portable definitions, priorities,
  dependency graphs, transitive Builds, or an AssetBuildCore scheduler.
- Multiple output values, partial-value retrieval, cache-record redesign, or a
  new DDC backend. Each definition still returns one opaque value.
- Turning Skeleton into a Build function or combining SkeletalMesh and
  AnimationClip into one cache object or one key.
- Moving image decode, panorama source capture, scene parsing, Skeleton
  construction, terrain source decode, reflected-object mutation, package
  publication, GPU upload, or Cook publication into AssetBuildCore.
- Replacing StandardAssetImport's asynchronous task groups, Terrain request
  coalescing, subscriber bounds, generation checks, or GameThread continuations.
- Changing builder/schema/projection versions, cache roots, cleanup budgets,
  payload value bytes, authored `.dasset` data, DAST v4, DBLK, or Cook manifests.
- Migrating shaders, thumbnails, environment lighting, or unrelated derived
  products.
- Keeping test-only direct store functions merely as cache-seeding seams once
  production migration provides a session-backed way to exercise the same
  behavior.

## Design Decisions and Invariants

### Function identities and one-value contracts

The migrations use the existing local-only request vocabulary and add no new
executor abstraction:

| Function identity | Local input | Output value | Cache root |
| --- | --- | --- | --- |
| `Durin.TextureBuild.TextureCube`, version 1 | `TextureCubeBuildInput` | `TextureCubePayload` | `TextureCube/Objects` |
| `Durin.GeometryBuild.SkeletalMesh`, version 1 | `SkeletalMeshBuildInput` | `SkeletalPayload` | `SkeletalMesh/Objects` |
| `Durin.GeometryBuild.AnimationClip`, version 1 | `AnimationClipBuildInput` | `SkeletalPayload` | `AnimationClip/Objects` |
| `Durin.GeometryBuild.TerrainHeightmap`, version 1 | `TerrainHeightmapBuildInput` | `TerrainHeightmapPayload` | `TerrainHeightmap/Objects` |

Function identity versions are dispatch/lifetime facts only. Existing family
builder and payload versions remain in canonical key bytes, so registration
does not double-version or invalidate DDC.

Each function owns its validated root, expected value name, maximum byte size,
and cleanup policy. Cached and newly built bytes must be completely consumed by
the existing family codec and pass semantic validation before return, store, or
typed publication. AssetBuildCore never includes Texture, Skeletal, Animation,
Terrain, Engine object, import, or scene headers.

### TextureCube boundary

- The canonical key remains `FTextureCubeBuildKeyInput`, including source
  layout, exact face or panorama content hashes, face dimension, exposure,
  sRGB, projection version, platform, and profile.
- Source capture, image decoding, six-face ordering, and panorama projection
  remain TextureBuild/StandardAssetImport normalization work. The immutable
  local input contains the six normalized projected faces plus the settings
  required for mip generation and compression; it contains no paths, package
  pointers, callbacks, or reflected objects.
- A valid cache hit skips `BuildCubePlatformData`, mip generation, compression,
  TXPL encoding, and store. Panorama projection may already have occurred as
  source normalization before the session; the plan does not make source
  translation lazy.
- The typed adapter retains normalized `FTextureCubeSourceData` for authored
  publication while decoding only the opaque platform payload returned by the
  session. Cache-only post-load definitions omit local input and never invoke
  source work.
- Existing TextureCube writes are transactional from the caller's perspective,
  so build-capable import requests require store success. The function adopts
  the existing intended 4 GiB cleanup budget and 16-object cleanup limit now
  stranded as unused Engine-local constants; the duplicate Runtime Engine
  ObjectStore helper and constants are removed.

### SkeletalMesh and AnimationClip boundary

- SkeletalMesh and AnimationClip remain separate definitions and cache roots,
  but they migrate in one stage because both depend on the same captured scene
  closure, stable output identity, and exact Skeleton compatibility identity.
- The canonical keys remain `FSkeletalMeshBuildKeyInput` and
  `FAnimationClipBuildKeyInput`. Provider identity/version, source closure,
  settings, provider state, payload input fingerprint, stable output identity,
  Skeleton compatibility identity, platform, and profile remain byte-for-byte
  unchanged.
- Scene parsing and construction of detached mesh/track payloads remain in
  StandardAssetImport. The local input encodes the detached payload plus the
  exact validation context: Skeleton bone count and compatibility identity,
  and, for SkeletalMesh, material-slot count and mesh-node bind transform.
- The function validates the complete local input, reconstructs the typed
  candidate, emits the existing DSKM or DANM bytes, and validates cached bytes
  against the definition's target facts. The typed adapter restores
  `MeshNodeBindTransform` or `ClipName` from request-owned metadata rather than
  adding it to cached payload bytes.
- A valid hit avoids function execution and payload re-encoding, but does not
  claim to avoid the upstream scene import that produced the detached candidate.
- Scene-import build products retain their current best-effort DDC persistence:
  a valid local product may publish with a visible store diagnostic. Explicit
  cache-seeding helpers, if any production caller survives Stage 0, use required
  store policy; otherwise the public `StoreSkeletalMeshDerivedData` and
  `StoreAnimationClipDerivedData` seams are removed with their test-only callers.
- Cache-only post-load through `ISkeletalDerivedDataFeature` uses definitions
  with target facts and no local inputs. A corrupt, incompatible, wrong-
  Skeleton, or trailing-byte value fails without partial asset mutation.

### TerrainHeightmap boundary

- The canonical key remains `FTerrainHeightmapBuildKeyInput`, including exact
  source content hash, decoder identity/version, source format/profile,
  builder/schema version, platform, and profile.
- The local input is a bounded canonical encoding of width, height, normalized
  `uint16` samples, and decoder/profile facts. Source path, file metadata,
  reflected asset state, task handles, coalescing keys, and publication
  callbacks remain outside the definition.
- `bPersistDerivedData == true` maps to the ordinary query/build/store request;
  `false` maps to local-build-only policy with cache query and store disabled,
  preserving the current explicit non-persisting path.
- Authored post-load preserves its warm-path ordering: it first performs a
  cache-only session request before source capture. After a miss or invalid hit,
  the family worker captures and decodes source, then issues a build-capable
  session request with cache query disabled so the fallback does not perform a
  redundant second query. This is two typed requests owned by the Terrain
  coordinator, but neither request manually touches DDC.
- The worker adapts its `FTaskCancellationToken` to
  `FBuildCancellationToken`. AssetBuildCore remains synchronous and executes on
  that existing worker; StandardAssetImport continues to own coalescing,
  admission, subscriber state, generation checks, cancellation of unshared
  work, and deferred GameThread publication.
- Terrain's direct object-path probe/read/decode timing is retired. The session
  records bounded family-neutral phase durations in `FBuildOutput` for cache
  query, cached validation, local build, built validation, and store. Terrain
  maps those facts into its diagnostics without exposing a physical DDC path.
  Timing is diagnostic only and never changes success or cache policy.

### Registration and module lifetime

- TextureBuild owns Texture2D and TextureCube registrations as one startup
  transaction. GeometryBuild owns StaticMesh render/collision, SkeletalMesh,
  AnimationClip, and TerrainHeightmap registrations as one startup transaction.
- Startup rolls back registrations acquired in the current transaction if any
  identity fails. Shutdown prevents new lookup and resets all registrations
  under the existing module callback gate before the owning module unloads.
- Registration locks protect only token state. No registry lock is held across
  codecs, cache I/O, family code, task cancellation, diagnostics, or teardown.
- Direct-linked tests/tools that intentionally omit module startup may lazily
  establish the same process-resident registrations. Module startup must adopt
  or replace that state without duplicate identities, and module shutdown must
  reset every family registration it owns.

### Failure, publication, and compatibility policy

- Invalid definitions, key/input disagreement, missing function identity, and
  wrong target facts fail as request/lookup errors, never as cache misses.
- Missing, corrupt, incompatible, oversized, wrong-value, semantically invalid,
  or trailing-byte cached data is rebuildable only when authoritative local
  input is present and policy allows local build. Cache-only callers receive a
  structured failure without source work.
- Required store failure prevents typed publication. Best-effort store failure
  preserves a complete local value and surfaces `StoreDiagnostic`; adapters do
  not erase it when mapping to existing family results.
- Cancellation before query, before local execution, at family checkpoints,
  and before store returns `Canceled`. A completed result is still only a
  detached product; the session never mutates an asset or schedules publication.
- DDC keys, cache roots, encoded bytes, Cook companions, logical payload IDs,
  hard Skeleton dependencies, and cooked runtime decoding remain compatible
  with assets and cache objects produced before this migration.

## Current Foundations and Gaps

| Family | Foundation to preserve | Gap closed by this plan |
| --- | --- | --- |
| AssetBuildCore | Synchronous session, immutable definition, private registry, policy, cancellation, value validation, module gates | Phase timings required by Terrain; additional production registrations and shared lifecycle coverage |
| TextureCube | Deterministic cube key, normalized six-face/panorama paths, projection, mip/compression builder, TXPL codec, detached product, authored load, Cook | Direct ObjectStore read/write, no registered function, no common status/policy path, unused cleanup ownership in Engine |
| SkeletalMesh | Stable scene-output and Skeleton compatibility identity, detached payload, DSKM codec, module post-load feature, Cook | Manual query/store helpers, build path does not query, validation and persistence policy are outside a session |
| AnimationClip | Shared scene closure/Skeleton identity, detached tracks, DANM codec, module post-load feature, Cook | Same manual cache sequencing as SkeletalMesh and no registered function |
| TerrainHeightmap | Deterministic key and payload, normalized samples, direct and non-persisting builds, cache-first authored fallback, bounded async coalescing/publication, Cook | Direct path probe/read/decode, manual store, cancellation not carried through store, cache policy split across callers |

## Implementation Stages

### Stage 0: Freeze Remaining-Family Contracts

Dependencies: Completed Local Derived Data Build Requests plan.

- [x] Inventory every production and test call that builds a key, queries,
  validates, builds, stores, cleans, times, publishes, cooks, or loads the four
  payload families; classify direct store helpers as production or test-only.
- [x] Record golden key-input bytes, key strings, value names, roots, maximum
  sizes, cleanup settings, builder/schema/projection versions, and
  representative TXPL/DSKM/DANM/Terrain payload hashes.
- [x] Characterize cold build, warm hit, cache-only miss, corrupt and
  incompatible hit, source-unavailable fallback, query-disabled build,
  non-persisting Terrain build, required/best-effort write failure,
  cancellation, module retirement, and cooked load.
- [x] Freeze the function identities, local-input layouts, target facts, status
  mappings, and exact registration owner names from this plan in focused tests.
- [x] Verify TextureCube import retains normalized source independently of the
  opaque platform payload, and verify skeletal scene publication can consume a
  cache hit without changing transaction membership or Skeleton relationships.
- [x] Confirm no family requires multiple named outputs, lazy input callbacks,
  a generic scheduler, or a dependency graph; record a plan decision before
  implementation if evidence contradicts this assumption.

#### Acceptance Gate

- Golden key and payload fixtures exist for all four outputs before behavior
  changes, and current persistence/failure behavior is evidence-backed.
- Every direct DDC call site and publication boundary has one selected migrated
  owner; no unresolved format, identity, policy, thread, cancellation, or
  lifecycle decision remains for Stage 1.

### Stage 1: Add Shared Timing and Migrate TextureCube

Dependencies: Stage 0 complete.

- [x] Add session-owned phase durations to `FBuildOutput`, populate them around
  existing state-machine phases, and cover hit, miss, build, validation, store,
  cancellation, and failure without changing policy or status semantics.
- [x] Add the canonical TextureCube local-input codec and registered function;
  validate face count/order/layout, dimensions, channel data, sRGB and target
  facts, TXPL value name, complete decode, consistency, and maximum size.
- [x] Route six-face and both panorama build overloads through one typed
  definition/session adapter while preserving normalized source data and
  publication context outside the function.
- [x] Route `LoadTextureCubeDerivedData` and authored post-load cache queries
  through a cache-only definition and preserve missing/incompatible/corrupt
  status mapping and source fallback.
- [x] Replace direct writes and reads in TextureCube operations with session
  policy; configure root, required store, budget, and cleanup through the
  function.
- [x] Register TextureCube beside Texture2D under TextureBuild startup/shutdown,
  including atomic rollback and direct-linked lazy registration.
- [x] Remove the unused Runtime Engine TextureCube ObjectStore helper and
  cleanup constants after repository searches prove there is no runtime DDC
  owner.
- [x] Extend TextureCube tests for six-face/panorama cold and warm requests,
  build avoidance, query-only load, corrupt rebuild, required-store failure,
  registration retirement/reload, unchanged source publication, and exact key
  and TXPL bytes.

#### Acceptance Gate

- Every production TextureCube DDC operation uses `FBuildSession`; no
  TextureCube operation or Runtime Engine asset implementation constructs an
  ObjectStore or manually reads/writes DDC.
- A valid hit performs no mip generation, compression, TXPL encoding, or store;
  cache-only authored load performs no source capture.
- Golden identities and payloads plus TextureCube import, authored-load, Cook,
  cooked-runtime, rendering-resource, and editor regressions pass.

### Stage 2: Migrate SkeletalMesh and AnimationClip Together

Dependencies: Stage 1 complete; the shared timing addition is qualified by a
production family.

- [x] Add bounded canonical local-input codecs and registered functions for
  SkeletalMesh and AnimationClip, with shared helpers only for genuinely
  identical Skeleton/context and codec behavior.
- [x] Validate Skeleton compatibility identity, bone count, material-slot
  bounds, stable output identity, payload fingerprint, target facts, complete
  DSKM/DANM decode, and semantic relationships on cached and built values.
- [x] Adapt `BuildSkeletalMeshProduct` and `BuildAnimationClipProduct` to build
  definitions, execute sessions, reconstruct detached typed products, retain
  request-owned bind transform/clip name, and preserve best-effort store
  diagnostics.
- [x] Adapt both `Load*DerivedData` paths and
  `ISkeletalDerivedDataFeature` to cache-only sessions with no reflected asset
  mutation inside GeometryBuild.
- [x] Remove manual shared query/store/ObjectStore helpers and remove the public
  `Store*DerivedData` APIs if Stage 0 confirms they have no production caller;
  rewrite cache tests through production/session-backed seams.
- [x] Register both functions atomically beside StaticMesh functions in
  GeometryBuild and qualify duplicate rejection, partial-start rollback,
  active-call retirement, shutdown, and reload.
- [x] Extend scene import and skeletal asset tests for mixed output sets, cold
  and warm values, per-output cache corruption, wrong Skeleton identity,
  source-unavailable cache-only load, best-effort store failure, no partial
  publication, and unchanged hard dependencies.

#### Acceptance Gate

- SkeletalMesh and AnimationClip production build/load paths use the session,
  and no migrated code manually sequences cache query/store or exposes a
  test-only direct store API.
- One output failure cannot publish an incompatible or partial heterogeneous
  scene-import result; Skeleton compatibility and transaction membership are
  unchanged on both cache hits and local builds.
- Golden key/payload bytes and scene import, reimport, skeletal authored load,
  animation playback, Cook, cooked-runtime, rendering, and editor tests pass.

### Stage 3: Migrate TerrainHeightmap and Its Authored Coordinator

Dependencies: Stage 2 complete; GeometryBuild registration aggregation is in
place.

- [x] Add the canonical normalized-sample local-input codec and registered
  TerrainHeightmap function with dimension/sample bounds, decoder/profile,
  target, complete payload decode, semantic validation, and cancellation
  checkpoints.
- [x] Adapt `BuildTerrainHeightmap` to session policies for ordinary persisted
  builds and explicit query-disabled/store-disabled local builds; reconstruct
  the shared immutable payload from the returned value.
- [x] Route `LoadTerrainHeightmapDerivedData` through a cache-only session and
  replace physical path/read/decode timing with common phase timing/status
  mapping.
- [x] Adapt synchronous authored fallback and `BuildTerrainLoadResult` so the
  existing worker performs a cache-only request first and, only after source
  capture/decode, a build request with cache query disabled.
- [x] Carry the worker cancellation predicate into both session requests and
  preserve coalescing, two-work admission, subscriber bounds, generation
  checks, unshared-work cancellation, completion pumping, and deferred
  GameThread publication.
- [x] Remove TerrainHeightmap direct ObjectStore and `FBuildCacheClient` access;
  configure its root, maximum size, required store, cleanup behavior, and
  diagnostics through the function/session boundary.
- [x] Register TerrainHeightmap atomically with the other GeometryBuild
  functions and extend tests for cold/warm/coalesced loads, one query per
  phase, corrupt rebuild, source loss, non-persisting mode, cancellation before
  build/during validation/before store, supersession, shutdown, and unchanged
  publication revision.

#### Acceptance Gate

- Direct, synchronous authored, and asynchronous authored TerrainHeightmap DDC
  operations use `FBuildSession`; no Terrain operation probes object paths or
  reads/writes the ObjectStore directly.
- Warm authored load performs no source capture or payload build; fallback
  performs one cache query and no redundant query after source decode.
- Existing async ownership and publication remain outside AssetBuildCore, and
  golden key/payload, authoring, cancellation, coalescing, Cook, cooked-runtime,
  Terrain rendering, picking, and editor tests pass.

### Stage 4: Integrate, Document, and Complete

Dependencies: Stages 1-3 complete.

- [x] Search production and tests for direct ObjectStore/BuildCache access in
  all in-scope families, duplicate manual cache state machines, obsolete direct
  store APIs, and stale statements that only StaticMesh/Texture2D use sessions.
- [x] Qualify TextureBuild and GeometryBuild registration startup, atomic
  rollback, duplicate rejection, direct-linked lazy startup, owner-gate
  retirement, active-call drain, reload, and final capture destruction.
- [x] Measure cold, warm, query-only, corrupt-rebuild, and Terrain coalesced
  paths sufficiently to prove one intended query, no local function execution
  on valid hits, no duplicate store, bounded input/output copies, and no
  material warm-path regression.
- [x] Update Asset Data Lifecycle, Code Modules, and any directly affected
  TextureCube/Skeletal/Terrain contract documentation with the implemented
  function, ownership, thread, timing, cancellation, and failure boundaries.
- [x] Run focused AssetBuildCore, TextureCube, Skeletal, SceneImport, Terrain,
  import, Cook, module-lifecycle, and cooked-runtime tests, then the default
  `all` build and complete native aggregate because the shared modules and
  registration lifecycle changed.
- [x] Run the hidden-window editor startup/shutdown smoke to qualify module and
  authored-service lifetimes, then run changed/all documentation and all-plan/
  all-roadmap validation.
- [x] Record validation evidence, update lasting documentation, and mark this
  plan completed only after every family acceptance gate passes.

#### Acceptance Gate

- StaticMesh, Texture2D, TextureCube, SkeletalMesh, AnimationClip, and
  TerrainHeightmap are real production consumers of the same local request
  model; only explicitly deferred unrelated families retain direct DDC paths.
- Keys, roots, payload bytes, authored publication, Cook output, and cooked
  runtime behavior remain compatible, while valid hits avoid family-local
  Build execution.
- Focused and aggregate tests, default build, editor smoke, repository
  searches, and all required document validators pass with evidence recorded.

## Validation Matrix

Follow [Agent Build And Run](../Agents/BuildAndRun.md) and
[Agent Testing](../Agents/Testing.md); select focused targets first and never
overlap build process trees.

| Concern | Required evidence |
| --- | --- |
| Shared output | Phase timings are populated only for executed phases; status, policy, cancellation, and failure mapping remain backward compatible in AssetBuildCoreTests |
| TextureCube identity | Six-face and panorama key bytes/strings, face order, projection/settings facts, TXPL bytes, root, size, and cleanup policy are unchanged |
| TextureCube behavior | Cold/warm/query-only/corrupt/write-failure paths; six-face and both panorama pixel types; import/reimport, authored load, Cook, cooked load, render resource, and editor flows |
| Skeletal identity | DSKM/DANM keys and bytes preserve provider, closure, settings, state, fingerprint, stable output, Skeleton compatibility, platform, and profile facts |
| Skeletal behavior | Cold/warm/query-only/corrupt/wrong-Skeleton/store-failure paths; heterogeneous scene import/reimport, hard relationships, no partial publication, Cook/runtime/playback/render/editor flows |
| Terrain identity | Key bytes/string, normalized sample ordering, dimensions, decoder/profile, payload bytes, root, maximum size, and non-persisting policy are unchanged |
| Terrain behavior | Cold/warm/cache-only/corrupt/source-loss/non-persisting paths; synchronous fallback, async coalescing, cancellation/supersession, revision publication, Cook/runtime/render/picking/editor flows |
| Module lifetime | Atomic registration, rollback, duplicate identity, lazy direct-link, lookup during retirement, active-call lease, reset/reload, and capture destruction for both modules |
| Performance | One intended query per path, no local function on valid hit, no duplicate store, bounded canonical input/output copies, and no material warm-load regression |
| Integration | AssetBuildCoreTests, TextureTests/TextureCube coverage, SkeletalAssetTests, SceneImportTests, TerrainHeightmapTests and focused Cook/runtime/module targets; default `all`; complete native tests; hidden-window editor smoke |
| Documentation | Changed/all document validation, all-plan validation, all-roadmap validation, and lasting contract updates |

## Completion Evidence

Completed on 2026-08-16 with the following repository evidence:

- production searches found no direct `FDerivedDataObjectStore`,
  `FBuildCacheClient`, or physical object-path access in the four migrated
  family operations; the obsolete skeletal store APIs and Runtime TextureCube
  store helper were removed;
- golden key/payload fixtures remained unchanged in `TextureTests`,
  `SkeletalAssetTests`, and `TerrainHeightmapTests`;
- focused tests passed: `AssetBuildCoreTests`, `TextureTests`,
  `SkeletalAssetTests`, `SceneImportTests`, `TerrainHeightmapTests`,
  `TerrainHeightmapCookTests`, `TextureCookIntegrationTests`,
  `SkeletalMeshEditorTests`, `TerrainRenderPrimitiveTests`,
  `TerrainRenderVulkanTests`, and `SkeletalMeshRenderResourcesVulkanTests`;
- the default `all` build and complete `test all` native aggregate passed;
- `DurinEditor` completed a hidden-window eight-tick startup/shutdown smoke;
- changed/all documentation, all-plan, and all-roadmap validators passed.

The implemented functions preserve their original key codecs, cache roots,
TXPL/DSKM/DANM/Terrain value bytes, publication ownership, Cook companions,
and cooked-runtime consumers. Session phase durations replace Terrain's direct
filesystem timing, and the authored Terrain fallback issues a cache-only
request followed by a query-disabled build only after source decode.

## Definition of Done

- Four new production functions are registered with the selected identities,
  roots, value names, bounds, cleanup settings, and module lifetime contracts.
- TextureCube, SkeletalMesh, AnimationClip, and TerrainHeightmap production DDC
  query/build/validate/store paths all execute through `FBuildSession`.
- No in-scope operation directly constructs `FDerivedDataObjectStore`, invokes
  `FBuildCacheClient`, probes a cache object path, or retains a parallel manual
  cache state machine.
- Cache hits skip the family-local platform/payload Build work claimed by this
  plan; cache-only loads never invoke source capture or local execution.
- Required/best-effort stores, cancellation, async ownership, coalescing,
  detached products, GameThread publication, and Skeleton relationships match
  the selected contracts and are covered by failure tests.
- Canonical key bytes, key strings, roots, payload bytes and schemas, authored
  data, Cook companions/manifests, and cooked runtime loading remain compatible.
- Focused tests, default build, full native aggregate, hidden-window editor
  smoke, performance/search checks, and documentation lifecycle validators pass.
- Lasting Runtime/Workspace documentation owns the implemented behavior and
  this plan records evidence before completion.

## Deferred Follow-ups

- Consider making direct public `FBuildCacheClient` use private only after a
  repository-wide inventory proves no intended non-asset consumer remains.
- Consider lazy local-input production only if measured source normalization
  cost on warm TextureCube/Terrain paths justifies a generic contract with a
  real second consumer.
- Consider multiple named values only when a production recipe needs partial
  retrieval or independently addressable payloads.
- Consider in-flight key coalescing in AssetBuildCore only if at least two
  families need semantics that cannot remain in their existing coordinators.
- Remote execution, priorities, dependency graphs, and portable definitions
  require a separate plan and measured production need.

## Related Documentation

- [Local Derived Data Build Requests Plan](Archive/2026-08/LocalDerivedDataBuildRequests.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Cube Textures](../Runtime/Rendering/CubeTextures.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Terrain Editing Architecture](../Editor/Architecture/TerrainEditing.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`BuildTypes.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildTypes.h)
- [`BuildFunction.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildFunction.h)
- [`BuildSession.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildSession.h)
- [`AssetBuildCore.cpp`](../../Engine/Source/Developer/AssetBuildCore/Private/AssetBuildCore.cpp)
- [`TextureBuildModule.cpp`](../../Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp)
- [`TextureCubeBuildOperations.h`](../../Engine/Source/Developer/TextureBuild/Public/Texture/TextureCubeBuildOperations.h)
- [`TextureCubeBuildOperations.cpp`](../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuildOperations.cpp)
- [`TextureCubeDerivedData.h`](../../Engine/Source/Developer/TextureBuild/Public/Texture/TextureCubeDerivedData.h)
- [`TextureCubeSourceTranslation.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/TextureCubeSourceTranslation.cpp)
- [`TextureCubePostLoadPolicy.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/TextureCubePostLoadPolicy.cpp)
- [`GeometryBuildModule.cpp`](../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildModule.cpp)
- [`SkeletalBuildOperations.h`](../../Engine/Source/Developer/GeometryBuild/Public/Skeletal/SkeletalBuildOperations.h)
- [`SkeletalBuildOperations.cpp`](../../Engine/Source/Developer/GeometryBuild/Private/Skeletal/SkeletalBuildOperations.cpp)
- [`SceneImport.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp)
- [`TerrainHeightmapBuildOperations.h`](../../Engine/Source/Developer/GeometryBuild/Public/Terrain/TerrainHeightmapBuildOperations.h)
- [`TerrainHeightmapBuildOperations.cpp`](../../Engine/Source/Developer/GeometryBuild/Private/Terrain/TerrainHeightmapBuildOperations.cpp)
- [`TerrainHeightmapAuthoringPolicy.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp)
- [`AssetBuildCoreTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/AssetBuildCoreTests.cpp)
- [`TextureCubeTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp)
- [`SkeletalAssetTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp)
- [`SceneImportTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp)
- [`TerrainHeightmapTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp)
