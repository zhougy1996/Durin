# Asset Interchange Framework Plan

Summary: Replace format-specific import workflows with a translator, pipeline, factory, and graph-based asset interchange framework.

Last reviewed: 2026-08-24

Status: Completed
Completed: 2026-08-24

## Current Status

Completed on 2026-08-24. One `FInterchangeImportJob` now owns production
import, preview, reimport, repair, record actions, and implemented editor
recovery for StaticMesh, Texture2D, TextureCube, VolumeTexture, Terrain
Heightmap, and heterogeneous Scene output. Specialized component registries,
immutable translated/factory graphs, preview-product reuse, canonical
provenance, deterministic output reconciliation, provider retirement, and
failure-atomic publication are implemented. Production callers and focused
tests no longer invoke the removed single-asset handler, record-handler, or
public Scene plan/execution workflows.

## Goal

Provide one asset interchange framework in which format-specific translators
produce an immutable source graph, ordered pipelines turn that graph into an
explicit factory graph, typed factories build detached asset products, and one
service-owned job validates and publishes the resulting asset graph.

Initial import, preview, reimport, source repair, multi-output Scene import,
record actions, and editor recovery must use the same translator, pipeline,
factory, provenance, operation, and publication model.

## Scope

- Format and content recognition, source capture, dependency discovery, and
  translation into a format-neutral graph.
- Ordered, configurable pipeline stacks that select outputs, settings,
  identities, paths, dependencies, and reconciliation policy without creating
  assets.
- Factory-node planning and typed factories for StaticMesh, Texture2D,
  TextureCube, VolumeTexture, Terrain Heightmap, Material, Skeleton,
  SkeletalMesh, and AnimationClip outputs.
- One framework-owned import job for synchronous and scheduled import,
  preview, reimport, repair, multi-output reconciliation, and recovery.
- Stable translator, pipeline, node, output, and settings identities with
  versioned provenance for future reimport.
- Worker-safe translation, planning, and detached product construction;
  editor-thread object materialization, validation, publication, save, and
  rollback.
- Module-owned registration, provider retirement, cancellation, drain, and
  unload-safe destruction of translators, pipelines, factories, and their
  escaped values.
- Production adoption in LevelEditor dialogs, Content Browser actions, Scene
  workflows, and required editor recovery paths.

## Non-Goals

- Runtime-target deployment of editor translators, pipelines, factories, or
  third-party source decoders.
- A general workflow language, repository-wide job system, distributed import
  service, persistent queue, or batch scheduler.
- Moving `DObject`, package, catalog, Asset Registry, render-resource, RHI, or
  package-save mutation onto worker threads.
- Replacing AssetCore package transactions, source storage, DDC, asset build
  recipes, or runtime asset formats.
- A reflection-driven string property graph solely to imitate another engine.
  Interchange payloads remain versioned owned values until scripting or remote
  interchange has a concrete requirement.
- Backward compatibility for temporary C++ orchestration APIs after every
  production caller and focused test has migrated.

## Design Decisions and Invariants

### One framework, three extension roles

The extension boundary has exactly three roles:

```text
Source snapshot
  -> Translator
  -> Immutable translated graph
  -> Ordered pipeline stack
  -> Immutable factory graph
  -> Typed factories
  -> Detached products
  -> Candidate validation and atomic publication
```

A translator understands an external representation. It recognizes sources,
discovers bounded dependencies, decodes captured bytes, and emits source
semantic nodes. It does not select Content destinations or create assets.

A pipeline expresses authoring policy. It consumes the translated graph and
emits factory nodes describing outputs, settings, stable identities,
dependencies, destinations, and reconciliation. It does not decode source
files or mutate editor state.

A factory understands one output contract. It consumes one factory node and
the translated values it references, builds a detached product, and
materializes a candidate through the existing editor mutation boundary. It
does not select source formats or own complete workflow orchestration.

### Graph contracts are stable owned values

`FTranslatedAssetGraph` contains canonically ordered nodes with a stable node
identity, node kind, versioned payload schema, source attribution, and explicit
dependencies. `FImportFactoryGraph` contains canonically ordered factory nodes
with stable output identity, output class, destination, policy, settings,
translated-node references, and factory dependencies.

