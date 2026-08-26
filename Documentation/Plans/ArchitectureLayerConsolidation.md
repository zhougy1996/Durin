# Architecture Layer Consolidation Plan

Summary: Collapse redundant compilation, thumbnail, import, and renderer ownership layers while preserving domain behavior

Last reviewed: 2026-08-26

Status: Active
Completed:

## Current Status

Selected and ready for Stage 0. The preceding asset-compilation refactor
established `FAssetCompilingManager` plus concrete compilation domains and
removed Texture2D's independent Coordinator/Service/Scheduler architecture.
A follow-up source audit found the same ownership duplication in material
compilation, a deeper public layering stack in rendered thumbnails, a hidden
process job registry behind `FImportService`, and simultaneous dependency
injection and active-global lookup for renderer resource invalidation.

No implementation in this plan has started. Stage 0 must freeze the retained
behavior and symbol-removal audits before any public thumbnail surface changes.

## Goal

- Make each selected subsystem expose one clear owner for lifecycle, mutable
  state, admission, cancellation, completion publication, and diagnostics.
- Keep scheduling queues, state machines, caches, and callback registries as
  private mechanisms unless they are independently owned extension boundaries.
- Remove forwarding types and hidden process singletons that give one logical
  subsystem two apparent owners.
- Preserve observable compilation, thumbnail, import, and renderer invalidation
  behavior while reducing public API and test coupling to implementation layers.
- Replace generic `Service` names used only as static function containers with
  names that state the operation or editing domain.

## Scope

- Fold `FMaterialCompileService` state and behavior directly into
  `FMaterialCompilationDomain`, including task scope, single-flight records,
  retained programs, mailbox, diagnostics, and shutdown.
- Consolidate rendered-thumbnail ownership around one provider-registration
  boundary and one cache/generation owner. Demote queue and pipeline mechanics
  from public architecture to private cache implementation.
- Move the async import job registry under the owned implementation state of
  `FImportService`; remove its independent process singleton and forwarding
  call chain.
- Remove the active-global renderer resource coordinator lookup while retaining
  the coordinator's generation and invalidation-ordering responsibilities.
- Rename or replace stateless static `Service` containers in the touched
  authoring surfaces when the resulting API is materially clearer.
- Rewrite focused tests to assert public domain behavior and retained
  invariants instead of constructing layers removed by this plan.
- Update the authoritative runtime and editor architecture contracts as each
  stage lands.

## Non-Goals

- Removing `FAssetCompilingManager`, `IAssetCompilationDomain`, the Core task
  system, or concrete domain-owned async policy.
- Combining unrelated compilation families into one domain or moving material
  compilation out of Runtime `Engine`.
- Changing material program identity, shader generation, last-known-good
  publication, retained-program limits, or Cook data.
- Redesigning thumbnail appearance, preview scenes, persistent key or PNG
  formats, GPU texture lifetime, priority policy, or frame budgets.
- Replacing the AssetForge translator/planning-pass/builder extension model or
  changing import transaction semantics, preview ownership, claims, or inline
  execution behavior.
- Removing `FRendererResourceCoordinator`; it owns real render-thread ordering
  and generation state. Only its duplicate global access path is in scope.
- Collapsing AssetCore's runtime state, load service, and mutation coordinator.
  Those types own distinct lifecycle, loading-transaction, and persistent
  mutation responsibilities.
- Combining the level viewport edit-mode registry and manager. Their process
  descriptor lifetime and workspace active-instance lifetime are distinct.
- Folding component-local `FTerrainCollisionCoordinator` or
  `FContentBrowserRefreshCoordinator` merely because their names are broad.
- Broad renaming of every `Manager`, `Service`, `Registry`, `Coordinator`, or
  `Scheduler` in the repository.

## Design Decisions and Invariants

### One public owner per mutable domain

For each selected subsystem, one object owns mutable state and lifecycle. A
private helper may implement a queue, cache, store, or state transition, but it
does not receive a second process lifetime, global accessor, public shutdown
contract, or forwarding facade.

The intended ownership shapes are:

```text
FAssetCompilingManager
  -> FMaterialCompilationDomain
       -> private task/flight/mailbox state

FAssetThumbnailRegistry
  -> provider registrations and owner gates

FRenderedAssetThumbnailCache
  -> private request queue, generation state, object store, preview, upload

FImportService
  -> component registry state
  -> private async job state

FSceneRenderer / RendererModule
  -> FRendererResourceCoordinator
       -> explicit invalidation request route
```

