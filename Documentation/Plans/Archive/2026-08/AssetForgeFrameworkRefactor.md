# AssetForge Framework Refactor Plan

Summary: Reframe AssetForge as the asset-import framework, separate built-in implementations, and replace Interchange-derived terminology without changing import behavior.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Stages 0 and 1 are complete. The superseded provider registry, provider lease,
generic plan, legacy async-plan, and public multi-output orchestration have
been removed. Scene planning now returns a private typed immutable result, and
the surviving framework retains source capture, graph component leases,
operation scheduling, candidate exchange, records, and failure-atomic graph
publication. Post-removal validation passed the focused framework, built-in,
and Scene targets. Stage 2 is complete: the lightweight framework is
`AssetForge` in `Durin::AssetForge`, concrete implementations are
`AssetForgeBuiltins` in `Durin::AssetForge::Builtins`, and no compatibility
namespace aliases remain. Dependency closure remains editor-only. The renamed
`AssetForgeTests` (18), `AssetImportTests` (17), and `SceneImportTests` (5)
passed, and the `DurinAssetTool` consumer target builds with the new ownership.
Stage 3 is complete. Framework contracts are split by graph, extension,
operation, persistence, request, result, and service responsibility. Built-in
registration and implementation are split by texture, mesh, Terrain, and Scene
families. Stage 4 is complete: framework fingerprints, preview reuse, task
attribution, diagnostics, Scene state, and record-index domains use
`Durin.AssetForge.*`; builder components use `.Builder`; and framework contract
version 2 is strict. All 24 affected authored packages were canonically resaved
or regenerated, including both Scene records and their heterogeneous outputs.
The strict 30-package audit reports every package compatible and current with
zero canonicalization or deprecated-route evidence; canonical-resave CI is a
zero-work no-op. `AssetForgeTests` (18), `AssetImportTests` (17), and
`SceneImportTests` (5) pass after removing all migration aliases and retired
provider reconstruction.

Stage 5 is complete. The complete `asset-import` domain passed all five targets:
`AssetForgeTests` (18), `AssetImportTests` (17), `SceneImportTests` (5),
`SceneImportVulkanTests` (1), and `SkeletalSceneLifecycleTests` (1). The focused
asset-family and concurrency targets also passed: `TextureTests` (86),
`StaticMeshTests` (73), `TerrainHeightmapTests` (11), and
`CoreConcurrencyTests` (141). The complete `asset-cook` domain passed its four
targets, covering bulk containers, generic cook, Terrain cook, and Vulkan
texture cook integration. The strict authored baseline reports all 30 packages
as current DAST v5. `Win64-Debug-DurinEditor/all` and a newly configured
`Win64-Debug-DurinGame/all` both build; the latter compiled the runtime closure
without either editor import module. Lasting import, async-operation,
asset-lifecycle, mounted-source, CubeTexture, VolumeTexture, Terrain, skeletal,
runtime-startup, and module-ownership documentation now describes the final
AssetForge/AssetForgeBuiltins contracts. Qualification also found and repaired
one stale StaticMesh test include/namespace, one missing non-editor test-source
exclusion, and one null-unsafe Scene Vulkan cleanup assertion.

## Goal

- Make the physical `AssetForge` module the lightweight editor-only asset
  import framework rather than a concrete provider aggregate.
- Give framework contracts one Durin-owned vocabulary for immutable source
  graphs, planning passes, build graphs, asset builders, jobs, publication,
  records, and provenance without retaining Unreal Engine Interchange names.
- Move built-in image, texture, mesh, Terrain, Scene, glTF, FBX, and Assimp
  behavior behind an `AssetForgeBuiltins` module boundary.
- Retire the superseded provider/plan orchestration still present beside the
  graph-based framework instead of renaming and preserving two competing APIs.
- Preserve production behavior, worker/editor mutation boundaries,
  deterministic fingerprints, failure-atomic publication, cancellation,
  provider retirement, cooking, and runtime deployment independence.