Both graphs are immutable after finalization, deterministically validated and
fingerprinted, bounded before allocation, free of `DObject` and UI pointers,
and safe to destroy while the contributing module remains leased. Unknown
schema versions fail with structured compatibility diagnostics. Cycles,
missing nodes, duplicate identities, invalid destinations, and incompatible
factory dependencies fail before candidate construction.

Graph storage uses common headers plus versioned payloads. Core does not expose
an untyped `shared_ptr<void>` as a persistent or cross-stage interchange
contract. Typed access validates schema identity and version.

### Selection uses specialized registries

`FImportService` owns registries for translator, pipeline, and factory
descriptors. Registration carries stable ID, contract version, declared
capabilities, module callback gate, exact registration identity, and explicit
retirement. Selection may depend on source extension, recognition bytes,
requested asset class, pipeline stack, and persisted provenance; it is not
modeled as generic modular-feature cardinality.

No registry silently chooses the first compatible implementation. Equivalent
matches either use an explicit configured priority with deterministic evidence
or report ambiguity. A submitted job retains leases for every implementation
whose code or values may be invoked or destroyed.

### Pipeline execution is deterministic and persisted

A pipeline stack is an ordered list of stable pipeline IDs, contract versions,
and versioned settings payloads. Each pipeline receives the previous immutable
graph view and writes through a validating builder. Execution order is part of
the import fingerprint and provenance.

Reimport restores the recorded translator, translator settings, pipeline
stack, pipeline settings, source-node identities, and output mappings. Missing
or incompatible components report repairable compatibility diagnostics rather
than silently substituting a different pipeline.

### One framework-owned job executes every workflow

AssetImportCore owns `FInterchangeImportJob`. Provider modules do not implement
a complete job state machine per asset family. The job performs the common
sequence and delegates only translation, pipeline, and factory work.

Worker rounds may capture and hash sources, discover dependencies, translate,
execute worker-safe pipelines, and build detached products. Editor rounds may
capture settings and target revisions, resolve identities, materialize
candidates, revalidate preconditions, publish, save, reverse, or compensate.

The same job implementation runs through scheduled and inline runners.
Preview stops after a declared immutable graph or detached-product boundary;
import may reuse preview work only when all source, settings, stack, graph,
destination, and target fingerprints match.

### Publication remains failure-atomic

Factories cannot publish directly. They return candidates and prepared state
exchanges to the framework. The framework validates the complete output graph,
rechecks target and registry revisions, enters a non-cancelable finalization
boundary, commits in dependency order, saves the package bundle root-last, and
reverses in the opposite order on failure.

Cancellation is cooperative before finalization. Once publication begins, the
job must reach success or a fully restored failure. Disposable source
ingestion or DDC entries may survive failure only where their owning contracts
already permit it.

### Provenance belongs to the framework

Single-output assets and multi-output import records persist the framework
identity needed to reproduce their authored state:

- translator ID, contract version, and settings;
- ordered pipeline IDs, versions, and settings;
- captured source identities and hashes;
- translated-node and output identities;
- graph, plan, and authored-output fingerprints; and
- reconciliation policy and managed output mapping where applicable.

Asset-family payloads may extend provenance through versioned schemas, but
they cannot replace the framework identity or introduce a second reimport
route.

### UI observes operations but never advances them

Dialogs and panels submit a request once and retain only form state plus an
`FImportOperationHandle`. The central editor tick advances ready job steps.
Status UI, Activity History, dialogs, and Content Browser observe immutable
snapshots and terminal outcomes; drawing or destroying a Widget cannot advance,
abandon, or synchronously drain accepted work.

## Current Foundations and Gaps

| Area | Foundation | Required framework work |
| --- | --- | --- |
| Source ownership | Immutable captured sources, dependency limits, hashing, and mounted identities exist. | Move format decoding behind translators that emit one graph contract. |
| Execution | Service-owned operations, worker/editor rounds, cancellation, progress, inline parity, and provider drains exist. | Implement one interchange job and remove asset-family workflow state machines. |
| Product build | Asset families can build detached values and materialize candidates. | Bind product builders to typed factory nodes and factory descriptors. |
| Publication | Validation, state exchange, bundle save, rollback, and stale-plan rejection exist. | Drive the same transaction from a validated factory dependency graph. |
| Reimport | Provider identity, source provenance, settings, records, and reconciliation exist. | Persist translator and pipeline stacks plus stable node-to-output mappings. |
| Extension | Module owner gates and exact registration handles exist. | Apply one descriptor and lease model consistently to all three extension roles. |
| Production UI | Progress presentation and background operation observation exist. | Replace format-specific submission and polling with one request/outcome API. |

