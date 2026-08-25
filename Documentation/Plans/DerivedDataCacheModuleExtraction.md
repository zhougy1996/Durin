# Derived Data Cache Module Extraction Plan

Summary: Extract a backend-neutral `DerivedDataCache` module and move cache contracts and local filesystem storage out of `AssetBuildCore` without changing derived-data behavior.

Last reviewed: 2026-08-26

Status: Active
Completed:

## Current Status

Stages 0-2 are implemented. `DerivedDataCache` is a target-selected Developer
module with a public synchronous Bucket/Key/Get/Put/Trim contract over Core's
immutable `FSharedByteBuffer` and a private filesystem backend. The backend
preserves the historical two-character sharding and `.bin` layout, atomic
replacement, bounded reads, deterministic trim, dynamic test directory, and
contained-path checks. A process facade serializes get, put, and trim without
exposing locks or backend state.

`AssetBuildCore` now privately depends on DDC, no longer depends on `AssetCore`,
and retains the complete query/validate/build/validate/store policy machine.
`FDerivedDataObjectStore`, `FBuildCacheClient`, their duplicate statuses, and
the logical `CacheRoot` field are removed. Direct DDC tests pass 5/5 and
AssetBuildCore policy/host tests pass 8/8.

Stage 3 focused coverage passes for Texture (87), StaticMesh/collision (74),
skeletal/animation (34), TerrainHeightmap (11), TerrainWorld (15), AssetImport
(17), TerrainHeightmap Cook (1), and Texture Cook (1). The Editor all build,
hidden-window smoke, DurinGame build, module closure assertions, and deployment
exclusion checks pass. Stale renderer test calls were updated for the current
render-policy signatures; `VolumetricCloudVulkanTests` passes 1/1 and
`EditorGridVulkanTests` passes 7/7. Skeletal Scene lifecycle now passes 1/1
after owning a rendering-thread admission scope and cooking its Material through
the required DMAT descriptor path. `SceneImportVulkanTests` passes 1/1 after
refreshing two stable renderer golden hashes; its injected sampler-creation
failure remains an expected recovery assertion. The native aggregate builds and
executes all 82 registered targets, with these DDC qualification cases passing,
but its previous run remained red on `SkyBoxVulkanIntegrationTests`. That
access violation came from the test dereferencing transient TextureCube source
pixels that cache-hit publication intentionally omits; its reference colors now
come from a fixture projection independent of the cached asset, and the exact
Vulkan integration target passes 1/1. The native aggregate has not been rerun.
All changed/all documentation, all-plan, and all-roadmap validators pass. The
plan remains active until the external native aggregate runtime gate passes.

## Goal

- Establish `DerivedDataCache` as the sole owner of generic derived-data cache
  contracts, local cache lifecycle, filesystem layout, atomic persistence,
  status reporting, and bounded cleanup.
- Replace misleading `ObjectStore`, `Store`, and physical `CacheRoot` vocabulary
  on the DDC path with explicit `Cache`, `Backend`, `Bucket`, `Entry`, `Get`,
  `Put`, and `Trim` vocabulary.
- Make `AssetBuildCore` consume the cache through a private module dependency
  while remaining the only public cache/build orchestration path available to
  asset recipe modules.
- Preserve every current cache key, bucket path, value byte, hit/miss decision,
  build/store policy, failure classification, phase timing, disk budget, and
  disposable-data behavior.
- Leave a narrow contract that a later memory, shared, or remote backend can
  implement without moving Build definitions or asset payload schemas again.

## Scope

- Add `Engine/Source/Developer/DerivedDataCache` with its module descriptor,
  CMake target, API export surface, module lifetime, public synchronous cache
  types, and private filesystem backend.
- Introduce family-neutral cache bucket, key, get result, put result, and trim
  result contracts under `Durin::DerivedData`.
- Reuse Core's `FSharedByteBuffer` for immutable returned bytes rather than
  creating a second cache-owned byte container.