## Scope

- `AssetImportCore` and `AssetForge` module manifests, source trees, export
  macros, namespaces, CMake targets, project descriptors, dependency
  declarations, tests, and consuming editor modules.
- Framework source capture, schema payload, source graph, build graph,
  extension registration, job execution, progress, import record, provenance,
  reconciliation, and publication contracts.
- Built-in Texture2D, TextureCube, VolumeTexture, StaticMesh, Terrain
  Heightmap, Material, Skeleton, SkeletalMesh, AnimationClip, and heterogeneous
  Scene import implementations.
- Removal or internal replacement of the old `IImportProvider`, generic
  provider-plan, legacy async-plan, and public multi-output orchestration seams
  that no longer own a production import workflow.
- A deliberate versioned transition for persisted framework identities,
  fingerprint domain separators, diagnostic identities, import-record state,
  and repository-authored assets affected by vocabulary changes.
- Lasting architecture, module-ownership, async-operation, asset-lifecycle,
  and user-workflow documentation updates.

## Non-Goals

- Changing supported source formats, import UI, output placement, asset build
  settings, normalized geometry/image behavior, or package publication
  semantics.
- Introducing a universal reflected scene graph, scripting interchange
  language, remote import protocol, persistent job queue, or batch scheduler.
- Deploying `AssetForge`, built-in source decoders, import records, provenance,
  or source authoring dependencies in cooked runtime targets.
- Making built-in source formats independently unloadable unless their actual
  third-party deployment or release lifecycle later requires another module.
- Publishing every built-in normalized representation as a permanent
  framework schema merely because it exists during this refactor.
- Retaining source-level aliases for temporary `Interchange` or
  `AssetImportCore` names after repository callers have migrated.
- Supporting arbitrary historical asset or import-record schemas outside the
  repository-baseline compatibility policy.

## Design Decisions and Invariants

### Module ownership

The selected dependency direction is:

```text
Core / CoreDObject
        |
AssetCore
        |
AssetForge
  framework contracts, operations, records, and publication
        |
AssetForgeBuiltins
  built-in translators, planning passes, builders, and authoring facades
        |
Editor hosts
```

- The current `AssetImportCore` implementation becomes the physical
  `AssetForge` module after obsolete orchestration is retired.
- The current concrete `AssetForge` implementation becomes
  `AssetForgeBuiltins`. It owns the `Engine`, `TextureBuild`, `GeometryBuild`,
  Assimp, glTF, FBX, image-family, and concrete authoring dependencies.
- `AssetForge` may publicly depend on `AssetCore` and generic Core facilities.
  It must not depend on `Engine`, concrete runtime asset classes,
  `TextureBuild`, `GeometryBuild`, or Assimp.
- `AssetForgeBuiltins` is one aggregate initially. A source-format adapter
  becomes a separate module only when optional deployment, unload lifecycle,
  ownership, or release cadence creates a real boundary.
- Runtime-only targets continue to load cooked assets without either editor
  module or any import decoder.

### Namespace and vocabulary

- Public framework contracts live in `Durin::AssetForge`. Built-in extension
  implementations and typed facades live in
  `Durin::AssetForge::Builtins`. Implementation-only helpers use the nearest
  `Private` namespace beneath their owner.
- Module or framework names are not repeated inside type names when the
  namespace supplies the context. For example, use
  `AssetForge::FImportRequest`, not `FAssetForgeImportRequest`.
- The selected role vocabulary is source translator, planning pass, and asset
  builder:

