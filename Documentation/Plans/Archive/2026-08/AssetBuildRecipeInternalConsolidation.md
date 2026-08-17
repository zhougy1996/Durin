# Asset Build Recipe Internal Consolidation Plan

Summary: Consolidate migrated asset Build recipes behind clear internal seams, remove public cache bypasses, and reduce repeated validation and registration mechanics without creating a DerivedDataCache module.

Last reviewed: 2026-08-16

Status: Archived
Completed: 2026-08-16

## Current Status

The completed
[Remaining Asset Derived Data Build Migrations Plan](RemainingAssetDerivedDataBuildMigrations.md)
made `FBuildSession` the production query, validation, local-build, and store
path for StaticMesh, Texture2D, TextureCube, SkeletalMesh, AnimationClip, and
TerrainHeightmap. The architecture boundary is sound, but the migration left
recipe definition construction, canonical local-input codecs, function
callbacks, registration state, typed adapters, and some publication work
co-located in large `*BuildOperations.cpp` files. StaticMesh is approximately
one thousand lines, and the other migrated operation files range from roughly
four hundred to five hundred and fifty lines.

Implementation and validation are complete. AssetBuildCore's ObjectStore cache
adapter is private and `FBuildSession` is the sole public Build cache path.
TextureBuild and GeometryBuild now separate module-private local-input schemas,
Build-function execution, typed operations, and registration transactions;
skeletal key encoding also has its own source owner. TextureBuild registers two
functions atomically and GeometryBuild registers five, with current-attempt
rollback and reverse-order shutdown. Production boundary searches find no raw
cache access or family registration state outside the selected owners.

A post-completion simplification pass removed GeometryBuild's empty Build Host
service contribution, derives local-input availability directly from the
definition's input collection, and keeps the physical cache policy private to
AssetBuildCore. TextureBuild remains the only asynchronous Build Host service;
GeometryBuild owns function registration only. Texture and Geometry local-input
schemas now share Core's canonical derived-data primitive reader/writer instead
of carrying duplicate byte helpers, while their schema construction and
validation remain module-private.

Focused AssetBuildCore, Texture, Texture Cook, StaticMesh, collision
qualification, skeletal, SceneImport, Terrain, and Terrain Cook tests pass, as
do the default `all` build, complete native aggregate, and hidden-window editor
smoke. Lasting Asset Data Lifecycle and Code Modules contracts record the
implemented public/private and registration ownership.

Stage 2 decision: no test-only API was added for private local-input codecs or
registration tokens. Existing golden inputs, malformed source/payload cases,
cache corruption, direct-linked recipe calls, import/reimport, and Cook tests
exercise the extracted implementation through production adapters; exposing a
new seam solely to call private helpers would weaken the boundary this plan is
establishing.

## Goal

Make the migrated local Build implementation easier to understand and harder
to bypass while preserving every externally observable asset result and cache
contract. After completion:

- a reader can locate public typed request/result adaptation, canonical local
  input encoding, family Build-function execution, and module registration
  without tracing one mixed operation file;
- recipe modules cannot construct or invoke the AssetBuildCore cache client
  through a public header;
- repeated strict target-fact parsing and module-local primitive byte coding
  have one selected owner at their appropriate layer;
- TextureBuild and GeometryBuild each own one explicit atomic registration
  transaction, including direct-linked test/tool fallback; and
- the refactor introduces no `DerivedDataCache` module, new cache backend, new
  recipe abstraction, key change, payload change, or publication change.

## Scope

- The AssetBuildCore public/private boundary around `FBuildSession`,
  `FBuildDefinition`, target facts, `FBuildCacheClient`, and the underlying
  ObjectStore adapter.
- Internal structure for the six migrated asset families and seven registered
  outputs: StaticMesh render
  data, StaticMesh collision, Texture2D, TextureCube, SkeletalMesh,
  AnimationClip, and TerrainHeightmap.