- Move the behavior currently implemented by `FDerivedDataObjectStore` into a
  private `FFileSystemCacheBackend`, preserving its physical layout and safety
  checks.
- Remove `FBuildCacheClient` and adapt `FBuildSession` directly to the public
  DDC contract while keeping Build policy and Build status mapping private to
  `AssetBuildCore`.
- Rename logical `CacheRoot` configuration to `CacheBucket` without changing
  the persisted relative bucket strings in this plan.
- Remove `AssetBuildCore`'s unused `AssetCore` dependency if the final include
  and link audit confirms that no remaining implementation requires it.
- Move physical cache tests from ObjectStore terminology to direct
  `DerivedDataCache` contract coverage and retain session-level policy tests in
  `AssetBuildCoreTests`.
- Update module graph assertions, Editor target selection, native-test metadata,
  deployment exclusions, and lasting module/data-lifecycle documentation.

## Non-Goals

- Renaming `AssetBuildCore`, `TextureBuild`, or `GeometryBuild`, or splitting
  Mesh and Terrain recipe modules.
- Moving `FBuildDefinition`, `FBuildSession`, `IBuildFunction`, function
  registration, Build Host, authoring coordination, or family recipes into
  `DerivedDataCache`.
- Moving `FBuildValue` wholesale into DDC. It remains a Build-level named value
  and may adopt `FSharedByteBuffer` internally to avoid copies.
- Making `FBuildSession` or DDC asynchronous, adding priorities, request owners,
  request merging, dependency graphs, workers, or cancellation inside the cache.
- Adding memory, multi-tier, shared, network, Zen-like, or remote backends;
  backend routing, promotion, authentication, and transport protocols are
  separate plans.
- Centralizing all family disk budgets, maximum payload sizes, or cleanup
  schedules. The first extraction preserves caller-supplied limits and timing.
- Changing Build function names or versions, canonical key encodings, payload
  schemas, value names, target facts, source capture, validation, publication,
  Cook output, or runtime loading.
- Renaming physical bucket directories such as `Textures/Objects`, changing the
  two-character key sharding, changing the `.bin` suffix, or migrating/deleting
  an existing DDC. These names are internal historical layout, not the new API.
- Moving shader caches, thumbnail disk caches, asset catalog/residency stores,
  authored bulk storage, cooked bulk storage, or unrelated `*Store` types.
- Exposing cache entry paths, filesystem backend instances, or private test
  hooks to recipe modules.

## Design Decisions and Invariants

### Developer module and dependency direction

`DerivedDataCache` is a Developer module because current game targets neither
query nor populate rebuildable DDC entries. Physical placement is an ownership
convention; target roots and dependencies remain authoritative. The required
dependency graph is:

```text
Core
  ^
  |
DerivedDataCache
  ^ private implementation dependency
  |
AssetBuildCore
  ^
  +-- TextureBuild
  +-- GeometryBuild
```

`DerivedDataCache` depends publicly only on `Core`. It must not depend on
`AssetCore`, `Engine`, `AssetBuildCore`, recipe modules, editor modules, RHI, or
offline codecs. `AssetBuildCore` declares DDC as a private dependency so its
public consumers do not acquire a supported raw-cache path transitively. Any
future direct DDC consumer must declare an explicit dependency.

`DurinEditor` selects `DerivedDataCache`; `DurinGame` and package-only roots do
not. Configuration-time closure and deployment checks name the module
explicitly rather than relying on its `Source/Developer` directory.

### Public cache vocabulary and minimum contract

The public API lives in `Durin::DerivedData` and uses the following concepts:

