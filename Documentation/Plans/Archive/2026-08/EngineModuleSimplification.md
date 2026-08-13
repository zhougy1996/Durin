# Engine Module Simplification Plan

Summary: Reduce Runtime Engine code, public surface, and authoring leakage while preserving runtime asset ownership, wire compatibility, and existing editor/game behavior.

Last reviewed: 2026-08-13

Status: Archived
Completed: 2026-08-13

## Current Status

Stage 0 completed on 2026-08-13 against baseline commit
`b495da4143f5d009bf69b17d2cf52a934e4efab9`. The qualified Engine inventory is
203 files: 107 public headers, 94 private translation units, one module
descriptor, and one PCH source. Its 107 public headers have 334 direct include
edges. `Engine.dmodule` declares nine public dependencies, no private
dependencies, and 56 reflection inputs. Direct module consumers are
`GeometryBuild`, `TextureBuild`, `DurinEd`, `LevelEditor`, `MainFrame`,
`MaterialEditor`, `SkeletalMeshEditor`, `StandardAssetImport`,
`StaticMeshEditor`, `TextureEditor`, `Launch`, and `Renderer`.

The repository does not publish or install Engine as an external SDK; its
supported source surface is repository-owned public headers compiled through
declared module dependencies. This qualifies removal of unconsumed umbrella
headers, but public-header self-sufficiency remains required. `DMeshComponent`
is retained and documented as the reflected mesh-component marker because
StaticMesh, SplineMesh, and Terrain components use it in their serialized
superclass chain and editor type queries.

The selected mounted-source contract is `Asset/MountedSource.h` in
`Durin::Asset`. It retains the existing transaction/result type names and
explicit caller-owned transaction lifetime, and replaces the Engine boolean
with `EMountedSourceMutationContext::{DependencySafe, EngineAuthoring}`. No
forwarding header survives Stage 2. The selected TextureCube import surface is
`StandardAssetImport/TextureCubeSourceTranslation.h`, mirroring the existing
Texture2D import/reimport/source-mutation service. TextureBuild receives owned,
normalized cube build requests and returns detached products; Engine retains
only publication/exchange, serialization, Cooked load, render-resource, and
uncooked post-load seams. StandardAssetImport remains the single lifecycle
owner and drains through the existing AssetBuild host.

The selected AssetCore Cooked helper is
`LoadCookedPackagePayload(const FPackageLoadContext&, string_view,
const FCookedPayloadDescriptor&, ECookTargetPlatform, ECookTargetProfile,
FCookedPackagePayload&, string*)`.
`FCookedPackagePayload` owns its `FCookedBulkContainer` and exposes a span valid
for the result lifetime. Engine callers retain payload identity, schema,
target/profile, compression, semantic validation, diagnostics, and detached
publication. The minimum private wire primitives are checked alignment plus
bounded little-endian `uint32`/`uint64` read and append operations; asset-family
field order, chunks, limits, hashes, and error messages remain local.

Baseline validation passed before source movement: all-scope plan validation;
TextureTests 65 passed with two established skips; StaticMeshTests 68/68;
SkeletalAssetTests 34/34; TerrainHeightmapTests 6/6; MaterialTests 78/78;
EditorAssetWorkflowTests 80 passed with one established skip; and
PhysicsSceneTests 38/38. These suites own the current golden authored/DDC/
Cooked bytes for Texture2D, TextureCube, StaticMesh/DCOL, SkeletalMesh,
AnimationClip, TerrainHeightmap, EnvironmentLighting, and Material v1/v2/v3.

Stage 1 completed on 2026-08-13. The empty RenderTarget pair and
ColorVertexBuffer header plus the unconsumed EngineMinimal/EngineFwd umbrellas
were removed. `DMeshComponent` remains the documented reflected marker.
StaticMesh and Terrain recreate contexts are private, ApplicationCore is a
private Engine dependency, and primitive physics creation/update share one
typed descriptor builder. Configure, Engine, EngineViewportHeaderTests 1/1,
PhysicsSceneTests 38/38, and StaticMeshTests 68/68 passed.
The current Editor profile default `all` build also passed, compiling the
complete configured direct-consumer closure.

Stage 2 completed on 2026-08-13. `Asset/MountedSource.h` and its AssetCore
implementation now own mounted reference resolution and file, byte,
replacement, and relocation transactions in `Durin::Asset`. The mutation
context is an explicit enum, every production/editor/test consumer uses the
AssetCore contract directly, and Engine's Source implementation/header were
removed without a facade. AssetMountedSourceTests 1/1 links no Engine and
passes; the Editor `all` build, EditorAssetWorkflowTests 80 with one established
skip, TextureTests 65 with two established skips, StaticMeshTests 68/68, and
TerrainHeightmapTests 6/6 also pass.

Stage 3 completed on 2026-08-13. StandardAssetImport now owns TextureCube
validation, import/reimport, source mutation, transaction composition, asset
creation, and package save workflows, while TextureBuild accepts normalized
owned build settings. Runtime Engine retains only TextureCube metadata,
serialization, Cooked load, render-resource publication, imported-state
exchange, and a narrow uncooked post-load seam; the mutable authoring callback
registry and TextureCubeAuthoring files were removed. Editor and DurinGame
`all` builds passed, along with TextureTests 65 with two established skips,
SceneImportTests 15/15, SkyBoxTests 10/10, TextureThumbnailTests 7/7,
TextureCookIntegrationTests 1/1, and MaterialTests 78/78.

