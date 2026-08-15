# Asset Build And Runtime Domain Simplification Plan

Summary: Reduce AssetBuildCore to production-used cache and host behavior and construct AssetCore in one immutable authored or cooked execution domain.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-16

## Current Status

This is the completed M4 child plan of the
[Asset Architecture Simplification Roadmap](../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md).
M2 already separated deterministic package decode from asset-specific
post-load work. Stage 0 freezes the real cache, host, runtime-domain, payload,
startup, and test consumers before deleting speculative Build execution APIs.

## Outcome

- AssetBuildCore exposes only immutable cache values/policy plus the
  production-used authoring build host.
- Local function registries, portable definitions, generic request owners, and
  executor callbacks with no production consumer are removed.
- AssetCore is initialized once with an immutable `Authored` or `Cooked`
  execution domain; callers cannot mutate a process-wide package-load mode.
- Payload behavior is explicit: authored execution may rebuild from source or
  derived data, while cooked execution requires validated cooked payloads.
- Engine asset `PostLoad` code asks the immutable runtime service for domain and
  payload policy rather than branching on mutable global mode state.

## Scope

- AssetBuildCore public cache, host, request, definition, registry, policy,
  module, implementation, and tests.
- AssetCore runtime construction, cooked payload context, public load surface,
  and initialization/shutdown tests.
- Engine animation, environment lighting, mesh, terrain, and texture authored
  rebuild versus cooked payload behavior.
- Lasting Asset Packages, Asset Data Lifecycle, build/cook, and roadmap
  contracts affected by the implemented boundary.

## Non-Goals

- Remote build execution, portable build RPC, a general scheduler, or new DDC
  storage backends without a production consumer.
- Changing DAST v4, DBLK, cook-manifest, derived-data key, or payload bytes.
- Redesigning importer-specific build recipes or adding asynchronous package
  streaming.
- Final repository-wide legacy cleanup owned by M5.

## Stage 0: Freeze Build And Runtime-Domain Inventory

Dependencies: M2 complete.

- [x] Inventory every production and test consumer of AssetBuildCore cache,
  host, definition, function registry, request owner, and module APIs.
- [x] Inventory runtime-domain configuration, startup/shutdown, Engine
  `PostLoad`, cooked-payload, authored rebuild, and test-reset callers.
- [x] Characterize cache hit/miss/error policy, host registration/start/drain,
  authored fallback, cooked hard failure, and initialization lifetime.
- [x] Assign every public type/function to the retained cache/host surface, the
  immutable runtime construction contract, or a deletion stage.

### Acceptance Gate

- Every production build/domain entry has one named destination.
- Focused baseline tests cover behavior crossing the deletion or construction
  boundary.

### Frozen inventory

Production AssetBuildCore use divides into two retained destinations:

| Current surface | Production consumers | Destination |
| --- | --- | --- |
| `FBuildValue`, `FBuildPolicy`, and `FBuildCacheClient` | GeometryBuild static/skeletal/terrain operations | cache value, cache policy, and client retained with executor-only policy fields removed |
| build service contribution and host lifecycle | GeometryBuild, TextureBuild, AssetBuildCore module, and MainFrame startup | build host retained with registration, start, pump, wait, snapshot, and ordered drain behavior |

`FBuildFunctionIdentity`, `FBuildDefinition`, `FBuildFunctionResult`,
`FBuildRequestOwner`, the local function registry, and its execution/callback
surface have no production consumer. Their only consumer is
`AssetBuildCoreTests`, where six tests self-characterize the speculative
executor. They are Stage 1 deletion targets rather than a compatibility
contract. Four cache/host tests remain and cover query/store policy, required
versus best-effort writes, multi-service startup/pump/snapshot/ordered drain,
partial-start rollback, and module-owner retirement.