| Concept | Responsibility |
| --- | --- |
| `FCacheBucket` | Validated logical namespace made of canonical relative path segments; it is not an operating-system path |
| `FCacheKey` | Validated canonical lowercase 128-bit hexadecimal cache identity used by the current backend |
| `FCacheGetRequest` / `FCacheGetResult` | Bucket, key, maximum accepted bytes, immutable returned bytes, precise status, and diagnostic |
| `FCachePutRequest` / `FCachePutResult` | Bucket, key, immutable bytes, maximum accepted bytes, precise status, and diagnostic |
| `FCacheTrimRequest` / `FCacheTrimResult` | Bucket budget, maximum deletion count, before/after/deleted accounting, completion status, and diagnostic |
| `FDerivedDataCache` | Synchronous backend-neutral facade for `Get`, `Put`, and `Trim` |

The exact header split may follow repository conventions, but the first public
surface must not expose an `ObjectStore`, filesystem path, backend pointer, or
asset/Build type. `GetDerivedDataCache()` or the module interface returns the
process-owned facade; callers never construct the filesystem backend.

Get distinguishes at least hit, miss, invalid request, excessive value, and
storage failure. Put distinguishes stored, invalid request, excessive value,
and storage failure. A miss is not an error. Trim reports bounded partial
completion separately from get/put success. Diagnostics remain owned strings
and do not expose a path to domain callers.

### Immutable byte ownership

The DDC treats values as opaque bytes and uses Core's `FSharedByteBuffer` for a
successful get result. It does not own a second shared-vector implementation
and does not attach asset schema or Build value names to the bytes.

`FBuildValue` remains in `AssetBuildCore` because it is a named Build input and
output as well as a cached byte value. Its storage may change from a private
`shared_ptr<vector<byte>>` to `FSharedByteBuffer`, but its public name, content
identity, validity, bytes, and size behavior remain unchanged. `FBuildSession`
adds the expected Build value name after a cache hit and passes only opaque
bytes to DDC on put.

`FBuildKey` remains distinct from `FCacheKey`. The session performs an explicit
validated conversion at the boundary. Equal current encodings do not erase the
different ownership or guarantee that all future Build and cache keys evolve
together.

### Local filesystem backend

The private implementation is named `FFileSystemCacheBackend`; filenames use
`FileSystemCacheBackend.*`. Its terms describe implementation rather than a
generic storage abstraction:

- `Get`, `Put`, and `TrimToBudget`, not `Read`, `Write`, and
  `CleanupToBudget` on an ObjectStore;
- cache directory, bucket, entry, and entry path, not root/object terminology;
- entry status/result types remain at the public facade boundary rather than
  leaking backend-specific filesystem results.

For an existing bucket and key, the backend preserves the exact effective path
`DerivedDataCacheDir/<bucket>/<first-two-key-characters>/<key>.bin`. It retains
lexical and resolved containment checks, rejects symlink/path escapes, bounds
file sizes before allocation, uses Core atomic byte publication, ignores
noncanonical files during trim, orders candidates by oldest write time with a
deterministic path tie-breaker, and never deletes beyond the requested bound.

The backend resolves the currently configured DDC directory according to the
existing `FPaths` test/runtime contract; module initialization must not capture
a stale test root. Public callers receive no physical path API.

### Build orchestration remains in AssetBuildCore

`FBuildSession` retains this exact order:

```text
request validation
function lookup
cache get
cached value validation
local build
built value validation
cache put
bounded trim
```

DDC does not know whether a local build is permitted or whether persistence is
required. `FBuildPolicy::bQueryCache`, `bAllowLocalBuild`,
`bStoreBuildResult`, `bRequireStoreSuccess`, and `bReturnData` remain Build
policy. `AssetBuildCore` maps DDC statuses to existing `EBuildStatus`,
`EBuildFailurePhase`, diagnostics, store diagnostics, and phase timings.

`FBuildCacheClient`, `FBuildCachePolicy`, `EBuildCacheQueryStatus`, and their
ObjectStore pointer are removed rather than exported or renamed as a second
cache facade. Small private mapping functions are allowed where they keep the
session readable, but there is one public cache contract and one public Build
session contract.