Stage 3.5 completed on 2026-08-13. AssetBuildCore, TextureBuild, and
GeometryBuild now use `Durin::Asset::Build`; AssetImportCore and canonical
StandardAssetImport entry points use `Durin::Asset::Import`; and standard
scene-parser intermediates use `Durin::Asset::Import::Standard`. Scene parser
diagnostics have scene-specific category and value names while reusing the
framework severity enum losslessly. `ImportRecord` C++ declarations moved to
the new hierarchy while DHT `PersistentName` metadata preserves their exact
legacy class, struct, and enum serialization identities. DHT generation tests
76/76, AssetBuildCoreTests 10/10, AssetImportCoreTests 25/25, SceneImportTests
15/15, TextureTests 65 with two established skips, and
EditorAssetWorkflowTests 80 with one established skip passed. Clean Editor and
DurinGame `all` builds also passed.

Stage 4 completed on 2026-08-13. AssetCore now resolves the owning virtual
package path, relocatable `.dbulk` companion, requested target/profile, and one
exact descriptor into `FCookedPackagePayload`, whose owned container makes the
opaque byte-span lifetime explicit. Texture2D, TextureCube, TerrainHeightmap,
StaticMesh/DCOL, SkeletalMesh, AnimationClip, and EnvironmentLighting use that
helper while retaining their type-specific descriptor checks and detached
publication. Engine-private `EngineWire` primitives now provide the bounded
little-endian reader/writer and alignment mechanics shared by StaticMesh,
Skeletal, Texture, and Terrain codecs. Descriptor-publication callbacks remain
local because their count, required payload set, metadata mutation, rollback,
and diagnostics are type-specific. AssetCookTests 13/13, TextureTests 65 with
two established skips, StaticMeshTests 68/68, SkeletalAssetTests 34/34,
TerrainHeightmapTests 6/6, TerrainHeightmapCookTests 1/1, and
EnvironmentLightingTests 3/3 passed.

Stage 5 completed on 2026-08-13. `DStaticMesh` remains one public owner while
its lifecycle/publication, Cook, collision/query, and vertex/index/render
resource implementations are separate translation units. Material authored
schema, render layout compatibility, immutable representation/builder, and
fallback diagnostics now have separate implementations; renderer-facing types
use the narrower self-sufficient `MaterialRenderTypes.h` while reflected
authored types remain in `MaterialTypes.h`. `DWorld` remains one contract split
between core level ownership, play/restart/tick lifecycle, and collision query/
debug mechanics. The direct include count of the authored Material header fell
from 10 to 8, and its declarations fell from 602 to 272 lines; the narrow
render header has seven direct includes. The remaining translation unit above
the Stage 0 review threshold is `StaticMeshDerivedData.cpp`; it remains intact
because it owns one coupled static-mesh/DCOL wire-codec responsibility whose
chunks, hashes, limits, and error mapping must evolve together. MaterialTests
78/78, StaticMeshTests 68/68, WorldTests 102/102, and PhysicsSceneTests 38/38
passed.

Stage 6 and this plan completed on 2026-08-13. `EngineTests/CMakeLists.txt`
retains the sole shared helper and includes six target-local domain fragments
in the original registry/discovery order; a mechanical reconstruction matched
all 1,209 original declaration lines. Configuration retained exact ownership,
metadata, private-source rationale, runtime dependencies, locks, timeouts, and
deployment policy. Rebase qualification also removed stale pre-namespace test
calls and made the mixed Vulkan integration target compile its offline texture
compression case only in Editor configurations, keeping the Game closure free
of TextureBuild. `EngineViewportHeaderTests`, `AssetImportTests`, the bounded
`@asset-cook` set, `fast-all`, Editor and Game ordinary aggregates, focused
Editor/Game Vulkan targets, and clean Editor/Game builds passed. The Game
Cooked domain passed with a deployment containing only Runtime modules and no
AssetBuildCore, AssetImportCore, GeometryBuild, TextureBuild,
StandardAssetImport, editor module, Assimp, or offline compressor binary.

All stages and acceptance gates are complete. Lasting ownership rules now live
in the Asset lifecycle, reflection, rendering, world, module, and native-test
documentation. The plan is eligible for the repository's monthly archive
transaction; that batch operation remains separate because it also archives
other completed August plans.

Before Stage 4, the plan now normalizes the Asset namespace family without
changing module ownership: AssetCore remains `Durin::Asset`, AssetBuildCore and
typed Build modules move from `Durin::AssetBuild` to `Durin::Asset::Build`, and
AssetImportCore plus canonical StandardAssetImport entry points move from
`Durin::AssetImport`/`Durin::StandardAssetImport` to
`Durin::Asset::Import`. Standard-provider-only intermediate and private types
may use `Durin::Asset::Import::Standard`. The framework import diagnostic stays
canonical; scene-parser diagnostics receive scene-specific names instead of
triggering a broader diagnostic-model redesign.

The Stage 0 baseline gaps were:

- AssetCore, AssetBuildCore/TextureBuild, and
  AssetImportCore/StandardAssetImport form one dependency family but expose
  four unrelated namespace roots, producing repetitive qualification and
  `using` declarations and one colliding import-diagnostic vocabulary;
- seven runtime asset families repeat the same Cooked package-companion lookup,
  DBLK load, and payload-resolution sequence;
- empty files, unused umbrella/forward headers, implementation-only public
  headers, and one private-only dependency widen the module surface;
- asset payload implementations repeat endian, bounded-reader, alignment, and
  chunk-envelope mechanics;
