# Content Browser Data Source Alignment Plan

Summary: Align Content Browser item interfaces and ownership with UE data-source concepts while retaining existing discovery, mutation, and recovery implementations.

Last reviewed: 2026-11-10

Status: Archived
Completed: 2026-11-10

## Current Status

Stages 0-4 are complete. All selected native and documentation gates passed;
the two platform-permission skips are recorded below. The execution checkout started at
`1f877ca0f3b0d02193e86268e2ad8986fed64363` (the plan commit); the
proposal baseline `6cc14a7a0` is retained as historical context only.

Production call map: MainFrame constructs the public tool; the factory constructs
Panel with one `FContentBrowserHostContext`. Panel owns that context, DataSource,
and Model in lifetime order. Model borrows DataSource; synchronous tests explicitly
construct a source before their model. Panel commands use item inputs; clipboard
asset identity is converted to an item and virtual destinations to physical paths
at the Panel boundary. Save accepts selected items and deduplicates packages.
An item duplication overload with a physical destination is retained because
clipboard paste needs both the resulting identity and save-failure reveal.

Discovery remains serial per Model with a DataSource-private shared ItemDiscovery collector;
DataSource prepares the catalog and item/directory capture callables on the owner
thread, reusing the existing request inputs and snapshot result types. Model
only invokes or schedules that work. Tree workers own independent
collectors. Workers pin catalog, collector and cancellation token, never the
DataSource or Panel. Model retains scheduling and publication rights. Failed
preparation publishes no request. Shutdown closes source admission, releases its
deletion sessions, cancels Model publication, and drains worker bodies before
module unload. Synchronous calls use the same owned inputs without launching work.

Deletion display declarations move to `Operations/ContentDeletionPlan.h`; the
execution type moves to `Operations/ContentDeletionOperation.h` and remains
constructible only by DataSource. The concrete source lives in Panels, with
mutation implementations in `Operations/ContentBrowserItemOperations.cpp`.
Scoped host notification is the sole publisher when supplied; the legacy host
revision callback is a construction-time fallback. No notifier rebind is needed.

Workspace symbol audit found production consumers only in Engine ContentBrowser
and MainFrame; Sandbox and RoadWeaver have none. Behavior coverage resides in
ContentBrowser model/panel/refresh tests and EditorAssetWorkflowTests. Existing
filesystem fault, deletion hook, and async scan-gate seams remain; notifier setter
usage in tests migrates to construction, and standalone Models receive explicit
source lifetimes. No injectable high-level asset service is introduced.

Final validation (Windows MSVC x64, `Win64-Debug-DurinEditor`, 14 build jobs):

- `ContentBrowserWorkflowTests`: 156 passed, 2 environment skips out of 158.
  The two existing symbolic-link tests require Windows privileges unavailable
  on this machine; their test bodies remain enabled. Receipt:
  `Build/.agent-state/logs/20261110-173950-402469-46284-ContentBrowserWorkflowTests.log`.
- The tests cover the native inputs committed with this plan update on top of
  `1f877ca0f`; subsequent edits only finalize documentation. Added coverage checks
  prepared work after source destruction, shutdown with an admitted
  capture, rejected preparation, silent folder validation and destination races,
  exactly one scoped publisher, and shared-package item save deduplication.
- Four pre-existing deletion fixtures assumed each saved package survived the
  previous `UnloadPackage` call. That operation runs GC across all unreferenced
  objects and returns NotFound for an already collected package. Fixtures now
  unload only resident packages and assert every intended package is absent;
  deletion/reference/companion assertions remain intact.
- `EditorAssetWorkflowTests`: 44/44 passed. Receipt:
  `Build/.agent-state/logs/20261110-174019-589324-41156-EditorAssetWorkflowTests.log`.
  Its declared dependencies include MainFrame and ContentBrowser. MainFrame and
  the affected host consumers compiled and linked in the initial consumer build
  (`20261110-173522-221862-44180-cmake.log`); the final incremental build receipt
  is `20261110-174018-073269-41156-cmake.log`. No duplicate standalone build was
  needed for the same target inputs.
- Changed-document validation, all-plan lifecycle validation and Git diff checks
  passed. Workspace source/test searches found no legacy operation-service,
  construction-services or post-construction notifier APIs. The implemented
  ownership and lifecycle contract is in the Content Browser architecture document.