- Module-private canonical local-input schemas within TextureBuild and
  GeometryBuild, using Core's shared derived-data primitive reader/writer
  without sharing asset-family schemas across modules.
- Strict family-neutral target-fact access/parsing where the same semantics are
  already repeated by multiple recipe modules.
- TextureBuild and GeometryBuild function-registration ownership, rollback,
  lazy direct-link adoption, shutdown, and reload.
- Focused characterization and regression tests, dependency/source-boundary
  audits, lasting ownership documentation, and aggregate validation.

## Non-Goals

- Creating a `DerivedDataCache` module or moving
  `FDerivedDataObjectStore` out of AssetCore.
- Remote caches, multiple cache tiers, eviction redesign, in-flight request
  merging, asynchronous sessions, priorities, dependency graphs, workers, or
  recipe scheduling.
- Changing Build function identities or versions, canonical key bytes/strings,
  target-fact names or values, local-input bytes, payload value names or bytes,
  cache roots, maximum sizes, cleanup policies, or required/best-effort store
  behavior.
- Combining asset families behind a generic recipe base class, generic typed
  product, templated mega-adapter, or universal asset codec.
- Moving source capture, image decode, scene parsing, reflected mutation,
  GameThread publication, Cook publication, GPU upload, authoring
  coordination, or Terrain coalescing into AssetBuildCore.
- Reorganizing unrelated builders, derived products, runtime ObjectStore
  consumers, tests that intentionally inspect or corrupt physical cache
  objects, or documentation outside directly affected contracts.
- Enforcing arbitrary file-length limits or one-class-per-file rules. The
  acceptance criterion is responsibility separation, not line-count movement.

## Design Decisions and Invariants

### Keep the current module boundary

`AssetBuildCore` continues to own family-neutral immutable definitions,
function lookup, synchronous session state, cache policy, and opaque cache I/O.
TextureBuild and GeometryBuild continue to own recipe schemas, local inputs,
payload validation, typed reconstruction, and registration declarations.
AssetCore continues to own the physical `FDerivedDataObjectStore`.

A separate DDC module is deferred until there is a real non-Build consumer or
backend capability with an independently testable `key -> opaque object`
contract, such as multi-tier local/remote lookup, request merging, eviction, or
shared statistics. This refactor must not create a pass-through module merely
to relocate the existing cache client.

### Make cache access an implementation detail

`BuildCache.h` and `FBuildCacheClient` are not part of the supported public
AssetBuildCore surface after this plan. The cache adapter moves under
AssetBuildCore `Private`; `FBuildSession` remains the only public mechanism
that recipe code can use to query or store Build values. Cache-client unit
coverage moves to session-level policy coverage, while AssetCore retains direct
ObjectStore tests for physical storage semantics.

Tests may still use `FDerivedDataObjectStore` when the test's purpose is to
inspect, remove, or deliberately corrupt a physical object. Such use is a test
fixture seam, not a production recipe path and must not be wrapped in a new
public Build API.

### Separate four responsibilities without inventing a framework

For each migrated recipe, the implementation has four named responsibilities:

1. canonical local-input encode/decode and its bounds;
2. Build-function configuration, cached/built validation, and execution;
3. public typed definition/session adaptation and result reconstruction; and
4. module registration ownership.

The first three may share a private header where declarations are required,
but no `*BuildOperations.cpp` may continue to own all four. Public operation
headers and call signatures remain stable. Small family-specific rules stay
beside their recipe; no common virtual interface or generic asset product is
introduced.

StaticMesh render and collision execution are separated because they are
distinct registered functions, schemas, values, and policies. SkeletalMesh and
AnimationClip may share bounded Skeleton-context helpers but remain distinct
functions and payload validators. Texture2D and TextureCube may share primitive
codec mechanics but not source or payload schemas.

### Share only stable mechanical behavior

