# AssetTools Asset Operations Plan

Summary: Move editor asset-operation policy and orchestration into AssetTools while retaining transactional asset mechanisms in Engine.

Last reviewed: 2026-08-29

Status: Archived
Completed: 2026-08-29

## Current Status

All stages are complete. The editor now uses the selected three-layer boundary:

- Engine owns asset identity, persistence, catalog/residency, graph copying,
  and opaque atomic mutation mechanisms.
- AssetTools owns reusable editor asset-command policy and orchestration.
- ContentBrowser and feature editors own selection, dialogs, presentation,
  mixed ordinary-file operations, and host refresh/reveal behavior.

AssetTools exposes capability-named typed requests and results for creation,
import, duplicate, save/resave, relocation, redirector fix-up, and deletion
preflight while retaining one service lifetime. ContentBrowser no longer
sequences Engine graph-copy, save, relocation, Fix Up, or deletion preparation;
its recursive deletion remains the sole mixed-filesystem and physical-staging
coordinator. The DurinEd relocation adapter is removed and the editor-wide Fix
Up command is composed by MainFrame, so no reverse module dependency was added.

The ambiguous Engine root `AssetTools.h` aggregate is removed. Runtime,
offline, and test consumers use explicit `Asset/...` capabilities. Final
validation passed the configured `all` build, all 83 non-application native
test targets, the focused AssetTools/ContentBrowser/package selections, full
documentation validation, and a hidden 10-tick editor startup/shutdown smoke.

## Goal

Make `AssetTools` the single reusable editor boundary for package-backed asset
commands while retaining the authoritative asset mechanisms in Engine and the
user interaction in editor hosts. After this plan, editor code can request
create, import, duplicate, save, relocate, delete, or redirector fix-up through
capability-named AssetTools operations without directly sequencing Engine
mutation phases.

The resulting ownership must make operation acceptance, warnings, committed
state, editor history, and failure cleanup consistent across ContentBrowser and
other editor callers without turning AssetTools into a UI, filesystem browser,
or second asset runtime.

## Scope

- Inventory production consumers of Engine asset authoring and mutation APIs,
  including their editor transaction, workspace, notification, and refresh
  responsibilities.
- Expand AssetTools from Factory creation/import into capability-named editor
  operations for duplication, package save, relocation, deletion, and
  redirector fix-up where the workflow is reusable outside one panel.
- Introduce typed request, preflight/summary, result, and notification values;
  replace success inference from a nullable object and unstructured message
  text where an operation has warnings or committed-state distinctions.
- Move the shared editor-history adapter for opaque Engine asset transactions
  from DurinEd to AssetTools without introducing a reverse DurinEd dependency.
- Retarget ContentBrowser asset commands to AssetTools while preserving its
  ownership of selection, confirmation modals, folder traversal, ordinary-file
  mutation, physical deletion staging, and presentation refresh/reveal.
- Retarget editor-wide redirector fix-up composition to the same AssetTools
  path and keep command registration in a non-cyclic host layer.
- Replace production and test dependence on Engine's ambiguous root
  `AssetTools.h` aggregate with precise `Asset/...` capability includes, then
  retire or rename that aggregate according to the verified consumer audit.
- Update lasting module and editor architecture documents after implementation.

## Non-Goals

- Moving package formats, catalog/reference projections, serialization,
  loading/residency, Cook, compatibility, or canonical-resave mechanisms out
  of Engine.
- Reimplementing or exposing the phases inside Engine's opaque relocation,
  deletion, or redirector-fix-up transactions.
- Moving `DPackage`, `FAssetPath`, object identity, GC, reflection, or Outer
  ownership out of CoreDObject.
- Moving ContentBrowser widgets, selection, dialogs, filters, navigation,
  filesystem snapshots, mixed folder planning, or undo staging storage into
  AssetTools.
- Making runtime import, a public importer/provider graph, asynchronous asset
  loading, source-control integration, or multi-user coordination part of this
  migration.
- Changing DAST/DBLK/DABK bytes, redirect semantics, reference rewrite rules,
  asset residency policy, or package fixtures.