AssetCore owns one `FPackageLoadContext` inside its process singleton. The
public `ConfigurePackageLoadContext` mutates that context before the first load;
no production source configures cooked mode, while cook and affected Engine
tests repeatedly switch the singleton between authored and cooked values.
Animation, environment lighting, static mesh, skeletal mesh, terrain,
`Texture2D`, and `TextureCube` `PostLoad` paths read the global context to choose
authored source/DDC reconstruction or mandatory cooked DBLK payloads. AssetCore
save, mutation, load, and cook-reachability paths also branch on its mode.

Stage 3 replaces that pair with validated authored/cooked runtime
configuration factories. `InitializeAssetManager` becomes the construction
boundary; the selected execution domain, cook root, and payload policy remain
fixed until shutdown. Tests that need another domain must shut down and
initialize a fresh runtime. Engine consumers retain a read-only configuration
query and branch on explicit payload policy rather than a mutable load mode.

Baseline qualification passed on 2026-08-15: AssetBuildCoreTests passed 11/11
and AssetCookTests passed 13/13. The former covers all retained and deleted
Build boundaries; the latter covers deterministic DBLK/manifest encoding,
cooked-path resolution, invalid runtime context, source/DDC fallback policy,
read-only cooked behavior, and missing payload diagnostics.

## Stage 1: Remove Unused Build Execution Abstractions

Dependencies: Stage 0 complete.

- [x] Remove local Build function registration/execution and its module-owned
  callback lifetime machinery.
- [x] Remove portable definition, function identity, result, terminal callback,
  and generic request-owner APIs that have no production consumer.
- [x] Retain only the value and cache-policy facts required by production DDC
  clients, with names that describe cache behavior rather than a hypothetical
  executor.
- [x] Replace registry self-tests with cache and host contract coverage.

### Acceptance Gate

- No production or test-only local Build executor path remains.
- AssetBuildCore's value/policy vocabulary is justified by cache consumers.

The local Build function registry, request owner, definition, function
identity/result, terminal callback, and both public registry/request headers
are removed. Their six self-characterization tests are gone. `FBuildValue`
now describes immutable DDC bytes, and `FBuildCachePolicy` contains only query,
store, and required-store facts used by cache clients; executor-only local
build, returned-data, and priority fields were deleted.

Retired-symbol search is empty across production source and native tests.
AssetBuildCoreTests passes its five retained cache/host tests, and the
GeometryBuild and TextureBuild production modules build independently against
the reduced surface.

## Stage 2: Publish A Cache-And-Host-Only Build Surface

Dependencies: Stage 1 complete.

- [x] Isolate cache and host public headers from deleted execution concerns.
- [x] Preserve GeometryBuild and TextureBuild host registration, startup,
  pumping, cancellation, wait, drain order, and module retirement behavior.
- [x] Preserve cache query/store skip, best-effort, required-write, and storage
  error behavior across production build families.
- [x] Search all source and tests for retired Build execution symbols and
  obsolete include/dependency edges.

### Acceptance Gate

- AssetBuildCore contains only cache/value/policy and build-host behavior used
  by named production modules.
- Existing build families compile and retain DDC/host behavior.

`BuildTypes.h`, `BuildCache.h`, and `BuildHost.h` now form the complete public
surface. AssetBuildCoreTests passes 5/5 retained cache/host tests; GeometryBuild,
TextureBuild, Engine, and their affected asset suites compile without a deleted
executor dependency. Retired execution-symbol search is empty across source and
native tests.

## Stage 3: Construct One Immutable Asset Runtime Domain

Dependencies: Stage 0 complete.

- [x] Replace `EPackageLoadMode` and `ConfigurePackageLoadContext` with an
  immutable authored/cooked runtime configuration selected at AssetCore
  initialization.
- [x] Make authored fallback versus cooked-payload requirement an explicit,
  validated payload policy of that configuration.
- [x] Prevent runtime configuration replacement while AssetCore is initialized;
  tests switch domains only through shutdown and fresh initialization.
- [x] Keep cooked root and target validation local to cooked construction and
  package/payload resolution.

### Acceptance Gate