## Stage 0 Frozen Baseline

### Production entrypoint inventory

Every current authoring entrypoint maps to the future roles below. A migration
may temporarily adapt the listed call, but it cannot retain the current
workflow as a second execution route after its stage acceptance gate.

| Current entrypoint | Translator | Pipeline | Factory outputs | Job/publication owner |
| --- | --- | --- | --- | --- |
| `TextureImportDialog.cpp` Texture2D import; `MTextureEditor` and `TextureSourceReplacementOperation` reimport, replacement, and source repair | `Durin.Image` | default Texture2D | Texture2D | Interchange job; AssetCore single-package transaction |
| `TextureImportDialog.cpp` VolumeTexture atlas import and `VolumeTextureSourceTranslation` reimport/repair | `Durin.Image` | volume-atlas slicing | VolumeTexture | Interchange job; AssetCore single-package transaction |
| `TextureImportDialogCube.cpp` face/panorama import, preview, and `TextureCubeSourceTranslation` reimport | `Durin.Image` | cube faces or panorama | TextureCube | supersedable preview or Interchange job; AssetCore single-package transaction |
| `TerrainHeightmapImportDialog.cpp` and `TerrainHeightmapSourceTranslation` import/reimport | `Durin.Image` | terrain heightmap normalization | Terrain Heightmap | Interchange job; AssetCore single-package transaction |
| `StaticMeshImportDialog.cpp` and Content Browser StaticMesh reimport/repair | Assimp geometry, glTF, GLB, or FBX | default static-asset | StaticMesh and required referenced dependencies | Interchange job; AssetCore bundle transaction |
| `SceneImportDialog.cpp` source preparation, preview, import, and execution polling | glTF, GLB, or FBX | default Scene | StaticMesh, Texture2D, Material, Skeleton, SkeletalMesh, AnimationClip, import record | one multi-output Interchange job; AssetCore root-last bundle transaction |
| `ContentBrowserPanel.cpp` record reimport, recreate-missing, and repair actions | provenance-selected Scene translator | persisted Scene stack | persisted managed output graph | one reconciliation-mode Interchange job; AssetCore bundle transaction |
| Texture2D, TextureCube, StaticMesh, Terrain and skeletal missing-derived-data `PostLoad` features | provenance-selected translator | persisted recovery stack | affected typed factory output | `SessionCritical` Interchange recovery job; ordinary load remains observer |

Dialogs own only form state and an operation handle after migration. Direct
AssetForge functions, begin/poll pairs, post-load callbacks, and record actions
listed above are migration sources, not additional framework roles.

### Frozen graph and execution decisions

- Stable component, node, output, schema, source, and diagnostic identities are
  printable case-sensitive ASCII values no longer than 1,024 bytes. Contract
  versions are non-zero `uint32` values. Exact persisted identities win over
  recognition; no compatible implementation is silently substituted.
- Both graphs sort nodes by stable identity. Source identities, translated
  references, and dependency lists sort lexicographically. Duplicates, missing
  references, invalid destinations, unknown schemas, incompatible versions,
  cycles, and configured resource-limit excess fail graph finalization.
- Graph fingerprints are XXH3-128 over a framework domain tag, framework
  contract version, canonical node fields, payload schema/version/hash, and
  canonical references. Factory fingerprints also include the translated graph
  fingerprint. Pipeline stack order remains authored order and participates in
  job/provenance fingerprints; pipeline entries are never sorted.
- Default limits are 1,000,000 nodes, 4,000,000 dependency edges, 4,096
  diagnostics, and 16 GiB aggregate payload bytes. A request may lower these
  bounds but may not bypass them. Source-capture limits remain independently
  enforced before translation.
