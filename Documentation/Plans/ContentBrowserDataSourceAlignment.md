# Content Browser Data Source Alignment Plan

Summary: Align Content Browser item interfaces and ownership with UE data-source concepts while retaining existing discovery, mutation, and recovery implementations.

Last reviewed: 2026-11-10

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. The baseline is commit
`6cc14a7a0`. Previous cleanup already unified move submission, removed high-level
asset-service injection, deferred folder creation until confirmation, moved
thumbnail request data out of items, removed operation-owned mount snapshots,
and removed redundant deletion-root metadata and capture version counters.

The selected direction is structural alignment, not a Service-to-Operations
rename alone. This plan does not authorize changing deletion semantics or
introducing a general multi-provider framework.

## Goal

Provide one concrete `FContentBrowserDataSource` through which the browser
obtains content and invokes item capabilities and operations. Keep discovery
work, UI publication, native asset operations, and deletion execution separate
behind that boundary. Remove the old public-to-the-Panel operation-service
entrypoint when migration completes.

Required local references:

- [Content Browser contract](../Editor/Architecture/Assets/ContentBrowser.md)
- [Asset thumbnails](../Editor/Architecture/Assets/AssetThumbnails.md)
- [Asset mutation](../Runtime/Assets/Packages/AssetCatalogAndMutation.md)
- [Build workflow](../Agents/BuildAndRun.md) and [test workflow](../Agents/Testing.md)

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

- [ ] Audit affected symbols across Engine, Sandbox, and RoadWeaver source/tests.
- [ ] Map every production caller to the target interface; settle discovery-work
  preparation and host-context ownership without adding a parallel request model.
- [ ] Record the lifetime sequence for synchronous tests, asynchronous panels,
  failed preparation, and shutdown; settle file placement and exact method names.
- [ ] Inventory tests that assert behavior versus old injectable or projection APIs.

Completion: the call map and final interface decisions are recorded here, with
no unresolved ownership or duplicate-notification responsibility.

### Stage 1: Separate Discovery And Deletion Declarations

Dependency: Stage 0.

- [ ] Rename the current collector to `FContentBrowserItemDiscovery` and migrate
  capture ownership without changing execution or publication ordering.
- [ ] Extract deletion confirmation and execution headers; remove reverse includes
  from deletion execution to the large operation-service header.
- [ ] Keep native operations and deletion guarantees unchanged.

Completion: affected consumers compile; discovery, stale-result rejection,
snapshot publication, and deletion regression tests pass. No new runtime layer
is required merely to move declarations.

### Stage 2: Establish The Item Data Source

Dependency: Stage 1.

- [ ] Establish the concrete data source and connect Model discovery preparation.
- [ ] Move item capability and mutation admission into this boundary, reusing
  current move/copy/save/Fix Up commit implementations.
- [ ] Migrate Panel and tests to item-oriented inputs and necessary bulk methods.
- [ ] Retire `FContentBrowserOperationService` and redundant public path overloads;
  remove temporary adapters by the end of this stage.

Completion: a production caller cannot bypass the data-source boundary to invoke
the former operation service. Mixed moves, rename, save deduplication, copy,
read-only checks, and recovery tests pass.

### Stage 3: Unify Host And Confirmation Lifecycles

Dependency: Stage 2.

- [ ] Install `FContentBrowserHostContext` once at construction and remove obsolete
  post-construction setters and direct member installation.
- [ ] Route temporary-folder validation/finalization through DataSource and retain
  Panel-owned edit state, cancellation, and reveal behavior.
- [ ] Put deletion sessions under DataSource ownership; preserve confirmation
  replacement, cancellation, retry, and partial-completion behavior.
- [ ] Verify one committed-change publisher and safe shutdown with pending work.

Completion: multi-panel notifications, temporary creation, deletion sessions,
host shutdown, and asynchronous lifetime tests pass without new test-only service
injection. Update the implemented Content Browser contract in the same stage.

### Stage 4: Final Consumer Validation And Cleanup

Dependency: Stages 1-3.

- [ ] Search all workspace projects for old symbols, redundant adapters, obsolete
  includes, and tests coupled only to removed implementation details.
- [ ] Validate `ContentBrowserWorkflowTests` and `EditorAssetWorkflowTests` against
  the final relevant inputs. Include thumbnail targets if their inputs change.
- [ ] Build affected host consumers, including `MainFrame`, because the public
  construction contract changes; select exact targets using the build workflow.
- [ ] Recheck affected-test analysis and cover additional discovered consumers.
- [ ] Record tested revision, selections, configuration, skips, and applicable
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
