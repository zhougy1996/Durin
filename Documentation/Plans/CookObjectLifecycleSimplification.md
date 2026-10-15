# Cook Object Lifecycle Simplification Plan

Summary: Remove primary-asset ownership of package Cook, unify object eligibility and preparation, and simplify target policy while preserving family DDC contracts.

Last reviewed: 2026-10-15

Status: Completed
Completed: 2026-10-15

## Current Status

Stages 0–5 are complete. Cook now loads and publishes packages independently of
the first asset, captures one registration snapshot, and prepares every eligible
object through the common save path. Stable names are unique across classes.
Engine owns built-ins; standalone captures use scoped access to the same registry.
Source-only admission runs before reuse, while generic capture receives the same
object policy. Platform descriptions expose capabilities without profile state.
The baseline remains production commit `2fca9b889`; historical passing results
are not counted as this plan's acceptance evidence.

### Completion evidence (2026-10-15)

Retention correction (2026-10-15): generated Cook packages, shader libraries,
manifests, state snapshots and the cold-DDC inventory have been removed from Git
at the user's request. The snapshot comparisons below describe the original
qualification only; they are no longer checked-in test inputs. Functional tests
generate their own assets and retain warm-hit, forced-rebuild and publisher-change
invalidation checks. Existing focused codec tests retain deterministic roundtrip,
corruption and legacy-version coverage. The snapshot-only reader case was removed;
continuous byte comparison against pre-migration packages is no longer covered.
Future historical snapshots belong under ignored `Build/` or in CI artifacts.
Retention cleanup validation: `AssetCookTests` passes 44/44 and
`CookFunctionalTests` passes 19/19 in Win64-Debug-DurinEditor. Logs:
`Build/.agent-state/logs/20261015-154341-195798-38260-AssetCookTests.log` and
`Build/.agent-state/logs/20261015-154403-963267-22180-CookFunctionalTests.log`.

- The original combined texture failure was split into Texture2D, Cube and
  Volume cases. All three failed independently before migration in
  `Build/.agent-state/logs/20261015-141556-275566-46428-ctest.log`.
  Missing common preparation caused the failures. Texture2D also required
  finishing an existing request without launching a duplicate build.
- The final Cook functional suite passes 19/19 sequentially in
  `Build/.agent-state/logs/20261015-150008-977437-7556-CookFunctionalTests.log`.
  The preceding 18 cases pass individually in
  `Build/.agent-state/logs/20261015-145301-551487-46824-ctest.log`; the added
  reverse-creation mixed generic/material/instance case passes alone in
  `Build/.agent-state/logs/20261015-150006-162802-33100-ctest.log`.
  Coverage includes referenced nested textures, delayed Texture2D completion,
  warm saves, transient/explicitly omitted textures, both EditorOnly retention
  policies, internal/external source-only references, empty runtime roots,
  declaration conflicts, cancellation and common publication.
- All 18 required CPU targets pass: 1031 tests, no failures or disabled cases.
  Per-target XML is under `Build/NativeTestResults/Win64-Debug-DurinEditor/`.
  `Build/.agent-state/CookLifecycleValidation.json` records exact target counts,
  report paths, build logs and the retained compatibility fixture run.
- Families and Mixed cooked bytes match the immutable Stage 0 fixtures. The
  family test injects real baseline CookState and proves deliberate misses,
  subsequent current-state hits, warm-DDC capture and forced-rebuild parity.
  Material selection and recipe mutations invalidate dependent packages.
  All 31 DDC records emitted by the retained final fixture match baseline keys,
  sizes and SHA-256 values; there are no new keys. Two historical Shader records
  are not emitted by this run. No payload or DDC codec version was changed.
- Publisher identity is now `engine-package` version 2. This intentionally
  invalidates old Engine CookState entries across families. Family behavior has
  one active version; the legacy producer word remains readable and is written
  as 1. Family recipe declarations retain their independent versions.