- Splitting Engine Asset into another runtime shared module solely to reduce
  source-directory size.
- Forcing Scene import through single-object AssetTools operations; its
  dependency-ordered multi-package transaction remains private to
  AssetForgeBuiltins.

## Design Decisions and Invariants

- The boundary is semantic rather than name-based: Engine answers how an asset
  mutation remains correct; AssetTools answers how an editor command is
  accepted, committed, recorded, and reported.
- Engine retains `Asset::DuplicateAsset`, `Asset::SavePackage`, mutation
  preparation, and opaque transaction types as mechanism-level capabilities.
  AssetTools composes them and does not duplicate their validation or rollback.
- AssetTools may depend on DurinEd's generic Factory, transaction-manager,
  workspace, and notification contracts. DurinEd must not acquire an
  AssetTools dependency. Commands currently initiated inside DurinEd either use
  an injected capability or move to MainFrame/another existing composition
  owner.
- Public AssetTools operations use typed request/result values. A result
  distinguishes at least preflight rejection, warnings, committed live state,
  persisted state, rollback/cleanup failure, and affected asset identities;
  human-readable diagnostics remain available but are not the control plane.
- Creation/import keep their current Factory ownership. Their API may adopt the
  shared result vocabulary, but concrete factories and family build behavior
  stay in DurinEd, AssetForgeBuiltins, and the family build modules.
- Duplicate policy, including destination selection and copy suffixes, belongs
  to AssetTools. Engine continues to clone the complete persistent object graph.
- Save policy in AssetTools coordinates editor-visible eligibility and result
  publication; Engine remains the only package persistence authority.
- Relocation and fix-up retain one opaque Engine transaction. AssetTools may
  prepare and commit it once and retain it in editor history; callers may not
  sequence internal publication or compensation phases.
- Pure package-backed asset deletion may be exposed as an AssetTools command.
  ContentBrowser's recursive mixed-item deletion remains its coordinator
  because it owns ordinary files, directory traversal, confirmation, and
  physical same-volume staging. It delegates only the asset-safety and opaque
  Engine transaction portion.
- Operation completion is published once by the orchestration owner. UI panels
  refresh/reveal in response and must not infer success from filesystem or
  catalog polling.
- AssetTools owns no ImGui, platform file dialog, ContentBrowser model, or
  concrete asset-family dependency.
- Engine's root `AssetTools.h` is not a valid name for a runtime/offline
  capability aggregate once Editor AssetTools is authoritative. Consumers are
  moved to precise includes; no second header with that spelling remains as a
  compatibility facade unless Stage 0 finds a supported external consumer.
- Every stage preserves existing transaction recovery, source-companion,
  redirector closure, dirty-package, package-byte, and fixture behavior.

## Current Foundations and Gaps

- `IAssetTools` already owns Factory-backed creation/import, package adoption,
  result validation, and failed-package discard, so it is the natural editor
  authoring seam rather than a new module.
- AssetTools publicly depends on Engine, Core, CoreDObject, and DurinEd and has
  no concrete asset-family dependency, which is the desired direction for a
  reusable editor operation service.
- DurinEd currently owns `ExecuteAssetRelocations`, which composes an Engine
  transaction with global editor history. That behavior belongs with other
  asset commands, but moving it requires preserving the one-way
  `AssetTools -> DurinEd` dependency.
- ContentBrowser directly calls Engine duplication, save, relocation,
  deletion, and fix-up APIs. Its private operations implementation therefore
  mixes panel policy, ordinary filesystem work, editor transaction ownership,
  and runtime mutation mechanisms.
- ContentBrowser's recursive deletion is not a generic asset-only command: it
  combines registered packages, ordinary files, companion payloads, physical
  staging, confirmation, and recovery. Only its reusable asset-operation slice
  should move.
- DurinEd editor startup/commands directly compose redirector fix-up. A naïve
  call from DurinEd into AssetTools would create a module cycle.
- The existing `FAssetToolsResult` reports success through `Asset != nullptr`
  and one message string. That is adequate for first import but cannot express
  multi-asset warnings, committed-but-unsaved outcomes, or recovery-required
  failures.
