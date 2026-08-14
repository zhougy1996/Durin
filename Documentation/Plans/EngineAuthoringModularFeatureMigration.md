# Engine Authoring Modular Feature Migration Plan

Summary: Replace six Runtime Engine process-global authoring callback families with typed modular features owned by StandardAssetImport and GeometryBuild module instances.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Milestones 1 and 2 provide bounded typed invocation, manager-owned module
generations, synchronous feature retirement, asynchronous operation groups,
selected Game Thread drain, and fail-closed native unload. Runtime Engine still
stores Plugin callables for static mesh, texture, terrain, and skeletal uncooked
authoring. Stage 0 will freeze the six feature interfaces and exact legacy
cutover map before provider code moves into module instances.

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

## Current Foundations and Gaps

| Family | Current boundary | Provider | Gap |
| --- | --- | --- | --- |
| Static mesh authoring | `FStaticMeshAuthoringHandlers` global bundle | StandardAssetImport | Three copied Plugin callables and parameterless unregister |
| Static mesh collision | Global collision function pointer/callback | GeometryBuild | Provider lifetime is not owner attributed |
| Texture2D | Global uncooked post-load handler plus editor adapters | StandardAssetImport | Runtime retains Plugin callback |
| TextureCube | Global uncooked post-load handler | StandardAssetImport | Runtime retains Plugin callback |
| Terrain | Global post-load, wait, and source-change handlers | StandardAssetImport | Async work and three callback slots require one owned contract |
| Skeletal payloads | Global mesh and animation loader functions | GeometryBuild | Pair is identity-free and copied across DLL boundary |

## Implementation Stages

### Stage 0: Freeze interfaces and cutover inventory

- [ ] Record every declaration, definition, provider registration, consumer,
  test, and documentation reference for the six legacy families.
- [ ] Define feature names, version 1 method signatures, result/error mapping,
  cardinality, thread rules, and synchronous versus asynchronous ownership.
- [ ] Identify Terrain worker roots, Game Thread continuations, result handles,
  and shutdown releases that must use a StandardAssetImport operation group.
- [ ] Freeze a deletion checklist for every migrated legacy symbol.

#### Acceptance Gate

- Each current callback maps to exactly one typed method and provider module.
- Interface signatures use Runtime Engine/Core-owned data and retain no Plugin
  callable, raw provider pointer, or provider-owned polymorphic result.
- Every async escape path has a named operation group and publication rule.

### Stage 1: Migrate GeometryBuild feature families

- [ ] Add `IStaticMeshCollisionBuildFeature` and
  `ISkeletalDerivedDataFeature` Runtime Engine contracts.
- [ ] Make `FGeometryBuildModule` own provider implementations, registration
  tokens, and any required operation group.
- [ ] Convert static-mesh collision, skeletal-mesh, and animation-clip consumers
  to bounded typed invocation with explicit unavailable/ambiguous results.
- [ ] Delete the legacy collision and skeletal loader registration APIs and add
  focused provider-present, unavailable, ambiguous, and retirement tests.

#### Acceptance Gate

- GeometryBuild unload leaves no migrated callable in Runtime Engine storage.
- Static-mesh collision and both skeletal payload flows retain their current
  functional results and report missing/ambiguous providers deterministically.

### Stage 2: Migrate synchronous StandardAssetImport feature families

- [ ] Add `IStaticMeshAuthoringFeature`, `ITexture2DAuthoringFeature`, and
  `ITextureCubeAuthoringFeature` contracts.
- [ ] Move provider behavior behind StandardAssetImport module-instance feature
  implementations and owner registration tokens.
- [ ] Convert Runtime Engine consumers and editor adapters without retaining
  visitor references or copied callbacks.
- [ ] Delete static-mesh and texture legacy globals/APIs and update focused
  post-load, source-reference, import, and authoring tests.

#### Acceptance Gate

- Static mesh, Texture2D, and TextureCube authoring pass with the provider and
  preserve explicit no-provider behavior without process-global Plugin storage.
- Provider retirement prevents later calls and waits for admitted visitors.

### Stage 3: Migrate Terrain with explicit async ownership

- [ ] Add `ITerrainHeightmapAuthoringFeature` covering uncooked post-load, load
  wait, and source-reference mutation without exposing provider callables.
- [ ] Move Terrain pending-work maps and cancellation policy into owned
  StandardAssetImport provider state.
- [ ] Associate worker roots and Game Thread publication continuations with the
  module operation group and stable abort reasons.
- [ ] Delete all three Terrain callback globals/APIs and add deterministic
  completion, cancellation, source-change, shutdown-drain, and capture-
  destruction tests.

#### Acceptance Gate

- Terrain publication either completes for the current generation or cancels
  without publishing, and module drain reports zero tasks, results, and
  retained callables.
- Runtime Engine retains no Terrain Plugin callable or provider pointer.

### Stage 4: Regression, audit, and handoff

- [ ] Run a repository search proving all frozen legacy symbols and storage are
  deleted and all six feature consumers use bounded invocation.
- [ ] Build affected Runtime, Developer, Editor, and test targets and run their
  focused native suites under the repository workflows.
- [ ] Publish the lasting Runtime Engine authoring contract and update the
  parent roadmap with evidence.
- [ ] Create `Documentation/Plans/DynamicModuleRegistrySafetyAudit.md` only
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

- [Parent roadmap](../Roadmaps/ModularFeatureAndDllUnloadSafety.md)
- [Modular features and module retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [Static mesh authoring boundary](../../Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshAuthoring.h)
- [Texture2D post-load boundary](../../Engine/Source/Runtime/Engine/Public/Texture/Texture2DPostLoad.h)
- [TextureCube post-load boundary](../../Engine/Source/Runtime/Engine/Public/Texture/TextureCubePostLoad.h)
- [Terrain authoring boundary](../../Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapPostLoad.h)
- [Skeletal payload boundary](../../Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalAssetPostLoad.h)
- [StandardAssetImport providers](../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [Terrain authoring policy](../../Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp)
- [GeometryBuild module](../../Engine/Source/Developer/GeometryBuild/Private/GeometryBuildModule.cpp)