| Current | Selected |
| --- | --- |
| `FInterchangePayload` | `FSchemaPayload` |
| `CInterchangePayloadValue` | `CSchemaPayloadValue` |
| `FTranslatedAssetNode` / `FTranslatedAssetGraph` | `FSourceNode` / `FSourceGraph` |
| `FImportFactoryNode` / `FImportFactoryGraph` | `FBuildNode` / `FBuildGraph` |
| `IInterchangeTranslator` | `ISourceTranslator` |
| `IInterchangePipeline` | `IPlanningPass` |
| `IInterchangeFactory` | `IAssetBuilder` |
| `IInterchangeFactoryProduct` | `IBuildProduct` |
| `IInterchangeFactoryReconciliationContext` | `IReconciliationContext` |
| `FInterchangeMaterializationContext` | `FMaterializationContext` |
| `FInterchangeImportRequest` / `Result` / `Handle` | `FImportRequest` / `FImportResult` / `FImportHandle` |
| `FInterchangeProvenance` | `FImportProvenance` |
| `EInterchangeImportMode` | `EImportMode` |
| `FInterchangeComponentLease` | `FComponentLease` |
| `FInterchangeRegistration` | `FComponentRegistration` |

- `Translator` remains because it accurately describes external source
  normalization. `PlanningPass` replaces the overly broad pipeline term, and
  `AssetBuilder` replaces factory because the role builds detached products,
  reconciles prospective state, and materializes typed candidates.
- Public typed APIs use import vocabulary, such as
  `MakeTexture2DImportRequest`, `InspectTexture2DImportProvenance`, and
  `SubmitTexture2DImport`. Filenames describe the complete facade, such as
  `Texture2DImport.h`, rather than the narrower former
  `Texture2DSourceTranslation.h` name.

### Framework nodes and schemas

- `AssetForge` owns the generic `FSourceNode`, `FBuildNode`, their immutable
  graphs, builders, bounds, dependency validation, canonical ordering,
  fingerprints, schema payloads, source attribution, output policies, and
  stable identities.
- A reusable normalized value schema may move into `AssetForge` only when it
  is value-only, bounded, versioned, independent of `DObject` and concrete
  build modules, and reused across at least two extension implementations.
- Candidate shared schemas begin private. Image, mesh, sampler, material, or
  scene values do not become public framework contracts until their actual
  reuse and version owner are demonstrated.
- Concrete asset builders, `DObject` materialization, typed state exchange,
  DDC recipe construction, and recovery policy remain in
  `AssetForgeBuiltins` even when they consume a shared source-node schema.
- The framework continues to use versioned owned payloads. This refactor does
  not introduce an untyped persistent `shared_ptr<void>` contract.

### Files and public surface

The target framework layout groups contracts by responsibility:

```text
AssetForge/Public/AssetForge/
  ImportRequest.h
  ImportResult.h
  ImportService.h
  Graph/SchemaPayload.h
  Graph/SourceGraph.h
  Graph/BuildGraph.h
  Extensions/SourceTranslator.h
  Extensions/PlanningPass.h
  Extensions/AssetBuilder.h
  Extensions/ComponentRegistration.h
  Operations/ImportOperation.h
  Operations/ImportJob.h
  Persistence/ImportProvenance.h
  Persistence/ImportRecord.h
  Persistence/ImportRecordIndex.h
```

The target built-in layout groups implementation by asset/source family:

```text
AssetForgeBuiltins/Public/AssetForge/Builtins/
  Texture2DImport.h
  TextureCubeImport.h
  VolumeTextureImport.h
  StaticMeshImport.h
  TerrainHeightmapImport.h
  SceneImport.h

AssetForgeBuiltins/Private/
  Image/
  Texture2D/
  TextureCube/
  VolumeTexture/
  StaticMesh/
  Terrain/
  Scene/
  Registration/
```

- Public headers include the smallest owning contract and do not depend on one
  umbrella header for unrelated graph, job, persistence, and publication
  types.
- The current monolithic `Interchange.h` and `AssetForgeProviders.cpp` are
  decomposed along these ownership seams. File splitting must not create one
  physical module per C++ role or asset class.