- several large translation units combine independent responsibilities, and
  the Engine native-test CMake declaration has become a 1,200-line composition
  root.

Stage 0 must qualify every deletion and compatibility decision before source
movement begins. In particular, repository-local non-use is not by itself
permission to break a documented public SDK surface or reflected inheritance
contract.

## Goal

- Remove dead Engine files and declarations with evidence that they have no
  supported source, reflection, serialization, or deployment consumer.
- Move generic mounted-source transactions to AssetCore without moving
  source-format translation or asset-family policy there.
- Make TextureCube follow the established runtime/import/build boundary used by
  the other authored asset families.
- Normalize the existing Asset module family under `Durin::Asset::Build` and
  `Durin::Asset::Import` without flattening Runtime, Developer, and Editor
  responsibilities or changing the module graph.
- Consolidate generic Cooked companion access and private payload-wire
  mechanics without centralizing asset-family schema ownership.
- Narrow Engine public dependencies and headers so consumers inherit only the
  dependencies and declarations required by runtime contracts.
- Split large implementation and build-composition files along existing
  ownership boundaries without creating new production modules.
- Preserve authored package bytes, DDC values, Cooked payload bytes, class
  identity, runtime behavior, editor workflows, and rollback guarantees unless
  Stage 0 explicitly qualifies a versioned incompatibility.

## Scope

- `Engine/Source/Runtime/Engine` source, public headers, module descriptor, and
  related reflection input.
- `Engine/Source/Runtime/AssetCore` only for generic mounted-source transaction
  and Cooked companion storage/access mechanics.
- `Engine/Source/Developer/AssetBuildCore` and
  `Engine/Source/Editor/AssetImportCore` for the namespace migration and
  reflected import-record compatibility qualification.
- `Engine/Source/Editor/StandardAssetImport` for TextureCube import, reimport,
  source-reference mutation, source-byte acquisition, and package workflow.
- `Engine/Source/Developer/TextureBuild` for TextureCube recipe/build
  invocation and authoring-only validation that belongs with texture
  production.
- Direct Engine consumers that must migrate from removed or relocated headers
  and APIs.
- `Engine/Tests/Native/EngineTests` source ownership and CMake composition
  required to preserve or clarify coverage for the affected boundaries.
- Lasting module, asset, rendering, and source-transaction documentation whose
  contracts change during implementation.

## Non-Goals

- Splitting Runtime Engine into one DLL per asset family or introducing an
  `EngineAssetRuntime`, `TextureRuntime`, `GeometryRuntime`, or similar module
  without a measured deployment, lifecycle, optional-dependency, or release
  boundary.
- Moving runtime/platform asset values or their complete `Serialize`
  implementations away from the module that owns those values.
- Moving texture decoding, mesh translation, offline compression, Build
  recipes, or editor UI into AssetCore.
- Changing TXPL, DMSH, DCOL, DSKM, DANM, THPL, environment-lighting, material,
  authored-package, DDC, or Cooked schema bytes.
- Removing Material render-layout v1/v2 factories, validators, or decoders;
  they remain an intentional compatibility boundary.
- Redesigning World, Actor, Component, Renderer, RHI, Aether, package, DDC, or
  garbage-collection architecture.
- Introducing a generic base class merely to make unrelated asset types look
  uniform. Shared abstractions must remove repeated policy or state-machine
  mechanics while retaining typed validation and publication.
- Merging Build, import, storage, parser, and wire diagnostics into one generic
  result hierarchy. Stage 3.5 may reuse a lossless severity enum and must rename
  colliding scene-specific diagnostics, but semantic diagnostic consolidation
  remains a deferred follow-up.
- Renaming test targets, changing test kinds/domains, or changing suite
  ownership as part of the mechanical CMake file split unless a concrete
  ownership defect is separately recorded and qualified.

## Design Decisions and Invariants

### Runtime Value and Serialization Ownership

- Engine continues to own reflected runtime asset schemas, runtime/platform
  value types, payload identifiers, layout limits, semantic validation,
  render-resource creation, and complete value serialization.
- Serialization helpers may own byte-level mechanics but may not own the field
  order, compatibility decision, validation policy, or schema version of an
  asset-family value.
- Save and load continue through the same type-owned Archive customization.
  No direction-named encoder/decoder facade is reintroduced.

### AssetCore Boundary

- AssetCore may own mounted-path resolution, dependency/mutation policy,
  transactional file staging, atomic file publication, Cooked companion path
  resolution, DBLK loading, and opaque payload lookup.
- AssetCore does not include Engine asset headers, interpret an Engine payload,
  select a builder, or decide whether a runtime schema is compatible.
- Any payload byte view returned by an AssetCore helper has an explicit owner
  lifetime. A caller-owned container plus span, an owning result, or an
  equivalent lifetime-safe contract is required; a dangling span-producing
  convenience API is forbidden.

### Import and Build Boundary

- StandardAssetImport owns standard TextureCube source workflow: validation of
  provider/source availability, source-byte acquisition, creation/reimport
  orchestration, package save, and transaction composition.
- TextureBuild owns TextureCube recipe execution, source-independent Build
  validation, DDC policy, and production diagnostics.
- Runtime Engine exposes only the narrow TextureCube value publication,
  exchange, uncooked-load capability, and runtime rebuild seam that a selected
  authoring host genuinely requires.
- Runtime Engine must not retain a permanent global registry whose public API
  combines encoded source bytes, filesystem paths, mutable assets, and import
  settings. Temporary migration adapters are stage-local and are deleted in
  the same stage after all consumers move.

### Asset Namespace Hierarchy