AssetBuildCore gains at most a small family-neutral target-fact validation
surface for required/optional exact string facts and canonical bounded integer
facts. Stage 0 freezes the currently accepted and rejected text forms before
extraction; the helper must preserve those semantics and return diagnostics
that retain the failing fact name.

Primitive little-endian append/read and bounded span-cursor mechanics belong
to private support inside each recipe module. They are not public
AssetBuildCore API because local-input byte layouts remain recipe-owned. A
helper must provide bounds/overflow checks and complete-consumption support,
but family code remains responsible for enum domains, dimensions,
relationships, fingerprints, and payload semantics.

### Registration is one transaction per recipe module

TextureBuild owns one registration set for Texture2D and TextureCube;
GeometryBuild owns one set for StaticMesh render, StaticMesh collision,
SkeletalMesh, AnimationClip, and TerrainHeightmap. Registration state and
rollback order move out of individual operation files into one private owner
per module.

Module startup registers the complete set or rolls back everything acquired in
that attempt. Shutdown first prevents new owner callbacks, then resets the
complete set in reverse acquisition order and drains through the existing
owner gate. Direct-linked tests and tools may still lazily establish the same
process-resident set; later module startup adopts that state without duplicate
identities, and shutdown/reload remains deterministic.

### Preserve behavior before improving it

This is a structural refactor. Cache hits must still avoid family execution;
cache-only calls must still avoid source work; query-disabled and
store-disabled requests stay disabled; required and best-effort store failures
retain their current publication behavior; cancellation checkpoints and phase
timings remain unchanged. Key codecs, local-input codecs, payload codecs, Cook
companions, authored assets, and runtime consumers remain byte-compatible.

## Current Foundations and Gaps

| Area | Foundation to preserve | Gap closed by this plan |
| --- | --- | --- |
| AssetBuildCore | Immutable definitions, private registry, synchronous session, policy, cancellation, phase timing | Public `BuildCache.h` exposes a bypass that only AssetBuildCore implementation and tests now use; repeated target-fact parsing lacks one strict contract |
| TextureBuild | Registered Texture2D/TextureCube functions and typed public operations | Function callbacks, codecs, registration globals, definition construction, and adapters are mixed in operation files; primitive byte coding is duplicated |
| GeometryBuild | Registered StaticMesh/collision, skeletal/animation, and Terrain functions | StaticMesh operations combine several independent responsibilities in about one thousand lines; family registration and parsing/codec mechanics repeat |
| Module lifetime | Owner gates, registration tokens, rollback, direct-link fallback | Each family exposes ad hoc ensure/shutdown state and module startup manually coordinates partial rollback |
| Tests | Golden keys/payloads and broad cold/warm/corrupt/lifecycle coverage | Direct cache-client tests preserve a public implementation detail; responsibility and no-bypass boundaries are not explicitly qualified after extraction |

## Implementation Stages

### Stage 0: Freeze Behavior and Select Extraction Seams

Dependencies: completed Remaining Asset Derived Data Build Migrations plan.

- [x] Inventory definitions, local-input codecs, function callbacks,
  validators, registration globals, public typed adapters, publication helpers,
  and direct cache/ObjectStore access for all six production outputs.
- [x] Record a before-refactor responsibility map for each affected source file
  and select the private destination file/header for every moved symbol; avoid
  adding a file that would own only forwarding code.
- [x] Add or confirm focused tests for canonical target-fact parsing, truncated
  and trailing local inputs, overflow/bounds failures, cache policy, valid-hit
  build avoidance, required/best-effort store failure, cancellation, partial
  registration rollback, direct-link lazy registration, shutdown, and reload.
- [x] Freeze golden Build identities, versions, key bytes/strings, target facts,
  local-input bytes, value names/bytes, roots, size/cleanup policy, and typed
  status/diagnostic mapping before moving code.
- [x] Confirm by repository search that no production caller outside
  AssetBuildCore constructs `FBuildCacheClient`; separately classify intentional
  test-only direct ObjectStore use.
