# Architecture Layer Consolidation Plan

Summary: Collapse redundant compilation, thumbnail, import, and renderer ownership layers while preserving domain behavior

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

Completed. Material compilation state is domain-owned, rendered-thumbnail
registration and cache ownership are explicit, async import jobs are isolated
per `FImportService` instance, and renderer invalidation enters through
`FRendererModule`. The touched stateless authoring boundaries now use operation
names. Focused tests, applicable Vulkan integration tests, the full build,
editor startup smoke, documentation validation, and removed-symbol audits form
the completion evidence recorded below.

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

- Fold material compilation state and behavior directly into
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
- Material compilation still uses a domain-to-process-singleton forwarding
  path; the domain mostly forwards lifecycle and aggregate operations.
- Material objects and tests also enter compilation through a compile-service
  access shim, exposing the implementation layer beyond the domain owner.
- Rendered thumbnails currently expose provider registry, rendered-thumbnail
  service, scheduler, pipeline, and cache types. The service forwards registry
  operations, while each cache separately composes a scheduler and pipeline.
- `AssetThumbnailContractTests.cpp` constructs the intermediate thumbnail
  layers directly across many tests, making their existence appear contractual.
- `FImportService` owns component registration state, but every async operation
  forwards to a function-local process job-registry singleton.
- Renderer sub-renderers already receive the coordinator by reference, while
  device invalidation and test seams also use an active-global coordinator.
- The SkyBox and material-graph authoring helpers are stateless static function
  containers; their former `Service` suffix communicated ownership that they
  do not have.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Record baseline behavior and focused test counts for `MaterialTests`,
  `ThumbnailTests`, `MaterialThumbnailTests`, `TextureThumbnailTests`,
  `StaticMeshThumbnailTests`, `AssetForgeTests`, and `EditorRenderingTests`.
- [x] Audit every non-archive source and documentation reference to the symbols
  selected for removal, including private test access and module registration
  signatures.
- [x] Confirm the final thumbnail public surface: one provider-registration
  owner plus `FRenderedAssetThumbnailCache`, with queue and generation pipeline
  types private to the cache implementation.
- [x] Select the explicit renderer invalidation entry point and prove its
  lifetime is bounded by RendererModule/SceneRenderer shutdown.
- [x] Record any static `Service` rename that would create source churn outside
  the already touched authoring modules; defer broad or ambiguous renames.

#### Acceptance Gate

- The retained behaviors, public owners, symbols to remove, affected modules,
  focused targets, and full-build requirement are explicit with no unresolved
  ownership decision.

### Stage 1: Make material compilation domain-owned

- [x] Move task scope, attribution, flight table, completion mailbox, retained
  program cache, budgets, diagnostics, and admission state into
  `FMaterialCompilationDomain`.
- [x] Remove the material compile forwarding container and its process-global
  owner without
  changing GameThread publication or worker value ownership.
- [x] Replace the compile-service access shim with domain-neutral material
  lifecycle functions or narrowly named private access required by `DMaterial`.
- [x] Preserve object construction fallback, normalization failure behavior,
  single-flight consumers, force recompilation, owner cancellation, stale
  result rejection, and last-known-good state.
- [x] Update material compilation tests to exercise the domain/public material
  lifecycle rather than the removed service layer.
- [x] Update the asset-compilation and material-system contracts.

#### Acceptance Gate

- Material compilation has one mutable owner below `FAssetCompilingManager`;
  focused material and asset-compilation tests pass, and a non-archive symbol
  audit finds no removed service/access names.

### Stage 2: Consolidate rendered-thumbnail ownership

- [x] Remove the forwarding rendered-thumbnail facade and retain its
  registration API on the selected provider registry boundary.
- [x] Migrate editor feature module registrations to the retained registry
  type and preserve unload-safe owner gates, generations, exact-class lookup,
  source-image capture, replacement, and shutdown behavior.
- [x] Move request coalescing, priority ordering, queue budgets, state
  transitions, persistent-object interaction, and pipeline statistics under
  `FRenderedAssetThumbnailCache` private implementation.
- [x] Remove public `FRenderedThumbnailRequestQueue` and
  `FRenderedAssetThumbnailGeneration` construction from production consumers.
- [x] Preserve warm hits, generated-pixel bypass, parked resource waits,
  preview render allowance, retry, cancellation, stale revision rejection,
  upload, GPU eviction, and persistent publication.
- [x] Rewrite thumbnail tests around registry and cache behavior; retain small
  direct tests only for independent pure object-store/key algorithms.
- [x] Update thumbnail and asynchronous editor-operation contracts.

#### Acceptance Gate

- Production code exposes only the selected registration boundary and cache
  owner; all thumbnail feature targets pass; no removed Service/Scheduler/
  Pipeline symbols remain outside historical archives; test coverage still
  demonstrates every retained queue and publication invariant.

### Stage 3: Give import jobs to FImportService

- [x] Move async job tables, admission, claim ownership, preview ownership,
  task scope, mailbox/editor steps, provider state, and drain behavior into
  `FImportService::FImpl`-owned state.
