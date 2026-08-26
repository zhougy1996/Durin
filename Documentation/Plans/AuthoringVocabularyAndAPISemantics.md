# Authoring Vocabulary and API Semantics Plan

Summary: Replace overloaded Authoring names with lifecycle-specific asset, load, save, and editor-operation vocabulary while preserving precise Authored state terminology.

Last reviewed: 2026-08-26

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete. StaticMesh build, post-load, and source mutation
have independent modular features because their callers require independent
availability; one AssetForgeBuiltins implementation object still implements
all three contracts. TextureCube and VolumeTexture now use post-load feature
vocabulary, while Terrain derived-data loading and source mutation use separate
features and generation-safe publication names.

The Stage 1 `AssetForgeBuiltins` module closure builds; `StaticMeshTests` passed
74/74, `TextureTests` passed 78/78, and `TerrainHeightmapTests` passed 11/11.
Stage 2 renamed content-write, project-edit, asset-operation, package-
serialization, and save-readiness APIs, removed the AssetCore authoring
umbrella, and retained `AuthoringWritable` only as a legacy descriptor input.
Launch and all affected editor/program targets build. `CoreFileSystemTests`
completed with 36 passes and 2 skips; `AssetMountedSourceTests` passed 4/4,
`AssetSaveReadinessTests` 3/3, `AssetPackageTests` 123/123,
`EditorAssetWorkflowTests` 35/35, and the focused DurinHeaderTool descriptor and
configuration tests 57/57. Stage 3 is the next open stage.

## Goal

Make public names communicate the lifecycle, authority, and side effects of
each operation without requiring callers to know that `Authoring` previously
served as a catch-all editor term.

The completed result must:

- reserve `Authored` for authoritative persisted object/package state;
- identify content-write admission and project edit ownership explicitly;
- identify serialization and save-readiness APIs explicitly;
- distinguish import/build products from uncooked derived-data loading and
  source-reference mutation;
- name LevelEditor and MaterialEditor APIs after the concrete mutation,
  placement, build, query, or command boundary they expose;
- remove obsolete umbrella headers, old feature identifiers, file names, test
  names, and documentation terms rather than preserving parallel vocabulary.

## Scope

This plan owns semantic renames and narrowly required interface decomposition
across:

- `Core` mount write policy and process-local project edit ownership;
- `AssetCore` asset operations, mounted-source mutation, package
  serialization, and umbrella include cleanup;
- `Engine` asset save readiness, StaticMesh build/publication products,
  TextureCube and VolumeTexture uncooked post-load seams, and Terrain
  derived-data load/source-mutation seams;
- `StaticMeshBuild`, `TextureBuild`, `TerrainBuild`, `AssetForge`, and
  `AssetForgeBuiltins` implementations and registrations that satisfy those
  Engine contracts;
- `DurinEd`, `ContentBrowser`, asset editors, and programs that consume the
  renamed public APIs;
- LevelEditor StaticMesh mutation, Terrain placement, SkyBox placement, and
  graybox scene-build APIs;
- MaterialEditor graph operation APIs;
- affected native targets, fixtures, schemas, checked-in test data, and
  authoritative documentation.

The migration includes local variables, diagnostics, operation-group labels,
feature-name strings, and test fixture names when they describe one of the
renamed contracts. It does not require changing ordinary English prose where
“authoring” genuinely describes the human activity rather than an API owner.

## Non-Goals

- Renaming `Authored` concepts such as `FAuthoredOverrideLedger`, authored
  package save domains, authored bulk storage, authored revisions, or authored
  material parameter names.
- Changing `.dasset`, DDC, cooked payload, import graph, material program,
  transaction, or actor mutation behavior.
- Moving asynchronous scheduling out of the owners established by the asset
  compilation and async-operation contracts.
- Replacing the modular-feature mechanism or adding a general provider,
  coordinator, service, or operation-dispatch layer.