The final thumbnail registration type name may remain
`FAssetThumbnailProviderRegistry` if Stage 0 confirms it is already the clearest
public boundary. The invariant is removal of the forwarding service and public
queue/pipeline architecture, not a rename for its own sake.

### Preserve real boundaries

- Provider registries remain public only when unloadable modules independently
  register callbacks and require owner gates and retained resource leases.
- Domain objects retain lifecycle methods when a process aggregate invokes
  those methods through a stable contract.
- Thread handoff and publication checks remain explicit even after the helper
  class that currently contains them is removed.
- Object stores and deterministic pure algorithms may remain separately
  testable implementation types when they have no lifecycle authority.

### Instance ownership excludes hidden singleton ownership

An instance API must not forward mutable work to a second process singleton.
`FImportService::FImpl` owns its async job state, and material domain methods
operate on the domain instance's state. Tests may construct isolated owners
without sharing hidden state with other tests or the editor process.

### Dependency injection and service location are alternatives

Renderer consumers continue receiving `FRendererResourceCoordinator&` from
their composition owner. The global active pointer and get/set functions are
removed. Rare cross-module invalidation requests enter through an explicit
RendererModule or request-sink boundary that is unavailable after renderer
shutdown and never retains backend callbacks beyond module retirement.

### Tests follow observable contracts

Tests retain coverage for ordering, coalescing, bounded admission,
supersession, cancellation, stale-result rejection, publication, shutdown, and
module retirement. Tests whose only purpose is to construct a forwarding layer
or repeat the same transition through adjacent public helpers are removed or
merged into owner-level contract tests.

### No compatibility architecture

Public types removed by a stage receive no forwarding aliases, deprecated
facades, or parallel compatibility singleton. All repository consumers migrate
in the same stage. Serialized asset, DDC, Cook, shader, thumbnail-key, and
persistent object formats remain unchanged.

## Current Foundations and Gaps

- `FTexture2DCompilationDomain` is the reference shape: the domain directly
  owns per-asset state, bounded worker admission, fairness, memory budget,
  cancellation, mailbox, diagnostics, and GameThread publication.
- Material compilation still uses
  `FMaterialCompilationDomain -> GMaterialCompileService`; the domain mostly
  forwards lifecycle and aggregate operations to the service.
- Material objects and tests also enter compilation through
  `FMaterialCompileServiceAccess`, exposing the implementation-layer name
  beyond the domain owner.
- Rendered thumbnails currently expose provider registry, rendered-thumbnail
  service, scheduler, pipeline, and cache types. The service forwards registry
  operations, while each cache separately composes a scheduler and pipeline.
- `AssetThumbnailContractTests.cpp` constructs the intermediate thumbnail
  layers directly across many tests, making their existence appear contractual.
- `FImportService` owns component registration state, but every async operation
  forwards to a function-local process `FImportJobRegistry` singleton.
- Renderer sub-renderers already receive the coordinator by reference, while
  device invalidation and test seams also use `GActiveRendererResourceCoordinator`.
- `FSkyBoxLevelAuthoringService` and `FMaterialGraphService` are stateless
  static function containers; their `Service` suffix communicates ownership
  that they do not have.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [ ] Record baseline behavior and focused test counts for `MaterialTests`,
  `ThumbnailTests`, `MaterialThumbnailTests`, `TextureThumbnailTests`,
  `StaticMeshThumbnailTests`, `AssetForgeTests`, and `EditorRenderingTests`.
- [ ] Audit every non-archive source and documentation reference to the symbols
  selected for removal, including private test access and module registration
  signatures.
- [ ] Confirm the final thumbnail public surface: one provider-registration
  owner plus `FRenderedAssetThumbnailCache`, with queue and generation pipeline
  types private to the cache implementation.
- [ ] Select the explicit renderer invalidation entry point and prove its
  lifetime is bounded by RendererModule/SceneRenderer shutdown.
- [ ] Record any static `Service` rename that would create source churn outside
  the already touched authoring modules; defer broad or ambiguous renames.

#### Acceptance Gate

- The retained behaviors, public owners, symbols to remove, affected modules,
  focused targets, and full-build requirement are explicit with no unresolved
  ownership decision.

### Stage 1: Make material compilation domain-owned

- [ ] Move task scope, attribution, flight table, completion mailbox, retained
  program cache, budgets, diagnostics, and admission state into
  `FMaterialCompilationDomain`.
- [ ] Remove `FMaterialCompileService` and `GMaterialCompileService` without
  changing GameThread publication or worker value ownership.