- [x] Capture focused build/test timing and representative cold/warm request
  timing only as regression baselines; this plan does not optimize them.

#### Acceptance Gate

- Every moved responsibility has one selected owner and every externally
  observable contract has a characterization test or golden fixture.
- There is no unresolved decision about helper visibility, registration
  ownership, accepted target-fact syntax, cache policy, cancellation, or
  publication behavior before structural changes begin.

### Stage 1: Close the AssetBuildCore Cache Boundary

Dependencies: Stage 0 complete.

- [x] Move `FBuildCacheClient`, cache query result/status, and direct
  `FDerivedDataObjectStore` inclusion from the public `AssetBuild/BuildCache.h`
  surface into AssetBuildCore private implementation.
- [x] Keep `FBuildSession` behavior and public request/output types unchanged;
  do not expose a replacement raw query/store API.
- [x] Replace direct cache-client tests with session-level tests for hit, miss,
  storage error, query/store policy, cleanup, required/best-effort store, and
  cancellation; retain physical ObjectStore mechanics in AssetCore tests.
- [x] Add the minimal strict target-fact helper selected in Stage 0 and qualify
  it directly for missing, malformed, overflow, optional, and mismatch
  semantics with useful diagnostics; recipe migrations occur in Stages 2-4.
- [x] Remove stale public includes and verify TextureBuild and GeometryBuild
  compile without any cache client or ObjectStore production access.

#### Acceptance Gate

- No public AssetBuildCore header exposes `FBuildCacheClient` or includes
  `DerivedDataObjectStore.h`; recipe modules can perform cache I/O only through
  `FBuildSession`.
- AssetBuildCore and AssetCore focused tests prove session policy and physical
  storage behavior without weakening existing failure coverage.

### Stage 2: Decompose TextureBuild Recipes

Dependencies: Stage 1 complete and target-fact helper qualified.

- [x] Add one private TextureBuild primitive codec/cursor support owner and
  migrate identical little-endian/bounds mechanics from Texture2D and
  TextureCube without changing their canonical local-input bytes.
- [x] Separate Texture2D local-input codec and Build-function execution from
  its public typed request/result and publication operations.
- [x] Separate TextureCube local-input codec and Build-function execution from
  six-face/panorama typed adaptation, derived-data load, and publication.
- [x] Move Texture2D/TextureCube registration tokens, ensure/adoption logic,
  rollback, and shutdown into one private TextureBuild registration owner;
  remove operation-file registration globals and module-local forward
  declarations.
- [x] Preserve Texture build-service lifetime ordering and qualify its
  interaction with the consolidated function-registration set.
- [x] Extend focused tests around extracted seams for malformed/trailing local
  input, wrong target facts, corrupt values, hit build avoidance, lazy
  direct-link registration, rollback, shutdown, and reload.

#### Acceptance Gate

- Texture operation implementations expose typed Build/publication behavior
  but no longer own local-input primitive mechanics or registration state.
- Texture2D and TextureCube golden keys, inputs, TXPL values, cache policies,
  source retention, import/reimport, Cook/runtime load, and module lifecycle
  remain unchanged.

### Stage 3: Decompose StaticMesh Render and Collision Recipes

Dependencies: Stage 1 complete; may proceed independently of Stage 2 after the
shared AssetBuildCore helper is stable.

- [x] Add one private GeometryBuild primitive codec/cursor support owner and
  migrate repeated little-endian/bounds mechanics without changing canonical
  bytes.
- [x] Separate StaticMesh render local input, function validation/execution,
  and definition construction from reconciliation, typed product adaptation,
  and GameThread publication.
- [x] Separate StaticMesh collision local input, function
  validation/execution, and definition construction from the render recipe and
  its public collision adapter.
- [x] Preserve reconciliation snapshots, material-slot stability, source
  provenance, render/collision policy, payload validation, and independent
  cache identities.