- Built-in registration is owned by `AssetForgeBuiltins` module startup and
  tears down in strict reverse order. Loading framework core alone never
  admits Assimp or concrete asset builders.

### Execution and publication

- One service-owned `FImportJob` remains the production state machine for
  import, preview, reimport, source replacement, repair, recovery, and import
  record actions.
- Source capture, hashing, dependency discovery, translation, worker-safe
  planning, and detached product construction remain worker-safe value work.
- `DObject`, package, catalog, registry, editor-model, render-resource, RHI,
  state-exchange, and publication mutation remain editor-thread work.
- Component leases outlive every invocation and escaped component-owned value.
  Registration replacement and retirement remain exact-generation operations.
- Cancellation stays cooperative before finalization. Publication remains
  non-cancelable, dependency ordered, root-last, failure atomic, and reversible
  in the opposite order.
- The synchronous executor remains the semantic reference for scheduled
  execution. This refactor cannot introduce an asset-family-specific job.

### Compatibility transition

- C++ names, files, namespaces, and modules migrate first without changing
  serialized bytes, provider IDs, schema IDs, diagnostic identities, or hash
  domain separators.
- Persisted identities are inventoried and classified before any string
  change. Stable domain identities such as `Durin.Image` and
  `Durin.SceneGraph` remain unchanged unless their semantics change.
- Framework-owned `Durin.Interchange.*`, `.Factory`,
  `Durin.Scene.InterchangeState`, diagnostic identities, and fingerprint
  separators may transition to `Durin.AssetForge.*` and `.Builder` only in one
  explicit schema revision.
- That revision bumps the framework/provenance contract, updates every writer
  and strict reader together, regenerates or rewrites all affected repository
  assets and import records, and proves the construct-free compatibility audit.
  It is not performed with an unreviewed textual replacement.
- No source compatibility namespace or type aliases survive completion. The
  repository-baseline authored corpus is upgraded in the same change as the
  strict reader and writer.

## Current Foundations and Gaps

- `AssetImportCore` already has the desired lightweight physical dependency on
  `AssetCore`, and owns source snapshots, diagnostics, operation state, import
  records, registries, graph contracts, and the service-owned import job.
- The current `AssetForge` module already aggregates the intended built-in
  functionality, but its name incorrectly suggests framework ownership while
  its public dependencies include `Engine` and `TextureBuild` and its private
  dependencies include `GeometryBuild`.
- Production import callers use the graph-based path, while the older
  `IImportProvider`, `FImportPlanRequest`, `FImportPlanBuilder`, legacy async
  plan, and public multi-output orchestration remain in headers and focused
  tests. Scene preparation also reuses `FImportPlanBuilder` as an internal
  value container.
- Framework contracts are concentrated in `Interchange.h`; registry and job
  implementations retain the same vocabulary. Built-in component types and
  serialization helpers are concentrated in the very large
  `AssetForgeProviders.cpp`, with a second image-family concentration in
  `ImageFamilyInterchange.cpp`.
- Public built-in headers expose a mixture of raw source translation, request
  construction, submission, reimport, recovery, and direct authoring under
  `SourceTranslation` filenames.
- Interchange wording is present in public C++, private symbols, filenames,
  tests, diagnostics, task attribution, documentation, persisted identities,
  and fingerprint domain separators. These categories require different
  migration treatment.

### Stage 0 Baseline Inventory

- Framework public surface: the ten headers under
  `Engine/Source/Editor/AssetImportCore/Public` own source capture, the old
  provider/plan API, async operations, import jobs, graph contracts, records,
  the service, and multi-output publication. Built-in public surface: the
  twelve headers under `Engine/Source/Editor/AssetForge/Public` own module
  startup, imported Scene values, and the six typed asset-family facades.