- Translation, worker-safe pipelines, and detached product construction may run
  on workers. Object lookup/materialization, target revalidation, state
  exchange, publication, save, reverse, and compensation remain editor-owned.
- Factory dependencies determine deterministic materialization/publication
  order. The complete graph validates before construction. Publication enters
  one non-cancelable boundary, saves bundle dependencies before the root, and
  reverses committed exchanges in strict reverse order after any failure.

### Compatibility and authored-result baseline

Existing `FSingleAssetProvenance` and `DImportRecord` payloads remain readable
only as migration inputs. The first Interchange reimport/repair attempt maps
known built-in identities (`DurinImage`, `Assimp`, and the Scene provider) to an
explicit translator and default versioned pipeline stack before any candidate
build. A successful atomic publication persists `FInterchangeProvenance` and
removes the legacy-only reproduction dependency. An unknown provider, unknown
settings version, ambiguous format, incomplete source set, or asset-family
mapping without a lossless default is rejected before construction with an
actionable compatibility diagnostic; it is never guessed from extension alone.

The authored-value baseline is the existing focused fixture behavior in
`SingleAssetImportTests.cpp`, `SceneImportTests.cpp`,
`VolumeTextureSourceImportTests.cpp`, `TextureCubeTests.cpp`, terrain import
tests, and `AssetImportCoreTests.cpp`: stable output paths and identities,
source/settings hashes, diagnostics, unchanged-output preservation, explicit
orphans/collisions, detached candidate validation, reverse exchange, root-last
save, cancellation before finalization, one terminal outcome, and provider
drain before value destruction must remain byte- or value-equivalent as
applicable. New golden graph fixtures supplement rather than weaken those
authored-result assertions.

## Implementation Stages

### Stage 0: Freeze contracts and select the graph model

- [x] Inventory every production import, preview, reimport, record action, and
  recovery entrypoint; map each to translator, pipeline, factory, job, and
  publication responsibilities.
- [x] Define stable IDs, schema/version rules, node and factory dependency
  rules, canonical ordering, fingerprints, resource limits, diagnostics, and
  typed payload access for both graphs.
- [x] Define translator, pipeline, factory, descriptor, registration, lease,
  request, provenance, result, and inspection contracts without retaining a
  second workflow-specific execution API.
- [x] Freeze representative authored outputs, output identities, provenance,
  diagnostics, cancellation behavior, rollback state, and provider-unload
  behavior for every in-scope asset family.
- [x] Select compatibility behavior for existing single-asset provenance and
  import records, including the exact point at which each is upgraded or
  rejected with an actionable diagnostic.
- [x] Add header-level and value-contract tests for graph validation,
  determinism, schema mismatch, ambiguity, dependency cycles, resource limits,
  registration retirement, and lease destruction order.

#### Acceptance Gate

- No unresolved decision changes graph identity, pipeline ordering,
  persistence, registry selection, thread ownership, or publication ordering.
- Existing fixtures provide a reproducible compatibility baseline before any
  production path changes.

### Stage 1: Implement Interchange Core and registries

- [x] Add immutable translated-node and factory-node graph builders,
  finalization, canonical serialization, fingerprints, validation, bounded
  diagnostics, and typed schema access in AssetImportCore.
- [x] Add translator, pipeline, and factory interfaces plus immutable
  descriptors carrying stable identity, version, capabilities, and settings
  schema.
- [x] Add exact scoped registration, metadata-only enumeration, deterministic
  selection, ambiguity reporting, module gates, implementation leases, and
  retirement/drain integration for all three registries.
- [x] Add pipeline-stack configuration and validation with ordered execution,
  immutable intermediate views, settings capture, and settings migration
  diagnostics.
- [x] Add framework provenance values and serialization for translator,
  pipeline stack, sources, graph fingerprints, and output mappings without yet
  changing production asset packages.
- [x] Add focused tests for concurrent lookup/retirement, escaped graph values,
  registry revision changes, pipeline failure isolation, and deterministic
  graph output across repeated runs.

#### Acceptance Gate

- Synthetic translators, pipelines, and factories can register, build and
  validate deterministic graphs, retire safely, and leave no callable or
  provider-owned value after drain.
- Interchange Core depends on no concrete asset class, decoder, build recipe,
  editor Widget, or provider module.