- [ ] Replace `FMaterialCompileServiceAccess` with domain-neutral material
  lifecycle functions or narrowly named private access required by `DMaterial`.
- [ ] Preserve object construction fallback, normalization failure behavior,
  single-flight consumers, force recompilation, owner cancellation, stale
  result rejection, and last-known-good state.
- [ ] Update material compilation tests to exercise the domain/public material
  lifecycle rather than the removed service layer.
- [ ] Update the asset-compilation and material-system contracts.

#### Acceptance Gate

- Material compilation has one mutable owner below `FAssetCompilingManager`;
  focused material and asset-compilation tests pass, and a non-archive symbol
  audit finds no removed service/access names.

### Stage 2: Consolidate rendered-thumbnail ownership

- [ ] Remove the forwarding `FRenderedAssetThumbnailService` or merge its
  registration API into the selected provider registry boundary.
- [ ] Migrate editor feature module registrations to the retained registry
  type and preserve unload-safe owner gates, generations, exact-class lookup,
  source-image capture, replacement, and shutdown behavior.
- [ ] Move request coalescing, priority ordering, queue budgets, state
  transitions, persistent-object interaction, and pipeline statistics under
  `FRenderedAssetThumbnailCache` private implementation.
- [ ] Remove public `FAssetThumbnailScheduler` and
  `FRenderedAssetThumbnailPipeline` construction from production consumers.
- [ ] Preserve warm hits, generated-pixel bypass, parked resource waits,
  preview render allowance, retry, cancellation, stale revision rejection,
  upload, GPU eviction, and persistent publication.
- [ ] Rewrite thumbnail tests around registry and cache behavior; retain small
  direct tests only for independent pure object-store/key algorithms.
- [ ] Update thumbnail and asynchronous editor-operation contracts.

#### Acceptance Gate

- Production code exposes only the selected registration boundary and cache
  owner; all thumbnail feature targets pass; no removed Service/Scheduler/
  Pipeline symbols remain outside historical archives; test coverage still
  demonstrates every retained queue and publication invariant.

### Stage 3: Give import jobs to FImportService

- [ ] Move async job tables, admission, claim ownership, preview ownership,
  task scope, mailbox/editor steps, provider state, and drain behavior into
  `FImportService::FImpl`-owned state.
- [ ] Remove the function-local `GetImportJobRegistry()` singleton and the
  `FImportJobRegistry` friendship from public operation types.
- [ ] Make `FImportOperationHandle` cancellation resolve only through the
  owning service lifetime without retaining a dangling service pointer.
- [ ] Preserve inline and submitted execution ordering, cancellation, owner and
  provider drains, component revision checks, and editor tick pumping.
- [ ] Add isolated multi-instance tests if supported by the public constructor,
  proving jobs and shutdown state do not leak between service instances.
- [ ] Update the asset-import architecture contract.

#### Acceptance Gate

- `FImportService` is the only import lifecycle owner, `AssetForgeTests` pass,
  editor shutdown drains accepted work, and no global import job registry or
  forwarding accessor remains.

### Stage 4: Remove renderer active-global lookup

- [ ] Route shader reload, retry, and future device invalidation through the
  Stage 0 selected RendererModule/SceneRenderer request boundary.
- [ ] Remove `GActiveRendererResourceCoordinator`,
  `GetRendererResourceCoordinator`, and
  `SetActiveRendererResourceCoordinator`.
- [ ] Preserve render-thread generation ordering, forced shader-recompile
  generation, resource release/recreation fan-out, console command owner gates,
  and shutdown rejection.
- [ ] Update integration test fixtures to own or obtain the coordinator through
  normal composition rather than installing an active global.
- [ ] Update renderer resource recovery documentation.

#### Acceptance Gate

- Coordinator ownership is explicit from RendererModule/SceneRenderer to every
  consumer, `EditorRenderingTests` and applicable Vulkan integration tests
  pass, and no active-global coordinator symbol remains.

### Stage 5: Clarify stateless authoring APIs

- [ ] Replace `FSkyBoxLevelAuthoringService` with an operation/domain name or
  namespace consistent with the existing level authoring vocabulary.
- [ ] Replace `FMaterialGraphService` with an editing/domain name or namespace
  consistent with the material graph authoring contract.
- [ ] Apply the rename only to stateless static containers confirmed in Stage
  0; leave stateful services and independently owned managers unchanged.
- [ ] Update call sites, focused editor tests, and authoritative authoring docs.

#### Acceptance Gate

- Touched APIs communicate operations rather than nonexistent service
  lifetimes, focused level/material editor tests pass, and no compatibility
  aliases preserve the ambiguous names.

