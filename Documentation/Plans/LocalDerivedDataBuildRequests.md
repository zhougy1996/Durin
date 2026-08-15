# Local Derived Data Build Requests Plan

Summary: Add a production-used local derived-data Build request model and migrate StaticMesh and Texture2D cache/build paths without changing their DDC keys or payload formats.

Last reviewed: 2026-08-16

Status: Completed
Completed: 2026-08-16

## Current Status

Implementation and validation are complete. AssetBuildCore now owns immutable
definitions, validated keys, structured policy/output, request-scoped
cancellation, module-aware function registration, and the synchronous
query/validate/build/validate/store/cleanup session. The deleted speculative
registry/request-owner headers were not restored.

GeometryBuild registers StaticMesh render and collision functions and routes
their production DDC operations through `FBuildSession`. TextureBuild registers
the Texture2D function; direct builds, authored cache-only loads, and the
existing authoring coordinator all use the same synchronous request path. The
existing DMSH, collision, and TXPL key builders, roots, value names, encoded
bytes, Cook formats, publication boundaries, and worker ownership remain
unchanged. Direct linked test/application calls can lazily establish a
process-resident local registration when module startup is intentionally absent;
normal module startup supplies the module callback gate and shutdown resets the
registration.

Validation passed on 2026-08-16: AssetBuildCoreTests (7/7), TextureTests (66
passed, 2 environment-gated skips), StaticMeshTests (68/68), AssetImportTests
(17/17), a focused SceneImport regression, default `all` build, complete native
aggregate, hidden-window editor startup/shutdown smoke, repository searches,
and all documentation/plan/roadmap validators.

## Goal

Provide one family-neutral local Build request path that:

- accepts an immutable definition, local inputs, policy, and caller-owned
  cancellation state;
- validates the definition and resolves one module-owned local Build function;
- queries the correct DDC bucket, validates opaque hits through the owning
  function, executes locally on a safe miss, validates the new output, and
  applies explicit store policy;
- reports cache hit, local build, cancellation, and phase-specific failure in
  one structured result;
- is used by every production StaticMesh render/collision and Texture2D
  derived-data query/build/store path in scope.

The implementation is complete only when the migrated assets no longer
manually sequence DDC query/build/store and the new AssetBuildCore request
surface has production consumers in both GeometryBuild and TextureBuild.

## Scope

- AssetBuildCore local definition, function, session, policy, result,
  cancellation, cache-bucket, validation, and module-lifetime APIs.
- A synchronous, caller-thread `FBuildSession::Build` pipeline. Existing asset
  coordinators may invoke it from their own workers; AssetBuildCore does not
  add a scheduler in this plan.
- One immutable opaque output value per definition, backed by the existing
  `FDerivedDataObjectStore` and `FBuildCacheClient`.
- GeometryBuild registrations and adapters for StaticMesh render data and
  StaticMesh collision data.
- TextureBuild registration and adapters for Texture2D platform data,
  including direct import, authored cache-only load, and the existing
  asynchronous Texture2D authoring coordinator.
- Focused cache, lifecycle, cancellation, deterministic identity, asset import,
  authored load, and cooked-regression validation.
- Lasting Asset Data Lifecycle and Code Modules documentation for the
  implemented request boundary.

## Non-Goals

- Restoring the deleted `BuildRegistry.h`, `BuildRequest.h`, or their exact
  types and ownership model.
- Remote execution, worker processes, RPC, exportable definitions, shared
  schedulers, priorities, dependency graphs, or transitive Builds.
- AssetBuildCore-owned worker threads, completion callbacks, main-thread
  publication, or replacement of TextureBuild/GeometryBuild service queues.
- Multi-value cache records, partial-value requests, cache backend redesign,
  Zen-style services, or new physical DDC layouts.
- Migrating TextureCube, SkeletalMesh, AnimationClip, TerrainHeightmap,
  shaders, thumbnails, or EnvironmentLighting in this plan.
- Changing StaticMesh, StaticMesh collision, or Texture2D canonical key bytes,
  builder/schema versions, DDC relative roots, payload bytes, cooked payloads,
  DAST v4, DBLK, or cook manifests.
- Moving source decoding, import reconciliation, reflected asset mutation,
  package publication, GPU upload, or Cook publication into AssetBuildCore.