- Module boundaries remain authoritative for dependency, linkage, runtime
  availability, and deployment; namespace nesting does not grant a module an
  undeclared dependency.
- AssetCore remains `Durin::Asset`. AssetBuildCore and typed Build modules use
  `Durin::Asset::Build`; AssetImportCore and canonical built-in import entry
  points use `Durin::Asset::Import`.
- Standard-provider-only intermediate representations and implementation
  details may use `Durin::Asset::Import::Standard` and its `Private` child.
  Public APIs are not placed there merely to mirror the module name.
- The migration must not introduce `using namespace` directives, permanent
  forwarding namespaces, duplicate registrations, or compatibility facades.
  Unqualified parent lookup and short child qualification such as `Build::`
  are preferred inside the Asset namespace family.
- `Asset::Import::FImportDiagnostic` remains the framework diagnostic.
  Scene-parser diagnostics use `FSceneImportDiagnostic` and
  `ESceneImportDiagnosticCategory`; they may reuse the framework severity only
  when every existing value maps losslessly. Categories, stable identities,
  messages, persistence, and failure behavior do not change in this stage.
- Reflected import-record classes/structs, generated code, serialized class
  identity, and authored record compatibility are qualified before their C++
  namespace moves. Namespace cleanup is not permission to version or rewrite
  stored import records.

### Transaction and Failure Semantics

- Source preparation is non-destructive until commit. Failure before package
  publication rolls back staged files; failure after replacement publication
  restores previous bytes according to the existing transaction contract.
- Import/reimport never publishes partial TextureCube state. Existing complete
  asset state remains observable after source, Build, DDC, save, or publication
  failure.
- Cooked loads build and validate a detached candidate before replacing live
  CPU or render-resource state.
- Error classification remains at least as specific as the current behavior:
  missing data, incompatible schema/target, corrupt payload, I/O failure, and
  Build capability absence must not collapse into one generic failure.

### Threading and Lifecycle

- Object capture, package mutation, source transaction commit/rollback,
  publication, and registration changes occur on the GameThread unless an
  existing documented boundary states otherwise.
- Build workers consume immutable/owned inputs and never access a mutable
  `DObject`, package, editor model, or render resource.
- Render-resource retirement/publication preserves existing render-command
  ordering, revision, fence, and stale-result rejection behavior.
- Removing the TextureCube authoring registry must not create a second host or
  distribute authoring shutdown ownership. Existing selected authoring hosts
  remain the lifecycle owners.

### Compatibility and Deletion Policy

- A public header or class is deleted only after checking repository includes,
  generated/reflection inputs, serialized class names, documentation, examples,
  tests, deployment, and any declared external/source compatibility policy.
- `DMeshComponent` may be removed only if Stage 0 proves that changing the
  reflected superclass chain is compatible with supported packages and editor
  type queries. Otherwise it remains as an explicitly documented marker.
- Material v1/v2 render layouts remain supported. This plan may physically
  separate their implementation from current v3 code but cannot weaken their
  exact validation and decode contract.
- Golden payload and package bytes are the compatibility authority for
  mechanical helper extraction and translation-unit splits.

### Module and File Granularity

- No new production module is added by default. Existing module ownership is
  corrected before module count is increased.
- Translation units are split where a stable header/value owner already
  exposes separable responsibilities; file splitting must not create new
  service locators, manager classes, or forwarding layers.
- A shared helper must have at least two real consumers and own a coherent
  policy or mechanism. Tiny type-specific code remains local when extraction
  would add more concepts than it removes.

## Current Foundations and Gaps

### Foundations to Preserve

- Engine asset values already own the Archive serialization used by DDC and
  Cooked paths.
- TextureBuild and GeometryBuild own source-independent recipes and Build/DDC
  policy, while StandardAssetImport owns concrete source translation.
- AssetCore already owns `FSourcePath`, mount policy, packages, Cook context,
  DBLK containers, and opaque DDC object storage.
- StaticMesh, SkeletalMesh, AnimationClip, Texture, TerrainHeightmap, and
  EnvironmentLighting validate detached payloads before publication.
- Focused tests cover source transactions, import/reimport rollback, payload
  golden values, Cooked runtime load, render-resource lifecycle, material
  compatibility, and editor consumers.
- Module closure and deployment checks already distinguish Runtime, Developer,
  Editor, Renderer, and dynamically selected backend responsibilities.

### Gaps to Close

- AssetCore, AssetBuildCore/typed Build, and
  AssetImportCore/StandardAssetImport expose `Durin::Asset`,
  `Durin::AssetBuild`, `Durin::AssetImport`, and
  `Durin::StandardAssetImport` as unrelated roots despite their layered module
  relationship; this creates qualification noise and colliding diagnostic
  names without expressing an ownership boundary.
- `DTextureCube` publicly combines runtime state with complete asset workflow,
  while other migrated assets expose narrower publication seams.
- `TextureCubeAuthoring` stores a process-global bundle of callbacks in Runtime
  Engine and makes authoring availability a mutable global lookup.
- Engine's mounted-source implementation contains no Engine asset type but is
  exported with `ENGINE_API` and named as an Engine authoring concern.
- Engine repeats Cooked companion lookup/load/resolve calls across seven asset
  families, including nearly identical failure plumbing.
- StaticMesh, SkeletalMesh, Texture, and Terrain payload code repeats bounded
  little-endian reads, offset alignment, and envelope mechanics.
- `ApplicationCore` is public in `Engine.dmodule` even though its observed
  Engine includes occur only in private `Engine.cpp` and `GameEngine.cpp`.
