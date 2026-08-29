# Factory Asset Import Migration Plan

Summary: Migrate first-time asset import to package-backed factories and introduce reimport orchestration after the creation path stabilizes.

Last reviewed: 2026-08-28

Status: Archived
Completed: 2026-08-28

## Current Status

Stages 0 through 5 are complete. The repository now has standalone creation
and reimport authorities for every supported production family:

- `DurinEd` owns `DFactory`, reflected discovery, bounded per-invocation
  diagnostics, and self-registering reimport handlers. The lightweight
  `AssetTools` editor module depends on DurinEd and owns `IAssetTools` plus safe
  package discard; DurinEd does not link AssetForgeBuiltins.
- `CreatePackage` creates a live `Standalone` asset package without using
  `Asset::CreateAsset` or `AddToRoot`.
- `IAssetTools` creates and adopts a package into Engine residency, invokes
  `FactoryCreateNew` or `FactoryCreateFromFile`, validates the returned main
  asset, and discards a failed unsaved package.
- `Public` and `Standalone` object flags flow through `NewObject`; GC retains
  standalone packages without treating them as permanent roots.
- Texture2D production import now uses a reflected `DTexture2DFactory`, direct
  build-into population, and an explicit save after `IAssetTools` succeeds.

Texture2D, TextureCube, TerrainHeightmap, VolumeTexture, and standalone
StaticMesh now use the new path. TextureCube uses one reflected factory with
explicit panorama and six-face configurations; class-qualified lookup resolves
overlapping extensions while extension-only PNG lookup remains ambiguous.
All migrated families directly populate formal objects, use explicit save,
cover failed-package cleanup, and expose no production first-import wrappers.
Transient StaticMesh construction remains package-free and separate. All
first-import tests now use explicitly named Factory test helpers and no longer
depend on obsolete runtime result structs. Host-owned `NotifyAssetCreated`
callbacks refresh and reveal successful post-save imports without adding UI
policy to IAssetTools. Remaining `Asset::CreateAsset` calls in the import
closure belong to the documented Scene multi-output transaction and its
material creation helper.

`FReimportManager` now uses optional methods on the same reflected concrete
factories as the single class-aware discovery path. It exposes retained-source
and from-file capabilities, reports unsupported, missing-source, source/build,
success, and persistence outcomes separately, and applies optional saving only
after a complete live replacement. Texture2D asynchronous completion and all
synchronous family handlers preserve their prior candidate/swap safety.
Content Browser no longer owns reimport family enums or a closed host switch;
Scene advertises no reimport capability. Family free functions remain only as
focused seams for family tests and non-host helpers.

Stage 6 confirmed that Scene deliberately retains its private Engine
materialization seam, family build adapters, dependency binding, rollback, and
atomic package-set save without calling single-object AssetTools. Obsolete
feature-module reimport entrypoints, helpers, includes, and closed routing types
were removed. Qualification passed EditorOperationTests (17), TextureTests
(78), TerrainHeightmapTests (11), ContentBrowserWorkflowTests (59 passed, 1
skipped), EditorAssetWorkflowTests (31), AssetImportTests (17), SceneImportTests
(4), TerrainHeightmapCookTests (1), and AssetCookTests (13). The first
`fast-all` run exposed an unrelated parallel render-admission failure in
MaterialTests; the isolated target passed all 100 tests and the complete
62-target `fast-all` rerun passed. The full `all` editor build completed, and a
hidden DurinEditor smoke launch remained healthy for eight seconds until the
test harness terminated it.

## Goal

Make `CreatePackage -> IAssetTools -> concrete DFactory -> NewObject` the
single first-import path for every supported standalone asset family, while
preserving family-owned decoding, build, import-data, DDC, and source-hint
behavior. After that creation path is stable, introduce `FReimportManager` as
the editor-wide discovery and routing authority for mutation of existing
assets without weakening failed-reimport safety.

## Scope

- Establish module ownership that lets concrete built-in factories depend on
  the generic Factory/AssetTools contracts without a `DurinEd` and
  `AssetForgeBuiltins` dependency cycle.
- Define invocation settings and diagnostic propagation for stateless factory
  CDO discovery and optional per-operation concrete factory instances.
- Add concrete factories for Texture2D, TerrainHeightmap, VolumeTexture,
  standalone StaticMesh, and both TextureCube source layouts.
- Refactor family import implementations into reusable capture/decode/build-
  into helpers that populate a newly created formal object directly.
- Migrate import dialogs, Content Browser dispatch, tests, and programmatic
  first-import callers to `IAssetTools`.
