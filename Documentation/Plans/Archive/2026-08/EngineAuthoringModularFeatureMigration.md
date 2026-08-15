# Engine Authoring Modular Feature Migration Plan

Summary: Replace six Runtime Engine process-global authoring callback families with typed modular features owned by StandardAssetImport and GeometryBuild module instances.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

Stage 0 is complete. The six callback families map one-to-one to the feature
contracts and provider modules in the inventory below. Terrain worker and Game
Thread publication work will use one StandardAssetImport-owned
`TerrainAuthoringLoads` operation group; source change uses the group's explicit
per-request supersession cancellation while module retirement closes the group
with the stable module-shutdown abort reason.

Stage 1 is complete. Runtime Engine now defines and invokes
`IStaticMeshCollisionBuildFeature` and `ISkeletalDerivedDataFeature` without
retaining provider callbacks. `FGeometryBuildModule` implements both interfaces
and owns their move-only registrations. The GeometryBuild target compiles; the
focused functional validation is complete: all 34 SkeletalAsset tests and all
68 StaticMesh tests pass, while the existing Core registry suite supplies the
zero/one/many and retirement concurrency matrix. Stage 2 is ready to migrate
the synchronous StandardAssetImport feature families.

Stage 2 is complete. Runtime Engine now invokes typed StaticMesh, Texture2D,
and TextureCube authoring features; all related callback bundles, handler
globals, registration functions, parameterless unregister functions, and
copying accessors are deleted. `FStandardAssetImportModule` owns one provider
object and three generation-bound registration tokens. Native test processes
that do not start the module install the same provider behind an isolated test
owner. StaticMeshTests pass 68/68, TextureTests pass all enabled tests, and the
five-target `asset-import` domain passes.

Stage 3 is complete. Runtime Engine now invokes one
`ITerrainHeightmapAuthoringFeature` for post-load, wait, and source mutation.
StandardAssetImport owns the provider, pending/coalesced maps, registration,
and `TerrainAuthoringLoads` group. Worker roots and Game Thread publishers use
the group scope and cancellation token; source mutation cancels only the
superseded subscriber and an unshared worker. TerrainHeightmapTests pass 11/11,
StaticMeshTests pass 68/68, TextureTests pass all 66 enabled tests, and the
five-target `asset-import` domain passes. The Core async-operation-group tests
continue to cover cancel/drain and retained callable/result destruction. Stage
4 is ready for the deletion audit and lasting contract handoff.

Stage 4 and the plan are complete. Targeted repository searches find none of
the frozen callback types, registration APIs, parameterless unregister APIs,
or process-global authoring storage in live code. All six consumers use
bounded `InvokeSingle<T>` calls. Engine, GeometryBuild, and StandardAssetImport
build; SkeletalAssetTests pass 34/34, StaticMeshTests pass 68/68,
TerrainHeightmapTests pass 11/11, TextureTests pass all 66 enabled tests, and
the five-target `asset-import` domain passes. The lasting ownership and Terrain
async boundary is recorded in Asset Data Lifecycle and the Milestone 4 child
plan is active.

## Goal

Make Runtime Engine invoke optional authoring behavior only through typed,
bounded modular-feature visitors, with provider implementations and registration
tokens owned by their dynamic module instances and no migrated Plugin callable
retained in process-global Engine storage.

## Scope

- Add the six typed feature contracts named by the parent roadmap.
- Migrate Runtime Engine static mesh, Texture2D, TextureCube, Terrain, skeletal
  mesh, and animation-clip consumers to bounded registry invocation.
- Implement StandardAssetImport and GeometryBuild providers as module-instance
  state with move-only registration tokens.
- Bind Terrain and other asynchronous publication chains to module-owned
  operation groups where work can outlive the synchronous feature call.
- Delete the corresponding callback globals, registration functions,
  parameterless unregister functions, and callback-copying accessors.
- Preserve current cooked-build behavior and explicit unavailable/failure
  behavior when no authoring provider is loaded.

## Non-Goals

- Do not migrate asset-import provider selection or build-host contribution
  registries; Milestone 4 owns those specialized registries.
- Do not add ranking or registration-order selection to the generic feature
  registry.
- Do not change asset formats, derived-data keys, import provenance, or authoring
  product structures except where a typed interface needs a stable boundary.
