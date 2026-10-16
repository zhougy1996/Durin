# Target Platform Build Policy Plan

Summary: Introduce immutable target policy resolution and explicit family format requirements across material, physics, texture DDC, and offline Cook without moving asset execution into platform policy.

Last reviewed: 2026-10-15

Status: Archived
Completed: 2026-10-15

## Current Status

Stages 0-6 are complete. Stage 6 closes the review findings for mixed/nested
family Cook identity, transient/omitted object preparation and cooked-only
material configuration selection, and removes the single-choice Shader settings
wrapper. Final Editor/Game all builds and 1016 CPU cases across 17 targets pass;
all 11 Cook functional cases also pass in independent processes. See the Stage 6
evidence below. Game's generated graph excludes TargetPlatform. MacOS collision
remains explicitly unsupported; GPU/application smoke execution was not required
or run.

The completed [material async build plan](MaterialAsyncDerivedDataBuild.md)
is the baseline: retain translation/Shader cache separation, shared flights,
bounded execution, and generation-safe application. Coordinate overlapping DDC
changes with [DDC optimization](../../DerivedDataCacheOptimization.md); this plan does
not replace its storage or execution work.

## Goal

Give target interpretation one authority and make each build consume explicit,
immutable target requirements. Preserve ownership of family recipes, DDC actions,
object publication, and Cook transactions. Fail unsupported combinations before
expensive production or output publication, while allowing valid cached products
to be used without an available local producer.