- `RenderTarget.h/.cpp` and `ColorVertexBuffer.h` are empty; `EngineMinimal.h`
  and `EngineFwd.h` have no repository consumer; two recreate-context headers
  are public despite having no module-external caller.
- `StaticMesh.cpp`, `MaterialTypes.cpp`, and `World.cpp` contain stable groups
  of responsibilities that can be separated without changing ownership.
- Engine native-test CMake composition mixes many feature domains in one large
  declaration file even though the resulting tests are already separate
  targets.

## Implementation Stages

### Stage 0: Freeze the baseline and deletion decisions

Dependencies: clean qualified repository baseline and the completed Engine
asset-boundary/modularization plans.

- [x] Record the exact Engine module source/header inventory, public include
  edges, module dependency visibility, reflection-header list, and direct
  consumer modules.
- [x] Record golden authored/DDC/Cooked payload coverage for Texture2D,
  TextureCube, StaticMesh/DCOL, SkeletalMesh, AnimationClip,
  TerrainHeightmap, EnvironmentLighting, and Material v1/v2/v3.
- [x] Inventory every consumer and lifecycle owner of
  `TextureCubeAuthoring`, the TextureCube import/reimport/source APIs, and the
  mounted-source transaction APIs.
- [x] Decide and document whether Engine public headers are a repository-only
  API or a supported external SDK surface; use that decision to qualify
  removal of `EngineMinimal.h` and `EngineFwd.h`.
- [x] Audit reflected class/package compatibility and editor type-query usage
  of `DMeshComponent`; select either removal with migration evidence or
  explicit retention as a marker class.
- [x] Confirm that `Client/RenderTarget.h/.cpp` and
  `Rendering/ColorVertexBuffer.h` are absent from generated, installed,
  documentation, test, and external compatibility inputs.
- [x] Define the exact AssetCore mounted-source API names, namespace, result
  lifetimes, and migration mapping.
- [x] Define the exact TextureCube Runtime/Import/Build API split and host
  lifecycle after removal of the global authoring-handler bundle.
- [x] Define the generic Cooked companion helper contract and list the
  type-specific validation that must remain in each Engine caller.
- [x] Identify the minimum byte-reader/writer primitives whose extraction
  preserves every golden byte and error classification.

#### Acceptance Gate

- Every prospective deletion has a recorded compatibility decision and exact
  consumer inventory.
- TextureCube ownership and lifecycle have one selected design with no
  simultaneous permanent old/new path.
- AssetCore helper contracts contain no Engine types or asset-family schema
  decisions.
- Baseline focused tests and golden artifacts pass before code movement begins.
- The plan's all-scope document validator passes.

### Stage 1: Remove dead surface and narrow dependencies

Dependencies: Stage 0 deletion and compatibility decisions.

- [x] Delete the empty `Client/RenderTarget.h/.cpp` pair and empty
  `Rendering/ColorVertexBuffer.h` after the Stage 0 gate permits removal.
- [x] Delete or retain-with-documentation `EngineMinimal.h`, `EngineFwd.h`, and
  `DMeshComponent` according to the selected Stage 0 decisions; migrate every
  direct consumer in the removal case.
- [x] Move StaticMesh and Terrain render-state recreate-context headers from
  `Public` to `Private`, remove unnecessary export macros, and retain only the
  forward declarations needed for friendship.
- [x] Move `ApplicationCore` from `PublicDependencies` to
  `PrivateDependencies` and verify that no Engine public header resolves only
  because of the previous transitive edge.
- [x] Extract the repeated `FPhysicsBodyDesc` construction in
  `DPrimitiveComponent` into one private typed helper used by create and
  update paths.
- [x] Add or retain compile-only header coverage that proves supported Engine
  public headers include what they use.
- [x] Update module/API documentation for every intentional removal or retained
  marker.

#### Acceptance Gate

- Engine and every direct consumer module compile without relying on removed
  umbrella headers or the transitive ApplicationCore public edge.
- Reflection generation succeeds and supported class/package behavior is
  unchanged.
- Primitive physics create/update tests preserve descriptor, revision, and
  fallback behavior.
- No implementation-only recreate context remains installed/exported as an
  Engine public header.
- The configured runtime/game module closure remains valid.

### Stage 2: Move mounted-source transactions to AssetCore

Dependencies: Stage 1 public-surface baseline.

- [x] Move mounted-source file, byte, replacement, and relocation transaction
  types and operations beside AssetCore's `FSourcePath` and mount policy.
- [x] Replace `ENGINE_API` and Engine include paths with an AssetCore-owned
  namespace/header contract; use explicit mutation context/policy terminology
  instead of an Engine-named boolean when Stage 0 selects it.
- [x] Preserve file-equality reuse, mount dependency checks, writable-target
  checks, temporary/backup naming, atomic publication, and bounded diagnostics.
- [x] Migrate Texture2D, TextureCube, StaticMesh-related authoring adapters,
  StandardAssetImport, DurinEd relocation, TextureEditor, LevelEditor, tests,
  and tools to the AssetCore API.
- [x] Remove Engine's `Public/Source` and `Private/Source` implementation after
  the last consumer migrates; do not leave a forwarding facade.
- [x] Move lasting source-transaction rules into the owning Asset documentation.

#### Acceptance Gate

- AssetCore builds and tests the mounted-source transaction contract without
  linking Engine.
- Existing source reference, ingestion, replacement, relocation, commit, and
  rollback tests preserve bytes and failure behavior.