- Engine's root `AssetTools.h` aggregates runtime/offline APIs and is included
  by DurinAssetTool, production ContentBrowser code, and many tests. Its name
  collides with the Editor AssetTools module and obscures actual dependencies.
- The current Engine transaction and catalog contracts already provide the
  atomicity, immutable summaries, and failure seams needed for this plan; a new
  mutation framework is unnecessary.

## Implementation Record

Stage 0 froze the following production ownership before code moved:

| Caller group | Existing responsibility | Implemented destination |
| --- | --- | --- |
| ContentBrowser duplicate and save/resave | Name selection, graph-copy/save composition, cleanup, refresh/reveal | AssetTools owns package-backed policy, structured completion, and cleanup; ContentBrowser retains mount eligibility and presentation |
| ContentBrowser asset/folder relocation | Selection, destination planning, Engine transaction, editor history | ContentBrowser retains selection/folder planning; AssetTools owns the opaque Engine transaction and DurinEd history adapter |
| ContentBrowser Fix Up | Redirector collection, Engine prepare/commit, refresh | ContentBrowser retains collection/presentation; MainFrame injects the AssetTools command because it already composes both modules |
| DurinEd Fix Up console command | Command registration and Engine transaction | Registration moved to MainFrame's console host and execution uses AssetTools; DurinEd has no AssetTools dependency |
| ContentBrowser recursive deletion | Mixed traversal, physical staging, Engine asset safety and transaction | ContentBrowser retains immutable mixed plan and the sole physical stage/restore callback; AssetTools owns asset preflight values and the opaque Engine deletion transaction |
| LevelEditor asset move coordinator | Feature state capture plus shared relocation adapter | Feature state remains local and relocation calls AssetTools directly through the existing one-way dependency |
| Factory creation/import | Package adoption, Factory invocation, failed-package discard | Existing AssetTools service retained and adapted to the shared result vocabulary |
| Scene/family atomic import and Engine/native-test setup | Private multi-package transaction or mechanism characterization | Remains a documented Engine mechanism caller; it is not reusable editor-command orchestration |

The implemented graph remains `Engine -> DurinEd -> AssetTools -> editor hosts`;
MainFrame composes AssetTools and ContentBrowser, ContentBrowser has a private
AssetTools dependency, and DurinEd has no reverse dependency. The former Engine
root `AssetTools.h` consumers were runtime/offline tools or test setup; all now
include explicit `Asset/...` capabilities and no supported external consumer
requires the ambiguous aggregate.

`FAssetOperationResult` records operation kind, terminal state (`Rejected`,
`Completed`, or `RecoveryRequired`), persistence state, warnings, affected
identities, diagnostics, and publication. Requests carry only the host seams
they require: physical path resolution, completion publication, or the DurinEd
transaction manager. Direct save/duplicate completion invokes the request's
publication callback once. Relocation and Fix Up enter global history once, so
the transaction manager remains the single Execute/Undo/Redo event and
mounted-content invalidation publisher. Mixed deletion continues to publish
through that same history entry. Failed or compensated attempts publish none.

Focused validation selections frozen for this migration are
`EditorOperationTests`, `ContentBrowserWorkflowTests`, `AssetPackageTests`, and
the `asset-workflow` domain, followed by the configured non-application
`test all` gate, a complete `all` build, and the documented editor smoke run.
Existing DAST/DBLK/DABK fixtures, catalog projections, opaque transaction
semantics, copy suffixes, source companions, and command availability are
characterization constraints rather than migration targets.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory every production caller of `Asset::CreateAsset`,
  `DuplicateAsset`, `SavePackage`, relocation, deletion, and redirector fix-up;
  record which behavior is mechanism, editor policy, host presentation, mixed
  filesystem coordination, or test-only setup.
- [x] Record the current module graph and select a non-cyclic owner for every
  DurinEd-initiated asset command; prove that DurinEd does not need a new
  AssetTools dependency.
- [x] Define typed operation requests, summaries, terminal states, warnings,
  affected identities, persistence state, and recovery diagnostics shared by
  the selected AssetTools commands.