## Design Decisions and Invariants

### Public vocabulary and file boundaries

The new API follows the useful parts of Unreal Engine's Derived Data Build
vocabulary without claiming remote or portable execution:

| API | Ownership and purpose |
| --- | --- |
| `FBuildKey` | Validated opaque 128-bit lowercase key used to address one existing family DDC bucket |
| `FBuildFunctionIdentity` | Stable owner-qualified function name plus explicit implementation version |
| `FBuildDefinition` | Immutable function identity, key, target facts, expected value name, and optional local input values |
| `FBuildDefinitionBuilder` | The only public construction path; rejects duplicate names, invalid values, incomplete target facts, and key/input disagreement before producing a definition |
| `FBuildPolicy` | Explicit cache-query, local-build, cache-store, required-store, and return-data intent; it contains no unused priority or remote flags |
| `FBuildOutput` | One validated opaque `FBuildValue`, origin, status, failure phase, store diagnostic, and cache/build facts |
| `FBuildContext` | Read-only definition/input access, cancellation query, and output publication surface visible to a function |
| `IBuildFunction` | Asset-family implementation of cached-output validation and local output construction |
| `FBuildFunctionRegistration` | Move-only module-owned lifetime token for one function identity |
| `FBuildSession` | Lightweight synchronous request facade that owns the query/validate/build/store state machine |

These types live in focused `BuildDefinition.h`, `BuildFunction.h`, and
`BuildSession.h` headers, with shared value/policy/result vocabulary in
`BuildTypes.h`. The deleted broad `BuildRegistry.h` and `BuildRequest.h` names
are not reintroduced. `BuildCache.h` and `BuildHost.h` remain focused on their
existing responsibilities.

### Definition and key identity

- Existing asset key builders remain authoritative during migration. A
  build-capable typed adapter supplies the exact canonical key-input bytes and
  the existing 32-character key; the definition builder verifies that the key
  is the XXH3-128 identity of those bytes. A cache-only adapter may construct a
  validated `FBuildKey` from the persisted key when authoritative build inputs
  are intentionally unavailable.
- Function identity and implementation version are explicit dispatch and
  diagnostic facts. Existing builder and payload schema versions remain inside
  the canonical family key input, so the migration does not double-version or
  invalidate current DDC objects.
- Definition inputs are immutable named `FBuildValue` objects. The reserved
  key-input value is identity-bearing; local execution inputs contain
  canonical normalized source data required by the function. Physical source
  paths, timestamps, reflected objects, package pointers, RHI state, and
  callbacks never enter a definition.
- Cache-only definitions may omit local execution inputs and canonical
  key-input bytes but must still carry function identity, key, target facts,
  and expected value name. A local-build policy applied to incomplete inputs
  fails validation before function execution.
- StaticMesh and Texture2D retain their current key strings, DDC roots, value
  names, and encoded payload bytes. Golden identity and payload tests guard
  this invariant before and after migration.

### Function registration and module lifetime

- GeometryBuild and TextureBuild register concrete local functions during
  module startup and own the move-only registrations as module-instance state.
- Each `IBuildFunction` configures its validated relative cache bucket,
  maximum value size, cleanup budget/limit, and expected value contract. A
  definition cannot redirect a function to another family's storage.
- Registry state is private to AssetBuildCore. Public code can register a
  function and create/use a session, but cannot obtain or mutate a registry
  singleton.
- Module-managed registration requires a canonical unique identity and a valid
  `FModuleOwnedCallbackGate`. Execution retains a module resource lease and
  enters the owner gate before calling the function. Direct-linked test or tool
  processes that intentionally omit module startup may establish the same
  process-resident local registration without a gate; the owning module's
  shutdown hook resets it when module lifecycle is active.
- The registry lock protects lookup and generation changes only. Asset code,
  cache I/O, validation, build work, diagnostics, and registration destruction
  never run while that lock is held.
- Registration reset first prevents new lookup, then waits through the module
  callback gate/resource lease contract. It does not own or cancel callers'
  request groups. Duplicate identities and retirement races fail explicitly.

### Session, thread, and cancellation ownership

- `FBuildSession::Build` is synchronous and executes on its caller's thread.
  StaticMesh may call it synchronously; Texture2D's existing coordinator may
  call it from an admitted worker. The session creates no thread and pumps no
  completion.