- `test affected --explain` was rerun after the final native edits. Its Engine-wide
  expansion comes from the explicit test CMake source-list update. Only the
  ContentBrowser target's private source registration changed; the graph and
  other test registrations did not. Selected gates remain ContentBrowser,
  EditorAsset and MainFrame. Source/thumbnail implementations and renderer inputs
  are unchanged; unrelated Python, external-tool and GPU labels do not introduce
  additional consumers of the changed construction API.
- Initial sandbox compilation failed with the documented MSVC C1902 PDB service
  mismatch. The same DevTool command succeeds outside the sandbox; all subsequent
  native gates use that environment. No all-build or application-startup gate is
  required for this bounded private-module and construction-contract change.

### Post-Implementation Review (2026-11-10)

Review of `2a22f4f15` found two incomplete details and redundant migration scaffolding:

- Tree work could receive a null or stale item catalog. Refresh the owned catalog
  at tree admission in both synchronous and asynchronous modes. A regression
  covers tree-only browsing and assets created after the last item capture.
- Ordinary Save eligibility omitted current mount writability/loading checks,
  and canonical-resave eligibility had no production caller. Match the native
  admission predicates and use the canonical query in the Resave menu; retain
  native execution revalidation and partial-save behavior.
- Move the serial collector fully under DataSource ownership. Remove the public
  collector factory, collector parameters and the Model collector member. Keep
  the two concrete owned work callables: they enforce the required worker lifetime
  boundary without introducing another request model or scheduler.
- Remove the deletion-plan test wrapper (including its unused selection set),
  deletion-driver adapter, 21 unused Model setups, and obsolete Panel callback
  aliases. Construct the reimport test callback through the real host context.
  Keep fault hooks, byte-identity, partial-failure and asynchronous lifetime tests.
- Strengthen the existing cross-panel clipboard test with the real Transactor,
  shared construction context, one-notification assertions and observer-panel
  refresh, instead of adding another mock service or duplicate test fixture.

Follow-up validation uses the same Windows MSVC Debug profile. The affected
selection is ContentBrowser, EditorAsset, PythonScriptPanel, SceneImport,
StaticMeshThumbnail and Thumbnail by module labels. This change touches only the
private browser boundary, menu eligibility and its tests; other labeled feature
implementations and renderer inputs are unchanged. ContentBrowser and EditorAsset
(with its MainFrame dependency) remain the bounded gates. Follow-up validation on top of `2a22f4f15` covers the native changes in this
review commit:

- ContentBrowser: 158 passed, 2 existing Windows symlink-permission skips out of
  160; `Build/.agent-state/logs/20261110-175230-085131-20352-ContentBrowserWorkflowTests.log`.
- EditorAsset: 44 passed; `Build/.agent-state/logs/20261110-175053-885697-24560-EditorAssetWorkflowTests.log`.
  MainFrame compiled and deployed in the accompanying consumer build. Later edits
  only strengthened ContentBrowser's test source and normalized whitespace/documentation, so
  the EditorAsset/MainFrame receipt remains applicable without a duplicate run.
- Changed-document validation, all-plan validation, workspace obsolete-symbol
  searches and diff checks passed. These review receipts supersede the initial
  implementation receipts above for the final relevant native inputs.

## Goal

Provide one concrete `FContentBrowserDataSource` through which the browser
obtains content and invokes item capabilities and operations. Keep discovery
work, UI publication, native asset operations, and deletion execution separate
behind that boundary. Remove the old public-to-the-Panel operation-service
entrypoint when migration completes.

Required local references:

- [Content Browser contract](../../../Editor/Architecture/Assets/ContentBrowser.md)
- [Asset thumbnails](../../../Editor/Architecture/Assets/AssetThumbnails.md)
- [Asset mutation](../../../Runtime/Assets/Packages/AssetCatalogAndMutation.md)
- [Build workflow](../../../Agents/BuildAndRun.md) and [test workflow](../../../Agents/Testing.md)

## UE Reference And Local Decisions