- Physical targets are `AssetImportCore` and `AssetForge`. The former publicly
  depends only on `AssetCore`; the latter publicly depends on
  `AssetImportCore`, `AssetCore`, `Core`, `CoreDObject`, `Engine`, and
  `TextureBuild`, privately depends on `GeometryBuild`, and directly links and
  deploys Assimp. `Engine.dproject` includes both only in the editor runtime
  variant, while root closure assertions exclude both from `Engine` and
  `DurinLauncher`.
- Production consumers are `DurinEd`, `MainFrame`, `LevelEditor`,
  `TextureEditor`, `SkeletalMeshEditor`, built-in post-load/recovery policy,
  and the registered native-test targets. They use the graph request/service
  path. No production caller constructs an old public `IImportProvider` or
  `FMultiOutputImportPlan`; the remaining declarative-provider adapter is
  internal compatibility code.
- `IImportProvider`, `FImportPlanRequest`, `FImportPlan`, the plan coordinator,
  legacy async-plan entry points, and public multi-output orchestration are
  removable. `FImportPlanBuilder` survives only as a private immutable Scene
  planning result replacement. Generic operation handles, progress/history,
  task scopes, candidate exchange, dependency-ordered publication, and reverse
  rollback remain framework mechanics used by `FImportJob`.
- C++/file/test/diagnostic `Interchange` occurrences all migrate in Stage 3.
  Persisted strings are isolated for Stage 4: graph domains
  `Durin.Interchange.TranslatedGraph`, `Durin.Interchange.FactoryGraph`, and
  `Durin.Interchange.PreviewReuse`; framework diagnostics beginning with
  `Interchange`; Scene state `Durin.Scene.InterchangeState`; and component IDs
  ending in `.Factory`. Stable domain/provider identities including
  `Durin.Image`, `Durin.SceneGraph`, decoder/importer identities, source schema
  IDs, and authored output paths retain their bytes unless Stage 4 proves a
  semantic role change.
- The strict baseline is `InterchangeContractVersion == 1`,
  `ImportRecordVersion == 2`, exact current-version record loading, and exact
  component contract selection. The supported authored corpus is the 30
  repository `.dasset` files under `Engine/Content` and `Sandbox/Content`,
  including two Scene import roots, their heterogeneous managed outputs,
  Terrain, Texture2D, TextureCube, and VolumeTexture assets. DDC and graph keys
  remain derivable rather than separately checked in.
- Frozen focused coverage is `AssetImportCoreTests` for graph ordering,
  validation, selection, retirement, cancellation, provenance, records,
  reconciliation, failure rollback, and construct-free serialization;
  `AssetImportTests` for built-in request/output behavior; `TextureTests`,
  `TerrainHeightmapTests`, `SceneImportTests`, and
  `SkeletalSceneLifecycleTests` for asset-family import/reimport/recovery; and
  `AssetCookTests`, `TerrainHeightmapCookTests`, and
  `TextureCookIntegrationTests` for cook/runtime stripping. Stage 0 directly
  ran and passed the first two targets plus `SceneImportTests`.
- The build-safe sequence is: remove old orchestration while both current
  modules exist; rename current `AssetForge` to `AssetForgeBuiltins`; move
  current `AssetImportCore` to `AssetForge`; update descriptors/dependencies;
  then rename namespaces/files/types and finally version persisted identities.
  The target public layout and complete name map in this plan are authoritative;
  no temporary compatibility aliases survive a stage commit.

## Implementation Stages

### Stage 0: Freeze the refactor and compatibility baselines

Dependencies: none.

- [x] Inventory every public `AssetImportCore` and `AssetForge` header, export,
  consuming module, CMake target, project descriptor entry, and runtime
  deployment exclusion.
- [x] Classify the old provider/plan and async-plan APIs as removable,
  internally reusable publication mechanics, or still-required public
  contracts; record every surviving production caller.
- [x] Inventory all `Interchange` occurrences by C++ identifier, filename,
  diagnostic identity, task attribution, test name, schema/component ID,
  serialized state field, and fingerprint domain separator.