- [x] Establish the single private GeometryBuild registration owner with the
  two StaticMesh tokens; Stage 4 expands this same owner to all five Geometry
  registrations without creating a second transaction.
- [x] Extend StaticMesh focused tests for malformed/trailing local input,
  invalid render/collision facts, cold/warm/corrupt paths, independent output
  failure, publication atomicity, and exact golden bytes.

#### Acceptance Gate

- No single StaticMesh implementation file mixes reconciliation/publication,
  render-function execution, collision-function execution, local-input
  primitive mechanics, and registration state.
- StaticMesh render/collision identities, keys, payloads, policies, authored
  publication, Cook output, cooked loading, and rendering behavior are
  unchanged.

### Stage 4: Decompose Remaining Geometry Recipes and Consolidate Registration

Dependencies: Stage 3 complete; TextureBuild consolidation from Stage 2
provides the qualified module-transaction pattern.

- [x] Separate SkeletalMesh and AnimationClip key/local-input/function work
  from their typed product/load adapters while sharing only exact
  Skeleton-context and bounded codec mechanics.
- [x] Separate TerrainHeightmap local-input/function work from direct build,
  cache-only load, diagnostics mapping, and publication adapters; keep
  authoring coalescing and asynchronous ownership outside GeometryBuild recipe
  execution.
- [x] Centralize all five GeometryBuild function registrations in one private
  owner with deterministic acquisition, full rollback, lazy direct-link
  adoption, reverse-order reset, and owner-gate retirement.
- [x] Remove family-level registration globals, exported ensure/shutdown
  implementation seams, and manual partial rollback from
  `GeometryBuildModule.cpp`.
- [x] Preserve skeletal best-effort persistence, Skeleton compatibility,
  Terrain required/non-persisting policy, cancellation predicates, phase
  timing mapping, and detached-product publication.
- [x] Extend focused lifecycle and family tests for malformed/trailing inputs,
  wrong facts/fingerprints, corrupt values, partial registration failure,
  direct-link startup, active-call retirement, shutdown, and reload.

#### Acceptance Gate

- SkeletalMesh, AnimationClip, and Terrain operation implementations no longer
  own local-input primitive mechanics or registration state, and their
  family-specific validation remains visible beside the relevant recipe.
- GeometryBuild startup is one atomic function-registration transaction and
  all five identities retain current behavior across direct-link, module
  startup, retirement, shutdown, and reload.

### Stage 5: Qualify Boundaries, Integrate, and Document

Dependencies: Stages 1-4 complete.

- [x] Search production code for `FBuildCacheClient`, direct
  `FDerivedDataObjectStore`, physical cache path access, family registration
  globals, duplicate primitive codecs, and operation files that still mix all
  four responsibilities; classify any surviving occurrence explicitly.
- [x] Verify the public headers and `.dmodule` dependencies preserve the
  selected ownership direction and introduce no Texture, Geometry, Engine, or
  import dependency into AssetBuildCore.
- [x] Compare golden keys, local inputs, target facts, payloads, cache roots,
  policies, diagnostics/status, Cook companions, and representative cold/warm
  timing against Stage 0 baselines.
- [x] Run focused AssetCore, AssetBuildCore, Texture, StaticMesh, Skeletal,
  SceneImport, Terrain, Cook/runtime, rendering, editor, and module-lifecycle
  tests, then the default `all` build and complete native aggregate.
- [x] Run the hidden-window editor startup/shutdown smoke to qualify module and
  authoring-service lifetimes.
- [x] Update Asset Data Lifecycle and Code Modules only where the implemented
  public/private or registration ownership contract changed; keep file-layout
  detail in code rather than lasting architecture documentation.
- [x] Run changed/all documentation, all-plan, and all-roadmap validation;
  record evidence and mark this plan completed only after every acceptance gate
  passes.

#### Acceptance Gate