- Do not perform real repeated DLL reload qualification; Milestone 5 owns it.

## Design Decisions and Invariants

- `IStaticMeshAuthoringFeature` is implemented by StandardAssetImport;
  `IStaticMeshCollisionBuildFeature` is independently implemented by
  GeometryBuild because the two providers have distinct unload lifetimes.
- Texture2D, TextureCube, and Terrain authoring features are implemented by
  StandardAssetImport. `ISkeletalDerivedDataFeature` is implemented by
  GeometryBuild and contains both skeletal-mesh and animation-clip payload
  entry points because they share provider lifetime and derived-data policy.
- Runtime Engine calls `InvokeSingle<T>` at the existing decision point and
  never stores the feature reference outside the visitor.
- Zero providers maps to the existing unavailable/no-authoring behavior.
  Multiple providers is an explicit ambiguous failure and never selects by
  registration order.
- Provider objects and registration handles are fields of their module
  instances. Startup registers only after provider state is ready; shutdown
  relies on owner retirement and destroys provider state while mapped.
- A synchronous feature visitor may submit work only after associating it with
  the provider module's operation group. Feature availability is not used as a
  cancellation token.
- Each legacy callback family has one cutover commit state: consumer and
  provider use the typed feature and the old storage/API is deleted together.

## Completed Cutover Inventory

| Family | Removed boundary | Provider | Resolution |
| --- | --- | --- | --- |
| Static mesh authoring | `FStaticMeshAuthoringHandlers` global bundle | StandardAssetImport | Typed feature and module-owned registration |
| Static mesh collision | Global collision callback | GeometryBuild | Typed feature and owner-attributed provider |
| Texture2D | Global uncooked post-load handler | StandardAssetImport | Typed feature and module-owned registration |
| TextureCube | Global uncooked post-load handler | StandardAssetImport | Typed feature and module-owned registration |
| Terrain | Global post-load, wait, and source-change handlers | StandardAssetImport | Owned feature state plus operation group |
| Skeletal payloads | Global mesh and animation loader functions | GeometryBuild | One typed owner-attributed feature |

## Implementation Stages

### Stage 0: Freeze interfaces and cutover inventory

- [x] Record every declaration, definition, provider registration, consumer,
  test, and documentation reference for the six legacy families.
- [x] Define feature names, version 1 method signatures, result/error mapping,
  cardinality, thread rules, and synchronous versus asynchronous ownership.
- [x] Identify Terrain worker roots, Game Thread continuations, result handles,
  and shutdown releases that must use a StandardAssetImport operation group.
- [x] Freeze a deletion checklist for every migrated legacy symbol.

#### Acceptance Gate

- Each current callback maps to exactly one typed method and provider module.
- Interface signatures use Runtime Engine/Core-owned data and retain no Plugin
  callable, raw provider pointer, or provider-owned polymorphic result.
- Every async escape path has a named operation group and publication rule.

### Stage 1: Migrate GeometryBuild feature families

- [x] Add `IStaticMeshCollisionBuildFeature` and
  `ISkeletalDerivedDataFeature` Runtime Engine contracts.
- [x] Make `FGeometryBuildModule` own provider implementations, registration
  tokens, and any required operation group.
- [x] Convert static-mesh collision, skeletal-mesh, and animation-clip consumers
  to bounded typed invocation with explicit unavailable/ambiguous results.
- [x] Delete the legacy collision and skeletal loader registration APIs and add
  focused provider-present, unavailable, ambiguous, and retirement tests.

#### Acceptance Gate

- GeometryBuild unload leaves no migrated callable in Runtime Engine storage.
- Static-mesh collision and both skeletal payload flows retain their current
  functional results and report missing/ambiguous providers deterministically.

### Stage 2: Migrate synchronous StandardAssetImport feature families

- [x] Add `IStaticMeshAuthoringFeature`, `ITexture2DAuthoringFeature`, and
  `ITextureCubeAuthoringFeature` contracts.
- [x] Move provider behavior behind StandardAssetImport module-instance feature
  implementations and owner registration tokens.
- [x] Convert Runtime Engine consumers and editor adapters without retaining
  visitor references or copied callbacks.
- [x] Delete static-mesh and texture legacy globals/APIs and update focused
  post-load, source-reference, import, and authoring tests.