- Cancellation is caller-owned and request-scoped. A small read-only
  `FBuildCancellationToken` may be absent for synchronous callers and adapts
  Texture2D's existing cancellation predicate when present.
- The session checks cancellation before cache I/O, before function execution,
  at function-defined deterministic checkpoints through `FBuildContext`, and
  before store. A completed validated output is not published by AssetBuildCore;
  typed adapters and existing main-thread publication transactions retain that
  responsibility.
- `BuildHost` remains the process lifecycle aggregator for existing
  GeometryBuild and TextureBuild services. It neither becomes a scheduler nor
  duplicates session request ownership.

### Cache, validation, and failure policy

The session owns this ordered state machine:

1. Validate the request and definition for the selected policy.
2. Query the configured cache bucket when allowed.
3. On a hit, ask the registered function to validate the opaque value.
4. Return a valid hit without invoking local build.
5. On missing or invalid cached data, build only when policy permits and all
   local inputs are present; otherwise return the exact cache outcome.
6. Validate function output, value name, maximum size, and complete codec
   consumption before any store or typed publication.
7. Store under the existing key when allowed, distinguishing required from
   best-effort failure.
8. Run bucket cleanup only after a successful store; cleanup diagnostics never
   replace the primary request outcome.

Invalid request/key/function identity is never a cache miss. Missing,
incompatible, truncated, or corrupt cached bytes are rebuildable only when the
request has authoritative local inputs and policy allows build. A cache I/O
failure remains visible in structured diagnostics; policy determines whether a
complete validated local build may succeed despite it. Required-store behavior
preserves current StaticMesh and Texture2D transactional semantics during
migration; best-effort store remains available and must be tested separately.

`FBuildOutput` distinguishes at least `CacheHit`, `Built`, `CacheMiss`,
`Canceled`, and `Failed`, and records a failure phase such as `Request`,
`FunctionLookup`, `CacheQuery`, `CachedValueValidation`, `LocalBuild`,
`BuiltValueValidation`, or `CacheStore`. A boolean API may summarize success,
but callers do not infer status from diagnostic strings.

### Typed-family boundaries

- AssetBuildCore treats definition inputs and outputs as opaque bytes and does
  not include Engine asset headers or understand DMSH, collision, or TXPL.
- GeometryBuild owns canonical normalized StaticMesh input encoding, render and
  collision functions, family validation, typed result reconstruction,
  material-slot reconciliation, and StaticMesh diagnostics.
- TextureBuild owns canonical normalized RGBA8/settings input encoding,
  Texture2D function execution, TXPL validation, typed platform-data
  reconstruction, build metrics, and coordinator phase mapping.
- StandardAssetImport continues to decode source files and publish detached
  typed products. Runtime Engine continues to own asset state and cooked
  loading. Cook may reuse the resulting validated payload but is not routed
  through `FBuildSession` by this plan.

## Current Foundations and Gaps

| Area | Retained foundation | Gap closed by this plan |
| --- | --- | --- |
| Values and hashing | Immutable `FBuildValue` with XXH3-128 content identity | Typed key wrapper, definition construction, expected-output contract, and structured result |
| Cache | `FDerivedDataObjectStore`, `FBuildCacheClient`, explicit query/store policy | One session-owned query/validate/build/store sequence and family-supplied bucket configuration |
| Lifecycle | `FModuleOwnedCallbackGate`, resource leases, `BuildHost` start/pump/wait/drain | Local function registration whose lifetime is independent of caller cancellation |
| StaticMesh | Canonical render/collision key inputs, deterministic DMSH/collision codecs, detached products, cache tests | Registered functions and complete render/collision migration away from manual DDC sequencing |
| Texture2D | Canonical key input, normalized RGBA8 request, deterministic TXPL codec, cancellation checkpoints, async coordinator | Registered function, cache-hit short circuit, query-only load, and removal of direct ObjectStore access |
| Diagnostics | Family-specific enums and strings | Common origin/failure phase plus lossless mapping back to existing family diagnostics |
| Execution | Typed functions already run safely on caller or family worker threads | Synchronous session; no generic scheduler or callback executor |

## Implementation Stages

### Stage 0: Freeze Production Contracts

Dependencies: None.