- Splitting stateful services and managers whose lifetime or concurrency
  ownership is already explicit.
- Broad formatting, comment, namespace, or test-suite cleanup unrelated to the
  renamed boundaries.
- Preserving deprecated C++ aliases, forwarding headers, duplicate feature
  identifiers, or dual public entry points solely to ease this repository-wide
  source migration.

## Design Decisions and Invariants

### Vocabulary

| Term | Reserved meaning | Representative use |
| --- | --- | --- |
| `Authored` | Authoritative persisted user intent, contrasted with defaults, derived data, cooked data, or runtime state | authored overrides, authored packages, authored revisions |
| `ContentWrite` / `ContentWritable` | Admission to mutate files beneath a mounted content root | mount policy and Engine-content write override |
| `EditOwnership` | Exclusive process ownership that prevents two editor processes from editing one project | project lock acquisition and release |
| `AssetOperations` | Create, duplicate, and save operations on asset packages | AssetCore public operations header |
| `PackageSerialization` | Byte serialization, canonicalization, atomic package publication, and save overrides | package serialization header and options |
| `SaveReadiness` | Family-specific validation immediately before saving an asset | optional Engine readiness feature |
| `BuildProduct` | Detached CPU-owned result produced from import/build inputs | StaticMesh render and collision products |
| `PostLoad` / `DerivedDataLoad` | Uncooked DDC lookup, rebuild, wait, and publication during asset availability | texture post-load and Terrain load state |
| `SourceMutation` | Change, replace, or relocate a persisted mounted-source reference | family-specific source reference boundary |
| `Mutation`, `Placement`, `Build`, `Operations` | Concrete editor action or stateless command surface | LevelEditor and MaterialEditor APIs |

`Authoring` is not a lifecycle state, thread owner, data domain, or generic
synonym for “editor-only.” New names must select one row from this table.

### Public API migration rules

- Rename declarations, definitions, includes, CMake source lists, feature-name
  strings, registrations, tests, fixtures, and authoritative documentation in
  the same stage.
- Do not add aliases from old type/function names to new names and do not leave
  forwarding headers at old paths.
- Preserve externally observable behavior, failure ordering, thread
  requirements, cancellation, rollback, transaction boundaries, and data
  formats unless a stage explicitly states otherwise.
- Keep module ownership unchanged. A rename must not make Runtime depend on an
  Editor or Developer implementation module.
- Keep one module-owned implementation object free to implement several narrow
  modular-feature contracts; semantic interface names do not require one
  allocation or registration owner per method.
- Use stateless operation/domain names for static editor entry points. Do not
  introduce `Service`, `Manager`, `Coordinator`, or `Provider` merely to replace
  `Authoring`.

### Selected boundary changes

The planned public direction is:

| Current boundary | Selected semantic direction |
| --- | --- |
| `AssetAuthoring.h` | Remove the umbrella; consumers include the exact AssetCore contract |
| `Asset/AssetAuthoringOperations.h` | `Asset/AssetOperations.h` |
| `Asset/PackageAuthoring.h` | `Asset/PackageSerialization.h` |
| `IAssetAuthoringReadinessFeature` | `IAssetSaveReadinessFeature` |
| `ITextureCubeAuthoringFeature` | `ITextureCubePostLoadFeature` |
| `IVolumeTextureAuthoringFeature` | `IVolumeTexturePostLoadFeature` |
| StaticMesh authoring product/failure names | StaticMesh build product/failure names |
| Broad StaticMesh optional editor seam | Explicit build, uncooked-load, and source-mutation operations, split only where independent callers require separate availability |
| Terrain authoring load state | Terrain derived-data load state and wait/publication vocabulary |
| mount `AuthoringWritable` policy | `ContentWritable` policy |
| Engine authoring mutation context | explicit Engine-content write permission |
| project authoring ownership | project edit ownership |
| StaticMesh level authoring | StaticMesh level mutations |
| Terrain level authoring | Terrain placement |
| SkyBox level authoring | SkyBox placement |
| graybox scene authoring | graybox scene build |
| material graph authoring facade | material graph operations |