Required and best-effort store behavior is unchanged: DDC always reports the
actual put result, and the Build layer decides whether failure invalidates the
complete in-memory product. A failed trim never retroactively changes a
successful put or Build result; it remains a separate diagnostic as today.

### Concurrency and lifetime

The facade is process-owned and callable from the current synchronous editor,
Cook, tool, and worker-thread paths. The extraction must not weaken concurrent
same-key get/put safety, atomic replacement, or bounded trim behavior. Stage 0
characterizes concurrent access; the backend may add private per-bucket or
equivalent synchronization only where required to state and satisfy that
contract. No lock, backend handle, mutable buffer, or callback escapes the
module.

Module shutdown closes new cache calls before destroying backend state and
waits for admitted calls if the implementation owns mutable process state.
If the first facade remains stateless over Core filesystem operations, that
choice and its safe unload behavior are documented and tested rather than
inventing an unnecessary worker lifetime.

### Failure and compatibility policy

This is a structural and vocabulary refactor. Existing key strings, bucket
strings, payload bytes, disk files, maximum-size rejections, cache hit/miss
semantics, required/best-effort policies, cleanup limits, and public Build
diagnostics remain compatible. No DDC migration or cache flush is required.

DDC validates storage facts only. It never interprets texture, mesh, skeletal,
animation, Terrain, material, or shader payload bytes. Family Build functions
continue to classify incompatible or corrupt payloads during cached-value
validation and decide whether authoritative inputs permit a rebuild.

## Current Foundations and Gaps

| Area | Foundation to preserve | Gap closed by this plan |
| --- | --- | --- |
| Physical cache | Canonical 128-bit keys, sharded `.bin` paths, containment checks, bounded reads, atomic writes, deterministic bounded trim | `FDerivedDataObjectStore` is private to the wrong module and named as a generic object database |
| Build session | One query/validate/build/validate/store flow with explicit policy, cancellation, timings, and diagnostics | Cache transport, physical backend, and Build policy mapping are co-located in one implementation file |
| Byte ownership | Immutable `FBuildValue` bytes with content identity; Core already provides `FSharedByteBuffer` | DDC has no independent opaque-byte result and BuildValue carries its own shared-vector mechanism |
| Module graph | Recipe modules cannot directly access the private cache client; game closure excludes authoring builders | There is no named cache module, dependency boundary, or closure assertion reusable by another producer |
| Tests | Direct physical-store safety tests and broad cold/warm/corrupt/store-policy recipe coverage | Tests use private ObjectStore headers and terminology instead of a supported cache contract |
| Documentation | Asset data lifecycle distinguishes disposable DDC from authored and cooked data | Lasting docs still describe an opaque ObjectStore inside AssetBuildCore and do not name cache ownership |

## Implementation Stages

### Stage 0: Freeze cache behavior and finalize the public seam

Dependencies: completed Asset Build Recipe Internal Consolidation plan and the
current passing asset Build/session baseline.

- [x] Inventory every production/test include, symbol, module dependency,
  target root, closure assertion, runtime load name, and physical-path
  assumption involving `FDerivedDataObjectStore`, `FBuildCacheClient`,
  `CacheRoot`, or the current DDC layout.
- [x] Record golden bucket strings, key grammar and length, sharding, suffix,
  maximum value limits, disk budgets, deletion bounds/order, hit/miss/error
  mapping, required/best-effort store results, and phase timing behavior for
  every registered Build function.
- [x] Characterize local backend behavior for missing entries, canonical and
  invalid keys/buckets, oversized files and puts, blocked roots, symlinks and
  containment, atomic same-key replacement, ignored noncanonical files,
  bounded trim, and concurrent same-key get/put/trim interactions.
- [x] Finalize the exact public `FCacheBucket`, `FCacheKey`, request/result,
  status, facade/module access, and thread/lifetime contracts from the selected
  decisions above; no physical path or backend type may enter a public header.