### Stage 6: Complete cross-domain validation and documentation

- [ ] Run every focused target in the validation matrix after the final stage,
  not only the target associated with the last code change.
- [ ] Run the repository-required full build because the plan changes public
  APIs across Runtime, Developer, and Editor module boundaries.
- [ ] Run relevant editor/game smoke validation when composition or shutdown
  code changes require it under the build and testing guides.
- [ ] Run changed-document and all-plan validation.
- [ ] Audit non-archive source and documentation for every removed symbol and
  obsolete ownership statement.
- [ ] Publish lasting ownership and lifecycle rules in their authoritative
  domain documents, then record completion evidence here.

#### Acceptance Gate

- All required builds, focused tests, smoke checks, documentation validation,
  and symbol audits pass; lasting contracts describe the consolidated owners;
  all plan checklists and definition-of-done items are evidence-backed.

## Validation Matrix

| Change area | Required validation | Retained contract |
| --- | --- | --- |
| Material domain | `AssetCompilingManagerTests`, `MaterialTests` | lifecycle, single-flight, cancellation, publication, last-known-good |
| Thumbnail registry/cache | `ThumbnailTests`, `MaterialThumbnailTests`, `TextureThumbnailTests`, `StaticMeshThumbnailTests` | registration lifetime, queue policy, warm/cold generation, stale rejection, upload |
| Import ownership | `AssetForgeTests` plus affected import integration targets selected by the testing guide | isolated ownership, claims, cancellation, inline/async ordering, shutdown drain |
| Renderer routing | `EditorRenderingTests` plus affected Vulkan targets selected by the testing guide | generation order, shader reload, retry, release/recreate, shutdown rejection |
| Authoring API names | affected MaterialEditor and LevelEditor focused targets | unchanged editing and transaction behavior |
| Cross-module completion | repository full build and applicable editor/game smoke checks | public dependency and composition integrity |
| Documentation | changed-doc validation and all-plan validation | valid links, metadata, and lasting ownership statements |

Test selection and execution follow [Testing](../Agents/Testing.md); build and
smoke execution follow [Build And Run](../Agents/BuildAndRun.md). Each stage
runs the smallest focused targets first. Stage 6 repeats the complete matrix so
stage-local success cannot hide a later cross-domain regression.

## Definition of Done

- [ ] Material compilation has no domain-to-service forwarding layer or hidden
  global mutable compile owner.
- [ ] Rendered thumbnails expose one registration boundary and one cache owner;
  queue and pipeline mechanisms are private.
- [ ] `FImportService` owns all component and async-job state for its lifetime.
- [ ] Renderer resource invalidation uses explicit composition with no active
  global coordinator pointer.
- [ ] Selected stateless authoring APIs no longer use misleading `Service`
  names.
- [ ] Removed types have no compatibility facade and no non-archive source or
  documentation references.
- [ ] Tests assert retained behavior at stable owner boundaries and no longer
  duplicate coverage solely because intermediate layers existed.
- [ ] Focused tests, full build, required smoke checks, documentation validators,
  and source-symbol audits pass.
- [ ] Authoritative runtime/editor contracts describe the final ownership and
  lifecycle model, and this plan is marked completed with exact evidence.

## Deferred Follow-ups

- Unifying AssetCore load and mutation internals; current evidence supports
  their separate responsibilities.
- Changing viewport edit-mode registry/manager ownership.
- Renaming stateful services solely for naming consistency.
- Converting component-local asynchronous work into asset compilation domains.
- A generic repository-wide rule banning Manager, Coordinator, Scheduler,
  Service, Provider, Registry, or Domain suffixes.
- Redesigning persistent cache formats, task-system scheduling, or module owner
  gates after the redundant layers are removed.

## Related Documentation

- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Material Graph Authoring](../Editor/Architecture/MaterialGraphAuthoring.md)
- [Testing](../Agents/Testing.md)
- [Build And Run](../Agents/BuildAndRun.md)
- [Asset Compiling Manager Refactor Plan](AssetCompilingManagerRefactor.md)

## Related Code

- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialCompileLifecycle.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnailProvider.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnailScheduler.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailPipeline.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailService.h`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailCache.cpp`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/ImportService.h`
- `Engine/Source/Editor/AssetForge/Private/AsyncImport.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Editor/LevelEditor/Public/SkyBoxLevelAuthoring.h`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialGraphAuthoring.h`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialCompileLifecycleTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetThumbnailContractTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetForgeContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