Required references: [build protocol](../../../Runtime/Assets/DerivedDataBuild.md),
[Cook contract](../../../Runtime/Assets/Cooking.md),
[material lifecycle](../../../Runtime/Rendering/MaterialSystem.md#compile-lifecycle-and-cooked-programs),
[module ownership](../../../Workspace/CodeModules.md),
[build workflow](../../../Agents/BuildAndRun.md), and
[test workflow](../../../Agents/Testing.md).

## Selected Architecture

### Dependency direction

- Add `Engine/Source/Developer/TargetPlatform` with `TargetPlatform.dmodule`,
  `CMakeLists.txt`, and a small value-oriented public interface.
- `TargetPlatform` depends privately on `Core` and publicly on `RHI` only where
  its public values expose the existing `FShaderTargetIdentity`. Declare Core
  publicly if the module tooling requires it for public Core values. It must not
  depend on Engine, RenderCore, Renderer, Physics, PhysicsCore, DDC, or builders.
- Put only shared platform/profile identities and stable name conversion in a
  proposed `Core/Public/Misc/TargetIdentity.h` and matching private source.
  Keep generic Archive target storage in Core. Do not place project settings,
  material quality, collision algorithms, or a platform registry in Core.
- Editor/tool Engine paths gain an optional private TargetPlatform dependency.
  ShaderBuild and TextureCompressor gain private dependencies where they consume
  policy. Consumers receive resolved values rather than querying a mutable global
  service during execution. MeshBuilder gains no dependency unless the audit
  identifies a real target-policy consumer.
- Register the module in `Engine/Engine.dproject`, editor/tool inventories and
  `DurinAssetTool` build/bootstrap wiring. DurinGame must not acquire a
  TargetPlatform library or provider dependency. Runtime readers use lower-level
  format contracts and shared identities, never the Developer policy service.
- Keep `FShaderTargetIdentity`, code-format and backend contracts in RHI.
  Preserve family public contracts in their owning modules. Compatibility
  adapters may retain existing enum names and wire values during migration.

### Policy and execution ownership

| Owner | Owns | Does not own |
| --- | --- | --- |
| TargetPlatform | Target parsing, stable names, platform capabilities, selected Shader targets, immutable resolved settings and format constraints | Assets, compiler execution, DDC keys/sessions, task scheduling, Cook state/output |
| Material family in Engine | Graph snapshots, legal material variants, material macros and stages, translation action, result assembly and publication | Host-derived implicit Cook target selection |
| ShaderBuild | Compiler/toolchain identity, source closure, Shader action, producer availability, compilation and cooked library production | Cook package transaction or material object state |
| Physics family | Collision representation contracts, recipe settings/versions, action construction, algorithms and typed assembly | Platform-name parsing or unrelated target configuration |
| Texture family / TextureCompressor | Usage-dependent settings, legal format selection within platform constraints, compression recipe and typed validation | A duplicate platform/profile support table |
| DerivedDataCache | Generic immutable action, resolution, execution/cache protocol and record persistence | Target-platform registry or family policy |
| Cook | Frozen run inputs, contributor capture, dependency/reachability policy, cache reuse, preparation, budgets and transactional publication | Shader/physics format implementation |

Resolve three different questions explicitly: whether a target is known, whether
a family supports the requested representation/operation, and whether a local
producer is currently executable. Providers report the last question; Cook
aggregates it. Missing SDK/compiler must not reject an otherwise valid Cook or
DDC hit. Cold requests fail with a typed availability error when production is
required. Runtime RHI discovery is not an authority for an explicit Cook target.

Capture configuration into owned immutable values at run/request admission.
Families own configuration schemas for quality, feature/pass selections, and
recipe settings; TargetPlatform supplies platform constraints. Resolve the
intersection in the family. Preserve meaningful distinctions between build,
runtime-read, Game Cook and EditorValidation capabilities.

No device discovery, deployment, SDK installation, remote execution, new worker
pool, general build graph, MaterialBuild/PhysicsCooking extraction, or standalone
Cooker extraction is required by this plan. Explicit detached preparation in
Stage 5 is in scope; moving whole modules is not.

## Observed Migration Baseline

These are static observations, not runtime qualification results:

- Engine `Asset/CookedAsset.h`, `Asset/PayloadTargetPlatform.h`, RHI
  `RHIDefinitions.h`, RenderCore `Shader/ShaderCookedLibrary.h`, and Core
  `Serialization/Archive.h` expose separate target/profile representations.
- CookCoordinator, DurinAssetTool, MaterialCook, TextureDerivedData and
  TextureBuildTarget repeat platform/profile checks or conversions.
- MaterialCook selects Metal/Vulkan and enumerates every supported material
  quality/feature combination inside `SerializeCooked`.
- StaticMeshCook accepts MacOS/Game and passes that Archive target to collision
  serialization. PhysicsDerivedData's `ResolvePhysicsTarget` accepts only Win64;
  the collision branch therefore encounters a target rejection. The physics
  action also fixes its target constant to Win64. Stage 0 must reproduce the
  bounded path before selecting a compatibility change.
- TextureDerivedData accepts EditorValidation while TextureCompressor's target
  helper accepts Game only. Determine intended operation-specific support rather
  than making every check identical.
- CookBuildProviders manually assembles family versions. Family-owned identity
  declarations should replace that coupling; versions do not move into platform
  policy.

Stage 1 evidence: TargetPlatformTests passed 2/2 cases, the editor `all` build
passed, and `build --preset Win64-Debug-DurinGame --target Engine` passed.
The generated Game build graph contains no TargetPlatform module.
Logs: `Build/.agent-state/logs/20261015-104434-948746-47720-TargetPlatformTests.log`,
`Build/.agent-state/logs/20261015-104827-703219-27180-cmake.log`, and
`Build/.agent-state/logs/20261015-104846-893528-40984-cmake.log`.
Distinct existing enum types and wire values are preserved; descriptors and
persistent schemas are unchanged. CLI accepts canonical names in addition to
the existing lowercase spelling; unknown names and Archive target values no
longer silently select Win64.

Stage 2 evidence (Win64-Debug-DurinEditor): material compiler 84, lifecycle 6,
material Cook 7, Shader builder 30, Shader cache 14, cooked library 7, and Shader
Cook integration 1 cases passed. Reports are under
`Build/NativeTestResults/Win64-Debug-DurinEditor/` with those target names.
Matrix logs span `20261015-105524-075793-45632-MaterialCompilerTests.log` through
`20261015-105738-108709-1404-RenderShaderCookIntegrationTests.log` in
`Build/.agent-state/logs/`. The editor all build passed before the matrix;
the matrix also rebuilt the final coordinator adapter. No DDC descriptors or
payload codecs changed. Explicit material targets now use deterministic family
limits instead of the active device's limits; actual resolved inputs continue
to determine the existing action identity.

Sequencing refinement: Stage 2 establishes canonical material variant selection;
Stage 4 owns freezing/declaration plumbing and Stage 5 consumes selections during
detached preparation. This keeps selection policy separate from Cook integration.

Stage 3 evidence (Win64-Debug-DurinEditor): editor all build passed; target
contracts 2, StaticMesh/physics 186, texture 150, and Cook functional 6 cases
passed. Reports use those target names under the existing native report root.
Build log: `Build/.agent-state/logs/20261015-105914-648382-47664-cmake.log`;
matrix logs span `20261015-105943-005848-25432-TargetPlatformTests.log` through
`20261015-110058-741183-40660-CookFunctionalTests.log`.
Physics capture now owns its explicit payload target and passes it into actions;
the Win64 canonical constants/descriptor remain unchanged. Unsupported MacOS
collision is rejected before render/geometry production. PhysicsCore's existing
algorithms remain the shared authority for both direct and DDC paths.
Texture operation support is centralized, preserving metadata-only
EditorValidation and existing family format selection/validation. Cube and volume
use the same compressor helper. StaticMesh render's existing intermediate
identity is unchanged; target-specific disk encoding still belongs to its codec.
No native macOS qualification was run; collision support remains unadvertised.

Stage 4 evidence (Win64-Debug-DurinEditor): editor all build, Cook functional
7 cases, Cook state/codec 41 cases, and Shader Cook integration 1 case passed.
Logs: `Build/.agent-state/logs/20261015-110452-917460-7964-cmake.log`,
`Build/.agent-state/logs/20261015-110748-350462-41480-CookFunctionalTests.log`,
`Build/.agent-state/logs/20261015-110816-739606-41736-AssetCookTests.log`, and
`Build/.agent-state/logs/20261015-110819-418538-22576-RenderShaderCookIntegrationTests.log`.
The coordinator copies the request before callbacks and canonicalizes the owned
material selection. Family callbacks now own version declarations in their
material/texture/StaticMesh source roots; the central string dispatcher is removed.
New material/configurations declarations cause safe misses for old material Cook
dependency sets. Texture/mesh recipe bytes, DDC descriptors, CookState v2 and all
payload codecs remain unchanged. Preparation consumes the selected variants in
Stage 5; Stage 4 tests establish invalidation and snapshot isolation first.
Final cache-only/producer-free qualification is recorded in Stage 5 evidence below.

Stage 5 refinement before implementation: synthetic ProgramData/RenderData/
CollisionData fields are custom Archive fields, so reflected property overrides
cannot supply them. Use a bounded invocation-scoped detached product table at
Engine's package-capture boundary instead. Key records by object handle, package
revision and exact Archive target; serializers only retrieve prepared values.
Preserve the existing direct Engine package-save entrypoints by preparing before
capture. Keep CoreDObject and generic Archive independent of asset recipes.

## Stage 5 Completion Evidence

Engine package capture now owns a bounded scoped detached-product table before
Archive discovery/emission. Material and StaticMesh serializers consume exact
handle/revision/target products and never initiate producer work. Material
preparation reads authored snapshots independently of accepted render state;
selected variants flow from the frozen request into actual DMAT contents.
Direct Engine package-save APIs install the same preparation. Cancellation is
forwarded through serialization options into preparation and mesh build tokens.
No payload schema, DDC descriptor/bucket, CMNF or CookState version was changed.
Equivalent default selections preserve serialized family contents; changing
explicit selections changes only relevant Cook dependencies and selected outputs.

Win64-Debug-DurinEditor final `all` build passed:
`Build/.agent-state/logs/20261015-112207-821080-22988-cmake.log`.
Win64-Debug-DurinGame final `all` build passed:
`Build/.agent-state/logs/20261015-112850-328127-46796-cmake.log`.
The Game graph was separately searched and contains no TargetPlatform module.
Shared consumers in Engine, Sandbox and RoadWeaver were compiled by both all builds.

The final CPU coverage totals 999 passed cases across 19 targets, with one
existing symlink-privilege skip and no failures. TargetPlatform's unchanged
contract result from Stage 3 is reused; CookFunctionalTests ran after final
preparation changes. All other targets ran in the final qualification matrix.

| Target | Passed | Skipped |
| --- | --- | --- |
| AssetPackageTests | 189 | 0 |
| AssetCookTests | 41 | 0 |
| MaterialCompilerTests | 84 | 0 |
| MaterialFunctionTests | 30 | 0 |
| MaterialCookTests | 7 | 0 |
| MaterialRuntimeTests | 92 | 0 |
| MaterialCompileLifecycleTests | 6 | 0 |
| MaterialPackageTests | 6 | 0 |
| StaticMeshTests | 186 | 0 |
| TextureTests | 150 | 0 |
| CookedMeshLoadingTests | 4 | 0 |
| RenderShaderBuilderTests | 30 | 0 |
| RenderShaderCacheTests | 14 | 0 |
| RenderShaderCookedLibraryTests | 7 | 0 |
| RenderShaderCookIntegrationTests | 1 | 0 |
| DerivedDataBuildTests | 83 | 0 |
| DerivedDataCacheTests | 59 | 1 |
| CookFunctionalTests | 8 | 0 |
| TargetPlatformTests | 2 | 0 |

GoogleTest reports are `Build/NativeTestResults/Win64-Debug-DurinEditor/<Target>.xml`.
Final matrix logs span `20261015-112227-580896-41420-AssetPackageTests.log`
through `20261015-112809-704768-30120-DerivedDataCacheTests.log` under
`Build/.agent-state/logs/`. CookFunctionalTests log is
`20261015-112123-076136-8460-CookFunctionalTests.log`. The sole skipped DDC case
reports that the host account cannot create a test symlink; it is not a platform
policy failure or an initialization pass.

The two new frozen-request and detached-variant cases also passed independently
via the test launcher's isolation mode (one process per case, one scheduling slot).
Report: `Build/NativeTestResults/Win64-Debug-DurinEditor/TargetPolicyIsolated.xml`;
log: `Build/.agent-state/logs/20261015-113114-093117-34304-ctest.log`.
Coverage includes source preservation, relevant-policy invalidation and unrelated
reuse, warm Cook with no additional Shader compilations, producer-free refusal
of unprepared serialization, selected-variant decoding, cancelled preparation,
corruption/repair and transactional rollback, cooked-only loading, and generic
cache-only positive/negative/schema validation without local build.

Lasting contracts are in [target policy](../../../Runtime/Assets/TargetPlatforms.md),
the linked Cook/build/material/physics contracts, StaticMesh building, and module
ownership. No native macOS collision reader or GPU qualification was claimed.

## Stage 0 Decisions and Evidence

Baseline: `76434fce2`, Win64-Debug-DurinEditor, MSVC 14.44.35207.
Searches of Sandbox and RoadWeaver source/test roots found no direct consumers of
the five target/profile enum families. Engine owns all current consumers;
runtime Archive adapters also serve package loading and texture cooked loading.
The module generator filters optional dependencies by enabled runtime variant,
so Engine can consume TargetPlatform privately in tools without adding it to Game.

Selected API: Core keeps distinct existing enum types (including forward-declared
enum ABI) in one TargetIdentity header, plus fallible parsing and stable names.
Developer TargetPlatform returns an immutable resolved value with an explicit
Shader target and operation capabilities. Core owns no policy. Existing build
request values and Cook requests own configuration capture; no mutable registry
or independent configuration loader is introduced. Family quality and format
settings remain typed by their family and are projected into Cook declarations.

Initial MacOS collision policy: unsupported until native reader qualification;
reject before render/collision production when collision is requested. Do not
change Win64 collision action bytes or advertise cross-platform DDC sharing.
Texture EditorValidation is a metadata/key-validation capability at baseline;
the compressor produces Game recipes only. Preserve this distinction explicitly.
Serialization preparation will use detached, invocation-scoped save overrides;
it must preserve existing direct package-save callers and avoid persistent asset
mutation. Determine exact overrides during Stage 5 against existing APIs.

Compatibility baseline: platform IDs Invalid/Unknown=0, Win64=1, MacOS=2;
profile IDs Invalid=0, Game=1, EditorValidation=2. Physics build function,
constants and output schema are 2/2/2; collision payload schema is 3. DMAT
payload schema is 13, DSLB is 5, CookState is 2, CMNF is 1. Material normalization,
generator and adapter revisions are 7/11/11. No descriptor is changed in Stage 0.
Existing canonical action tests remain the executable identity baseline.

CPU evidence: the original five CookFunctionalTests cases passed in the full
run; the new fixture then passed alone after fixing its package path:
`DevTool.bat test CookFunctionalTests FCookFunctionalTests.MacOSMeshCollisionTargetBaseline --report`.
Log: `Build/.agent-state/logs/20261015-104043-324364-40380-CookFunctionalTests.log`.
Full baseline run log: `Build/.agent-state/logs/20261015-103952-058187-14232-CookFunctionalTests.log`.
Sandboxed compilation initially hit MSVC C1902; elevated launcher execution
resolved the PDB service restriction. No cache or build-directory deletion.

Representative SHA256 digests from the full baseline run's retained
`CookFailures/Output` (run `run-p49280-8163c19d76aa43f09de5a5b8d531fe5d`):

| File | SHA256 |
| --- | --- |
| CookState.bin | `16AEFD7DEC6C6D17630CF31EBF9D9670E9E8558F40FD66D95BEA334B89ABCB83` |
| CookTests/Saved.dasset | `45E6F507329708D925D540FFF823E0DEE160AEF68F3B652080050D4FE3D221FB` |
| CookManifest.bin | `CB8E178FE0221CD60F08660FEDFCE6A811AA966E370C759E3113EA296130977E` |

These record the exact fixture outputs, not a claim that randomized authored
identities produce identical files across unrelated fixture invocations.

## DDC and Persistent Compatibility

| Surface | Required policy |
| --- | --- |
| Identity/header relocation | Preserve explicit wire numbers and canonical bytes. Moving names/modules alone must not invalidate actions or files. Reject unknown values through fallible adapters. |
| Material.Translate | Preserve canonical translation identity. Bind only target fields that change normalization or generated source; do not add the entire target snapshot or Shader/compiler identity. |
| Shader DDC | Continue binding actual Shader target, options, compiler and source closure. Identical resolved requests retain identical actions. Host/SDK discovery paths do not enter keys. |
| Physics DDC | Separate destination platform, collision representation and recipe identity. Keep existing Win64 action bytes initially. A changed meaning/layout requires the affected function/constants/output version transition and explicit cold invalidation. |
| Cross-platform physics sharing | Permit only after semantic representation and cross-platform readers are proven compatible. Geometry DDC sharing does not imply identical final platform-tagged payload bytes. |
| Texture DDC | Preserve identical resolved recipe identity. Add only format/settings that affect the output; do not invalidate textures for unrelated Shader settings. |
| CookState and package outputs | Declare family-relevant resolved policy before lookup. Changed variant sets/format policy invalidate affected packages and auxiliary libraries. Preserve current codecs unless a documented transition is necessary. |
| Cache storage | No cache record schema, bucket rename, storage migration, cache purge or new cache backend is required. |

Each identity-changing stage records old/new descriptors, affected buckets and
Cook contributor versions, whether old persisted data remains readable, and
which reuse is deliberately lost. Do not choose version numbers until the
implementation baseline is inspected. Old records may remain as unreachable
entries; never delete the user's DDC to demonstrate correctness.

Policy identity is a family-specific canonical projection, not one global hash.
Exclude paths, timestamps, scheduling, module load state and execution
availability. Preserve existing relevant identity fields until evidence supports
a deliberate migration. Cook dependency identity and DDC identity are separate.

## Implementation Stages

### Stage 0: Audit target contracts and capture compatibility baselines

Dependencies: current source and the referenced contracts; no new module.

- [x] Search affected symbols across source/test roots of every project in
  `Durin.dworkspace`: Engine, Sandbox and RoadWeaver. Record actual consumers,
  module inventories and runtime/tool load paths, including non-editor callers.
- [x] Audit the baseline files listed above plus Engine
  `Private/Asset/CookedArchiveTarget.h`, `CookBuildProviders.cpp`,
  `EngineCookContributors.cpp`, `Private/Physics/PhysicsBuildFunction.cpp`,
  `PhysicsCookHelper.cpp`, and `Private/StaticMesh/StaticMeshCook.cpp`.
- [x] Record existing enum wire values, build descriptors/action fixtures,
  supported payload versions and representative CookState/package digests.
- [x] Reproduce MacOS-target StaticMesh collision rejection with a CPU fixture;
  distinguish no-collision success from collision-bearing failure. Record
  EditorValidation behavior and current invalid-target fallbacks.
- [x] Decide the exact descriptor API, project-setting capture owner, format
  contract locations and adapter retirement sequence. Select initial MacOS
  collision policy: explicit unsupported representation until qualification, or
  a separately qualified implementation. Do not declare support speculatively.

Compatibility: read-only audit/fixtures establish the baseline; no production
identity or format changes. Validation: targeted CPU cases, consumer/dependency
inventory, and registry-selected test baselines under the test workflow.
Exit: reviewed baseline and recorded decisions resolve the listed questions.

### Stage 1: Introduce shared identities and the TargetPlatform module

Dependencies: Stage 0; Core identities -> RHI contracts -> TargetPlatform policy.

- [x] Add proposed Core target identity files and Developer TargetPlatform module
  files. Provide fallible parse/format/adaptation, immutable descriptors and
  typed target/setting errors. Keep enum wire compatibility adapters as needed.
- [x] Migrate identity declarations/adapters in Engine
  `Public/Asset/CookedAsset.h`, `PayloadTargetPlatform.h`, RHI
  `Public/RHIDefinitions.h`, and RenderCore
  `Public/Shader/ShaderCookedLibrary.h` without moving family semantics.
- [x] Update `Engine/Engine.dproject`, relevant `.dmodule` declarations,
  `Programs/DurinAssetTool/CMakeLists.txt` and actual bootstrap/inventory owners
  found in Stage 0. Keep TargetPlatform absent from DurinGame.
- [x] Migrate CLI interpretation in
  `Programs/DurinAssetTool/Private/AssetToolMain.cpp` and explicit Archive
  conversion in `Engine/Private/Asset/CookedArchiveTarget.h`; reject invalid
  targets instead of defaulting to Win64.

Compatibility: identical valid inputs preserve wire/action bytes. Intended
behavior change: invalid inputs fail explicitly. Validation: round-trip and
unknown-value cases; module dependency/load inventory checks; editor/tool and
game builds; all-project shared-API consumer migration and an `all` build.
Exit: one identity authority, no dependency cycle or runtime Developer dependency.

### Stage 2: Resolve material and Shader target requirements

Dependencies: Stage 1; Engine/ShaderBuild consume TargetPlatform through private
tool dependencies; RHI/RenderCore remain lower-level runtime contracts.

- [x] Migrate platform-to-Shader mapping and target support checks from Engine
  `Private/Materials/MaterialCook.cpp` and `MaterialProgramCompiler.cpp`.
- [x] Pass explicit resolved targets through material snapshots/build operations;
  audit `MaterialTranslation.cpp`, `MaterialTranslationCodec.cpp`,
  `MaterialBuildOperation.cpp` and `MaterialProgramCache.cpp` for unchanged
  identity/application rules. Do not move graph/object ownership into policy.
- [x] Replace CookCoordinator's independent Shader platform mapping. Adapt
  ShaderBuild `Private/ShaderCookInput.cpp`, `ShaderCookInput.h` and the actual
  cooked-library producer to consume the same resolved selection.
- [x] Establish bounded, canonical material-owned quality/FeatureLevel variant
  selection within the existing pass contract. Wire frozen selections into Cook
  declarations in Stage 4 and consume them during Stage 5 preparation.

Compatibility: preserve translation and Shader keys for equivalent requests;
changing selected variants invalidates the affected Cook result. Validation:
material compiler/lifecycle/Cook and Shader builder/cache/library CPU suites;
identical targets under different host/runtime RHI settings; warm-hit producer
counters; unrelated texture policy does not change material keys; relevant target
changes do. Run shared-API `all` build when signatures change.
Exit: material and auxiliary Shader library share one explicit target selection.

### Stage 3: Make physics and texture format requirements explicit

Dependencies: Stage 1; Stage 2 establishes the integration pattern. Physics
algorithms remain in PhysicsCore; DDC adapters remain family-owned in Engine.

- [x] Migrate Engine physics `PhysicsCookDerivedDataKey.cpp`,
  `PhysicsBuildFunction.cpp`, `PhysicsCookHelper.cpp`,
  `PhysicsCookInputPrivate.h`, `PhysicsDerivedData.cpp`, and applicable public
  `PhysicsCookHelper.h`/`PhysicsDerivedData.h` contracts to explicit format
  requirements. Update StaticMesh collision callers in `StaticMeshCook.cpp`.
- [x] Preserve a single recipe authority across DDC and direct construction.
  Runtime decoding keeps lower-level compatibility validation without consulting
  TargetPlatform. Implement the Stage 0 MacOS decision; reject unsupported
  collision requirements before production, rather than after geometry cooking.
- [x] Migrate Engine texture `TextureDerivedData.cpp`,
  `TextureDerivedDataKey.h`, `TextureCookedData.h` and family build input headers;
  replace TextureCompressor `Private/Texture/TextureBuildTarget.h` duplicate
  platform tables with resolved requirements and recipe validation.
- [x] Audit `VolumeTextureDerivedData.cpp`, cube/volume build inputs and the
  StaticMesh render target mapping; migrate actual policy branches, keeping
  operation-specific support differences explicit.

Compatibility: retain Win64 actions unless representation semantics change;
record any deliberate family version transition. Cross-platform collision cache
sharing stays disabled until qualified. Validation: cold/warm/direct physics
equivalence, negative/unsupported target paths, collision payload round-trips,
StaticMesh Cook, texture 2D/cube/volume and EditorValidation cases. Required native
macOS reader/build evidence gates any new MacOS collision support; lack of a host
leaves that support unclaimed. Shared-API changes require an `all` build.
Exit: no fixed Win64 surrogate in newly resolved physics requests and no duplicate
texture policy table; every advertised family representation is validated.

### Stage 4: Bind frozen family policy to Cook discovery and reuse

Dependencies: Stages 2 and 3; Cook consumes resolved values and contributor-owned
identity declarations, not TargetPlatform-owned builder versions.

- [x] Update Engine `Private/Asset/CookCoordinator.cpp`,
  `CookDependencyDiscovery.cpp/.h`, `CookDependencies.cpp`, and public
  `Cook.h`/`CookDependencies.h` to retain the frozen target input and relevant
  family projections for the run. Preserve captured contributor lifetimes.
- [x] Update `EngineCookContributors.cpp` and `CookBuildProviders.cpp/.h` so
  families/providers declare their own recipe/format identity. Remove the
  coordinator's need to know individual builder version composition.
- [x] Include relevant resolved settings in declarations before every Cook hit
  decision. Keep reachability, EditorOnly stripping and build-only function
  dependencies under Cook's existing contracts.
- [x] Preserve `CookState.cpp`, manifest/output codecs and transaction behavior
  unless a demonstrated compatibility need requires a versioned change. Availability
  checks occur only when missing work actually needs the producer.

Compatibility: old dependency sets safely miss when new relevant declarations
are introduced; do not silently accept an incomplete old identity. Validation:
policy-change invalidation, unrelated-policy reuse, mid-run configuration change
isolation, cache-only success without a local producer, cold unavailable failure,
corrupt-output repair and auxiliary-library rollback. Record exact scope of any
CookState/contributor version transition. Shared-API changes require `all` build.
Exit: Cook hit and cold paths use the same captured target/settings authority.

### Stage 5: Separate platform preparation from serialization and qualify

Dependencies: Stage 4; existing contributor and detached-product infrastructure.

- [x] Move material variant compilation and StaticMesh render/collision
  construction out of `SerializeCooked` in `MaterialCook.cpp` and
  `StaticMeshCook.cpp` into explicit preparation returning detached products.
- [x] Extend family contribution/preparation contracts in `AssetCook.h`,
  `EngineCookContributors.cpp`, `CookedMeshProducts.h/.cpp` and offline
  preparation integration as needed. Preserve supported direct serialization
  callers through explicit preparation; inventory them before removing behavior.
- [x] Make serialization consume and validate completed products without
  capturing live authored inputs or initiating producers. Keep bounded lifetime,
  cancellation and memory admission in family/Cook execution owners.
- [x] Update lasting ownership/behavior in `Workspace/CodeModules.md`,
  `Runtime/Assets/DerivedDataBuild.md`, `Runtime/Assets/Cooking.md`, material and
  physics contracts. Add documentation routing only for a distinct new topic.

Compatibility: preparation refactoring alone preserves final bytes for equivalent
inputs. New APIs do not justify a payload/DDC version bump. Validation: cold/warm
Cook, producer-free serialization, detached lifetime/freshness, cancellation and
rollback, material default/instance/function variants, StaticMesh with/without
collision, authored editor compilation, and cooked-only loading without DDC,
compiler or TargetPlatform service. Complete shared-API `all` build and affected
project targets. No GPU execution is required unless GPU behavior changes.
Exit: required CPU matrix and platform-specific support gates pass; lasting
contracts are updated; no planned implementation item remains unchecked.

## Validation Evidence and Handoff

Discover current test targets through the registry under the linked testing
workflow, rather than treating source filenames as executable target names.
Relevant starting fixtures include EngineTests `Private/Materials/MaterialCookTests.cpp`,
`MaterialFunctionCookTests.cpp`, `Private/Texture/TextureDerivedDataTests.cpp`,
`TextureCookTests.cpp`, and StaticMesh derived-data/cache fixtures. Extend the
registered physics, Shader and Cook integration suites selected by semantic risk.

Record per stage: source baseline/commit, host/profile/toolchain, exact commands,
reports, action/output compatibility comparisons, and any unsupported or unrun
platform coverage. Do not mark a gate passed from static inspection, skipped
initialization or a previous implementation's report. Keep one source/build
writer per checkout and never overlap native build process trees.

Implementation commits update this plan and use exact trailers:
`Plan: Documentation/Plans/Archive/2026-10/TargetPlatformBuildPolicy.md` and
`Stage: Stage <N>: <exact stage title above>`. The planning-only commit must not
claim implementation-stage completion. Follow the repository commit rules.

Validate changed documentation and all plans according to the
[documentation workflow](../../../Agents/Documentation.md) and
[plan lifecycle](../../AGENTS.md). Complete/archive only after every required stage and
acceptance gate has evidence; unsupported optional capabilities stay explicitly
unsupported rather than being advertised as implemented.

### Stage 6: Correct Cook coverage and simplify target resolution

Dependencies: Stage 5; preserve family DDC actions and payload schemas.

Selected changes: collect contributor dependencies for every serialized object
class, coalescing identical cross-family declarations while rejecting conflicts;
share transient/override exclusion between preparation and capture, and build
complete field plans from capture's selected objects rather than retraversing
excluded descendants; validate and
select existing cooked material variants without invoking producers. Remove the
single-choice Shader settings wrapper and duplicate coordinator target checks.
Keep the invocation-scoped product table: generic Archive has no typed payload
context, and adding an extensibility framework solely for two asset families
would expand this fix unnecessarily.

- [x] Cover mixed top-level and nested asset family invalidation and cold/warm parity.
- [x] Cover transient asset subtrees and cooked-only material selection failures/subsets.
- [x] Validate target resolution, material, Cook and package tests plus Editor/Game all builds.
- [x] Update lasting contracts and record exact validation evidence.

Completion evidence (Win64-Debug-DurinEditor, 2026-10-15):

- 1016 passed cases, no failures or skips, across 17 CPU targets. Reports are in
  `Build/NativeTestResults/Win64-Debug-DurinEditor/<Target>.xml`.
- Targets/counts: CookFunctionalTests 11, TargetPlatformTests 2, CoreObjectTests
  105, CorePropertyValueSnapshotTests 29, PackageLinkerContractTests 20,
  PackagePersistenceTests 29, PackageWriterContractTests 20, AssetPackageTests
  189, AssetCookTests 41, MaterialCompilerTests 84, MaterialCookTests 7,
  MaterialRuntimeTests 92, StaticMeshTests 186, TextureTests 150,
  RenderShaderBuilderTests 30, RenderShaderCacheTests 14,
  RenderShaderCookedLibraryTests 7.
- Independent-process Cook execution: 11/11 passed, no skips;
  `CookFunctionalIsolated.xml` and
  `Build/.agent-state/logs/20261015-121057-125141-49332-ctest.log`.
- Final Editor all build:
  `Build/.agent-state/logs/20261015-121109-990276-49764-cmake.log`.
- Final Game all build:
  `Build/.agent-state/logs/20261015-121112-646601-15476-cmake.log`.
  Its generated graph contains no TargetPlatform object or DLL target. Shared
  Engine, Sandbox and RoadWeaver consumers compile in both all builds.
- Regression coverage includes mixed secondary exports and nested-only materials,
  identical Material/MaterialInstance declarations, selection invalidation with
  cold/warm byte parity, transient subtrees and explicit object omissions,
  selected-object field plans, missing/invalid cooked-only configurations,
  producer-free subset extraction and byte-identical equivalent resaving.
- The material functional fixture caught generic fallback registrations on nested
  authored expression nodes disabling warm Cook reuse. Nested resolution now
  excludes only the generic DObject fallback; top-level fallback behavior and
  specific family registrations remain intact. MaterialCookTests passed after
  that correction, as did AssetCookTests and both Cook functional execution modes.
  Earlier passing Core/package/material-compiler coverage is reused because that
  final discovery-only correction does not change those tested implementations.
- Family DDC action/bucket/codec versions and payload schemas are unchanged.
  Existing pure-family requests retain their dependency values; additional
  participating families add declarations so affected old CookState entries miss
  safely. No global policy hash or wholesale cache invalidation is introduced.