- Production recipe code reaches cache storage only through `FBuildSession`,
  every migrated recipe has a clear codec/function/adapter owner, and each
  recipe module has one registration transaction.
- Golden formats and behavior, focused and aggregate tests, default build,
  editor smoke, boundary searches, and all required documentation validators
  pass without a material cold/warm regression.

## Completion Evidence

Completed on 2026-08-16 with the following repository evidence:

- production searches found `FBuildCacheClient` and
  `FDerivedDataObjectStore` only inside `AssetBuildCore.cpp` for the in-scope
  Developer modules; no TextureBuild or GeometryBuild operation performs raw
  cache query, store, cleanup, or physical-path access;
- function-registration tokens and `RegisterBuildFunction` calls occur only in
  `TextureBuildFunctionRegistry.cpp` and
  `GeometryBuildFunctionRegistry.cpp`, owning complete two-function and
  five-function transactions respectively;
- operation files contain definition/session adaptation and typed
  reconstruction/publication, while module-private function sources own local
  inputs, cached/built validation, and recipe execution; primitive byte coding
  is shared once per recipe module;
- focused tests passed: `AssetBuildCoreTests`, `TextureTests`,
  `TextureCookIntegrationTests`, `StaticMeshTests`,
  `StaticMeshCollisionQualificationTests` in qualification mode,
  `SkeletalAssetTests`, `SceneImportTests`, `TerrainHeightmapTests`, and
  `TerrainHeightmapCookTests`;
- the default `all` build and complete `test all` native aggregate passed, and
  `DurinEditor` completed a hidden-window eight-tick startup/shutdown smoke;
- changed/all documentation, all-plan, and all-roadmap validators passed.

Golden Build identities, keys, target facts, local-input and payload bytes,
cache roots, policies, diagnostics, authored publication, Cook companions, and
cooked runtime behavior remain covered by the same production-facing tests.

## Validation Matrix

Follow [Agent Build And Run](../../../Agents/BuildAndRun.md) and
[Agent Testing](../../../Agents/Testing.md); select focused targets first and never
overlap build process trees.

| Concern | Required evidence |
| --- | --- |
| Public boundary | No public `BuildCache.h`/`FBuildCacheClient`; no production recipe includes ObjectStore or performs raw query/store/path access |
| Target facts | Missing, optional, malformed, overflow, non-canonical, and mismatched facts retain frozen accept/reject and diagnostic behavior |
| Local inputs | Golden bytes unchanged; bounded decode rejects truncation, overflow, invalid enum/domain data, and trailing bytes for every recipe |
| Texture recipes | Texture2D/TextureCube cold, warm, corrupt, query-only, store-failure, import/reimport, Cook/runtime, and registration lifecycle coverage |
| StaticMesh recipes | Render/collision independence, reconciliation/publication, cold/warm/corrupt, Cook/runtime/rendering, and exact identity/payload coverage |
| Skeletal recipes | Mesh/clip separation, fingerprints, Skeleton compatibility, best-effort store, scene transaction, Cook/runtime/playback/rendering coverage |
| Terrain recipe | Persisting/non-persisting, cache-only/fallback, timing, cancellation, coalescing, publication revision, Cook/runtime/rendering coverage |
| Module lifetime | Complete-set registration, failure rollback, lazy direct-link adoption, duplicate rejection, retirement, active-call drain, shutdown, and reload |
| Performance | Valid hits execute no family Build function; query/store counts and representative cold/warm timings show no material regression |
| Integration | Focused suites, default `all`, complete native aggregate, hidden-window editor smoke, and dependency/source-boundary audit |
| Documentation | Changed/all document validation, all-plan validation, all-roadmap validation, and accurate lasting ownership contracts |

## Definition of Done

- `FBuildSession` is the only public AssetBuildCore cache-query/store path and
  the raw cache client is private implementation detail.