- [x] Confirm by include/link audit that `AssetBuildCore` has no real
  `AssetCore` dependency and that `DerivedDataCache` can depend on `Core` only.
- [x] Select the exact native-test ownership and target metadata for direct DDC
  tests without exposing private backend headers.

#### Acceptance Gate

- Every observable cache and Build-session behavior has a frozen expectation,
  the new public/private type map is unambiguous, and the final dependency graph
  contains no unresolved AssetCore, Engine, recipe, editor, RHI, or codec edge.
- Concurrent and module-lifetime behavior is either already characterized as
  sufficient or has one bounded implementation change selected before files
  move.

### Stage 1: Establish DerivedDataCache and the filesystem backend

Dependencies: Stage 0 complete.

- [x] Add `DerivedDataCache.dmodule`, module CMake registration, API macro,
  module interface/facade lifetime, public cache contracts, and the private
  `FFileSystemCacheBackend` under `Source/Developer/DerivedDataCache`.
- [x] Implement validated Bucket and Key construction, opaque shared-buffer
  get results, explicit get/put statuses, trim accounting, and stable
  diagnostics without asset or Build vocabulary.
- [x] Move the current path resolution, containment, size bounds, atomic write,
  and deterministic bounded cleanup mechanics into the backend; remove
  ObjectStore terminology from new production code.
- [x] Preserve the existing physical directory layout exactly and keep the
  configured DDC directory dynamically testable through the established Core
  path contract.
- [x] Rename and migrate `DerivedDataObjectStoreTests.cpp` to direct public DDC
  tests covering the Stage 0 matrix; inspect/corrupt expected test files only
  as a test fixture and do not add a public entry-path accessor.
- [x] Add focused module startup/shutdown/reload and concurrent-operation tests
  if the selected facade owns process state or synchronization.
- [x] Add module dependency-closure assertions proving that DDC contains Core
  and excludes AssetCore, Engine, AssetBuildCore, recipes, editor modules, RHI,
  and third-party codecs.

#### Acceptance Gate

- `DerivedDataCache` builds and its public-contract tests pass without any
  `AssetBuildCore`, Asset, Engine, Editor, RHI, or recipe dependency.
- The new facade reproduces the frozen filesystem, status, safety, atomicity,
  trim, concurrency, and lifetime behavior through public Cache vocabulary.

### Stage 2: Cut AssetBuildCore over to DerivedDataCache

Dependencies: Stage 1 complete and the new cache contract qualified.

- [x] Make `DerivedDataCache` a private `AssetBuildCore` dependency and add it
  explicitly to the required Editor/tool target roots and deployment audits.
- [x] Replace session-local `FDerivedDataObjectStore` and
  `FBuildCacheClient` construction with direct use of the process DDC facade.
- [x] Convert `FBuildKey` to `FCacheKey` explicitly at the session boundary and
  adapt successful opaque bytes to the existing named `FBuildValue` contract.
- [x] Rename `FBuildFunctionConfig::CacheRoot` to `CacheBucket` and update every
  recipe/test initializer while preserving the exact configured strings.
- [x] Retain Build-owned query/store policy, cache-hit validation, local-build
  fallback, required/best-effort put handling, cancellation checkpoints,
  cleanup triggering, result origins, failure phases, diagnostics, and
  nanosecond timings.
- [x] Remove `FBuildCacheClient`, its private policy/query types,
  `FDerivedDataObjectStore`, old source files, old private includes, test-only
  access, and all production `ObjectStore` terminology on this path.
- [x] Adopt `FSharedByteBuffer` inside `FBuildValue` only if needed for
  zero-copy cache hits; preserve its complete public behavior and content hash.
- [x] Remove `AssetBuildCore`'s `AssetCore` dependency after a final source and
  target-closure audit proves it unused.

#### Acceptance Gate

- `FBuildSession` is the only production cache path used by asset recipe
  modules, while its physical cache operations go exclusively through
  `DerivedDataCache`.
