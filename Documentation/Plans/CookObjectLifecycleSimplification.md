# Cook Object Lifecycle Simplification Plan

Summary: Remove primary-asset ownership of package Cook, unify object eligibility and preparation, and simplify target policy while preserving family DDC contracts.

Last reviewed: 2026-10-15

Status: Active
Completed:

## Current Status

Planning only. No implementation stage has started. The baseline is commit
`2fca9b889` and the completed [Target Platform Build Policy plan](TargetPlatformBuildPolicy.md).
Its 1016 passing CPU cases and Editor/Game builds are historical baseline evidence,
not acceptance evidence for this plan.

The follow-up review found that declarations cover secondary/nested families but
preparation still depends partly on the primary contributor. Secondary textures
can miss compilation completion, and secondary authoring-only assets bypass the
primary contributor's rejection. Family-name deduplication also assumes name
uniqueness that registration does not enforce.

## Goal

Make package results independent of which top-level object is first. Objects own
eligibility, family inputs and platform-data preparation; one Engine package-save
path owns capture. The coordinator owns dependency evaluation, incremental reuse,
load scopes, output publication and rollback.

Use the UE distinction between object operations and package-root operations,
without copying its complete asynchronous cooker. UE documents object platform
data preparation separately from serialization, reserves root callbacks for the
primary asset, and filters editor-only objects during saving:
[Cook development reference](https://dev.epicgames.com/documentation/unreal-engine/cplusplus-cooking-development-reference?application_version=5.8),
[object filtering](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/IsEditorOnlyObject?lang=en-US).
These are design references; the changes below are Durin-specific decisions.

## Selected Scope and Responsibilities

| Owner | Responsibility after migration | Responsibility removed or excluded |
| --- | --- | --- |
| Core | Stable identities, parsing and explicit adapters | Asset recipes, target availability, object preparation |
| CoreDObject | Generic object exclusion, reference validation, reachability and selected-object field planning | Engine asset types, DDC or compiler calls |
| TargetPlatform | Immutable platform description, Shader target and collision-representation capability | Game/profile workflow admission and generic operation dispatch |
| Engine family registration | Unique stable family identity, Cook behavior version, output disposition, dependency declaration and optional object preparation | Publishing a package from a per-object callback |
| Engine package-save entry | Apply object policy, prepare candidates, own scoped products, capture a package once | Selecting behavior from the first top-level asset |
| Material/texture/mesh/physics families | Typed preparation, recipes, result validation and DDC projections | Generic package publication and global policy hashing |
| Cook coordinator/output store | Metadata admission before reuse, frozen inputs, package loading, one save plan, transactional output | Per-family builder composition and primary-object preparation classification |

Selected simplifications:

- Replace the operational use of `Contribute` with object preparation. The
  preparation context cannot call `AddPackage` or publish files. Keep dependency
  declaration separate because it runs before loading and cache lookup.
- Reuse the existing class-based registration and lifetime ownership; do not add
  a second preparation registry. Engine owns built-in initialization after the
  reflection system is ready. Direct Engine package saves must initialize/access
  the same built-ins without requiring coordinator or CLI registration.
- Require unique registration names as well as unique registered classes.
  Stable names are persistent family identities; handles remain run-local.
- Replace the root-dependent `ClassifyPreparation` callback with factual package
  results from the unified path. Do not invent an aggregation framework or report
  `DdcHit` from an unverified guess about several families.
- Separate platform capabilities from profile admission. Preserve the existing
  Game/EditorValidation acceptance matrix at its owning workflow/family boundary.
- Stop assigning two active versions the same meaning. Keep legacy persisted
  fields until a separate format migration is justified.
- Retain the invocation-scoped detached-product table and its TLS bridge during
  this plan. Adding texture products may extend the current bounded storage;
  a generic artifact store, persistent asset cache or new Archive service is not
  required. Revisit the bridge only after one lifecycle entrypoint exists.

Preserve frozen requests, owner-thread execution, input stability checks,
cancellation, bounded storage, provider lifetimes, and transactional publication.
Cook must not rewrite authored state or accepted material/render programs.
Small in-memory serialization/encoding transformations remain legal; expensive
construction and DDC requests finish before serialization. Unsupported MacOS
collision remains unsupported. Do not merge distinct wire enums, change payload
schemas for API cleanup, or add SDK/device management to TargetPlatform.

## Object and Dependency Semantics

Source build inputs and runtime exports are different sets. A stripped material
function can still invalidate a material through its source bytes, schemas and
transitive dependencies. Do not derive build dependencies only from surviving
exports or drop them when removing a preparation callback.

Before CookState reuse, inspect every top-level object and relevant registered
nested family. Apply metadata-resolvable eligibility and collect every distinct
family's version/declarations. Nested generic DObject fallback must continue to
leave authored expression nodes out of family recipe registration. Identical
cross-family declarations coalesce; conflicts and within-provider duplicates fail.
Missing required declarations disable reuse; failed secondary-family resolution
must not silently authorize a hit.

On a miss, load the package through `FAssetPackageLoadScope::LoadPackage`, rather
than selecting its first asset as the owner of Cook. Candidate selection shares
Transient, template, explicit omission and authoring-only policy with capture.
Prepare each eligible object before capture; CoreDObject still performs final
reference discovery and reachability pruning. Do not assume final reachability
can be obtained by invoking serializers that already require prepared products.

Authoring-only objects are filtered independent of top-level ordering. A mixed
package with runtime assets remains cookable. An explicitly requested package
with no runtime exports produces a typed empty-runtime-root diagnostic; a
source-only build dependency is retained as an input without a runtime output.
Runtime hard references to excluded objects must fail with an object/field cause;
EditorOnly references follow the existing retention policy. Retaining diagnostic
editor fields must not silently promote source-only assets into runtime payloads.
Record the exact `bRetainEditorOnlyData` matrix in Stage 0.

The same save entrypoint is used by coordinator and direct package-save callers.
Admission must precede producer work, and any failure/cancellation publishes no
partial plan. Capture a retained package once; zero-output/source-only outcomes
must not trip the old unconditional one-plan assumption.

## Implementation Stages

### Stage 0: Capture contracts and compatibility fixtures

Dependencies: baseline commit and the review findings above.

Files to inspect and extend:

- `Engine/Tests/Native/AssetTests/Private/CookFunctionalTests.cpp`
- `Engine/Tests/Native/AssetTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialFunctionCookTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureCookTests.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookState.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`

- [ ] Inventory callback/registration consumers across Engine, Sandbox and
  RoadWeaver source/test roots declared by `Durin.dworkspace`, including positional
  aggregate initializers, lifetime tests and direct package-save adapters.
- [ ] Capture cold/warm payload bytes, family action identities, CookState v2 and
  CMNF fixtures for material, texture, mesh/collision, generic and mixed packages.
- [ ] Add regression fixtures for delayed secondary/nested textures, reordered
  mixed packages, authoring-only secondary exports, duplicate registration names,
  missing declarations and conflicting cross-family declarations.
- [ ] Specify empty-root, source-only dependency, runtime hard-reference and
  `bRetainEditorOnlyData` outcomes before changing graph selection.

Compatibility: tests and fixture capture do not change production keys or codecs.
Exit: preserve failing-case evidence, record exact selected test targets, and
resolve initialization/extension-consumer questions without adding another registry.

### Stage 1: Establish registration identity and one package publisher

Dependencies: Stage 0.

Migration files:

- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- `Engine/Source/Runtime/Engine/Public/Asset/CookDependencies.h`
- `Engine/Source/Runtime/Engine/Private/Asset/CookCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookDependencyDiscovery.h`
- `Engine/Source/Runtime/Engine/Private/Asset/CookDependencyDiscovery.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/EngineModule.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- Registration and capture fixtures in `CookedAssetTests.cpp`,
  `CookFunctionalTests.cpp`, `PackageTests.cpp` and `MaterialFunctionCookTests.cpp`.

- [ ] Reject duplicate stable names atomically, including across different classes;
  retain callback code/lifetime pins for active snapshots and safe unregistration.
- [ ] Make package loading and capture independent of a primary asset's class.
  Move the sole `AddPackage`/capture decision into the common package workflow.
- [ ] Change family registrations toward metadata/declarations and per-object
  preparation; remove publishing privileges from preparation callbacks. Any
  temporary adapter must delegate to the same pipeline and cannot create a second plan.
- [ ] Give Engine one owner for built-in initialization; remove duplicate frontend
  ownership and migrate tests to scoped registration/lifetime fixtures.
- [ ] Remove root-only classification and define one package serializer identity
  for persisted save-plan bookkeeping. Preserve distinct family versions in the
  dependency set, not in first-asset fields.

Compatibility: keep the CookState codec readable. A changed package serializer
identity may deliberately cause one safe miss for migrated Engine packages;
record that breadth rather than claiming all unrelated CookState hits survive.
Exit: reordered generic/material/instance packages have equivalent output and
canonical family dependencies; duplicate names cannot swallow a callback, version
or missing-declaration state. No family owns package publication.

### Stage 2: Unify object eligibility and authoring-only filtering

Dependencies: Stage 1 identity/publisher contract.

Migration files:

- `Engine/Source/Runtime/Engine/Private/Asset/CookDependencyDiscovery.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedPreparation.h`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedPreparation.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerCapture.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageSerialization.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/PackageCapture.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/PackageCapture.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DefaultDeltaPlan.cpp`
- `Engine/Tests/Native/AssetTests/Private/PackageTests.cpp` and `CookFunctionalTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialFunctionCookTests.cpp`

- [ ] Represent family runtime/source-only disposition independently of package
  publication. Remove authoring-only rejection from primary-only callbacks.
- [ ] Run metadata admission before cache hits; share the resulting policy with
  live-object preparation and capture through generic omission/filtering contracts.
- [ ] Filter source-only secondary objects while retaining material-function source
  dependencies, schema closure and necessary transitive invalidation.
- [ ] Keep CoreDObject free of Engine classes: Engine supplies object policy;
  generic capture validates references and uses the selected-object complete plan.
- [ ] Handle empty-runtime-root/source-only outcomes explicitly in coordinator,
  manifest staging and diagnostics, with no partially published output.

Compatibility: selection/admission changes must invalidate affected old CookState
outputs, including previously accepted authoring-only mixed packages. Do not rely
on a new check reached only after an old CookHit. No family DDC schema change is
implied by filtering package exports.
Exit: authoring-only object position does not change eligibility; runtime reference
failures retain paths; source changes still invalidate dependent runtime materials.

### Stage 3: Prepare every eligible object through the common save path

Dependencies: Stages 1-2. Keep intermediate stages buildable; migrate an asset
family end-to-end before removing its compatibility adapter.

Migration files:

- `Engine/Source/Runtime/Engine/Private/Asset/CookedPreparation.h` and `.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerCapture.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCookedData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`, `TextureCube.cpp`,
  `VolumeTexture.cpp` and `Texture2DCompilation.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCook.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Physics/PhysicsCookHelper.cpp`
- Direct `ContributeEngineCookAsset` consumers in `CookedAssetTests.cpp`,
  `PackageTests.cpp`, `TextureCubeTests.cpp`, `Texture/TextureCookTests.cpp`,
  `Texture/TextureBuildTests.cpp`, `Texture/VolumeTextureSourceImportTests.cpp`,
  `Materials/MaterialCookTests.cpp`, `StaticMeshDerivedDataCacheTests.cpp` and
  `StaticMeshRenderPreparationVulkanTests.cpp`.

- [ ] Replace built-in `ContributeToCook`/friend dispatch wrappers with object
  preparation plus the common save entrypoint; migrate every shared-API consumer.
- [ ] Prepare all retained candidate textures, explicitly finish their compilation
  and validate platform data before serialization. Make direct saves independent
  of incidental PostLoad progress and coordinator registration.
- [ ] Keep detached material/mesh/collision production, selected material variants,
  target validation and pending mesh-source-mutation checks for every relevant object.
- [ ] Serializers only consume valid prepared/platform data and encode fields;
  they do not launch compilation, geometry construction or DDC requests.
- [ ] Preserve cancellation, bounded retained products, stale-input rejection,
  nested-capture isolation and cleanup on all terminal paths. Existing texture
  completion may apply transient platform data; authored state and accepted
  material/render programs must not be rewritten.

Compatibility: equivalent retained objects/settings preserve family payload bytes
and action identity. Adding readiness is not a reason to bump a texture codec.
Exit: delayed Texture2D/Cube/Volume secondary and referenced nested objects cook
correctly on cold and warm paths; omitted objects initiate no work; direct package
saves behave like coordinator saves; each retained package is captured once.

### Stage 4: Simplify target capabilities and version ownership

Dependencies: stable lifecycle from Stage 3.

Migration files:

- `Engine/Source/Developer/TargetPlatform/Public/TargetPlatform.h`
- `Engine/Source/Developer/TargetPlatform/Private/TargetPlatform.cpp`
- `Engine/Source/Developer/ShaderBuild/Private/ShaderLibraryProducer.cpp`
- `Engine/Source/Developer/TextureCompressor/Private/Texture/TextureBuildTarget.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramCompiler.cpp` and `MaterialCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Physics/PhysicsCookHelper.cpp` and `PhysicsCookDerivedDataKey.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookCoordinator.cpp`,
  `CookDependencyDiscovery.cpp`, `EngineCookContributors.cpp`, `CookOutputInternal.h` and `CookState.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- `Engine/Tests/Native/AssetTests/Private/TargetPlatformTests.cpp` and `CookedAssetTests.cpp`

- [ ] Replace generic `Supports(ETargetOperation)` workflow gating with explicit
  platform capabilities; move profile validation into Cook, Shader and texture
  boundaries. Do not widen EditorValidation production or unsupported collision.
- [ ] Remove profile from platform description/resolution if no capability needs
  it; retain profile in requests, action projections and payload tags where semantic.
- [ ] Keep stable platform adapters and explicit Shader identity validation;
  explicit builds remain independent of active RHI capabilities.
- [ ] Give Cook behavior one active family version. Preserve legacy nonzero
  `FamilyProducerVersion` wire fields as compatibility fields with documented
  deterministic values; do not silently reinterpret them as recipe versions.
  Actual recipe/function/constants/output versions remain family declarations.

Compatibility: platform API cleanup preserves the target/profile acceptance matrix
and equivalent DDC inputs. Version ownership may deliberately change CookState
identity, but does not require a binary codec/layout change.
Exit: all shared callers are migrated, no generic operation switch remains, and
Game has no TargetPlatform module dependency.

### Stage 5: Qualify compatibility and finish contract migration

Dependencies: Stages 0-4.

Files: stage-owned tests and codecs above, plus
`Documentation/Runtime/Assets/TargetPlatforms.md`, `Cooking.md`,
`DerivedDataBuild.md`, and this plan's status/evidence.

- [ ] Prove cold, warm DDC, CookState reuse and forced rebuild paths. A CookHit
  performs no family preparation; a miss with available DDC avoids producer work.
- [ ] Compare canonical dependencies and payload bytes for reordered mixed assets,
  selected material variants, all three texture families and mesh/collision.
- [ ] Load legacy CookState/CMNF fixtures and verify deliberate misses, accepted
  current hits, reader compatibility and absence of unsafe reuse after filtering.
- [ ] Run relevant CPU targets: CookFunctionalTests, AssetCookTests,
  AssetPackageTests, CoreObjectTests, PackageLinkerContractTests,
  PackagePersistenceTests, PackageWriterContractTests, TargetPlatformTests,
  MaterialCompilerTests, MaterialCookTests, MaterialFunctionTests,
  MaterialRuntimeTests, TextureTests, StaticMeshTests, CookedMeshLoadingTests,
  RenderShaderBuilderTests, RenderShaderCacheTests and RenderShaderCookedLibraryTests.
  Confirm target names/selection through the configured registry when implementing.
- [ ] Run new cases alone and the Cook functional suite in isolated processes;
  verify registration and asset-runtime cleanup between sequential cases.
- [ ] Complete Editor and Game `all` builds, including Engine, Sandbox and
  RoadWeaver consumers, and inspect the Game graph for TargetPlatform leakage.
- [ ] Compile affected GPU/application test consumers when API migration reaches
  them. GPU/application execution is required only for changed GPU behavior or an
  explicit gate; do not invent native MacOS collision qualification.
- [ ] Update lasting contracts, remove temporary duplicate publishing paths and
  obsolete helpers, record exact reports/logs and any unrun required gate, then
  mark this plan complete only after all required checks pass.

Use the repository [build workflow](../Agents/BuildAndRun.md),
[test workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md). Implementation commits must
carry `Plan: Documentation/Plans/CookObjectLifecycleSimplification.md` and the
exact `Stage: Stage <N>: <stage title above>` provenance. This planning-only commit
must not claim a stage has been implemented.

## DDC and CookState Compatibility Rules

| Surface | Required treatment |
| --- | --- |
| Material.Translate / Shader DDC | Preserve separation and equivalent semantic inputs; no active-device limits, scheduler state or registration handles in keys. |
| Texture and mesh/physics DDC | Keep family recipes, action/constants/output versions and valid Win64 actions unless their meaning or bytes change. Availability and waiting are not content identity. |
| Family payloads and platform enum wire values | Keep current readers and schemas; establish byte parity for unchanged selected outputs. Filtering may change package contents without changing family codecs. |
| Dependency declarations | Retain source-only build inputs, canonical uniqueness and relevant family policy/version projections; never use an opaque global policy hash as a substitute. |
| CookState v2 / CMNF | Keep layouts/readers initially. Distinguish package serializer identity from per-family behavior/recipe inputs. Legacy identities safely miss when ownership or eligibility changes. |
| Migration reuse cost | A one-time CookState miss across migrated Engine packages is allowed if the package publisher identity changes. Family DDC remains reusable; measure/report the breadth and ensure subsequent warm runs hit. |
| Unsafe prior outputs | Reject metadata-ineligible inputs before reuse or change the affected persistent dependency identity. A check only reached after a cache miss is insufficient. |

Record actual compatibility decisions and fixture results in the owning stage.
Do not bump a version merely for a renamed C++ API, promise all old keys survive,
or require a user-wide DDC deletion to make the implementation correct.