Stage 1 must choose the smallest StaticMesh interface split that makes
availability truthful: build, uncooked load, and source mutation may remain on
one explicitly named editor-operations feature only if every registered
implementation and consumer requires the full operation set. Otherwise they
become separate modular features. This is the only intentionally deferred
interface-shape decision; the vocabulary and module ownership are fixed.

### Project descriptor compatibility

The canonical project mount key becomes `ContentWritable`. Checked-in schemas,
fixtures, examples, and generated descriptor expectations migrate in the same
stage. Because project descriptors have no format-version migration boundary,
the loader accepts legacy `AuthoringWritable` only as an input migration path.
It rejects descriptors containing both keys, while schemas, checked-in data,
examples, generated descriptors, and all C++ APIs use only `ContentWritable`.

### Tests

Behavioral coverage follows the renamed contract. Rename targets and fixtures
whose identity contains an obsolete API name. Remove only tests whose sole
purpose is a deleted forwarding header, alias, duplicate registration path, or
other compatibility scaffolding; do not delete failure, rollback, cancellation,
stale-generation, transaction, serialization, or schema coverage as “rename
cleanup.”

## Current Foundations and Gaps

### Foundations to preserve

- Asset data lifecycle documentation already defines import, build,
  compilation, and `Authored` as distinct terms.
- Texture2D already uses `ITexture2DPostLoadFeature`, providing the target
  vocabulary for TextureCube and VolumeTexture.
- StaticMesh Level editing already models requests, mutation plans, deltas,
  execution contexts, and results; most of its internal vocabulary is more
  precise than its file, diagnostic, state, and facade names.
- Terrain Level editing already represents a finite placement request, plan,
  and result.
- Material graph edits already route through a stateless candidate-validated
  command boundary with transaction and compile-generation invariants.
- `IAssetCompilingManager` and domain owners already own asynchronous
  compilation scheduling; this plan does not recreate scheduling ownership.

### Gaps to close

- `Authoring` appears in 132 source files across 19 module/program groupings,
  with the highest concentrations in `AssetForgeBuiltins`, `Engine`,
  `LevelEditor`, `DurinEd`, and `AssetCore`.
- The 24 source paths containing `Authoring` imply false architectural
  categories even when their declarations are already build, post-load,
  placement, or mutation operations.
- `IStaticMeshAuthoringFeature` combines file build, uncooked post-load, and
  source-reference mutation while its product is already aliased as
  `FStaticMeshBuildProduct` by `StaticMeshBuild`.
- TextureCube and VolumeTexture use authoring feature names for the same
  uncooked post-load role that Texture2D names directly.
- Terrain uses authoring names for derived-data load generation, asynchronous
  wait, publication, and source-reference mutation.
- `AssetAuthoring.h` hides whether consumers need asset creation, mount policy,
  mutation, or serialization.
- Asset save readiness, package serialization, content write permission, and
  project edit locking share one misleading word despite different owners and
  failure semantics.
- LevelEditor and MaterialEditor static APIs use `Authoring` where their
  request/result models already expose the concrete operation.
- Project schema, fixture, test, feature identifier, diagnostic, and
  documentation spellings would preserve the old vocabulary unless migrated
  with their public contracts.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory source paths, public declarations, registrations, schema keys,
  tests, and authoritative documentation containing `Authoring` or `Authored`.
- [x] Classify `Authored` persisted-state concepts as retained vocabulary.
- [x] Select lifecycle-specific replacement vocabulary and dependency-ordered
  stages.
- [x] Define compatibility, test-retention, module-ownership, and validation
  rules.

#### Acceptance Gate