- Editor and Game `all` builds pass respectively in
  `Build/.agent-state/logs/20261015-145656-092805-7080-cmake.log` and
  `Build/.agent-state/logs/20261015-145422-995934-43180-cmake.log`, covering Engine,
  Sandbox and RoadWeaver. The Game Ninja graph has no TargetPlatform target.
- Affected Vulkan mesh and texture integration consumers compile in
  `Build/.agent-state/logs/20261015-145704-822501-380-cmake.log` and
  `Build/.agent-state/logs/20261015-145722-367041-44780-cmake.log`.
  An earlier broad `@asset-cook` isolation probe also selected texture integration
  cases and exposed missing per-case compiling-manager setup. That setup is fixed
  and compiled; its GPU continuation was not rerun. GPU/application execution is
  not a required gate for this lifecycle/API change, and no native MacOS collision
  qualification is claimed.
- Lasting contracts are updated in Cooking, TargetPlatforms and DerivedDataBuild.
  Changed-document and all-plan validation are required before the completion
  commit, together with a clean diff check.

### Stage 0 contract inventory

The `Durin.dworkspace` projects are Engine, Sandbox and RoadWeaver. Searching
their source/test roots found registration, positional aggregate, callback and
direct-save consumers only in Engine. The frontend owner is `AssetToolMain.cpp`;
test-owned registrations occur in `CookedAssetTests.cpp`, `CookFunctionalTests.cpp`,
`PackageTests.cpp` and `MaterialFunctionCookTests.cpp`. Direct family adapters
also occur in the texture, material, static-mesh DDC and Vulkan preparation tests
listed in Stage 3. `CookedAssetTests.cpp` contains retirement, reentrant owner
destruction and callback-before-owner destruction coverage; preserve all three.
Engine module startup is the intended built-in owner, with lazy access through
the same registration storage for standalone direct-save test hosts.

The selected object policy is:

| Case | `bRetainEditorOnlyData=false` | `bRetainEditorOnlyData=true` |
| --- | --- | --- |
| Runtime object and runtime hard reference | Retain; validate target | Retain; validate target |
| Ordinary EditorOnly field/reference | Strip; do not make a runtime dependency | Retain diagnostic field/reference |
| Source-only object, including secondary export | Omit runtime export; retain build inputs | Omit runtime export; retain build inputs |
| Runtime hard reference to source-only object | Fail with object/field cause | Fail with object/field cause |
| EditorOnly reference to source-only object | Strip | Omit the excluded target reference; never promote the target |
| Transient/template/explicitly omitted subtree | No preparation or export | No preparation or export |
| Explicit root with no runtime exports | Typed empty-runtime-root failure | Typed empty-runtime-root failure |
| Source-only build dependency | Retain source/schema/transitive inputs; no output | Retain source/schema/transitive inputs; no output |

The last three source-only behaviors intentionally replace the current
primary-contributor rejection; current retained editor references can incorrectly
promote a function package into the runtime set. Generic CoreDObject capture must
receive the exclusion policy from Engine, not inspect Engine classes.

Selected baseline targets are `AssetCookTests`, `CookFunctionalTests`,
`AssetPackageTests`, `MaterialFunctionTests` and `TextureTests`; the configured
registry confirms the first two. Existing `CooksSavedFamiliesAndReusesValidatedOutputs`
covers generic, material/instances, all texture families and mesh/collision output
hash parity, warm CookState hits and forced rebuilds. `MixedPackageDeclaresEveryMaterialFamily`
covers secondary/nested material declarations. CMNF has a fixed 137-byte golden
hash in `FCookManifestTests.IsDeterministicAndRejectsCorruptRecords`; CookState v2
has canonical roundtrip, truncation and legacy-v1 rejection coverage. These are
coverage inventory, not yet passing evidence for this execution.

Initial sandbox baseline build failed with MSVC C1902 before tests ran
(`Build/.agent-state/logs/20261015-130819-352558-45772-cmake.log`). The documented
outside-sandbox retry built successfully. Use outside-sandbox DevTool builds on
this host for the remaining stages; no toolchain repair or clean build is needed.