- Editor workflows and engine-authoring paths still enforce their exact mount
  mutation policy.
- Runtime Engine contains no generic mounted-source filesystem implementation
  or compatibility forwarding header.
- AssetCore includes no Engine asset-family declaration as a consequence of
  the move.

### Stage 3: Remove TextureCube authoring orchestration from Runtime Engine

Dependencies: Stage 2 AssetCore source transactions and the selected Stage 0
TextureCube boundary.

- [x] Move TextureCube source validation, source-byte acquisition, asset
  creation, source transaction composition, package save, import, reimport,
  source-reference change, and ingest/change workflows to StandardAssetImport.
- [x] Route normalized TextureCube requests through TextureBuild without a
  Runtime callback that accepts encoded bytes and a mutable asset.
- [x] Keep Engine-owned TextureCube source metadata, platform/runtime values,
  semantic validation, Serialize, Cooked load, render-resource creation,
  detached publication, and imported-state exchange.
- [x] Replace direct `DTextureCube::Import*`, `Reimport*`, `Change*Source`, and
  `IngestAndChange*` consumers with the selected import service/API.
- [x] Reduce the uncooked `PostLoad`/rebuild capability to the narrowest
  lifecycle seam required by selected authoring hosts.
- [x] Remove `TextureCubeAuthoring.h/.cpp`, its global mutex/callback state, and
  registration/unregistration calls after all callers migrate.
- [x] Preserve source fingerprints, face ordering, panorama settings, DDC keys,
  package save behavior, rollback, diagnostics, and render-resource revision
  behavior.
- [x] Update TextureCube, import, Build, runtime lifecycle, and module-boundary
  documentation.

#### Acceptance Gate

- DurinGame and Runtime Engine contain no TextureCube import/create/save/source
  workflow or authoring callback registry.
- StandardAssetImport and TextureBuild own exactly one TextureCube authoring
  path; module absence produces an explicit authoring capability error.
- Six-face and panorama import/reimport, source mutation, rollback, DDC warm
  load, Cook, source-free Cooked runtime load, thumbnails, SkyBox use, and
  render-resource publication pass their focused tests.
- Existing authored, DDC, and Cooked TextureCube golden bytes are unchanged.
- Authoring host startup/shutdown drains all accepted work before provider or
  Build registration disappears.

### Stage 3.5: Normalize the Asset namespace hierarchy

Dependencies: Stage 3 has established the final Runtime/Import/Build ownership
boundary, and Stage 4 has not yet added more APIs and consumers under the old
namespace roots.

- [x] Inventory declarations, generated/reflection inputs, explicit template
  specializations, registrations, documentation, and production/test callers
  using `Durin::AssetBuild`, `Durin::AssetImport`,
  `Durin::StandardAssetImport`, and StandardAssetImport-owned types currently
  placed directly in `Durin::Asset`.
- [x] Qualify whether the C++ namespace of reflected `ImportRecord` types
  participates in generated names, serialized class identity, authored record
  compatibility, or property lookup; record and test the compatibility
  strategy before moving those declarations.
- [x] Move AssetBuildCore, TextureBuild, GeometryBuild, and their consumers from
  `Durin::AssetBuild` to `Durin::Asset::Build` without renaming modules,
  changing API macros, or altering module dependencies and registration keys.
- [x] Move AssetImportCore and canonical StandardAssetImport entry points from
  `Durin::AssetImport`/`Durin::StandardAssetImport` to
  `Durin::Asset::Import`; place only standard-provider intermediate/private
  types beneath `Durin::Asset::Import::Standard`.
- [x] Keep the framework `FImportDiagnostic` canonical, rename the colliding
  scene-parser types to `FSceneImportDiagnostic` and
  `ESceneImportDiagnosticCategory`, and reuse the framework severity only if
  the mapping is lossless. Do not merge category enums, result types, messages,
  or persistence contracts.
- [x] Remove now-redundant Asset transaction `using` declarations and verbose
  sibling qualification where parent lookup or `Build::` is unambiguous; do
  not add `using namespace`, permanent forwarding aliases, or dual
  registrations.
- [x] Migrate all production, tool, test, generated-code customization, and
  documentation consumers in one stage, then remove the old namespace roots.

#### Acceptance Gate

- Repository source contains no live declaration or consumer in
  `Durin::AssetBuild`, `Durin::AssetImport`, or
  `Durin::StandardAssetImport`, excluding historical plan prose and explicit
  compatibility fixtures.
- AssetCore, AssetBuildCore, AssetImportCore, typed Build modules, and standard
  import implementations retain their existing module ownership, dependency
  direction, registration identity, and runtime/editor deployment boundary.
- ImportRecord reflection generation, authored record load/save, registry and
  property lookup, and golden serialized identity pass without a version bump
  or content rewrite.
- Framework and scene diagnostics preserve severity, category, stable identity,
  message, ordering, persistence, and failure behavior; no unrelated
  diagnostic abstraction is introduced.
- AssetCore, AssetBuildCoreTests, AssetImportCoreTests, TextureTests,
  SceneImportTests, EditorAssetWorkflowTests, clean Editor and DurinGame
  builds, and module-closure checks pass.

### Stage 4: Consolidate Cooked payload and wire mechanics

Dependencies: Stage 3 removal of the largest duplicated Runtime authoring path
and Stage 3.5 establishment of the final Asset namespace hierarchy.

- [x] Add an AssetCore helper that resolves an owning package path and DBLK
  companion, loads the container for the requested target/profile, and returns
  one descriptor-selected opaque payload with explicit container lifetime.