- [x] Inventory every StaticMesh render/collision and Texture2D call site that
  computes a key, queries DDC, builds bytes, validates bytes, stores bytes,
  performs cleanup, reports metrics, handles cancellation, or publishes typed
  state.
- [x] Record golden canonical key-input bytes, key strings, value names, DDC
  roots, maximum sizes, builder/schema versions, and representative encoded
  payload hashes for StaticMesh render data, StaticMesh collision, and
  Texture2D.
- [x] Characterize cold build, warm hit, query-only miss, corrupt/incompatible
  hit, disabled query, disabled build, required/best-effort store failure,
  cancellation, module retirement, and source-unavailable authored load.
- [x] Freeze the exact new header/type names and confirm they do not conflict
  with active production or completed-plan references.
- [x] Identify the GeometryBuild and TextureBuild module startup owners that
  will retain registrations and the existing worker boundaries that will call
  synchronous sessions.

#### Acceptance Gate

- All three migrated payload functions have a call-site and identity inventory
  plus golden key/payload fixtures before behavior changes.
- No unresolved ownership, thread, cache-error, cancellation, key-compatibility,
  or module-registration decision remains for Stage 1.

### Stage 1: Implement the Local Build Foundation

Dependencies: Stage 0 complete.

- [x] Add `FBuildKey`, `FBuildFunctionIdentity`, `FBuildDefinition`,
  `FBuildDefinitionBuilder`, `FBuildPolicy`, `FBuildOutput`, structured status
  and failure phase, and request-scoped cancellation vocabulary.
- [x] Add `FBuildContext`, `IBuildFunction`, move-only
  `FBuildFunctionRegistration`, and private module-safe function-registry
  storage without restoring the deleted registry header or public getter.
- [x] Add synchronous `FBuildSession::Build` with the ordered
  validate/query/validate-hit/build/validate-output/store/cleanup state machine.
- [x] Route cache access through existing `FBuildCacheClient`; extend its
  family-neutral status/value surface only where the session requires facts
  currently lost by the client.
- [x] Add focused tests for definition immutability and key agreement, duplicate
  inputs and registrations, query-only requests, valid hits, corrupt hits,
  local miss builds, output validation, policy enforcement, required and
  best-effort stores, cancellation checkpoints, exception containment,
  reentrant diagnostics, and owner retirement during an active call.
- [x] Prove that no registry/cache lock is held across family code or terminal
  module teardown and that session destruction requires no drain.

#### Acceptance Gate

- AssetBuildCoreTests pass with a family-free sample function that exercises
  the complete cache/build/store state machine rather than direct callback
  dispatch alone.
- The public surface has no scheduler, priority, remote/export, callback,
  public registry singleton, or registry-owned request owner.
- Existing GeometryBuild and TextureBuild modules still compile before their
  migrations and retain current `BuildHost` behavior.

### Stage 2: Migrate StaticMesh Render and Collision Builds

Dependencies: Stage 1 complete.

- [x] Add canonical local execution-input codecs and registered GeometryBuild
  functions for StaticMesh render data and StaticMesh collision without
  placing Engine objects or source paths in definitions.
- [x] Adapt `BuildImportedProduct` and `LoadDerivedDataProduct` to construct
  definitions and consume structured session outputs; preserve material-slot
  restoration, source-availability diagnostics, package-dirty behavior, and
  detached publication.
- [x] Adapt `BuildCollisionProduct` so cache hit, corruption rebuild, local
  collision construction, validation, required store, byte metrics, and
  diagnostics run through the same session state machine.
- [x] Remove migrated manual `FBuildCacheClient` query/store sequencing and
  private store helpers from StaticMesh operations while retaining the exact
  roots, budgets, cleanup limits, keys, and encoded bytes through function
  configuration.
- [x] Extend StaticMesh tests for session origin/failure mapping, repeated
  imports using a warm hit, corrupt-cache rebuild, write failure without partial
  publication, source-unavailable cache-only load, cancellation where
  applicable, and GeometryBuild registration retirement.

#### Acceptance Gate

- StaticMesh render and collision production paths use `FBuildSession`; no
  migrated operation manually sequences query/build/store.
- Golden DDC keys and payload bytes remain identical, and existing authored
  load, import, collision, Cook, and cooked-runtime tests pass.
- StaticMesh cache hits do not invoke local geometry conversion or collision
  construction.