The original compatibility inputs were captured under
`Engine/Tests/Native/AssetTests/Data/CookLifecycleBaseline` (removed by the retention
correction above). They contained actual
family and mixed package bytes, CMNF and CookState v2 dependency/action records,
captured before production changes. The new reader fixture verifies exact
re-encoding and package size/hash consistency. These snapshots came from the
existing functional tests' warm and forced-rebuild parity checks. A later isolated
run with a fresh family DDC reproduced identical bytes for all ten family packages;
`ColdDdcRecords.json` preserves 33 resulting record identities and checksums.
A gated Texture2D worker establishes controlled in-flight compilation before
direct save begins, with RAII release/drain on every failure path.

Evidence from the Win64-Debug-DurinEditor preset (all paths under
`Build/.agent-state/logs/`):

- `20261015-131130-717536-12408-AssetCookTests.log`: 41 existing cases passed;
  new `RejectsDuplicateStableNamesAcrossClasses` failed because the second
  registration returned a live handle. Cleanup retires both handles even on
  the old implementation.
- `20261015-131631-351596-36760-AssetCookTests.log`: new
  `ReadsLifecycleBaselineStateAndManifest` passed in a standalone run.
- `20261015-131712-885191-4268-CookFunctionalTests.log`: new
  `SecondaryFamilyDeclarationsControlReuseAndConflicts` passed alone. It checks
  missing declarations disable reuse, identical declarations coalesce, and
  conflicts leave previously published output unchanged.
- `20261015-131924-584321-49112-CookFunctionalTests.log`: 12 cases passed,
  including all 11 prior cases; two new regressions failed as expected.
  `DirectSavePreparesSecondaryTextureWithoutPostLoad` creates secondary 2D/cube
  and nested volume textures with no platform data; capture fails before
  preparation. `MixedAuthoringOnlyExportsAreFilteredForEitherRetentionPolicy`
  covers both root orders and both editor-data policies and exposes the
  unfiltered material-function export.
- `20261015-131959-636734-4468-ctest.log`: the same three new functional cases
  run in isolated processes (`--isolate --test-jobs 1`); declaration coverage
  passes and both lifecycle regressions reproduce independently.
- `20261015-132049-229850-33248-AssetCookTests.log`: final 43-case suite,
  42 passed and only the duplicate-name regression failed.
- `20261015-132227-415476-4876-ctest.log`: controlled delayed Texture2D
  compilation reproduces the direct-save failure in isolation.
- `20261015-132339-822585-25484-ctest.log`: isolated fresh-DDC family fixture
  passed cold/warm/forced-rebuild parity.
- `20261015-132416-345888-47412-ctest.log`: all 14 functional cases ran in
  isolated processes; 12 passed and only the two known lifecycle regressions
  failed. A separate SHA-256 comparison of this run's ten family package files
  against the checked-in baseline found every file byte-identical.

The regression suite is intentionally red at this characterization checkpoint.
No production behavior, keys or codecs have changed. Stages 1-5 and the full
Editor/Game build gates remain outstanding. A red Stage 0 fixture records a
reproduced defect, not acceptance evidence for the final lifecycle.

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

- [x] Inventory callback/registration consumers across Engine, Sandbox and
  RoadWeaver source/test roots declared by `Durin.dworkspace`, including positional
  aggregate initializers, lifetime tests and direct package-save adapters.
- [x] Capture cold/warm payload bytes, family action identities, CookState v2 and
  CMNF fixtures for material, texture, mesh/collision, generic and mixed packages.
- [x] Add regression fixtures for delayed secondary/nested textures, reordered
  mixed packages, authoring-only secondary exports, duplicate registration names,
  missing declarations and conflicting cross-family declarations.
- [x] Specify empty-root, source-only dependency, runtime hard-reference and
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

- [x] Reject duplicate stable names atomically, including across different classes;
  retain callback code/lifetime pins for active snapshots and safe unregistration.