### Stage 2: Implement the framework-owned import job

- [x] Implement `FInterchangeImportJob` over the shared operation runner with
  editor capture, worker translation/pipeline/product rounds, editor
  materialization, validation, finalization, compensation, and one terminal
  outcome.
- [x] Implement factory-graph topological scheduling, independent worker-safe
  product construction where permitted, deterministic editor materialization,
  and dependency-ordered publication.
- [x] Implement initial import, preview, reimport, source replacement, repair,
  and multi-output modes as request policy on the same job rather than
  separate job classes.
- [x] Implement scheduled and inline entrypoints through the same job, plus
  immutable inspection snapshots for configuration UI and previews.
- [x] Implement preview reuse with complete source/settings/stack/graph/target
  fingerprint checks and bounded retained memory.
- [x] Add cancellation and failure injection at every worker/editor boundary,
  including translator failure, pipeline rejection, factory failure, stale
  target, save failure, reverse exchange, provider retirement, project drain,
  and shutdown.

#### Acceptance Gate

- One synthetic multi-output import traverses Translator -> Pipelines ->
  Factories -> Publication with synchronous/scheduled parity and no UI-owned
  advancement.
- Cancel, failure, retirement, and shutdown leave no partial authored graph,
  active task, retained provider value, callable, lease, or hidden terminal
  outcome.

### Stage 3: Migrate the image and Texture2D vertical slice

- [x] Register one image translator that emits normalized image source nodes
  and one default texture pipeline that emits a Texture2D factory node.
- [x] Adapt Texture2D detached build and candidate materialization behind a
  typed factory without duplicating decode, build, validation, or publication.
- [x] Route Texture2D import, preview where applicable, current-source
  reimport, replacement-source reimport, and source repair through the
  framework job.
- [x] Persist and restore translator, pipeline, node, settings, source, and
  output provenance; qualify existing-asset compatibility behavior.
- [x] Replace Texture2D dialog and Content Browser submission with the generic
  request and operation outcome APIs.
- [x] Add deterministic, cancellation, stale-source, stale-target, save
  failure, rollback, inline/scheduled parity, module retirement, and reload
  tests for the complete vertical slice.

#### Acceptance Gate

- Texture2D has no production import or reimport path outside Interchange and
  produces the frozen authored result and provenance required by Stage 0.
- The vertical slice demonstrates that a new format, pipeline, or texture
  factory can be added without editing the framework job or editor host.

### Stage 4: Migrate geometry and Scene graphs

- [x] Register glTF, GLB, FBX, and supported geometry translators that emit
  stable mesh, material, texture, skeleton, skin, and animation source nodes
  with explicit dependencies.
- [x] Implement default static-asset and Scene pipeline stacks that select
  outputs, destinations, settings, identities, collision policy, and factory
  dependencies without source-format branches in factories.
- [x] Adapt StaticMesh, Material, Skeleton, SkeletalMesh, AnimationClip, and
  dependent Texture2D construction behind typed factories.
- [x] Route single StaticMesh import and multi-output Scene import through the
  same framework job and factory graph executor.
- [x] Persist node-to-output mappings and migrate Scene reimport, unchanged
  output preservation, added/removed output reconciliation, orphan reporting,
  collision handling, record actions, and record repair.
- [x] Add golden translated-graph and factory-graph fixtures plus authored
  parity, malformed input, unsupported feature, resource limit, dependency,
  cancellation, stale plan, rollback, and provider unload coverage.

#### Acceptance Gate

- StaticMesh and Scene differ only by translator/pipeline/factory graph content,
  not by orchestration API or UI-owned state machine.
- Reimport deterministically preserves stable outputs and reports removed,
  incompatible, or occupied outputs without partial publication.

### Stage 5: Migrate remaining asset families and recovery

- [x] Add pipelines and factories for TextureCube face/panorama workflows,
  VolumeTexture atlases, and Terrain Heightmap sources using the shared image
  translation nodes where their source semantics match.
- [x] Migrate TextureCube preview to supersedable framework preview requests
  and reuse matching detached products through the common fingerprint policy.
- [x] Migrate initial import, current/new-source reimport, repair, and asset
  inspection capabilities for every remaining family.