- Cold build, warm hit, query-only miss, corrupt cached value, query/store
  disablement, required/best-effort put failure, cancellation, cleanup, and
  diagnostics match the frozen baseline for all recipe families.
- Repository searches find no surviving `FDerivedDataObjectStore`,
  `FBuildCacheClient`, logical `CacheRoot` field, or DDC-path `ObjectStore`
  terminology; intentional unrelated Store types are explicitly out of scope.

### Stage 3: Qualify integration, deployment, and lasting ownership

Dependencies: Stage 2 complete.

- [x] Run focused `DerivedDataCache` and `AssetBuildCore` tests, then the
  affected Texture2D/TextureCube/VolumeTexture, StaticMesh/collision,
  skeletal/animation, TerrainHeightmap/TerrainWorld, import/reimport, Cook, and
  cache-corruption selections using the repository testing workflow.
- [x] Verify exact cache directories and bytes for representative texture,
  mesh, skeletal, animation, and Terrain entries without making those paths
  part of a production API.
- [ ] Build the default Editor target and complete native aggregate after
  focused tests pass; run the hidden-window Editor startup/shutdown smoke to
  qualify module load and unload ordering.
- [x] Build or otherwise qualify the configured Game closure and deployment
  audit, proving `DerivedDataCache`, `AssetBuildCore`, recipes, and offline
  codecs remain absent.
- [x] Audit module descriptors, public includes, generated binaries, and target
  dependency closures for the selected public/private direction and absence of
  accidental transitive raw-cache access.
- [x] Update Code Modules, Workspace Projects, Build System, and Asset Data
  Lifecycle only with implemented ownership, dependency, selection, and
  failure contracts; replace ObjectStore wording without documenting private
  file layout as public architecture.
- [x] Run changed/all documentation, all-plan, and all-roadmap validation, then
  record exact build/test/search evidence before completing the plan.

#### Acceptance Gate

- Focused and aggregate tests, default Editor build, Editor smoke, Game
  exclusion/deployment checks, boundary searches, and documentation validators
  pass with no key, byte, behavior, or closure regression.
- Lasting documentation names `DerivedDataCache` as the cache authority and
  `AssetBuildCore` as the Build orchestration authority without competing with
  this implementation plan.

## Validation Matrix

Follow [Agent Build And Run](../Agents/BuildAndRun.md) and
[Agent Testing](../Agents/Testing.md); select focused targets first and never
overlap build process trees.

| Concern | Required evidence |
| --- | --- |
| Module boundary | DDC publicly depends only on Core; AssetBuildCore privately depends on DDC; recipe modules retain no direct/transitive supported cache API |
| Public API | Canonical Bucket/Key validation and Get/Put/Trim results are usable without private headers, asset types, Build types, or physical paths |
| Filesystem safety | Traversal, absolute roots, symlinks, nonregular files, blocked directories, excessive values, and long paths retain bounded failure behavior |
| Atomicity and concurrency | Same-key puts publish complete values; gets observe complete hit or miss; bounded trim cannot escape its bucket or delete noncanonical files |
| Compatibility | Existing bucket strings, 128-bit keys, sharding, suffixes, payload bytes, maximum sizes, budgets, and cleanup ordering remain unchanged |
| Build policy | Query/build/store/return-data combinations, required versus best-effort puts, cancellation, validation fallback, timings, and diagnostics retain current results |
| Family integration | Texture, mesh, skeletal, animation, Terrain, import/reimport, and Cook cold/warm/corrupt paths pass without direct DDC access |
| Lifetime | Editor/tool initialization and shutdown admit and retire cache calls safely; reload behavior is deterministic where supported |
| Deployment | Editor and selected tools contain DDC; Game/package-only closures and deployed binaries contain no DDC, AssetBuildCore, recipes, or offline codecs |
| Documentation | Changed/all document validation, all-plan validation, all-roadmap validation, and accurate lasting ownership contracts pass |

## Definition of Done