UE exposes item capabilities, individual operations, and bulk operations on
[UContentBrowserDataSource](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/ContentBrowserData/UContentBrowserDataSource).
Its [temporary item context](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ContentBrowserData/FContentBrowserItemDataTemporary-)
separates name validation and finalization, while
[FContentBrowserItemData](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/ContentBrowserData/FContentBrowserItemData)
separates item identity from data-source-owned details.
[FAssetDeleteModel](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FAssetDeleteModel)
provides a reference for treating deletion as a distinct workflow.

These are interface and responsibility references, not claims of equivalent
threading, persistence, or deletion guarantees. The local design is:

| Concern | Selected local design |
| --- | --- |
| Data-source boundary | One concrete, private-module `FContentBrowserDataSource`; no interface hierarchy or source registry |
| Item identity | Retain `FContentBrowserItem`, exact asset identity, owning package, physical/virtual paths, and display metadata |
| Item operations | Item-oriented capability and execution methods, including bulk entrypoints for current callers |
| Discovery | Existing collector becomes `FContentBrowserItemDiscovery`; only owned inputs enter workers |
| Temporary creation | Folder-specific context with validation and finalization; no generic temporary-item protocol |
| Deletion | Separate confirmation and execution types, retaining current byte and recovery guarantees |
| Host integration | Existing callback/context aggregate becomes `FContentBrowserHostContext`, installed during construction |
| Thumbnails | Existing thumbnail references and manager retain admission, fingerprints, and pool ownership |

Do not introduce UObject ownership, a new editor subsystem, a generic payload
hierarchy, universal operation-result objects, or a second notification bus.
Do not split assets and ordinary files into registered providers: mixed operations
still need one coordinator and already have working implementations.

## Target Structure And Ownership

```text
Panel owns HostContext, DataSource, Model, and thumbnail references
  Panel -> DataSource: capability queries, item operations, confirmation workflows
  Model -> DataSource: prepare discovery inputs and capture work
    DataSource -> ItemDiscovery: owned catalog + directory + cancellation token
    DataSource -> existing AssetTools / filesystem commit implementations
    DataSource -> deletion session -> ContentDeletionOperation
  Model: schedules captures, rejects stale results, publishes snapshots
  RefreshCoordinator: reconciles committed changes and refreshes views
```

- Panel owns one data-source instance per browser. Model receives a non-owning
  reference; declaration and destruction order ensure DataSource outlives Model.
- Model retains navigation, search, selection projection, tree caches, capture
  request tracking, cancellation, and publication-version allocation.
- DataSource owns content-facing policy and the existing mutation/session state.
  Its owner-thread methods prepare discovery work from the shared asset query.
- Capture work owns its immutable catalog and required collector lifetime.
  Workers never capture Panel, DataSource, host callbacks, or live registry
  pointers. Preserve serial access to the item collector and independent tree
  captures; do not add a scheduler or move Model publication into DataSource.
- Existing operation implementations may remain in separate `.cpp` files or
  private helpers. Do not retain a complete public `OperationService` API behind
  an equally complete forwarding API indefinitely.
- Shutdown stops admission, cancels publication rights, releases confirmation
  sessions and thumbnail references, and permits already admitted capture work
  to finish against owned inputs without touching destroyed UI state.

Suggested file responsibilities, to be finalized during Stage 0:

| Type/file | Responsibility |
| --- | --- |
| `ContentBrowserDataSource.h/.cpp` | Owner-thread content interface, host policy, discovery preparation, operation admission |
| `ContentBrowserItemDiscovery.h/.cpp` | Current filesystem/catalog collection and directory index |
| `ContentBrowserItemOperations.cpp` | Implement data-source item operations using existing commit paths |
| `ContentDeletionPlan.h` | Confirmation scope, summaries, warnings, blockers, file identities |
| `ContentDeletionOperation.h/.cpp` | Existing destructive validation and execution |
| `ContentBrowserHostContext` in public contracts | Host callbacks, shared reconciliation state, weak editor ownership |

Only construction context crosses the public module boundary. Concrete
data-source, discovery, and deletion implementation types remain private.

## Main Interface Contract

The following is a target API sketch, not a compatibility promise or an instruction
to create unused overloads. Final signatures must be reconciled with production
call sites in Stage 0.