- Retire family first-import entrypoints and their `Asset::CreateAsset` call
  sites after the corresponding callers migrate.
- Introduce `FReimportManager` only after first-import Factory behavior is
  qualified, then migrate the existing finite built-in reimport dispatch.
- Update lasting import architecture and source-file workflow documentation as
  each contract becomes implemented.

## Non-Goals

- Removing `Asset::CreateAsset` from unrelated Material, Level, test-fixture,
  duplication, redirector, or general Engine workflows; that repository-
  wide removal requires separate ownership and validation.
- Routing runtime import or cooked builds through editor factories.
- Third-party importer plugins, hot-unloadable providers, asynchronous generic
  import jobs, planning graphs, or a new provider registry.
- Treating Scene import as a single-object factory or adding whole-scene
  reimport, generated-output reconciliation, or ownership tombstones.
- Making extension-only lookup choose silently among ambiguous formats such as
  PNG, which may represent Texture2D, VolumeTexture, TextureCube, or terrain
  data depending on the requested class and settings.
- Removing candidate/swap behavior from reimport. Direct in-place creation is
  selected only for a new disposable package.

## Design Decisions and Invariants

- `DurinEd` owns generic `DFactory` discovery and reimport handlers. The
  lightweight editor `AssetTools` module depends on DurinEd and owns
  `IAssetTools` plus package-backed creation results; neither depends on
  concrete asset families. AssetForgeBuiltins, feature editors, and
  command-line editor tools consume the required layer directly.
- Concrete built-in factories live with family import code in
  `AssetForgeBuiltins`; feature editor modules own dialogs and configure an
  invocation, but do not own decode/build behavior.
- A discovered factory CDO is immutable descriptor/default behavior. Default
  imports may call it directly. A dialog requiring non-default settings creates
  a transient instance of that concrete factory class, configures its typed
  fields, and passes the instance to `IAssetTools`.
- `SupportedClass` is authoritative. Extension lookup narrows candidates but
  never overrides a requested class, and ambiguous extension-only lookup fails
  with a diagnostic or presents an explicit user choice.
- `IAssetTools` owns destination validation, package creation, the Factory
  call, main-asset validation, package discard on creation failure, and editor
  creation notification. A successful creation returns a live Dirty package;
  saving remains a separate operation and save failure does not invalidate the
  created object.
- A concrete Factory owns source admission, immutable source capture, decode,
  family build invocation, direct population of the new object, and creation of
  concrete `DAssetImportData`. It does not call `SavePackage`.
- First import may create the formal object before every operation completes.
  Any failure discards the complete unsaved package, so no partially initialized
  object becomes an accepted asset.
- Reimport mutates an existing accepted object and therefore uses a different
  safety boundary: all fallible source capture/decode/build work completes into
  private candidate data before one final family-owned swap. The creation path
  does not force a public `Publish*Product` abstraction to remain solely for
  reimport.
- `FReimportManager` owns capability queries, source-path selection, handler
  selection, invocation, and terminal editor notification. It does not decode,
  build, save, or retain family-specific settings. Concrete family handlers may
  initially adapt the existing reimport functions; whether those handlers are
  optional `DFactory` methods or separate small interfaces is fixed in the
  reimport stage before call-site migration.
- All Factory discovery, asset creation, and reimport routing remain editor
  game-thread operations. Family build systems may perform their existing
  bounded worker work, but publication and accepted-object mutation return to
  the game thread.
- Source hints and concrete import-data schemas remain compatible throughout
  the cutover. Moving a family to Factory does not by itself change its authored
  package schema or DDC key.
- Scene retains its private preflight, dependency ordering, peer binding, and
  atomic multi-package save. It may reuse lower-level family build-into helpers
  but does not call single-object `IAssetTools::ImportAsset` for a partially
  committed output set.

## Current Foundations and Gaps

| Area | Foundation | Gap |
| --- | --- | --- |
| Object lifecycle | `Public`/`Standalone`, flagged `NewObject`, and standalone `CreatePackage` are implemented | Old Engine residency still uses a separate rooted-package path; Factory migration must not create duplicate live package identities |
| Factory discovery | Reflected CDO discovery, `SupportedClass`, formats, cached lookup, and invalidation exist | No concrete production factory exists; duplicate/ambiguous format diagnostics and invocation-instance conventions need qualification |
| Asset tools | `IAssetTools::CreateAsset` and `ImportAsset` own package creation and failed-package discard | No creation notification, typed settings convention, family diagnostic channel, or successful production caller exists |
| Family import | Direct family functions have qualified capture, decode, build, import-data, save, and reimport behavior | Creation, population, persistence, and result reporting are combined around `Asset::CreateAsset` |
| Family build | Texture, mesh, and terrain domains already own normalized inputs and DDC behavior | Several APIs expose build-product/publication boundaries rather than a direct build-into-new-object path |
| Editor dispatch | Built-in menus and dialogs explicitly select a finite family and settings | They call family free functions instead of configuring a Factory and invoking `IAssetTools` |
| Reimport | Every standalone family has explicit Reimport and Reimport From File functions | DurinEd uses a closed switch; there is no single capability/query/result manager |
| Scene | Creation-only multi-output import is dependency ordered and atomically saved | It shares helpers with standalone families but must remain outside single-object Factory commit semantics |