- [x] Migrate Texture2D, TextureCube, TerrainHeightmap, StaticMesh/DCOL,
  SkeletalMesh, AnimationClip, and EnvironmentLighting Cooked loads to the
  helper while retaining their type-specific descriptor/schema checks and
  detached candidate publication.
- [x] Evaluate repeated Cook package descriptor-publication callbacks and
  extract only the mechanics that are truly asset-family-neutral.
- [x] Introduce Engine-private bounded little-endian reader/writer and alignment
  primitives shared by at least StaticMesh, Skeletal, Texture, and Terrain
  payload implementations.
- [x] Retain each value's explicit field sequence, chunk identities, required
  versus optional chunk rules, limits, checksums, and compatibility mapping in
  its owning implementation.
- [x] Remove superseded local `ReadU32At`, `ReadU64At`, trivial writer/reader,
  and alignment copies after their callers migrate.
- [x] Preserve exact diagnostic categories and transactional rejection of
  malformed input.

#### Acceptance Gate

- Every affected save-load-save and golden-byte test remains byte-identical.
- Corrupt, truncated, oversized, misaligned, duplicate, unknown-required, and
  target/schema-incompatible fixtures preserve their expected classification.
- AssetCore treats all returned payloads as opaque bytes and has no dependency
  on Engine.
- Every Cooked load publishes only after complete typed validation and leaves
  prior state intact on failure.
- Cooked runtime smoke passes without source, translator, Build, or DDC
  availability.

### Stage 5: Split implementation and header responsibilities

Dependencies: Stages 1-4, including Stage 3.5, have removed or consolidated
behavior so file splits reflect the final boundary rather than preserving
obsolete structure.

- [x] Split `StaticMesh.cpp` into asset lifecycle/publication/Cook,
  collision-state/query support, and vertex/index/render-resource
  implementations; use the existing public value owners and avoid forwarding
  classes.
- [x] Split `MaterialTypes.cpp` and `MaterialTypes.h` into canonical authored
  parameter/schema types, render layout/compatibility, immutable render
  representation/builder, and diagnostics where consumers measurably use
  different subsets.
- [x] Preserve exact Material v1/v2/v3 layout tables, payload sizes, GUIDs,
  error material, fallback counters, and schema-upgrade behavior.
- [x] Split `World.cpp` into core world/level ownership, play/restart lifecycle,
  and collision query/debug implementation while retaining one `DWorld`
  contract.
- [x] Review remaining translation units above the agreed Stage 0 threshold
  and split only when at least two stable responsibilities are present.
- [x] Remove redundant includes and replace heavy includes with forward
  declarations only where complete-type, reflection, inline, and ownership
  requirements permit it.
- [x] Record compile-time/include-graph evidence where a public header split is
  justified; do not treat smaller files alone as an architectural outcome.

#### Acceptance Gate

- No split introduces a new module, manager, service locator, public forwarding
  facade, or duplicate owner for one state transition.
- StaticMesh collision/render lifetime, Material compatibility/rendering,
  World play/tick/transition, and World collision tests pass unchanged.
- Supported public headers remain self-sufficient and expose fewer unrelated
  declarations/includes where the split intended that result.
- Golden serialization and Cooked runtime results remain unchanged.

### Stage 6: Reshape test composition and complete qualification

Dependencies: all production stages complete.

- [x] Split `EngineTests/CMakeLists.txt` into domain-oriented included fragments
  while retaining one source owner, one suite owner, exact target metadata,
  links, runtime-only dependencies, resource locks, timeouts, and discovery
  order.
- [x] Keep production-private-source exceptions explicitly owned and justified;
  do not broaden include paths or export production symbols solely for the
  CMake split.
- [x] Verify source-ownership, exclusion, registry, discovery, deployment, and
  aggregate policy after the composition move.
- [x] Run the smallest affected targets during migration, the relevant
  bounded domains after each cross-module stage, `fast-all` for broad ordinary
  feedback, and the full ordinary native aggregate because shared runtime and
  test infrastructure changed.
- [x] Complete clean editor and game builds using the repository Build and Run
  workflow; inspect the final Runtime/Developer/Editor dependency closures and
  deployed binaries.
- [x] Run Cooked runtime validation without authoring modules, source files,
  translators, offline compressors, or DDC builders in the game closure.
- [x] Move lasting decisions from this plan to the owning module, asset,
  rendering, source-transaction, build, and native-test documentation.
- [x] Update `Current Status`, stage checklists, `Last reviewed`, lifecycle
  metadata, and archive eligibility only after every acceptance gate has
  evidence.

#### Acceptance Gate

- Native-test configuration reports no unowned/duplicate source or suite,
  invalid metadata, runtime deployment collision, or unjustified private-source
  seam.
- Focused, bounded-domain, `fast-all`, full ordinary native, clean editor/game
  build, dependency-closure, deployment, Cook, and source-free Cooked runtime
  gates pass.
- Runtime Engine contains no removed compatibility facade, TextureCube
  authoring registry, or generic mounted-source implementation.
- Lasting documentation describes final ownership without requiring this plan
  as a competing specification.
- The all-scope plan validator passes and the task's changes are committed with
  exact plan/stage provenance.

## Validation Matrix