- [x] Remove the function-local import-job accessor and registry friendship
  from public operation types.
- [x] Make `FImportOperationHandle` cancellation resolve only through the
  owning service lifetime without retaining a dangling service pointer.
- [x] Preserve inline and submitted execution ordering, cancellation, owner and
  provider drains, component revision checks, and editor tick pumping.
- [x] Add isolated multi-instance tests if supported by the public constructor,
  proving jobs and shutdown state do not leak between service instances.
- [x] Update the asset-import architecture contract.

#### Acceptance Gate

- `FImportService` is the only import lifecycle owner, `AssetForgeTests` pass,
  editor shutdown drains accepted work, and no global import job registry or
  forwarding accessor remains.

### Stage 4: Remove renderer active-global lookup

- [x] Route shader reload, retry, and future device invalidation through the
  Stage 0 selected RendererModule/SceneRenderer request boundary.
- [x] Remove the active-global coordinator pointer and its get/set accessors.
- [x] Preserve render-thread generation ordering, forced shader-recompile
  generation, resource release/recreation fan-out, console command owner gates,
  and shutdown rejection.
- [x] Update integration test fixtures to own or obtain the coordinator through
  normal composition rather than installing an active global.
- [x] Update renderer resource recovery documentation.

#### Acceptance Gate

- Coordinator ownership is explicit from RendererModule/SceneRenderer to every
  consumer, `EditorRenderingTests` and applicable Vulkan integration tests
  pass, and no active-global coordinator symbol remains.

### Stage 5: Clarify stateless authoring APIs

- [x] Replace the former SkyBox static service container with an operation/domain name or
  namespace consistent with the existing level authoring vocabulary.
- [x] Name the material graph static container `FMaterialGraphAuthoring`, consistent
  consistent with the material graph authoring contract.
- [x] Apply the rename only to stateless static containers confirmed in Stage
  0; leave stateful services and independently owned managers unchanged.
- [x] Update call sites, focused editor tests, and authoritative authoring docs.

#### Acceptance Gate

- Touched APIs communicate operations rather than nonexistent service
  lifetimes, focused level/material editor tests pass, and no compatibility
  aliases preserve the ambiguous names.

### Stage 6: Complete cross-domain validation and documentation

- [x] Run every focused target in the validation matrix after the final stage,
  not only the target associated with the last code change.
- [x] Run the repository-required full build because the plan changes public
  APIs across Runtime, Developer, and Editor module boundaries.
- [x] Run relevant editor/game smoke validation when composition or shutdown
  code changes require it under the build and testing guides.
- [x] Run changed-document and all-plan validation.
- [x] Audit non-archive source and documentation for every removed symbol and
  obsolete ownership statement.
- [x] Publish lasting ownership and lifecycle rules in their authoritative
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

- [x] Material compilation has no domain-to-service forwarding layer or hidden
  global mutable compile owner.
- [x] Rendered thumbnails expose one registration boundary and one cache owner;
  queue and pipeline mechanisms are private.
- [x] `FImportService` owns all component and async-job state for its lifetime.
- [x] Renderer resource invalidation uses explicit composition with no active
  global coordinator pointer.
- [x] Selected stateless authoring APIs no longer use misleading `Service`
  names.
- [x] Removed types have no compatibility facade and no non-archive source or
  documentation references.
- [x] Tests assert retained behavior at stable owner boundaries and no longer
  duplicate coverage solely because intermediate layers existed.
- [x] Focused tests, full build, required smoke checks, documentation validators,
  and source-symbol audits pass.
- [x] Authoritative runtime/editor contracts describe the final ownership and
  lifecycle model, and this plan is marked completed with exact evidence.

## Completion Evidence

- `AssetCompilingManagerTests`: 1/1 passed.
- `MaterialTests`: 99/99 passed.
- `ThumbnailTests`: 58/58 passed.
- `MaterialThumbnailTests`: 6/6 passed.
- `TextureThumbnailTests`: 9/9 passed.
- `StaticMeshThumbnailTests`: 9/9 passed.
- `AssetForgeTests`: 19/19 passed.
- `EditorRenderingTests`: 77/77 passed.
- `SkyBoxTests`: 11/11 passed.
- `RendererResourceReloadVulkanTests`: 1/1 passed.
- `MaterialVulkanTests`: 1/1 passed.
- `DevTool build`: the complete `Win64-Debug-DurinEditor` `all` target passed.
- Editor startup smoke: `DurinEditor` remained running after eight seconds and
  was then stopped by the smoke harness.
- Changed-document validation, all-plan validation, diff checks, and the
  non-archive removed-symbol/file audit passed on 2026-08-26.

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
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnailProvider.h`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailCache.cpp`
- `Engine/Source/Editor/AssetForge/Public/AssetForge/ImportService.h`
- `Engine/Source/Editor/AssetForge/Private/AsyncImport.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Editor/LevelEditor/Public/SkyBoxPlacement.h`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialGraphAuthoring.h`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialCompileLifecycleTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetThumbnailContractTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetForgeContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