## Implementation Stages

### Stage 0: Stabilize the Factory and AssetTools boundary

The checked work below records the original migration. A later ownership
refinement moved the generic Factory and reimport contracts back to DurinEd and
reversed the dependency so AssetTools consumes them.

- [x] Create the `AssetTools` editor module and move `DFactory`, `IAssetTools`,
  their implementation, reflection registration, tests, and public umbrella
  ownership out of DurinEd without changing behavior.
- [x] Remove the resulting obsolete DurinEd-to-AssetForgeBuiltins dependency if
  no remaining DurinEd implementation requires it; otherwise document the
  exact remaining owner and prevent a reverse dependency.
- [x] Define the per-operation factory-instance convention: immutable CDO for
  discovery/defaults, transient configured instance for dialog settings, and
  no mutable shared CDO state.
- [x] Add a bounded diagnostic channel to Factory invocation so capture,
  decode, build, and validation failures reach `FAssetToolsResult` without
  global last-error state.
- [x] Make class-plus-extension lookup explicit and return all candidates for
  ambiguous extension-only selection.
- [x] Add a single safe package discard operation and reconcile
  `FindPackage`, Engine residency, save, reload, and unload behavior for a
  Factory-created package.
- [x] Add lifecycle tests covering creation success, Factory failure, wrong
  class/Outer, GC, save failure, save/reload, and explicit unsaved discard.

#### Acceptance Gate

- AssetTools has an acyclic module boundary, a concrete test Factory can create
  and discard a real packaged object, and saving then loading that object cannot
  create a second live Package for the same path.

### Stage 1: Prove direct creation with Texture2D

- [x] Add `DTexture2DFactory` with the exact supported class and existing
  image extensions; defaults match the current Texture2D import dialog.
- [x] Split source capture/decode, import-data construction, and direct
  `DTexture2D` population from `ImportTexture2DAsset` without moving file
  decoding into the runtime asset class.
- [x] Add or adapt a builder entrypoint that fills a newly created Texture2D
  directly and leaves any private candidate/swap representation inside the
  builder only where reimport or asynchronous compilation requires it.
- [x] Route the Texture2D import dialog and programmatic first-import tests
  through a configured factory instance and `IAssetTools::ImportAsset`.
- [x] Preserve current source hints, content hashes, settings, DDC identities,
  Dirty state, save-success, and save-failure behavior.
- [x] Keep the existing Texture2D reimport functions unchanged in this stage.
- [x] Remove `ImportTexture2DAsset` after repository searches show no production
  or test caller needs the compatibility wrapper.

#### Acceptance Gate

- Texture2D first import has no `Asset::CreateAsset` or public
  build-product-publication call, failed import leaves no live Package, a
  successful unsaved import survives GC as Dirty state, save/reload preserves
  authored and runtime data, and existing reimport coverage still passes.

### Stage 2: Migrate single-source standalone families

- [x] Apply the Texture2D pattern to `DTerrainHeightmapFactory` and preserve
  source-format/profile metadata and terrain build behavior.
- [x] Apply the pattern to `DVolumeTextureFactory`, including atlas layout,
  channel selection, dimensions, and import-data settings.
- [x] Apply the pattern to `DStaticMeshFactory`, preserving immutable encoded
  source capture, Assimp options, canonical imported data, material slots,
  collision/build settings, authored bulk data, and DDC behavior.
- [x] Migrate the Terrain, VolumeTexture, and standalone StaticMesh dialogs and
  focused tests to IAssetTools.
- [x] Keep transient StaticMesh preview creation separate unless it naturally
  uses the same build-into helper without creating a formal package.
- [x] Remove each first-import free function only after its family caller and
  compatibility search gate passes.

#### Acceptance Gate

- TerrainHeightmap, VolumeTexture, and standalone StaticMesh first import use
  concrete factories exclusively; each family passes failure cleanup,
  save/reload, DDC cold/warm, import-data, and existing reimport regression
  coverage with no source schema change.