```cpp
class FContentBrowserDataSource
{
public:
    // Individual eligibility; optional reason, no mutation or diagnostics.
    bool CanSaveItem(const FContentBrowserItem&, EAssetSaveMode, std::string* Reason) const;
    bool CanDuplicateItem(const FContentBrowserItem&, std::string* Reason) const;
    bool CanRenameItem(const FContentBrowserItem&, std::string_view Name, std::string* Reason) const;
    bool CanMoveItem(const FContentBrowserItem&, std::string_view DestinationDirectory, std::string* Reason) const;

    bool RenameItem(const FContentBrowserItem&, std::string_view Name, FContentBrowserItem& OutItem);
    bool DuplicateItem(const FContentBrowserItem&, FContentBrowserItem& OutItem);
    bool BulkSaveItems(std::span<const FContentBrowserItem>, EAssetSaveMode);
    bool BulkMoveItems(std::span<const FContentBrowserItem>, std::string_view DestinationDirectory);
    bool BulkCopyItems(std::span<const FContentBrowserItem>, std::string_view DestinationDirectory);

    // Names illustrate the existing folder and deletion workflows.
    bool PrepareFolder(std::string_view Directory, FContentBrowserItem& OutTemporaryItem);
    bool CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* Reason) const;
    bool CreateFolder(std::string_view Directory, std::string_view Name, FContentBrowserItem& OutItem);
    FContentDeletionPlanPtr PrepareDeleteItems(std::span<const FContentBrowserItem>);
    bool DeleteItems(FContentDeletionPlanPtr& Confirmation);
    void DismissDeletion(FContentDeletionPlanPtr);
    void StopRequestAdmission();
};
```

Discovery preparation is intentionally specified by ownership above rather than
inventing a general task/context API here. Reuse the existing request, snapshot,
and cancellation types when wiring Model to DataSource.

### Item And Path Rules

- Browser-facing save, duplicate, move, and rename methods accept items. Convert
  to package or asset paths internally; deduplicate package saves when several
  selected assets share a package.
- Copy-to-directory preserves current clipboard semantics and distinct asset
  duplication behavior. Keep package/path helpers private unless a real external
  caller requires them. Avoid adding unused single-item/bulk method pairs.
- Destination parameters explicitly identify physical directories. Virtual-path
  callers convert through the existing mount/path service at their boundary;
  do not infer path kind from ambiguous strings.
- Project-wide and folder-scoped Fix Up remain explicit maintenance operations.
  They need not be disguised as single-item operations for naming symmetry.
- Capability checks share validation with execution but reserve nothing.
  Execution rereads host policy, mounts, source identity, names, and destinations.
  Preserve existing partial-commit and recovery diagnostics.

### Temporary Creation And Deletion

- Panel owns folder editing state and navigation revision. DataSource owns name
  and target validation and creation. Validate edited names without emitting
  repeated global diagnostics; finalization rechecks immediately before writing.
- Cancellation, navigation, and shutdown create no directory. Failed finalization
  retains the input; successful finalization reveals the real item.
- Deletion preparation remains the authoritative analysis. Do not implement a
  full extra scan merely to provide a UE-shaped `CanDeleteItem` method. A cheap
  capability query may be added only for an actual UI need and cannot authorize
  deletion.
- Retain immutable confirmation scope, source byte identity, directory membership,
  reference and companion ownership checks, mount/reparse boundaries, and current
  stale-versus-retry behavior. Preserve accurate reconciliation after partial
  deletion and the rule that physical completion is distinct from registry refresh.
- Keep execution capabilities private to the owning data source/session. Moving
  deletion declarations out of the operation header must not expose new setters
  or let callers construct an executable confirmation by editing display data.

### Host Context And Notifications

- Rename and reuse `FConstructionServices` as `FContentBrowserHostContext`.
  Preserve real host callbacks; do not reintroduce injectable high-level asset
  services. Settings and persistence callbacks remain distinct construction inputs.
- Pass the context once through the factory. Remove post-construction
  `SetContentChangeServices` and direct `AssetEditors` assignment when all callers
  have migrated. An update notifier setter should survive only if a real production
  rebind remains necessary.
- Reuse `FContentChangeBatch` for committed changes and existing feedback for
  diagnostics. Exactly one owner publishes each operation's observed changes;
  Panel and DataSource must not both publish the same batch.