| Change area | Minimum focused validation | Broader/final validation |
| --- | --- | --- |
| Dead/public surface and dependency visibility | Engine public-header compile coverage, `EngineViewportHeaderTests`, affected direct-consumer builds | runtime/game closure and clean editor/game builds |
| Primitive physics descriptor extraction | `PhysicsSceneTests` plus the smallest affected World/component cases | `@physics` ordinary domain |
| Mounted-source move | AssetCore package/source transaction tests, `EditorAssetWorkflowTests`, affected Texture/StaticMesh/Terrain source cases | `@asset-workflow`, `@texture`, `@static-mesh`, and `@terrain` as affected |
| TextureCube boundary | `TextureTests`, `SceneImportTests`, and focused TextureCube cases | `TextureCookIntegrationTests`, `SkyBoxTests`, `TextureThumbnailTests`, source-free Cooked runtime |
| Asset namespace hierarchy | AssetCore, `AssetBuildCoreTests`, `AssetImportCoreTests`, public-header/reflection generation, namespace-residue and registration checks | `TextureTests`, `SceneImportTests`, `EditorAssetWorkflowTests`, clean Editor/Game builds, and module/deployment closure |
| Cooked companion helper | affected asset Cook/load target for each migrated family | bounded asset-cook domains and source-free Cooked runtime |
| Payload wire primitives | exact golden/hostile codec target for each migrated value | `fast-all`, then full ordinary native aggregate at final shared-helper gate |
| StaticMesh/Material/World file and header splits | `StaticMeshTests`, `MaterialTests`, `WorldTests`, `PhysicsSceneTests` as affected | relevant Renderer/Vulkan integration only when render-resource behavior changes |
| Native-test CMake composition | configure, registry/source-ownership validation, one representative target per fragment | full ordinary native aggregate and deployment inspection |
| Plan/document lifecycle | `doc plan validate --scope all` | same validator before completion/archive transition |

Validation commands and recovery behavior follow
`Documentation/Agents/Testing.md`,
`Documentation/Development/Build/NativeTests.md`, and
`Documentation/Agents/BuildAndRun.md`. The implementation handoff records the
resolved targets selected by each bounded domain rather than maintaining a
test-count snapshot in this plan.

## Definition of Done

- Stage 0 records every deletion, compatibility, ownership, and helper-lifetime
  decision required by later stages.
- Empty/dead files and unneeded public surfaces are removed or explicitly
  retained with documented compatibility evidence.
- ApplicationCore is no longer a transitive public Engine dependency unless
  qualification proves a supported public-header requirement.
- Generic mounted-source transactions are owned and tested by AssetCore with no
  Engine dependency or forwarding facade.
- TextureCube authoring workflow is owned by StandardAssetImport/TextureBuild;
  Runtime Engine contains only runtime value, serialization, publication, and
  narrow lifecycle seams.
- Existing Asset modules use the coherent `Durin::Asset`,
  `Durin::Asset::Build`, and `Durin::Asset::Import` namespace hierarchy without
  changing module ownership, reflected identity, registration, or deployment
  boundaries.
- Cooked companion access and payload-wire mechanics have one coherent helper
  each where duplication justified extraction, while typed schemas remain with
  their values.
- Large Engine implementation/header files and Engine test CMake composition
  are split along existing responsibilities without new production modules or
  changed behavior.
- Authored package, DDC, Cooked payload, Material compatibility, runtime,
  editor, rollback, render-resource, and failure-classification contracts pass
  their complete validation matrix.
- Runtime/game closure remains free of source translators, authoring hosts,
  offline compressors, Build recipe modules, and editor UI modules.
- Lasting documentation is updated, plan validation passes, all task changes
  are committed, and the plan lifecycle is completed only with evidence.

## Deferred Follow-ups

- Resolved on 2026-08-13: DHT `LegacyNames` now supplies separately registered,
  read-only reflected-type aliases, while current qualified C++ names remain
  the only runtime and write identities. DAST v4 canonicalizes legacy class,
  struct, enum, declaring-type, and property-type identities at semantic read
  boundaries, and the ImportRecord migration gate proves old namespace bytes
  load and re-save with `Durin::Asset::Import` identities. The temporary
  `PersistentName` feature and annotations were removed.
- Consider broader reuse of import, Build, storage, parser, and wire diagnostic
  result types only after this plan completes and concrete lossless mappings
  justify it; Stage 3.5 resolves names and exact collisions but does not create
  a universal diagnostic abstraction.
- Extract runtime asset value types into a lower module only when a measured
  non-Engine runtime consumer or deployment boundary proves the need; move each
  value and its Serialize contract together.
- Remove Material v1/v2 compatibility only through a separately approved
  compatibility-window decision with content/deployment evidence.
- Replace or reorganize broader Actor/Component/World architecture only after
  profiling, ownership, or feature pressure identifies a concrete boundary;
  line count alone is insufficient.
- Split native-test targets or change classifications only through a dedicated
  ownership/selection task when the current target lifecycle is demonstrably
  incoherent.
- Add remote Build execution, shared DDC, worker processes, or distributed Cook
  only through their owning roadmap/plan.

## Related Documentation

- [Code Modules](../../../Workspace/CodeModules.md)
- [Engine Asset Serialization and Build Boundary Plan](EngineAssetBuildBoundary.md)
- [Developer Asset Build Modularization Plan](DeveloperAssetBuildModularization.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Native C++ Tests](../../../Development/Build/NativeTests.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/Public`
- `Engine/Source/Runtime/Engine/Private`
- `Engine/Source/Runtime/AssetCore/Public/Asset/SourcePath.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Developer/AssetBuildCore`
- `Engine/Source/Editor/AssetImportCore`
- `Engine/Source/Editor/StandardAssetImport`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Developer/GeometryBuild`
- `Engine/Tests/Native/EngineTests`