### Stage 3: Migrate TextureCube and multi-source invocation

- [x] Add `DTextureCubeFactory` with explicit panorama and six-face invocation
  settings rather than inferring layout solely from an ambiguous extension.
- [x] Support one panorama filename through the ordinary file argument and six
  immutable face sources through a typed configured factory instance/context.
- [x] Reuse direct TextureCube capture/decode/build-into helpers for both
  layouts while preserving face order, HDR/format validation, source hints,
  and build settings.
- [x] Route both TextureCube dialog modes through IAssetTools and remove their
  first-import free functions after call-site closure.
- [x] Verify that PNG/HDR extension overlap never causes an unintended factory
  choice when the requested class or source layout is known.

#### Acceptance Gate

- Panorama and six-face TextureCube imports create one valid formal asset via
  the same concrete factory, reject incomplete/ambiguous input before accepted
  publication, and preserve current save/reload and reimport behavior.

### Stage 4: Cut over editor dispatch and retire first-import compatibility

- [x] Make Content Browser and feature editor import actions select a supported
  class/factory pair and invoke IAssetTools instead of a family free function.
- [x] Add the creation notification needed for Content Browser refresh,
  selection/reveal, and editor opening without making IAssetTools responsible
  for UI presentation.
- [x] Migrate command-line/editor utility callers that perform supported
  first-import operations or explicitly retain a documented non-Factory path.
- [x] Delete obsolete first-import result structs and wrappers after all
  production and test callers migrate; retain family capture/build/reimport
  helpers under names that describe their remaining responsibility.
- [x] Search the standalone import closure for `Asset::CreateAsset`, direct
  package rooting, and old import entrypoints, and disposition every match.
- [x] Update the implemented import architecture and user workflow documents
  to make Factory/AssetTools the first-import authority.

#### Acceptance Gate

- Every supported standalone first-import UI and programmatic entrypoint routes
  through IAssetTools and a concrete factory; no compatibility wrapper or old
  direct first-import dispatch remains, and the editor's visible behavior and
  diagnostics remain equivalent or intentionally documented.

### Stage 5: Introduce FReimportManager

- [x] Record the final handler shape before implementation: reuse optional
  concrete Factory reimport methods or add a separate small family handler
  interface, but keep one reflected/discoverable ownership path and no second
  generic provider registry.
- [x] Implement `FReimportManager` capability queries for a loaded object,
  retained source hints, Reimport, and Reimport From File requests.
- [x] Make handler selection class-aware and deterministic; missing or
  ambiguous handlers fail with a family/actionable diagnostic.
- [x] Define a result model that distinguishes unsupported, missing source,
  source/build failure, successful live replacement, and persistence failure.
- [x] Adapt Texture2D first, proving that failed capture/decode/build preserves
  the complete existing live object and persisted package.
- [x] Migrate TerrainHeightmap, VolumeTexture, StaticMesh, and both TextureCube
  layouts without changing their concrete import-data schemas.
- [x] Replace DurinEd's closed reimport switch and direct family calls with the
  manager, then remove obsolete routing enums/functions.
- [x] Keep package saving an explicit manager option or caller step; a valid
  live replacement remains Dirty when persistence fails.

#### Acceptance Gate

- The editor has one class-aware Reimport/Reimport From File authority, every
  supported standalone family preserves prior accepted state on candidate
  failure, save failure remains distinguishable from mutation failure, and no
  Scene reimport capability is advertised.

### Stage 6: Scene boundary, cleanup, and qualification

- [x] Audit SceneDirectImport for reusable family capture/decode/build-into
  helpers while retaining complete-output preflight, private dependency order,
  peer binding, rollback, and atomic bundle save.
- [x] Do not call single-object IAssetTools in a way that exposes partial Scene
  output; document any deliberate direct package/materialization seam.
- [x] Remove obsolete module dependencies, includes, result types, tests, and
  comments left by the standalone importer cutover.
- [x] Run focused family tests, editor workflow tests, package lifecycle tests,
  Cook/runtime closure, the appropriate aggregate native-test scope, complete
  editor build, and smoke coverage selected by repository guidance.
- [x] Complete lasting documentation, record validation evidence, and close the
  plan only when every required family and caller is migrated.

#### Acceptance Gate

- Standalone first import and reimport use the new authorities, Scene remains
  atomic and creation-only, runtime/Cook do not depend on editor factory code,
  all selected validation passes, and lasting documentation describes only the
  implemented architecture.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Factory discovery | Concrete factories are found by exact class; normalized extensions narrow correctly; duplicate/ambiguous mappings fail deterministically; cache invalidation rediscovers newly available classes |