- [x] Make package loading and capture independent of a primary asset's class.
  Move the sole `AddPackage`/capture decision into the common package workflow.
- [x] Change family registrations toward metadata/declarations and per-object
  preparation; remove publishing privileges from preparation callbacks. Any
  temporary adapter must delegate to the same pipeline and cannot create a second plan.
- [x] Give Engine one owner for built-in initialization; remove duplicate frontend
  ownership and migrate tests to scoped registration/lifetime fixtures.
- [x] Remove root-only classification and define one package serializer identity
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

- [x] Represent family runtime/source-only disposition independently of package
  publication. Remove authoring-only rejection from primary-only callbacks.
- [x] Run metadata admission before cache hits; share the resulting policy with
  live-object preparation and capture through generic omission/filtering contracts.
- [x] Filter source-only secondary objects while retaining material-function source
  dependencies, schema closure and necessary transitive invalidation.
- [x] Keep CoreDObject free of Engine classes: Engine supplies object policy;
  generic capture validates references and uses the selected-object complete plan.
- [x] Handle empty-runtime-root/source-only outcomes explicitly in coordinator,
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

- [x] Replace built-in `ContributeToCook`/friend dispatch wrappers with object
  preparation plus the common save entrypoint; migrate every shared-API consumer.
- [x] Prepare all retained candidate textures, explicitly finish their compilation
  and validate platform data before serialization. Make direct saves independent
  of incidental PostLoad progress and coordinator registration.
- [x] Keep detached material/mesh/collision production, selected material variants,
  target validation and pending mesh-source-mutation checks for every relevant object.
- [x] Serializers only consume valid prepared/platform data and encode fields;
  they do not launch compilation, geometry construction or DDC requests.
- [x] Preserve cancellation, bounded retained products, stale-input rejection,
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

- [x] Replace generic `Supports(ETargetOperation)` workflow gating with explicit
  platform capabilities; move profile validation into Cook, Shader and texture
  boundaries. Do not widen EditorValidation production or unsupported collision.
- [x] Remove profile from platform description/resolution if no capability needs
  it; retain profile in requests, action projections and payload tags where semantic.
- [x] Keep stable platform adapters and explicit Shader identity validation;
  explicit builds remain independent of active RHI capabilities.
- [x] Give Cook behavior one active family version. Preserve legacy nonzero
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

- [x] Prove cold, warm DDC, CookState reuse and forced rebuild paths. A CookHit
  performs no family preparation; a miss with available DDC avoids producer work.
- [x] Compare canonical dependencies and payload bytes for reordered mixed assets,
  selected material variants, all three texture families and mesh/collision.
- [x] Load legacy CookState/CMNF fixtures and verify deliberate misses, accepted
  current hits, reader compatibility and absence of unsafe reuse after filtering.
- [x] Run relevant CPU targets: CookFunctionalTests, AssetCookTests,
  AssetPackageTests, CoreObjectTests, PackageLinkerContractTests,
  PackagePersistenceTests, PackageWriterContractTests, TargetPlatformTests,
  MaterialCompilerTests, MaterialCookTests, MaterialFunctionTests,
  MaterialRuntimeTests, TextureTests, StaticMeshTests, CookedMeshLoadingTests,
  RenderShaderBuilderTests, RenderShaderCacheTests and RenderShaderCookedLibraryTests.
  Confirm target names/selection through the configured registry when implementing.
- [x] Run new cases alone and the Cook functional suite in isolated processes;
  verify registration and asset-runtime cleanup between sequential cases.
- [x] Complete Editor and Game `all` builds, including Engine, Sandbox and
  RoadWeaver consumers, and inspect the Game graph for TargetPlatform leakage.
- [x] Compile affected GPU/application test consumers when API migration reaches
  them. GPU/application execution is required only for changed GPU behavior or an
  explicit gate; do not invent native MacOS collision qualification.
- [x] Update lasting contracts, remove temporary duplicate publishing paths and
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