- [x] Capture the supported repository asset/import-record corpus and its
  construct-free compatibility audit, provenance versions, graph
  fingerprints, DDC keys, and representative import/reimport outputs.
- [x] Freeze focused tests for translator selection, graph validation,
  component retirement, preview reuse, cancellation, recovery, record
  reconciliation, failure rollback, cook stripping, and runtime deployment.
- [x] Confirm the exact name map, target file layout, and temporary build-safe
  module rename sequence before moving source files.

#### Acceptance Gate

- Every removed or renamed API and every persisted identity has one recorded
  owner and migration disposition.
- The baseline proves current output, compatibility, failure, concurrency,
  dependency, and runtime-deployment behavior before implementation begins.

### Stage 1: Retire superseded provider and plan orchestration

Dependencies: Stage 0 inventories and focused baseline.

- [x] Replace Scene's internal `FImportPlanBuilder` reuse with a private,
  immutable Scene planning result owned by the graph-based implementation.
- [x] Remove old provider registration, provider leases, generic plan request,
  builder, result, and legacy async-plan entrypoints that have no production
  owner.
- [x] Move still-required generic source capture, progress, job scheduling,
  import-record, reconciliation, and publication mechanics behind the selected
  framework contracts instead of keeping legacy names public.
- [x] Remove or rewrite tests that exercise only the retired framework; retain
  their valuable bounds, lease, cancellation, ambiguity, and rollback cases
  against the graph-based service.
- [x] Prove that every production import family still enters the single
  service-owned graph job.

#### Acceptance Gate

- Production and test code contains no `IImportProvider` or legacy generic
  provider-plan submission path.
- Import records, source snapshots, publication, failure injection, and async
  operation behavior retain focused coverage through the surviving framework.

### Stage 2: Establish AssetForge and AssetForgeBuiltins module ownership

Dependencies: Stage 1 leaves one framework architecture.

- [x] Move the existing concrete `AssetForge` target and tree to
  `AssetForgeBuiltins`, including its export macro, module startup, tests,
  CMake metadata, project registration, and consuming dependencies.
- [x] Rename the cleaned `AssetImportCore` target and tree to `AssetForge`,
  including its export macro, PCH, reflected headers, task attribution, tests,
  and program/editor dependencies.
- [x] Move framework C++ into `Durin::AssetForge` and built-in APIs into
  `Durin::AssetForge::Builtins` without temporary namespace aliases.
- [x] Make the built-in module own registration and reverse-order retirement
  of all translators, planning passes, builders, and authoring/recovery
  features.
- [x] Audit public/private dependency placement so `AssetForge` has no
  concrete Engine, Build, decoder, or third-party dependency and runtime
  targets deploy neither module.

#### Acceptance Gate

- Generated target metadata, dependency-closure checks, editor/program links,
  and runtime deployment inspection identify the new module ownership only.
- Loading `AssetForge` alone exposes the framework but registers no built-in
  component and loads no Assimp or concrete Build dependency.

### Stage 3: Replace Interchange vocabulary and decompose the implementation

Dependencies: Stage 2 physical ownership is stable. Persisted IDs remain on
their Stage 0 values throughout this stage.

- [x] Apply the selected translator/planning-pass/asset-builder and
  source-graph/build-graph type map across framework, built-ins, editor hosts,
  tests, task attribution, operation titles, diagnostics, and non-persistent
  strings.
- [x] Split schema payload, source graph, build graph, extension contracts,
  operations, provenance, record, and service APIs into the selected public
  directories with minimal includes.
- [x] Split built-in registration and implementation by source/asset family;
  remove the monolithic provider and image-family interchange files once no
  target references them.
- [x] Rename typed public facades and source files from `*SourceTranslation`
  and `*Interchange*` to complete `*Import` responsibilities, and make helpers
  private unless an editor host requires them.