- [x] Define exactly one notification/publication point for Execute, Undo, and
  Redo and record how ContentBrowser panels, workspaces, and catalog refresh
  observe it.
- [x] Classify every `AssetTools.h` consumer and select precise replacement
  headers; explicitly record any supported consumer that prevents removal.
- [x] Capture focused native-test targets for AssetTools creation/import,
  package duplication/save, ContentBrowser asset operations, mutation
  transactions, redirector fix-up, and recursive deletion recovery.
- [x] Freeze package fixtures, catalog results, transaction semantics, copy-name
  policy, source-companion behavior, and user-visible command availability.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.
- Every current orchestration responsibility has one destination and the
  selected dependency graph contains no DurinEd/AssetTools cycle.
- Request/result and notification contracts cover partial persistence and
  recovery-required outcomes without relying on diagnostic-string parsing.

### Stage 1: Establish precise AssetTools contracts

- [x] Split public AssetTools declarations into capability-named headers and
  value types while retaining one module service/lifetime; do not create
  independent global singletons per command.
- [x] Adapt create/import and failed-package discard to the shared terminal
  result vocabulary without changing Factory invocation or first-import
  acceptance semantics.
- [x] Add dependency-free test seams for operation notifications and host
  services needed by editor history, workspace coordination, and refresh.
- [x] Replace Engine root `AssetTools.h` consumers with precise `Asset/...`
  includes and remove or rename the ambiguous aggregate according to Stage 0.
- [x] Add compile-time/module tests proving AssetTools has no ContentBrowser,
  ImGui, concrete asset-family, or reverse DurinEd dependency.

#### Acceptance Gate

- Existing creation/import behavior passes through the new contracts with no
  package, Factory, or diagnostics regression.
- Runtime/offline callers depend on explicit Engine capabilities and no active
  production code confuses Engine tooling aggregates with Editor AssetTools.
- The configured target graph remains acyclic and lower runtime modules do not
  acquire editor dependencies.

### Stage 2: Centralize duplicate and save workflows

- [x] Implement AssetTools duplicate preflight and execution over Engine graph
  duplication, including deterministic `_Copy`, `_Copy2`, and later destination
  selection against both catalog and physical occupancy.
- [x] Preserve clone-specific identity replacement hooks and make save versus
  leave-dirty policy explicit in the request and terminal result.
- [x] Implement reusable AssetTools save/resave eligibility and publication
  paths while keeping Engine as the only byte persistence authority.
- [x] Retarget ContentBrowser duplicate/paste and package save/resave commands
  to AssetTools; leave clipboard, current-folder selection, menu enablement,
  messages, refresh, and reveal in ContentBrowser.
- [x] Verify failure after graph duplication discards only the disposable
  destination and cannot alter the source or publish a false success event.

#### Acceptance Gate

- ContentBrowser contains no direct graph-duplication or package-save
  orchestration beyond host presentation and requests.
- Duplicate naming, graph remapping, persistence, dirty state, cleanup, and
  notification tests cover success and every injected failure boundary.
- Existing package fixtures and canonical bytes remain unchanged.

### Stage 3: Centralize relocation and redirector fix-up workflows

- [x] Move the reusable editor transaction adapter for relocation from DurinEd
  into AssetTools and retain exactly one opaque Engine mutation transaction in
  global editor history.
- [x] Add AssetTools relocation and redirector-fix-up requests with immutable
  preflight summaries suitable for host confirmation and structured results
  suitable for Execute, Undo, and Redo reporting.
- [x] Retarget asset rename, asset drag-move, folder asset relocation, and Fix
  Up call sites to AssetTools while preserving ContentBrowser folder modeling
  and presentation policy.
- [x] Move or inject DurinEd-owned Fix Up command composition at the selected
  non-cyclic host boundary.
- [x] Ensure successful Execute, Undo, and Redo each publish one mounted-content
  invalidation and that failed or compensated attempts publish none.

#### Acceptance Gate