- The plan distinguishes retained `Authored` state from ambiguous `Authoring`
  APIs, identifies the only deferred interface-shape decision, and defines
  independently buildable migration stages with objective validation.

### Stage 1: Clarify Engine optional asset-operation seams

- [x] Audit every caller and implementation of the StaticMesh optional seam,
  then record whether build, uncooked load, and source mutation require one or
  multiple independently available modular features.
- [x] Rename `FStaticMeshAuthoringProduct`, collision product, failure stage,
  publication parameters, file paths, and diagnostics to build-specific
  vocabulary; remove the redundant Developer-module build-product alias.
- [x] Replace `IStaticMeshAuthoringFeature` with the selected explicit
  operation contract or contracts without changing Runtime-to-Developer/Editor
  dependency direction.
- [x] Rename TextureCube and VolumeTexture authoring features and feature-name
  strings to uncooked post-load vocabulary consistent with Texture2D.
- [x] Rename Terrain feature, state, load generation, wait, failure,
  publication, and operation-group labels to derived-data load and source
  mutation vocabulary.
- [x] Rename the AssetForgeBuiltins implementation aggregate and support
  fixtures after the contracts it implements; keep registration lifetime and
  shutdown ordering unchanged.
- [x] Update StaticMeshBuild, TextureBuild, TerrainBuild, AssetForgeBuiltins,
  Engine consumers, cook paths, editor recovery paths, and focused tests in the
  same change.
- [x] Update the asset data lifecycle, VolumeTexture, TerrainHeightmap, import,
  and modular-feature ownership documents that name the old seams.

#### Acceptance Gate

- No active Runtime, Developer, or Editor source uses an `*AuthoringFeature`,
  `*AuthoringProduct`, or authoring-load name for build, post-load,
  derived-data load, or source mutation; optional-feature ambiguity,
  cancellation, generation safety, publication, and shutdown behavior remain
  covered and the affected module closure builds.

### Stage 2: Clarify content-write, project-edit, and persistence APIs

- [x] Rename `FMountPoint::bAuthoringWritable`, mount mutation checks,
  `EMountedSourceMutationContext::EngineAuthoring`, parameters, helpers,
  diagnostics, and UI labels to content-write vocabulary.
- [x] Decide and record the project-descriptor compatibility treatment, then
  migrate the schema, loader, checked-in fixtures, examples, and tests to the
  canonical `ContentWritable` key.
- [x] Rename project authoring ownership APIs and lock diagnostics to project
  edit ownership without changing lock path, process lifetime, or release
  ordering.
- [x] Rename `AssetAuthoringOperations.h` to `AssetOperations.h` and
  `PackageAuthoring.h` to `PackageSerialization.h`; update exact includes and
  keep save/canonicalization behavior unchanged.
- [x] Remove the `AssetAuthoring.h` umbrella and replace every consumer with
  the smallest direct AssetCore includes it uses.
- [x] Rename asset authoring readiness declarations, feature identifiers,
  implementation files, tests, and program call sites to asset save readiness.
- [x] Rename remaining import-dialog and mounted-source variables such as
  `bEngineAuthoringContext` to explicit Engine-content write permission.
- [x] Update workspace project, asset package, asset catalog, mounted source,
  import, and asset lifecycle documentation.

#### Acceptance Gate

- Content write permission, project edit locking, asset operations, package
  serialization, and save readiness have distinct public names; the old
  umbrella and old C++ symbols are absent; descriptor compatibility is
  explicit; Core, AssetCore, Launch, relevant programs, and affected editor
  modules build and their focused tests pass.

### Stage 3: Rename LevelEditor APIs after their concrete operations

- [ ] Rename StaticMesh level files, private directories/test hooks, error,
  state, diagnostic, delta, facade, test source, and test cases to level
  mutation vocabulary while retaining request/plan/execute semantics.
- [ ] Rename Terrain level files, diagnostics, facade, and tests to Terrain
  placement vocabulary.