- [x] Keep generic graph nodes in `AssetForge`; keep normalized image, mesh,
  material, sampler, and scene schemas private to built-ins until Stage 0 reuse
  evidence qualifies a value-only shared schema.
- [x] Update focused test target and case names to AssetForge vocabulary while
  preserving assertions and failure evidence.

#### Acceptance Gate

- Active C++ identifiers, namespaces, filenames, task labels, tests, and
  user-facing diagnostics contain no `Interchange` vocabulary.
- Any remaining occurrence is a Stage 4 persisted identity explicitly listed
  in the compatibility inventory.
- Public headers follow the target ownership layout and framework headers do
  not transitively expose built-in asset or Build headers.

### Stage 4: Version persisted AssetForge identities and migrate authored data

Dependencies: Stage 3 establishes final C++ ownership and vocabulary.

- [x] Select final `Durin.AssetForge.*` fingerprint, preview-cache, operation,
  diagnostic, and serialized-state identities; retain domain/provider IDs whose
  semantics did not change.
- [x] Rename factory-role component IDs to builder-role IDs where their
  persisted meaning changed, and update request reconstruction and component
  selection together.
- [x] Bump framework/provenance and affected provider-state schema versions;
  update strict serialization, validation, hashing, and compatibility
  diagnostics without a fallback reader for unsupported temporary contracts.
- [x] Regenerate or rewrite every affected repository-authored asset and import
  record, then validate graph fingerprints, output mappings, hard references,
  source hashes, DDC reconstruction, and record indexing.
- [x] Remove the inventoried legacy persisted strings after proving that no
  supported authored package or record requires them.

#### Acceptance Gate

- The complete repository corpus passes construct-free compatibility audit,
  load, reimport or recreate, save, reload, DDC rebuild/hit, cook, and runtime
  inspection with the new contract versions.
- Strict readers reject retired framework identities before package residency,
  and production contains no silent substitution or compatibility alias.

### Stage 5: Qualify behavior and publish lasting contracts

Dependencies: Stages 1 through 4 complete.

- [x] Run the focused framework, record, texture, static-mesh, Terrain, Scene,
  skeletal, concurrency, shutdown, failure-injection, DDC, cook, and runtime
  deployment suites selected through the repository test workflow.
- [x] Build the complete affected editor, game, tool, and native-test target
  closure according to the repository build workflow.
- [x] Exercise representative Texture2D, TextureCube, VolumeTexture,
  StaticMesh, Terrain Heightmap, and heterogeneous Scene import, preview,
  reimport, repair/recovery, save, reload, and cancellation workflows.
- [x] Update lasting architecture, async-operation, asset-lifecycle, module,
  rendering/asset-family, and mounted-source documents to the final module and
  vocabulary contracts; remove superseded plan-language references from active
  documentation.
- [x] Record final evidence and exact module/API ownership in this plan, mark
  it completed only after every gate passes, and leave physical archival to
  the normal monthly workflow.

#### Acceptance Gate

- No import behavior, output identity, authored state, threading boundary,
  publication guarantee, or runtime dependency differs from the accepted
  baseline except the explicit versioned identity transition.
- All affected builds, tests, documentation validation, authored corpus audit,
  cook, and runtime deployment inspection pass with only `AssetForge` and
  `AssetForgeBuiltins` ownership.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Module ownership | Generated targets and dependency closure show lightweight `AssetForge`, concrete `AssetForgeBuiltins`, and no runtime deployment |