### Stage 3: Migrate Texture2D Builds and Loads

Dependencies: Stage 1 complete; Stage 2 provides the first cross-module
production qualification of the foundation.

- [x] Add canonical normalized Texture2D local input encoding and a registered
  TextureBuild function that owns mip generation, compression, TXPL encoding,
  and opaque output validation.
- [x] Adapt direct Texture2D builds/imports to construct definitions and decode
  session outputs into `FTexture2DBuildProduct` while source translation and
  asset publication remain outside the function.
- [x] Route `LoadTexture2DDerivedData` through a cache-only session request and
  preserve missing, incompatible, corrupt, and source-fallback diagnostics.
- [x] Adapt `FTexture2DAuthoringCoordinator` to call the synchronous session
  from its existing admitted worker, map session status to current phases, and
  preserve cancellation, supersession, metrics, memory accounting, completion
  pumping, and main-thread publication.
- [x] Remove direct `FDerivedDataObjectStore` reads/writes and manual persistence
  callbacks from migrated Texture2D operations; configure the existing root,
  maximum value size, budget, and cleanup limit through the function/session
  boundary.
- [x] Extend Texture2D tests for cold/warm requests, cache-hit build avoidance,
  deterministic keys and TXPL bytes, corrupt-cache rebuild, query-only misses,
  required/best-effort write failure, cancellation before build/during recipe/
  before store, supersession, module retirement, and unchanged publication.

#### Acceptance Gate

- Every production Texture2D DDC query/build/store path uses `FBuildSession`,
  including the asynchronous coordinator worker and authored cache-only load.
- Texture2D has no direct ObjectStore access in migrated operations; source
  decoding, publication, and coordinator thread ownership remain unchanged.
- Golden keys and TXPL bytes remain identical, and existing Texture2D import,
  editing, cancellation, derived-data, Cook, and runtime-load tests pass.

### Stage 4: Integrate, Document, and Complete

Dependencies: Stages 2 and 3 complete.

- [x] Search production source and tests for the deleted executor names and for
  manual StaticMesh/Texture2D DDC sequencing that should have been removed;
  distinguish intentional deferred-family cache access.
- [x] Confirm GeometryBuild and TextureBuild registration startup, duplicate
  rejection, shutdown admission closure, active-call drain, reload, and final
  capture destruction under the existing module lifecycle contract.
- [x] Measure cold build, warm hit, and cache-only load behavior sufficiently to
  detect duplicate recipe execution, redundant payload copies, or a warm-path
  regression introduced by the session.
- [x] Update Asset Data Lifecycle and Code Modules with the implemented local
  Build boundary, migrated families, deferred families, ownership, thread, and
  failure contracts; remove the lasting statement that AssetBuildCore has no
  generic executor only after production migration is complete.
- [x] Run focused AssetBuildCore, StaticMesh, Texture2D, import, Cook, and module
  lifecycle targets, then the default `all` build and complete native aggregate
  because AssetBuildCore is shared infrastructure. Run the hidden-window editor
  smoke to qualify module startup/shutdown and authored asset services.
- [x] Run changed-document, all-document, all-plan, and all-roadmap validation;
  record evidence, complete the plan, and leave broader asset-family migration
  as explicit follow-up work.

#### Acceptance Gate

- StaticMesh render/collision and Texture2D are real production consumers of
  the new local request model with unchanged keys, payloads, Cook output, and
  runtime behavior.
- Focused tests, default `all` build, complete native aggregate, hidden-window
  editor smoke, repository searches, and documentation validators pass.
- Lasting documentation owns the implemented contract and names deferred
  family migrations without presenting them as already complete.

## Validation Matrix

Follow [Agent Build And Run](../Agents/BuildAndRun.md) and
[Agent Testing](../Agents/Testing.md); select the smallest named targets first
and do not overlap build process trees.