- No ContentBrowser or DurinEd caller directly prepares or commits an Engine
  relocation/fix-up transaction outside the documented AssetTools seam.
- Editor history preserves exact undo/redo, stale-token rejection,
  compensation, recovery-required, workspace, and catalog-refresh behavior.
- The module graph contains no new cycle and feature editor unload remains safe.

### Stage 4: Isolate asset deletion policy from mixed content deletion

- [x] Add the selected AssetTools asset-deletion preflight/transaction adapter
  for pure package-backed selections, including reference warnings, alias
  closure, dirty/resident blockers, companions, and structured recovery state.
- [x] Refactor ContentBrowser recursive deletion so its immutable mixed-item
  plan, confirmation, ordinary-file handling, and physical staging remain
  local while asset safety and opaque Engine transaction control enter through
  AssetTools.
- [x] Preserve the single coordinator rule: ContentBrowser owns each physical
  stage/restore transition and Engine/AssetTools must not stage the same bytes
  or independently sequence filesystem phases.
- [x] Retarget pure asset deletion commands to the common AssetTools operation
  and keep mixed folder deletion on the composed ContentBrowser path.
- [x] Verify transaction-history lifetime, staged-root cleanup, panel closure,
  automatic reconciliation suppression, and recovery-required retention.

#### Acceptance Gate

- Asset-only deletion policy is reusable without importing ContentBrowser
  models or UI, and mixed recursive deletion retains one atomic user command.
- Unknown packages, reparse points, cross-volume staging, external references,
  alias closure, companions, conflicts, Undo/Redo, and compensation retain
  their existing safety behavior.
- AssetTools owns no physical staging directory and no ordinary-file deletion.

### Stage 5: Contract the old call paths and publish ownership

- [x] Remove superseded ContentBrowser/DurinEd operation helpers and direct
  Engine mutation includes after every production caller uses the selected
  AssetTools capability.
- [x] Audit the AssetTools public surface for UI types, concrete families,
  mutable global state, mechanism duplication, and diagnostic-string control
  flow; contract any accidental exports.
- [x] Update `CodeModules.md`, Asset Import Architecture, Content Browser, and
  Asset Catalog And Mutation with the implemented three-layer ownership and
  operation notification rules.
- [x] Run changed-document validation, focused native targets, the complete
  configured non-application native-test gate required for the shared editor
  boundary, and the documented editor startup/shutdown smoke test.
- [x] Perform targeted stale-call and dependency searches and record exact
  validation evidence in this plan before completion.

#### Acceptance Gate

- Production editor hosts use AssetTools for reusable package-backed asset
  commands and retain only presentation or explicitly documented mixed-content
  coordination.
- Engine remains the sole asset mechanism/persistence owner, AssetTools remains
  family- and UI-neutral, and DurinEd remains independent of AssetTools.
- All required tests, build targets, documentation validators, dependency
  audits, and editor smoke validation pass before the plan is completed.

## Validation Matrix

| Change surface | Required validation | Evidence sought |
| --- | --- | --- |
| Plan and lasting ownership documents | Changed-document validation and all-plan validation following repository documentation guidance | Valid links, lifecycle metadata, and one authority for implemented contracts |
| AssetTools API and module graph | AssetTools/Factory tests plus configured editor-module build | Typed contracts compile without UI/family dependencies or module cycles |
| Creation and import | Existing Asset import/factory focused targets | Unchanged Factory selection, package adoption, failed-package discard, and diagnostics |
| Duplicate and save | Package graph-copy/save tests plus ContentBrowser operation tests | Stable copy naming, internal-reference remap, persistence state, cleanup, and single notification |
| Relocation and fix-up | Engine mutation/fix-up tests plus editor history/ContentBrowser tests | Opaque transaction ownership, exact Undo/Redo, compensation, invalidation, and workspace behavior |
| Recursive deletion | Existing recursive deletion and Engine deletion failure-injection targets | Mixed-item atomicity, one physical staging owner, alias/reference/companion safety, and recovery retention |
| Shared editor boundary | Complete configured non-application native-test run following testing guidance | No missed direct caller, dependency, lifecycle, or unload regression |
| Executable/editor composition | Complete configured editor build and documented startup/shutdown smoke | AssetTools service composition, command registration, module load, and teardown succeed |
| Stale ownership | Targeted searches for direct editor mutation preparation and ambiguous root `AssetTools.h` includes | Only documented Engine/offline mechanisms and test-private seams remain |