- [ ] Rename SkyBox level files and stateless entry point to TextureCube/SkyBox
  placement vocabulary.
- [ ] Rename graybox scene files and startup handler context to scene-build
  vocabulary; retain its lowering through StaticMesh mutations.
- [ ] Prefer namespace operations or a domain-named stateless facade where the
  current type has no identity or lifetime; do not introduce a service object.
- [ ] Update Scene Viewport, World Outliner, Details, startup commands,
  transaction integrations, and authoritative LevelEditor documents.
- [ ] Consolidate or delete only obsolete compatibility/name-only test support
  while preserving planning, stale-state, rollback, undo/redo, selection, and
  read-only coverage.

#### Acceptance Gate

- LevelEditor public APIs name mutation, placement, or build directly; no
  `LevelAuthoring`, `SceneAuthoring`, or private `Authoring` directory remains;
  transaction, optimistic validation, rollback, selection, and startup-command
  behavior are unchanged and focused LevelEditor tests pass.

### Stage 4: Rename the material graph boundary to operations

- [ ] Rename `MaterialGraphAuthoring` files and the
  `FMaterialGraphAuthoring` stateless facade to material graph operations.
- [ ] Update widgets, document lifecycle, automation, clipboard, layout,
  tests, and CMake source lists without changing command status or transaction
  behavior.
- [ ] Retain `authored` where it describes persisted program, presentation,
  parameter/resource names, revisions, or user intent.
- [ ] Remove obsolete compatibility/name-only test support while retaining
  candidate validation, no-change, stale owner, undo/redo, compile generation,
  clipboard schema, and geometry coverage.
- [ ] Rename the authoritative material graph document and repair direct links
  with the documentation move workflow.

#### Acceptance Gate

- Material graph public entry points communicate queries and commands through
  an operations boundary, the old header/class/test names are absent, graph
  serialization and editing behavior are unchanged, focused MaterialEditor
  tests pass, and all moved-document links validate.

### Stage 5: Complete the residual semantic audit

- [ ] Audit active source and non-archive documentation for `Authoring` and
  classify every remaining occurrence as precise human-activity prose,
  retained compatibility input, or a defect to rename.
- [ ] Audit `Authored` occurrences and change only those that do not describe
  authoritative persisted intent, such as temporary candidate or local count
  names.
- [ ] Remove old headers, implementation paths, feature identifiers,
  operation-group labels, CMake entries, test sources, and obsolete fixtures.
- [ ] Run all focused targets from the validation matrix after the final
  rename stage.
- [ ] Run the repository-required full build because public APIs and schema
  declarations cross Runtime, Developer, Editor, Program, and test modules.
- [ ] Run changed-document and all-plan validation and record final evidence.
- [ ] Move lasting vocabulary and ownership rules into their authoritative
  Runtime, Editor, and Workspace documents, then complete this plan.

#### Acceptance Gate

- Every remaining `Authoring`/`Authored` occurrence has an evidence-backed
  semantic classification; removed symbols and paths are absent; focused
  tests, the full build, and documentation validation pass; lasting contracts
  use the final vocabulary.

## Validation Matrix

Follow [Build and Run](../Agents/BuildAndRun.md) before building and
[Testing](../Agents/Testing.md) before selecting or running native tests. Use
the smallest affected targets during each stage and the full repository build
after Stage 5.