- [x] Migrate import-record actions and missing-derived-data editor recovery to
  framework requests with appropriate `EditorOperation` or `SessionCritical`
  lifetime.
- [x] Move all remaining dialogs, Content Browser actions, and recovery callers
  to generic request, inspection, operation snapshot, and outcome APIs.
- [x] Add cross-family concurrency, conflicting claims, project switch,
  workspace teardown, recovery, provider unload, and process-shutdown tests.

#### Acceptance Gate

- Every in-scope production workflow uses the same framework job and remains
  observable independently of its initiating Widget.
- No source decode, translation, pipeline execution, or detached product build
  runs synchronously from a UI draw or input callback.

### Stage 6: Remove superseded import architecture

- [x] Remove provider interfaces and registrations whose responsibilities are
  fully represented by translator, pipeline, and factory descriptors.
- [x] Remove workflow-specific plan/execution handles, begin/poll/cancel APIs,
  dialog/panel state machines, provider-owned complete jobs, and production
  synchronous authoring entrypoints.
- [x] Remove opaque cross-stage provider data and duplicate single-output,
  Scene, record-action, and recovery orchestration.
- [x] Remove compatibility adapters after repository search proves that no
  production caller, test fixture, serialized provenance, or module startup
  path requires them.
- [x] Consolidate capability inspection, diagnostics, progress phases, and
  terminal outcomes around Interchange vocabulary.
- [x] Update module dependency assertions and prove runtime and cooked targets
  deploy no editor interchange module or source decoder.

#### Acceptance Gate

- Repository search finds one production import orchestration model and no
  callable legacy provider path.
- AssetImportCore, AssetForge, LevelEditor, and affected runtime asset modules
  preserve their intended dependency direction and unload cleanly.

### Stage 7: Qualify and document the framework

- [x] Run focused graph, registry, job, asset-family, Scene lifecycle,
  provenance, publication, failure-injection, UI model, module retirement, and
  shutdown suites using the repository testing workflow.
- [x] Capture representative traces for large Texture2D, StaticMesh,
  TextureCube, VolumeTexture, Terrain Heightmap, and Scene imports; attribute
  any remaining editor-thread span above the accepted responsiveness budget.
- [x] Add extension qualification fixtures proving that an independently owned
  translator, pipeline, and factory can register, execute, retire, unload, and
  reload without stale code or values.
- [x] Update the lasting Asset Import Framework, async operation, asset data,
  provenance, module retirement, and user workflow documentation with only the
  implemented contracts.
- [x] Record final validation evidence, remove obsolete plan-only terminology,
  and complete the plan only after every acceptance gate and Definition of
  Done item is satisfied.

#### Acceptance Gate

- Focused and applicable editor validation passes with no legacy execution
  fallback, missing lifecycle evidence, or undocumented production behavior.
- Lasting contracts, extension guidance, and user workflows describe the
  implemented framework without depending on this plan.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Graph determinism | Repeated and concurrent translation/pipeline runs produce identical canonical nodes, dependencies, fingerprints, diagnostics, and output identities. |
| Schema safety | Unknown versions, malformed payloads, excessive allocations, missing references, duplicate IDs, and cycles fail before factory execution. |
| Selection | Format recognition, configured pipeline choice, persisted identity restoration, ambiguity, missing provider, and incompatible version tests are deterministic. |
| Thread ownership | Tests reject worker access to objects, packages, catalog, editor models, render resources, RHI, and publication state. |
| Semantic parity | Migrated fixtures preserve required authored values, paths, DDC identities, provenance, diagnostics, and reconciliation behavior. |
| Cancellation and failure | Every pre-finalization phase is cancelable; publication/save failures restore the complete authored graph and terminal state is unique. |
| Lifetime and unload | Provider retirement, project drain, shutdown, and DLL reload leave no active task, escaped value, callable, candidate, or lease. |
| Reimport | Translator/pipeline settings, source-node mappings, unchanged identities, replacements, orphans, collisions, repair, and compatibility failures are covered. |
| UI ownership | Dialog close/background, panel rebuild, concurrent operations, cancel feedback, reveal, retry, and persistent failure require no domain polling. |
| Dependencies | Runtime and cooked dependency-closure checks exclude editor interchange modules and source decoders. |
| Responsiveness | Representative traces place capture, decode, translation, pipeline work, and detached build outside UI callbacks and attribute remaining publication cost. |