| Concern | Required evidence |
| --- | --- |
| Definition contract | Deterministic builder output; key/input agreement; duplicate/invalid inputs rejected; query-only versus build-capable validation |
| Session policy | Query enabled/disabled, build enabled/disabled, cache hit/miss/corrupt/read failure, required/best-effort store, return-data behavior |
| Output safety | Wrong value name, oversized output, malformed family payload, incomplete consumption, exception, and cancellation fail without store or publication |
| Module lifetime | Duplicate registration, lookup during retirement, active-call lease, reset/reload, reentrant diagnostics, and final capture destruction |
| StaticMesh | Golden render/collision keys and bytes; cold/warm/source-unavailable/corrupt/write-failure paths; import, collision, Cook, cooked-runtime regressions |
| Texture2D | Golden key and TXPL bytes; cold/warm/query-only/corrupt/write-failure paths; direct import, async cancellation/supersession, editing, Cook, runtime regressions |
| Performance | One cache query per request, no recipe execution on valid hit, bounded payload copies, and no material warm-hit regression |
| Integration | AssetBuildCoreTests, StaticMeshTests, TextureTests and focused Cook/integration/module-lifecycle targets; default `all` build; complete native tests; hidden-window editor smoke |
| Documentation | Changed/all document validation plus all-plan and all-roadmap lifecycle validation |

## Definition of Done

- The selected public types, ownership rules, thread behavior, state machine,
  cache policy, cancellation semantics, and failure phases are implemented and
  covered by focused AssetBuildCore tests.
- StaticMesh render data, StaticMesh collision, and Texture2D platform data all
  use registered local functions and `FBuildSession` in production.
- The migrated asset operations contain no parallel manual
  query/build/validate/store path.
- Existing canonical keys, cache roots, payload schemas/bytes, package data,
  cooked data, and runtime loading remain compatible.
- No deleted speculative executor header or registry-owned request owner is
  restored, and no remote/scheduler surface is introduced without a consumer.
- Required focused, integration, aggregate, editor-smoke, search, and document
  validation passes with evidence recorded in this plan.
- Lasting Runtime and Workspace documentation describes the implemented model,
  and this plan is marked completed only after all acceptance gates pass.

## Deferred Follow-ups

- Migrate TextureCube after Texture2D proves that cube face/panorama inputs and
  multi-slice payload validation fit the local definition model.
- Migrate SkeletalMesh and AnimationClip together because they share captured
  scene closure and Skeleton compatibility identity.
- Migrate TerrainHeightmap and decide whether its asynchronous authored-load
  behavior needs only the existing family coordinator or a later common
  scheduler.
- Consider multiple named output values only after a production recipe needs
  partial retrieval or independently addressable values.
- Consider in-flight key coalescing, priority scheduling, dependency graphs,
  portable definitions, and remote workers only with measured production
  consumers and a separate plan.
- Reassess whether direct public `FBuildCacheClient` use can become private
  after every intended asset family has migrated.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Asset Build And Runtime Domain Simplification Plan](AssetBuildAndRuntimeDomainSimplification.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`BuildTypes.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildTypes.h)
- [`BuildCache.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildCache.h)
- [`BuildHost.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildHost.h)
- [`AssetBuildCore.cpp`](../../Engine/Source/Developer/AssetBuildCore/Private/AssetBuildCore.cpp)
- [`StaticMeshBuildOperations.h`](../../Engine/Source/Developer/GeometryBuild/Public/StaticMesh/StaticMeshBuildOperations.h)
- [`StaticMeshBuildOperations.cpp`](../../Engine/Source/Developer/GeometryBuild/Private/StaticMesh/StaticMeshBuildOperations.cpp)
- [`StaticMeshBuildDerivedData.h`](../../Engine/Source/Developer/GeometryBuild/Public/StaticMesh/StaticMeshBuildDerivedData.h)
- [`TextureBuildOperations.h`](../../Engine/Source/Developer/TextureBuild/Public/Texture/TextureBuildOperations.h)
- [`TextureBuildOperations.cpp`](../../Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildOperations.cpp)
- [`Texture2DDerivedData.h`](../../Engine/Source/Developer/TextureBuild/Public/Texture/Texture2DDerivedData.h)
- [`Texture2DAuthoringCoordinator.cpp`](../../Engine/Source/Developer/TextureBuild/Private/Texture/Texture2DAuthoringCoordinator.cpp)
- [`StandardAssetImportProviders.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [`Texture2DSourceTranslation.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/Texture2DSourceTranslation.cpp)
- [`AssetBuildCoreTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/AssetBuildCoreTests.cpp)
- [`StaticMeshDerivedDataCacheTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp)
- [`TextureBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp)
- [`TextureDerivedDataTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Texture/TextureDerivedDataTests.cpp)