| Public API | Header compile coverage for request/result, graphs, extensions, operations, provenance, records, and each typed built-in facade |
| Graph contracts | Canonical ordering, bounds, duplicates, missing references, cycles, schema mismatch, deterministic fingerprints, and cancellation |
| Registration | Deterministic selection, ambiguity, exact-generation replacement, escaped-value leases, retirement, reverse teardown, and shutdown drain |
| Execution | Inline/scheduled equivalence, preview reuse, supersession, progress, cancellation before finalization, and editor-thread mutation checks |
| Publication | Single and multi-output creation/replacement, collisions, reconciliation, dependency order, root-last save, failure injection, and reverse rollback |
| Asset families | Texture2D, TextureCube, VolumeTexture, StaticMesh, Terrain Heightmap, Material, Skeleton, SkeletalMesh, AnimationClip, and Scene workflows |
| Persistence | Provenance and record serialization, strict version rejection, repository corpus migration, redirects/indexing, save/reload, and canonical audit |
| Derived data | DDC key stability within one contract, intentional invalidation across the schema transition, rebuild/hit, recovery, and disposable cache behavior |
| Runtime | Cook stripping, cooked asset load, hard-reference preservation, and absence of AssetForge, built-ins, source files, Assimp, and editor DDC fallback |
| Documentation | Changed-scope validation, all-plan validation for lifecycle changes, and final architecture/module documentation consistency |

## Definition of Done

- `AssetForge` is the only framework module and owns no concrete asset, Build,
  decoder, or third-party dependency.
- `AssetForgeBuiltins` owns every Durin-provided translator, planning pass,
  asset builder, typed import facade, and recovery/authoring adapter.
- Public and private C++ use `Durin::AssetForge`, source-graph/build-graph, and
  translator/planning-pass/asset-builder vocabulary with no surviving
  Interchange or AssetImportCore compatibility surface.
- The obsolete provider/plan framework is removed; one service-owned graph job
  executes every production import workflow.
- Generic graph nodes are framework-owned, while concrete/shared normalized
  schemas satisfy the documented value-only and reuse boundary.
- Persisted framework identities are either deliberately retained as semantic
  domain IDs or migrated through an explicit version change with the complete
  repository-authored corpus.
- Focused and aggregate validation proves deterministic behavior,
  failure-atomic publication, concurrency and retirement safety, cooking, and
  runtime deployment independence.
- Lasting documentation describes the implemented AssetForge architecture and
  this plan records complete evidence with `Status: Completed`.

## Deferred Follow-ups

- Split Assimp, glTF, image codecs, or an asset family into an optional module
  only after deployment, unload, ownership, or release-cadence evidence
  requires it.
- Promote a built-in normalized image, mesh, material, sampler, or scene schema
  into public `AssetForge` only after multiple independent extensions require
  the same versioned contract.
- Add scripting, remote source interchange, a reflected property graph, batch
  scheduling, or persistent queues only under a separate requirement and plan.
- Reconsider plugin SDK stability after at least one external provider module
  has exercised registration, schema migration, retirement, and authored-data
  compatibility outside `AssetForgeBuiltins`.

## Related Documentation

- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Async Asset Operations](../../../Editor/Architecture/AsyncAssetOperations.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Mounted Source Workflows](../../../Editor/Guides/SourceFileWorkflows.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Asset Interchange Framework Plan](AssetInterchangeFramework.md)
- [Asset Import Framework Plan](AssetImportFramework.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- [`Engine/Source/Editor/AssetForge`](../../../../Engine/Source/Editor/AssetForge)
- [`Engine/Source/Editor/AssetForgeBuiltins`](../../../../Engine/Source/Editor/AssetForgeBuiltins)
- [`Engine/Source/Editor/LevelEditor`](../../../../Engine/Source/Editor/LevelEditor)
- [`Engine/Source/Editor/TextureEditor`](../../../../Engine/Source/Editor/TextureEditor)
- [`Engine/Tests/Native/AssetCoreTests`](../../../../Engine/Tests/Native/AssetCoreTests)
- [`Engine/Tests/Native/EngineTests`](../../../../Engine/Tests/Native/EngineTests)
- [`Engine/Engine.dproject`](../../../../Engine/Engine.dproject)
- [`CMakeLists.txt`](../../../../CMakeLists.txt)