- Production APIs cannot mutate the asset execution domain after startup.
- Authored and cooked configurations cannot form invalid domain/payload-policy
  combinations.

`FAssetRuntimeConfiguration` has private state and only authored and validated
cooked factories. Cooked construction binds the absolute normalized root to
`CookedPayloadRequired`; authored construction binds source/DDC fallback and no
cook root. AssetCore exposes the configuration read-only and rejects a different
configuration until shutdown.

## Stage 4: Migrate Engine Payload Consumers

Dependencies: Stages 2-3 complete.

- [x] Migrate animation, environment lighting, static/skeletal mesh, terrain,
  texture, and cube texture post-load paths to immutable domain/payload policy.
- [x] Preserve authored source/DDC reconstruction and cooked payload-only hard
  failure for every affected asset family.
- [x] Migrate startup and tests from mode mutation to explicit runtime
  construction and remove retired symbols.
- [x] Qualify cooked roots, missing/corrupt payloads, source absence, cache
  hit/miss, and restart behavior.

### Acceptance Gate

- Engine asset types do not read a mutable package-load mode.
- Authored rebuild and cooked hard failure remain explicit and covered.

AnimationClip, EnvironmentLighting, StaticMesh, SkeletalMesh, TerrainHeightmap,
Texture2D, and TextureCube query the immutable payload policy. Affected fixtures
now switch domains only by shutdown plus fresh initialization. Focused AssetCook,
StaticMesh, SkeletalAsset, SkeletalSceneLifecycle, Material, Texture,
TextureCookIntegration, and TerrainHeightmapCook suites pass with source/DDC
fallback, missing/corrupt payload, target/schema/hash, and restart coverage.

## Stage 5: Qualify And Publish Build/Domain Ownership

Dependencies: Stages 0-4 complete.

- [x] Run focused AssetBuildCore, derived-data, cook/payload, runtime-domain,
  affected Engine asset, and restart suites.
- [x] Run complete native qualification, default full build, and hidden-window
  editor smoke without concurrent build processes.
- [x] Update lasting runtime/build/cook and roadmap documentation with the
  implemented cache/host and immutable-domain contracts.
- [x] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record evidence for the M4 exit gate.

### Acceptance Gate

- No unused Build executor abstraction or mutable package-load mode remains in
  production APIs.
- Lasting documentation and validation evidence satisfy the M4 roadmap exit
  gate and unblock M5.

Final qualification on 2026-08-16 passed AssetBuildCoreTests 5/5, AssetCookTests
13/13, StaticMeshTests 68/68, SkeletalAssetTests 34/34,
SkeletalSceneLifecycleTests 1/1, MaterialTests 74/74, TextureTests 66 passed
with 2 existing conditional skips, TextureCookIntegrationTests 1/1, and
TerrainHeightmapCookTests 1/1. The default `Win64-Debug-DurinEditor` `all`
build, complete native-test aggregate, and eight-tick hidden-window Sandbox
editor smoke passed. Retired build-executor and mutable-domain searches are
empty. Asset Packages and Asset Data Lifecycle now own the lasting contracts;
all required documentation validators pass in the final M5 handoff.

## Completion Criteria

- All stages and acceptance gates pass with evidence recorded here.
- AssetBuildCore exposes only production-used cache and host abstractions.
- Authored/cooked domain and payload policy are immutable after AssetCore
  initialization, and all affected asset families retain qualified behavior.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`BuildCache.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildCache.h)
- [`BuildHost.h`](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildHost.h)
- [`AssetBuildCore.cpp`](../../Engine/Source/Developer/AssetBuildCore/Private/AssetBuildCore.cpp)
- [`CookedAsset.h`](../../Engine/Source/Runtime/AssetCore/Public/CookedAsset.h)
- [`AssetLoad.h`](../../Engine/Source/Runtime/AssetCore/Public/AssetLoad.h)
- [`AssetSystem.cpp`](../../Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp)