- Preserve cross-browser reconciliation, failed-revision retry suppression,
  renderer lifetime, and host shutdown behavior.

## Implementation Stages

### Stage 0: Confirm Interfaces And Ownership

Dependency: this plan and the baseline source.

- [x] Audit affected symbols across Engine, Sandbox, and RoadWeaver source/tests.
- [x] Map every production caller to the target interface; settle discovery-work
  preparation and host-context ownership without adding a parallel request model.
- [x] Record the lifetime sequence for synchronous tests, asynchronous panels,
  failed preparation, and shutdown; settle file placement and exact method names.
- [x] Inventory tests that assert behavior versus old injectable or projection APIs.

Completion: the call map and final interface decisions are recorded here, with
no unresolved ownership or duplicate-notification responsibility.

### Stage 1: Separate Discovery And Deletion Declarations

Dependency: Stage 0.

- [x] Rename the current collector to `FContentBrowserItemDiscovery` and migrate
  capture ownership without changing execution or publication ordering.
- [x] Extract deletion confirmation and execution headers; remove reverse includes
  from deletion execution to the large operation-service header.
- [x] Keep native operations and deletion guarantees unchanged.

Completion: affected consumers compile; discovery, stale-result rejection,
snapshot publication, and deletion regression tests pass. No new runtime layer
is required merely to move declarations.

### Stage 2: Establish The Item Data Source

Dependency: Stage 1.

- [x] Establish the concrete data source and connect Model discovery preparation.
- [x] Move item capability and mutation admission into this boundary, reusing
  current move/copy/save/Fix Up commit implementations.
- [x] Migrate Panel and tests to item-oriented inputs and necessary bulk methods.
- [x] Retire `FContentBrowserOperationService` and redundant public path overloads;
  remove temporary adapters by the end of this stage.

Completion: a production caller cannot bypass the data-source boundary to invoke
the former operation service. Mixed moves, rename, save deduplication, copy,
read-only checks, and recovery tests pass.

### Stage 3: Unify Host And Confirmation Lifecycles

Dependency: Stage 2.

- [x] Install `FContentBrowserHostContext` once at construction and remove obsolete
  post-construction setters and direct member installation.
- [x] Route temporary-folder validation/finalization through DataSource and retain
  Panel-owned edit state, cancellation, and reveal behavior.
- [x] Put deletion sessions under DataSource ownership; preserve confirmation
  replacement, cancellation, retry, and partial-completion behavior.
- [x] Verify one committed-change publisher and safe shutdown with pending work.

Completion: multi-panel notifications, temporary creation, deletion sessions,
host shutdown, and asynchronous lifetime tests pass without new test-only service
injection. Update the implemented Content Browser contract in the same stage.

### Stage 4: Final Consumer Validation And Cleanup

Dependency: Stages 1-3.

- [x] Search all workspace projects for old symbols, redundant adapters, obsolete
  includes, and tests coupled only to removed implementation details.
- [x] Validate `ContentBrowserWorkflowTests` and `EditorAssetWorkflowTests` against
  the final relevant inputs. Include thumbnail targets if their inputs change.
- [x] Build affected host consumers, including `MainFrame`, because the public
  construction contract changes; select exact targets using the build workflow.
- [x] Recheck affected-test analysis and cover additional discovered consumers.
- [x] Record tested revision, selections, configuration, skips, and applicable
  reused receipts; validate documentation and close only satisfied checklists.

Completion: no legacy operation entrypoint or duplicate construction path remains;
all selected gates pass and long-lived behavior is documented in its owning
architecture document. Mark the plan completed only after implementation and
these acceptance gates, not when this proposal is committed.

## Validation Boundaries

Follow the linked workflows for target selection and result reuse. This is an
editor-private restructuring with one public host-construction contract change;
bounded ContentBrowser and host-consumer coverage is the initial gate. An `all`
build is required only if implementation exposes a broader ABI/build-graph risk
that bounded builds cannot cover; record that reason before expanding scope.
No GPU qualification or full application startup gate is added by this plan.

Keep meaningful fault and recovery tests. Do not remove required protection
coverage merely because an old test seam disappears; migrate it to real
filesystem/asset behavior where practical, retaining a necessary narrow seam
when deterministic recovery coverage otherwise cannot be preserved.