| Boundary | Focused validation | Required evidence |
| --- | --- | --- |
| Engine optional asset operations | Engine, StaticMeshBuild, TextureBuild, TerrainBuild, AssetForgeBuiltins module closure; StaticMesh, Texture, Terrain, cook, recovery, and modular-feature tests | build/post-load/source mutation behavior, ambiguity, cancellation, generation, publication, unload, and shutdown unchanged |
| Content write and project edit ownership | CoreTests path/project coverage; AssetMountedSourceTests; AssetCore package/catalog/import contract tests | mount admission, dependency checks, Engine write permission, lock exclusion, rollback, and diagnostics unchanged |
| Package serialization and save readiness | AssetCoreTests plus renamed save-readiness functional target and DurinAssetTool coverage | authored/cooked serialization, overrides, canonicalization, atomic publication, and pre-save rejection unchanged |
| Level mutations and placements | renamed StaticMesh level mutation target, Terrain editing/heightmap coverage, Scene Viewport/Outliner/editor smoke targets | planning is mutation-free; stale/read-only/thread rejection, transactions, rollback, selection, and placement unchanged |
| Material graph operations | renamed material graph operations target and affected material editor/compile tests | candidate validation, transactions, clipboard, geometry, diagnostics, and compile generations unchanged |
| Descriptor schema | DurinHeaderTool schema/configuration tests and source-library reference contract tests | canonical key accepted/emitted, conflict handling and any legacy-input policy proven |
| Cross-module completion | full repository `all` build and changed/all-plan documentation validation | no stale includes, exports, CMake paths, links, or removed public symbols |

Vulkan or editor smoke targets are required only when a stage changes a path
used exclusively by those configurations or when focused non-rendering tests
cannot exercise the affected registration/startup closure. Renaming alone must
not be used to expand rendering behavior.

## Definition of Done

- [ ] The vocabulary table is reflected in public code and authoritative
  documentation.
- [ ] `Authored` remains reserved for authoritative persisted intent.
- [ ] No active C++ API uses `Authoring` for content write policy, edit
  ownership, serialization, save readiness, build products, post-load,
  derived-data load, source mutation, level mutation/placement, or material
  graph operations.
- [ ] No removed header, class, function, feature identifier, CMake source,
  test target, fixture, or documentation link remains.
- [ ] No C++ compatibility aliases or forwarding headers preserve the old API.
- [ ] Project descriptor compatibility is explicit and covered by schema and
  loader tests.
- [ ] Test cleanup removes only obsolete scaffolding and preserves behavioral
  failure and lifecycle coverage.
- [ ] Each implementation stage lands independently with its plan checklist
  and validation evidence updated in the same commit.
- [ ] Focused validation, the final full build, and documentation validation
  pass.
- [ ] Lasting contracts are updated and the plan is marked completed.

## Deferred Follow-ups

- Renaming unrelated English “authoring” prose that remains accurate for user
  workflows.
- Revisiting `Authored` persisted-state terminology without a concrete conflict
  with derived, cooked, default, or runtime state.
- General modular-feature consolidation beyond the one StaticMesh seam whose
  current operation availability must be made truthful.
- Broader editor command API standardization across domains that do not use the
  overloaded `Authoring` vocabulary.
- Project descriptor format-version changes beyond the minimum compatibility
  treatment required for `ContentWritable`.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog And Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Workspace Projects](../Workspace/WorkspaceProjects.md)
- [Static Mesh Level Authoring](../Editor/Architecture/StaticMeshLevelAuthoring.md)
- [Terrain Editing Architecture](../Editor/Architecture/TerrainEditing.md)
- [Material Graph Authoring](../Editor/Architecture/MaterialGraphAuthoring.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Public/Misc/Project.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/AssetOperations.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageSerialization.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/MountedSource.h`
- `Engine/Source/Runtime/Engine/Public/AssetSaveReadiness.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshAuthoring.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DPostLoad.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubePostLoad.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexturePostLoad.h`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapPostLoad.h`
- `Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForgeBuiltinsAssetFeatures.h`
- `Engine/Source/Editor/AssetForgeBuiltins/Public/TerrainHeightmapAssetFeatures.h`
- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Public/TerrainLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Public/SkyBoxLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Public/GrayboxSceneAuthoring.h`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialGraphAuthoring.h`
- `Engine/Source/Programs/DurinHeaderTool/schemas/durin-project.schema.json`