#### Acceptance Gate

- Static mesh, Texture2D, and TextureCube authoring pass with the provider and
  preserve explicit no-provider behavior without process-global Plugin storage.
- Provider retirement prevents later calls and waits for admitted visitors.

### Stage 3: Migrate Terrain with explicit async ownership

- [x] Add `ITerrainHeightmapAuthoringFeature` covering uncooked post-load, load
  wait, and source-reference mutation without exposing provider callables.
- [x] Move Terrain pending-work maps and cancellation policy into owned
  StandardAssetImport provider state.
- [x] Associate worker roots and Game Thread publication continuations with the
  module operation group and stable abort reasons.
- [x] Delete all three Terrain callback globals/APIs and add deterministic
  completion, cancellation, source-change, shutdown-drain, and capture-
  destruction tests.

#### Acceptance Gate

- Terrain publication either completes for the current generation or cancels
  without publishing, and module drain reports zero tasks, results, and
  retained callables.
- Runtime Engine retains no Terrain Plugin callable or provider pointer.

### Stage 4: Regression, audit, and handoff

- [x] Run a repository search proving all frozen legacy symbols and storage are
  deleted and all six feature consumers use bounded invocation.
- [x] Build affected Runtime, Developer, Editor, and test targets and run their
  focused native suites under the repository workflows.
- [x] Publish the lasting Runtime Engine authoring contract and update the
  parent roadmap with evidence.
- [x] Create `Documentation/Plans/Archive/2026-08/DynamicModuleRegistrySafetyAudit.md` only
  after the Milestone 3 exit gate passes.

#### Acceptance Gate

- Static mesh, collision, Texture2D, TextureCube, Terrain, skeletal mesh, and
  animation-clip regressions pass.
- No migrated call site copies or retains a Plugin callable or raw feature
  pointer, and the next specialized-registry audit has an evidence-backed
  inventory baseline.

## Validation Matrix

| Area | Validation | Evidence required |
| --- | --- | --- |
| Interface identity | Core/Engine feature tests | Stable name/version and explicit zero/one/many behavior |
| GeometryBuild | Static mesh and skeletal native tests | Same collision and payload results through typed visitors |
| StandardAssetImport | Import and post-load native tests | Same products and source mutations with module-owned providers |
| Terrain async | Barrier-controlled task tests | Current-generation publish or explicit cancel; zero retained storage |
| Retirement | Module-manager tests | Late calls rejected and admitted calls drained before provider destruction |
| Legacy deletion | Targeted repository search | No migrated callback storage, registration, unregister, or copying accessor |
| Regression | Affected builds and native targets | Runtime/Developer/Editor consumers compile and focused suites pass |

## Definition of Done

- All six typed feature contracts are public from Runtime Engine and use
  Core/Engine-owned boundary types.
- StandardAssetImport and GeometryBuild module instances own their provider
  implementations and registration tokens.
- Every migrated consumer uses bounded registry invocation and explicit
  unavailable/ambiguous handling.
- Terrain asynchronous work participates in module operation close and drain.
- All corresponding process-global callback storage and identity-free
  registration APIs are deleted.
- Focused functional, concurrency, build, and documentation gates pass.

## Deferred Follow-ups

- Asset import, build-host, editor extension, delegate, timer, watcher, render,
  and other specialized registry paths remain in Milestone 4.
- Real dynamic unload/reload stress remains in Milestone 5.

## Related Documentation

- [Parent roadmap](../../../Roadmaps/Archive/2026-08/ModularFeatureAndDllUnloadSafety.md)
- [Modular features and module retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Task System](../../../Runtime/Core/TaskSystem.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)

## Related Code

- [Static mesh authoring boundary](../../../../Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshAuthoring.h)
- [Texture2D post-load boundary](../../../../Engine/Source/Runtime/Engine/Public/Texture/Texture2DPostLoad.h)
- [TextureCube post-load boundary](../../../../Engine/Source/Runtime/Engine/Public/Texture/TextureCubePostLoad.h)
- [Terrain authoring boundary](../../../../Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapPostLoad.h)
- [Skeletal payload boundary](../../../../Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalAssetPostLoad.h)
- [StandardAssetImport providers](../../../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [Terrain authoring policy](../../../../Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp)
- [GeometryBuild module](../../../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildModule.cpp)