- `DerivedDataCache` is a first-class Developer module with a narrow
  backend-neutral synchronous cache contract and a private filesystem backend.
- Public DDC code uses Cache/Bucket/Key/Value/Get/Put/Trim vocabulary; the
  extracted path contains no ObjectStore or ambiguous physical CacheRoot API.
- `FDerivedDataObjectStore`, `FBuildCacheClient`, and their private duplicate
  policy/status wrappers are removed.
- `AssetBuildCore` privately consumes DDC and continues to own all Build
  definition, function, policy, validation, fallback, diagnostics, timing, and
  host behavior.
- Recipe modules use only `FBuildSession`; DDC has no asset-family knowledge and
  no asset recipe obtains a raw-cache bypass through dependency visibility.
- DDC depends only on Core, AssetBuildCore no longer depends on AssetCore unless
  Stage 0 finds and records a concrete unavoidable edge, and the complete
  closure is enforced by configuration checks.
- Existing cache files remain readable in place with identical key, path,
  bytes, bounds, cleanup, and failure behavior; no migration or flush is needed.
- Direct DDC tests, session tests, all affected family/integration/Cook tests,
  default build, native aggregate, Editor smoke, Game exclusion checks, and
  documentation validators pass.
- Lasting ownership rules are updated, completion evidence is recorded, and the
  plan lifecycle is closed only after every acceptance gate passes.

## Deferred Follow-ups

- Decide whether family-neutral `FBuildDefinition`, `IBuildFunction`, function
  registry, and `FBuildSession` should eventually join DDC as a general Derived
  Data Build framework; this extraction deliberately leaves them stable.
- Add configurable backend graphs, memory front caches, shared/remote caches,
  promotion, request coalescing, statistics, maintenance scheduling, and cache
  verification only from measured requirements and separate plans.
- Evaluate direct shader, material, thumbnail, and other generated-artifact
  caches as explicit DDC consumers after this boundary is qualified; do not
  absorb their schemas or publication workflows into DDC.
- Centralize Bucket registration, quotas, maximum value sizes, and maintenance
  policy only when multiple independent consumers require process-wide
  governance.
- Reassess asynchronous cache requests, request ownership, cancellation, and
  priority when an actual backend cannot satisfy the synchronous local
  contract efficiently.
- Rename or split `AssetBuildCore`, `TextureBuild`, and `GeometryBuild` in a
  separate module-ownership plan after the DDC dependency is stable.
- Review `FAssetThumbnailObjectStore` as a possible
  `FAssetThumbnailDiskCache` rename or DDC consumer independently; unrelated
  catalog, residency, reference, and persistent stores retain their domain
  names.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace Projects](../Workspace/WorkspaceProjects.md)
- [Build System](../Development/Build/BuildSystem.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Asset Build Recipe Internal Consolidation Plan](Archive/2026-08/AssetBuildRecipeInternalConsolidation.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`DerivedDataCache.dmodule`](../../Engine/Source/Developer/DerivedDataCache/DerivedDataCache.dmodule)
- [`DerivedDataBuildTypes.h`](../../Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataBuildTypes.h)
- [`DerivedDataBuildFunction.h`](../../Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataBuildFunction.h)
- [`DerivedDataBuildSession.h`](../../Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataBuildSession.h)
- [`DerivedDataBuild.cpp`](../../Engine/Source/Developer/DerivedDataCache/Private/DerivedDataBuild.cpp)
- [`DerivedDataCache.h`](../../Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataCache.h)
- [`FileSystemCacheBackend.cpp`](../../Engine/Source/Developer/DerivedDataCache/Private/FileSystemCacheBackend.cpp)
- [`DerivedDataBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/DerivedDataBuildTests.cpp)
- [`DerivedDataCacheTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/DerivedDataCacheTests.cpp)
- [`Engine.dproject`](../../Engine/Engine.dproject)
- [`CMakeLists.txt`](../../CMakeLists.txt)