| Package lifecycle | Success creates one `Standalone` Package with one `Public` main asset; ordinary GC preserves it; Factory failure and explicit discard release the Package path; save/reload never duplicates live identity |
| Direct creation | Each family creates and fills the formal object directly without routing first import through `Asset::CreateAsset`; partial failure never becomes an accepted asset |
| Diagnostics | Invalid path, unsupported class/format, missing source, decode failure, build failure, wrong Factory result, collision, and save failure remain distinguishable and reach the initiating UI/tool |
| Source metadata | Project-relative, asset-relative, and external absolute hints; content hashes; byte counts; roles; and family settings round-trip unchanged |
| Family data | Texture dimensions/mips/formats, cube layout, volume channels, terrain profile, StaticMesh canonical data/material slots/collision, and authored bulk payloads survive save/reload |
| DDC and compilation | Cold, warm, missing, and corrupt derived-data cases retain family-owned build behavior without making Factory a second job system |
| Reimport safety | Source/candidate failure preserves the complete prior live and persisted asset; successful mutation is Dirty; persistence failure reports separately and permits retry |
| Editor workflows | Dialog settings configure the correct invocation instance; Content Browser refresh/reveal and notifications occur once; Reimport capability is class-aware |
| Scene exception | Multi-output preflight, dependency order, rollback, and atomic save remain intact; no partial output is exposed through IAssetTools |
| Runtime closure | Cooked/Game targets load migrated assets without AssetTools, concrete Factory classes, source decoders, or editor import data |
| Removal | Repository searches find no retired standalone first-import wrapper, direct UI family dispatch, or standalone import call to `Asset::CreateAsset` |
| Documentation | Changed/all documentation and all-plan validation pass; implemented architecture and user guides are updated without copying stage status into contract docs |

## Definition of Done

- Texture2D, TextureCube panorama/six-face, VolumeTexture,
  TerrainHeightmap, and standalone StaticMesh first import use concrete
  reflected factories through IAssetTools.
- New standalone assets are created as one `Public` main object under one
  `Standalone` Package, filled directly, left Dirty on success, and discarded
  completely on creation failure.
- All old first-import family entrypoints, result wrappers, editor dispatch,
  and standalone-import `Asset::CreateAsset` calls are removed or explicitly
  retained outside scope with documented ownership.
- `FReimportManager` is the single editor authority for supported standalone
  Reimport and Reimport From File actions, while family code owns candidate
  build and safe live replacement.
- Failed reimport preserves prior accepted state; successful replacement and
  persistence remain separate observable outcomes.
- Scene remains creation-only and atomically publishes its complete
  dependency-ordered output set.
- Module dependencies are acyclic, runtime/Cook closure excludes editor import
  code, selected tests/build/smoke gates pass, lasting documentation is current,
  and the plan records evidence for every acceptance gate.

## Deferred Follow-ups

- Repository-wide removal of `Asset::CreateAsset` outside the standalone import
  closure.
- General `Create New` factories for Material, Level, animation authoring, or
  other assets not imported from an external source.
- Optional progress/cancellation UI for individual family compilation where
  the family build system already exposes safe cancellation.
- Third-party factory plugins or dynamically unloadable handlers.
- Whole-scene reimport and generated-output reconciliation.
- Automatic source-file copying, relocation, deletion, or version-control
  integration.

## Related Documentation

- [Asset Import Architecture](../../../Editor/Architecture/AssetImportFramework.md)
- [Source File Workflows](../../../Editor/Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Asset Import Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetImportSimplification.md)
- [Agent Build and Run](../../../Agents/BuildAndRun.md)
- [Agent Testing](../../../Agents/Testing.md)

## Related Code

- [`DFactory`](../../../../Engine/Source/Editor/DurinEd/Public/Factories/Factory.h)
- [`IAssetTools`](../../../../Engine/Source/Editor/AssetTools/Public/AssetTools/IAssetTools.h)
- [`CreatePackage`](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h)
- [`AssetForgeBuiltins`](../../../../Engine/Source/Editor/AssetForgeBuiltins)
- [`Texture2DImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`TextureCubeImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImport.cpp)
- [`VolumeTextureImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImport.cpp)
- [`TerrainHeightmapImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainHeightmapImport.cpp)
- [`StaticMeshImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
- [`SceneDirectImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
- `Engine/Source/Editor/DurinEd/Public/Editor/Import/BuiltinImportDispatch.h`