## Validation Evidence

- `EditorOperationTests`: 20 tests passed, including structured save/duplicate
  single-publication and disposable-destination cleanup coverage.
- `ContentBrowserWorkflowTests`: 61 tests passed and one existing conditional
  test skipped; relocation history and recursive deletion coverage passed.
- `AssetPackageTests`: 125 tests passed; `@asset-workflow` passed its five
  resolved targets.
- `test all`: all 83 configured non-application native targets passed. Earlier
  parallel attempts exposed isolated existing Material/Vulkan test flakes;
  both exact targets passed when isolated before the final clean full run.
- The configured `all` target built successfully, and DurinEditor completed
  `--hidden-window --exit-after-ticks=10` through normal shutdown.
- `doc validate --scope all`, `doc plan validate --scope all`, stale-call
  searches, `git diff --check`, and the module dependency audit passed.

## Definition of Done

- Engine owns and documents asset mechanisms; AssetTools owns reusable editor
  asset-command policy; ContentBrowser and feature editors own presentation and
  explicitly documented mixed-content coordination.
- AssetTools provides typed, tested creation/import, duplicate, save,
  relocation, deletion, and redirector-fix-up operations without depending on
  ContentBrowser, ImGui, or concrete asset families.
- ContentBrowser no longer directly sequences Engine graph duplication,
  persistence, relocation, or fix-up, and its recursive deletion delegates the
  asset transaction portion without surrendering physical staging ownership.
- DurinEd does not depend on AssetTools; editor-wide commands are composed or
  injected at a valid higher layer.
- Execute, Undo, and Redo outcomes publish one structured completion signal and
  preserve mounted-content/catalog refresh behavior without polling-based
  success inference.
- The ambiguous Engine root `AssetTools.h` aggregate is absent from active
  production ownership, with every caller using precise Engine or Editor
  capability headers.
- Package bytes, fixtures, redirect/reference semantics, dirty/residency rules,
  compensation, recovery, and source-companion behavior remain unchanged.
- Lasting documentation reflects the implemented boundaries and every stage
  acceptance gate and validation entry is evidenced before completion.

## Deferred Follow-ups

- Source-control and multi-user checkout policy for authoring operations.
- Asynchronous or cancellable editor asset commands and aggregate progress UI.
- Runtime package loading requests, priorities, cancellation, and range I/O.
- A shipping-size-driven split of editor-only Engine mutation implementation if
  target composition later demonstrates a real binary or dependency benefit.
- Generalized cross-tool command routing beyond the editor hosts currently in
  the repository.

## Related Documentation

- [Asset Import Architecture](../../../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Asset Catalog And Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [AssetCore Engine Consolidation](AssetCoreEngineConsolidation.md)
- [Build And Run Workflow](../../../Agents/BuildAndRun.md)
- [Testing Workflow](../../../Agents/Testing.md)

## Related Code

- [`IAssetTools`](../../../../Engine/Source/Editor/AssetTools/Public/AssetTools/IAssetTools.h)
- [`AssetTools` implementation](../../../../Engine/Source/Editor/AssetTools/Private/AssetTools/AssetTools.cpp)
- [`AssetTools` module descriptor](../../../../Engine/Source/Editor/AssetTools/AssetTools.dmodule)
- [Engine Asset public surface](../../../../Engine/Source/Runtime/Engine/Public/Asset)
- [Engine Asset operation capabilities](../../../../Engine/Source/Runtime/Engine/Public/Asset/AssetOperations.h)
- [`AssetTools` operation orchestration](../../../../Engine/Source/Editor/AssetTools/Private/AssetTools/AssetOperations.cpp)
- [`ContentBrowser` asset operations](../../../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserOperations.cpp)
- [`ContentBrowser` panel operations](../../../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.cpp)