- All six migrated asset families and seven registered outputs preserve exact
  identities, keys, target
  facts, local-input bytes, value bytes/names, roots, bounds, cleanup and store
  policies, status/diagnostics, cancellation, and publication behavior.
- Each recipe's canonical input, function execution, typed adapter, and
  registration responsibilities have clear owners; no operation file retains
  all four responsibilities.
- TextureBuild and GeometryBuild each own one atomic, lifecycle-safe function
  registration transaction with qualified direct-link fallback and reload.
- Stable target-fact and primitive codec repetition is consolidated at the
  narrowest correct layer without introducing a generic asset recipe
  framework.
- Production boundary searches find no recipe-side raw cache access; any
  intentional test-only physical ObjectStore access is documented by test
  purpose.
- Focused tests, default build, full native aggregate, hidden editor smoke,
  performance comparisons, and documentation lifecycle validators pass.
- Lasting contracts reflect the implemented ownership, completion evidence is
  recorded, and the plan is marked completed.

## Deferred Follow-ups

- Create a `DerivedDataCache` module only after a second independently useful
  cache-facing subsystem or backend capability establishes a boundary broader
  than Asset Build requests.
- Add multi-tier/remote cache lookup, request merging, eviction, statistics,
  or asynchronous sessions only from measured production requirements and a
  separate plan.
- Add lazy local-input production only if measured warm-path normalization cost
  justifies its lifetime and cancellation complexity for multiple families.
- Generalize registration transactions in AssetBuildCore only if a third
  module needs the same semantics after TextureBuild and GeometryBuild settle.
- Consolidate family semantic validators only when two schemas become truly
  identical; similar-looking asset rules are not sufficient.

## Related Documentation

- [Remaining Asset Derived Data Build Migrations Plan](RemainingAssetDerivedDataBuildMigrations.md)
- [Local Derived Data Build Requests Plan](LocalDerivedDataBuildRequests.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Skeletal Mesh Rendering](../../../Runtime/Rendering/SkeletalMeshRendering.md)
- [Skeletal Animation Playback](../../../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Agent Build And Run](../../../Agents/BuildAndRun.md)
- [Agent Testing](../../../Agents/Testing.md)

## Related Code

- [`BuildDefinition.h`](../../../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildDefinition.h)
- [`BuildSession.h`](../../../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildSession.h)
- [`AssetBuildCore.cpp`](../../../../Engine/Source/Developer/AssetBuildCore/Private/AssetBuildCore.cpp)
- [`TextureBuildModule.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp)
- [`TextureBuildFunctionRegistry.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildFunctionRegistry.cpp)
- [`TextureBuildFunctions.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildFunctions.cpp)
- [`TextureBuildOperations.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildOperations.cpp)
- [`TextureCubeBuildOperations.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuildOperations.cpp)
- [`GeometryBuildModule.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildModule.cpp)
- [`GeometryBuildFunctionRegistry.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildFunctionRegistry.cpp)
- [`StaticMeshBuildFunctions.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/StaticMesh/StaticMeshBuildFunctions.cpp)
- [`StaticMeshBuildOperations.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/StaticMesh/StaticMeshBuildOperations.cpp)
- [`SkeletalBuildOperations.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/Skeletal/SkeletalBuildOperations.cpp)
- [`SkeletalBuildFunctions.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/Skeletal/SkeletalBuildFunctions.cpp)
- [`TerrainHeightmapBuildOperations.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/Terrain/TerrainHeightmapBuildOperations.cpp)
- [`TerrainHeightmapBuildFunctions.cpp`](../../../../Engine/Source/Developer/GeometryBuild/Private/Terrain/TerrainHeightmapBuildFunctions.cpp)
- [`AssetBuildCoreTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/AssetBuildCoreTests.cpp)
- [`TextureCubeTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp)
- [`StaticMeshDerivedDataCacheTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp)
- [`SkeletalAssetTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp)
- [`TerrainHeightmapTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp)