## Definition of Done

- All production import, preview, reimport, repair, record action, and recovery
  paths enter through one `FImportService` interchange request surface.
- Source formats are implemented only by translators, authoring policy only by
  pipelines, and output construction only by typed factories.
- Immutable translated and factory graphs have stable versioned schemas,
  identities, dependencies, bounds, validation, and deterministic
  fingerprints.
- One framework-owned job provides scheduled/inline parity, progress,
  cancellation, preview reuse, lifetime ownership, and terminal outcomes.
- Workers operate only on captured or detached values; editor mutation and
  failure-atomic publication remain explicit and fully reversible.
- Provenance can reproduce or actionably reject the recorded translator,
  pipeline stack, source-node mapping, settings, and reconciliation contract.
- Every registered implementation retires and unloads without stale execution
  or provider-owned destruction after native release.
- Legacy provider orchestration and workflow-specific async APIs have no
  production or serialized compatibility dependency and are removed.
- Lasting architecture, extension, lifecycle, and user documentation is
  updated, and the complete validation matrix passes.

## Completion Evidence

Validated on Windows MSVC x64 Debug `DurinEditor` on 2026-08-24:

- `AssetImportCoreTests`: 61/61 passed, covering graph/schema contracts,
  deterministic selection, conflict admission, cancellation, retirement,
  preview reuse, provenance, publication failure, and extension lifetime.
- `TextureTests`: 83/83 passed, covering Texture2D, TextureCube,
  VolumeTexture, Terrain Heightmap, recovery, cache, source replacement, and
  migrated Interchange entrypoints.
- `SceneImportTests`: 5/5 passed, covering heterogeneous graphs, record
  actions, skeletal dependencies, scheduled cancellation, and runtime
  ownership.
- `SkeletalSceneLifecycleTests`: 1/1 passed, including unchanged reimport,
  cleared-DDC recovery, deterministic cook, and runtime-only load.
- `SceneImportVulkanTests`: 1/1 passed.
- `LevelEditor` built successfully after the final API removal and UI
  migration.
- Repository search found no source or test reference to the removed
  `PlanSceneImport`, `ExecuteSceneImport`, single-asset handler/capability, or
  import-record-handler entrypoints.

Representative test logs captured `Interchange.BuildIndependentProducts`
worker scheduling and terminal operation phases across the listed asset
families. Translation, pipelines, and detached construction execute outside UI
callbacks; focused runs exposed no remaining editor-thread span requiring a
separate responsiveness exception. Candidate materialization, dependency
binding, state exchange, and bundle save remain intentionally attributed to
the editor publication boundary.

## Deferred Follow-ups

- Blueprint, Python, or data-authored pipelines after a reflection and sandbox
  contract is selected.
- Runtime interchange deployment for explicitly qualified asset families.
- Remote translation, shared import workers, distributed builds, and
  persistent batch queues.
- A generic editor-operation framework for non-import domains.
- Background package serialization or publication if final profiling shows
  that editor-thread persistence, rather than interchange preparation, is the
  remaining responsiveness limit.

## Related Documentation

- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Modular Features and Module Retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Build and Run Agent Workflow](../Agents/BuildAndRun.md)
- [Testing Agent Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Editor/AssetImportCore/Public/AssetImportCore.h`
- `Engine/Source/Editor/AssetImportCore/Public/ImportService.h`
- `Engine/Source/Editor/AssetImportCore/Public/Interchange.h`
- `Engine/Source/Editor/AssetImportCore/Public/InterchangeJob.h`
- `Engine/Source/Editor/AssetImportCore/Public/ImportJob.h`
- `Engine/Source/Editor/AssetImportCore/Private/AssetImportCore.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/ImportService.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/InterchangeJob.cpp`
- `Engine/Source/Editor/AssetForge/Private/AssetForgeProviders.cpp`
- `Engine/Source/Editor/AssetForge/Private/SceneImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/ImportDialogState.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SceneImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportCoreTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SingleAssetImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp`
