# Content Browser UE Architecture Convergence Plan

Summary: Align Content Browser interfaces and implementation with UE provider ownership, item data, queries, operations, service updates, and module boundaries.

Last reviewed: 2026-11-12

Status: Active
Completed:

## Current Status

Planning baseline: `fb0f3d9dd`. Stage 0 is complete against Durin
`9c22dfbe2e9ca154fa157cf9d30ef5d4cfc4d312`: the verified UE reference, migration
destinations, consumer inventory, identity and operation semantics are recorded
below. Stage 1 is next; production implementation has not started. Stages 1–5 and
the final integration gates remain open.
The baseline already routes panels through the data subsystem and providers and
separates native item policy from command execution lifetime. Its 185 passing
workflow/provider cases and two Windows symlink-permission skips are historical
baseline evidence, not acceptance of this refactor.

The earlier [editor alignment plan](EditorResponsibilityAndUEInterfaceAlignment.md)
owns its existing toolkit work and final integrated gates. Its completed browser
stages establish the migration baseline; this plan owns the deeper browser changes
below. Do not reopen or reinterpret their historical acceptance as evidence that
these new boundaries already exist. Coordinate shared-file changes and reuse
applicable results without waiving either plan's explicit remaining gates.

## Goal

Align both public interfaces and implementation as closely as practical with a
verified UE source version. Match names, signatures, item/payload representation,
provider responsibilities, query/filter flow, operation dispatch, update delivery
and module direction. Map UE UObject naming to Durin's DObject naming conventions.
An API-shaped wrapper over the existing combined implementation does not satisfy
the goal.

A provider with no physical storage must participate in navigation, filtering,
selection, attributes and ordinary supported commands by implementing the provider
contract alone, without edits to the generic item, view or subsystem.

## Reference And Decision Policy

Stage 0 must locate and verify an accessible UE source tree, record its version,
changelist and branch, and inspect the relevant declarations and implementations.
The earlier plan names UE 5.8.0 CL 53629095 as its reference; verify availability
and identity rather than assuming that an installation directory is sufficient.
Official API documentation supports discovery but cannot establish private
implementation parity. If matching source is unavailable, record the missing
reference and leave source-dependent decisions unqualified.

Inspect UE's `ContentBrowserData` subsystem/source/item/filter code and the
`ContentBrowserAssetDataSource` and `ContentBrowserFileDataSource` implementations,
including payloads, utility functions, registration, enumeration, asset-registry
or filesystem notifications, and bulk operations. Relevant official entrypoints:

- [Data subsystem](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ContentBrowserData/UContentBrowserDataSubsystem)
- [Data source](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ContentBrowserData/UContentBrowserDataSource)
- [Asset data source module](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/ContentBrowserAssetDataSource)
- [Compiled filters](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ContentBrowserData/FContentBrowserDataCompiledFilte-)

For each affected interface and implementation responsibility, record the UE
symbol/file, current Durin symbol, selected destination and deletion condition for
the old code. Prefer the UE design whenever the underlying responsibility matches.
Every retained difference needs a concrete Durin constraint, observable semantics,
and validation. Do not invent a shared base class, coordinator, cache, scheduler or
compatibility interface when the verified UE structure already resolves the need.

## Selected Architecture And Preserved Behavior

- Providers own discovery, compiled filters, native payload interpretation and
  operation capabilities. Generic views consume virtual paths and provider items.
- ItemData describes one provider contribution; Item aggregates contributions and
  forwards operations. Provider-specific package/file metadata lives in payloads
  and is exposed through UE-aligned attributes and accessors where applicable.
- The generic subsystem owns source membership, logical aggregation, query routing
  and update publication. It must not depend on concrete Asset/File source classes.
- Asset and File providers independently own their native behavior. Share narrowly
  scoped mechanisms where justified by the reference and Durin's storage contract;
  remove the combined item-kind execution facade after consumer migration.
- Editor lifecycle drives data updates independently of open browser views.
  Providers publish committed changes; views retain selection, navigation, search,
  presentation settings and cancellation of their own presentation work.
- Target module direction: ContentBrowser UI depends on ContentBrowserData;
  ContentBrowserAssetDataSource and ContentBrowserFileDataSource depend on the
  data contracts and their native services. Generic data contracts do not depend
  on UI, concrete providers, thumbnails or rendering. Freeze precise manifests,
  bootstrap ownership and shared native utility placement against UE in Stage 0.
- Preserve activation/mount invalidation, owner-thread reflected dispatch,
  immutable asynchronous captures, shutdown fencing, mutation revalidation,
  save-mode semantics, redirectors, companion ownership, truthful failure/recovery reporting,
  confirmed deletion fingerprints and committed-effect notifications. Fit these
  into the selected UE responsibilities. Mixed-directory rollback and partial-success
  semantics may change as explicitly selected below; document those changes.
- Mixed package/file folder operations currently share atomic native execution.
  Preserving that behavior is not a requirement: the user explicitly permits
  changing mixed-directory execution, rollback and partial-success semantics to
  converge with UE. Inspect UE's folder expansion, source partitioning, execution
  order, stop/continue rules, failure reporting and rollback/compensation before
  selecting a design. Do not assume UE guarantees cross-source atomicity or that
  all bulk commands have identical failure behavior.
- Stage 0 must select semantics per move, copy, rename and delete operation,
  including destinations already present, package/ordinary-file/companion conflicts,
  cancellation and failures after some writes commit. Record before/after examples,
  which effects remain committed, which are reverted, which items remain unattempted,
  and how retry and UI reconciliation work. Make required feedback/result API changes
  explicit; a single success boolean must not conceal partial completion.
- Follow the verified UE behavior where practical. Retain narrow local rollback
  for native write integrity where needed, but do not build or preserve a global
  transaction solely to emulate the previous mixed-directory contract. Never
  delete committed content as speculative compensation when ownership is uncertain.
  Remove obsolete whole-batch rollback assumptions and tests when the selected
  semantics replace them, adding partial-success and recovery coverage instead.
- Keep create/import/details contributions and asset definitions independent of
  source registration. Move host policy, feedback and editing callbacks to their
  appropriate service/operation boundaries; generic enumeration must not require
  the full browser presentation host context.

Existing implemented contracts remain authoritative until each migration lands:
[Content Browser](../Editor/Architecture/Assets/ContentBrowser.md) and
[data providers](../Editor/Architecture/Assets/ContentBrowserData.md).
No asset format, package serialization, renderer or unrelated toolkit redesign is
included. No production remote backend is required to prove provider independence.

## Initial Reference Inspection (2026-11-12)

Reference identity was read from
`E:/Programs/Epic Games/UE_5.8/Engine/Build/Build.version`: UE 5.8.0,
CL 53629095, branch `++UE5+Release-5.8`, promoted non-licensee build.
Unlike an installation-directory check, inspection confirmed readable private
implementation files for the subsystem, item, Asset source and File source.
All UE paths below are relative to that installation's `Engine/` directory.
This is initial inspection evidence, not completion of the migration map.

| Observed UE source | Verified responsibility | Durin migration site and removal gate |
| --- | --- | --- |
| `Source/Editor/ContentBrowserData/Public/ContentBrowserDataSource.h`, `CompileFilter`, `EnumerateItemsMatchingFilter`, `RenameItem` | Enumeration emits provider ItemData; rename returns ItemData. Query signatures do not require a browser host. | `ContentBrowserDataSource.h` currently enumerates aggregate Item and takes Host. Migrate provider/query consumers before removing these signatures in Stages 1–3. |
| `Source/Editor/ContentBrowserData/Public/ContentBrowserItemData.h`, `IContentBrowserItemDataPayload`, `GetOwnerDataSource`, `GetPayload` | Concrete ItemData stores a weak source and source-specific payload. | `ContentBrowserItemData.h` and `ContentBrowserItem.h`; remove public package, physical-file and redirector metadata only after details, thumbnails and transfer consumers migrate in Stage 2. |
| `Source/Editor/ContentBrowserData/Private/ContentBrowserItem.cpp`, `FContentBrowserItemKey::Initialize` | Key uses item type and virtual path; file keys additionally use source. Folder keys omit source. Physical storage is not generic identity. | Replace `FContentBrowserItem::StableId()` and physical-path folder merging in `ContentBrowserDataSubsystem.cpp` in Stage 2; preserve activation/mount fencing as an explicit Durin constraint. |
| Same file, `FContentBrowserItem::Rename` | Iterates source contributions and appends successful returned ItemData. A valid resulting aggregate reports success. | Stage 3 must expose partial contribution outcomes before adopting this dispatch; existing bool callers cannot establish complete success. |
| `Plugins/Editor/ContentBrowser/ContentBrowserAssetDataSource/Source/ContentBrowserAssetDataSource/Private/ContentBrowserAssetDataSource.cpp`, `BulkCopyItems`, `BulkMoveItems` | Validates virtual-to-internal destination and known content root, then delegates to `ContentBrowserAssetData` utilities. | Split `DContentBrowserNativeDataSource` business behavior into Asset source and native utilities in Stage 3; utility failure/cancellation inspection remains outstanding. |
| `Plugins/Editor/ContentBrowser/ContentBrowserFileDataSource/Source/ContentBrowserFileDataSource/Private/ContentBrowserFileDataSource.cpp`, `BulkCopyItems`, `BulkMoveItems` | Independently resolves destination mount and delegates to File utilities. | File provider owns filesystem operations; remove concrete native casts and batch routing in subsystem after source transfer contracts migrate. |
| Same plugin's `Private/ContentBrowserFileDataCore.cpp`, `CopyItems`, `CopyFileItems`, `MoveItems`, `MoveFileItems` | Folder and file groups both execute; results are ORed. File copy uses no replacement and continues after individual failure. Move copies first, then deletes successfully copied sources through source control with filesystem fallback. | Global mixed-source rollback cannot be inferred from UE bools. Stage 3 requires committed/failed/unattempted reporting and recovery tests; full folder migration, cancellation and failure semantics remain unqualified pending utility inspection. |
| `Source/Editor/ContentBrowserData/Private/ContentBrowserDataSubsystem.cpp`, source membership and ticker setup | Tracks available/active sources separately and installs service ticking independently of browser queries. | Remove built-in construction and browser tick coordination in Stage 4; exact Durin lifecycle entrypoint and shutdown ordering remain to be frozen. |

Consumer discovery used every source root and `Tests/Native` root declared by
the three projects in `Durin.dworkspace`. Searches for `ContentBrowser`, item,
provider, native-filter, request-context and browser-frame symbols found:

- Engine ContentBrowser: public contracts, Data, Panels, Operations and thumbnail
  references own the principal migrations. `ContentBrowserItemDiscovery.cpp`
  still traverses native directories; `ContentBrowserDataQuery.cpp` still builds
  `FNativeContentBrowserFilter`. Stage 1 must remove both transitional paths.
- Engine MainFrame owns service/bootstrap integration. LevelEditor,
  AssetForgeBuiltins, MaterialEditor, StaticMeshEditor and TextureEditor consume
  browser host/extension contracts. Preserve contribution registration separately
  from source registration. DurinHeaderTool also references ContentBrowser and
  must be included in the module/reflection audit.
- Engine tests include Data, Model, ItemView, PanelExtension,
  ExtensionRegistry, PythonScriptPanel and StaticMeshThumbnailRenderer consumers.
- RoadWeaverEditor references browser contracts; no matching source/test symbols
  were found in Sandbox or RoadWeaver runtime/tests. This does not waive their
  final module-graph build gate.

Registry discovery: `.\DevTool.bat test list ContentBrowser` succeeded for
`Win64-Debug-DurinEditor`, returning `ContentBrowserDataTests`,
`ContentBrowserWorkflowTests`, `StaticMeshThumbnailTests` and `ThumbnailTests`.
Baseline execution on 2026-11-12 at the revision above, with unchanged production
inputs and the same preset/profile: `test ContentBrowserDataTests` passed 22/22;
`test ContentBrowserWorkflowTests` passed 163, with two Windows symlink-privilege
skips (165 registered cases). Both incremental target builds passed. Receipts:
`Build/.agent-state/logs/20261112-163548-425803-6416-ContentBrowserDataTests.log`
and `Build/.agent-state/logs/20261112-163557-711445-34584-ContentBrowserWorkflowTests.log`.
These establish the migration baseline, not acceptance of unimplemented stages.

## Selected Migration Map

Further source inspection confirmed the following destinations. These are selected
Durin changes, not claims that the current implementation already satisfies them.

| Responsibility | UE reference (relative to Engine) | Selected Durin contract and old-path removal |
| --- | --- | --- |
| Provider filters | `Source/Editor/ContentBrowserData/Public/ContentBrowserDataFilter.h`, `FContentBrowserDataCompiledFilter::CompiledFilters`; Asset source `CompileFilter`, File source `CompileFilter` | Keep general path/type/category/recursion criteria in the data module; store immutable typed compiled data per source, with source activation, mount and catalog revisions. Asset source owns registry capture/index; File source owns directory discovery/index. Delete `FNativeContentBrowserFilter` and snapshot injection after both list/tree queries use source enumeration (Stage 1). |
| Discovery publication | File source `Tick`, `BackgroundDiscovery->GetAndTrimSearchResults`; Asset source `CompileFilter` and `FAssetDataSourceFilterCache` path subscriptions | Worker captures own values and cancellation only. Sources publish owner-thread results after checking editor/project/source/mount identity. View cancellation cancels its presentation request, never another view's service discovery. Retire panel-native collectors and query-owned catalog caching in Stage 1. |
| Payload and attributes | Asset plugin `Public/ContentBrowserAssetDataPayload.h`, folder/file payloads; File plugin `Public/ContentBrowserFileDataPayload.h`; data source `GetItemAttribute`, `GetItemAttributes`, `GetItemPhysicalPath` | Separate Asset folder/file and File folder/file payloads. ItemData holds owner, flags, virtual path, name/display name and payload; Item aggregates ItemData and forwards accessors. Attribute values carry typed scalar values and optional metadata. Physical path is optional, queried through source. Move all current Item native fields into payloads and delete native-kind presentation dispatch after attribute/thumbnail/details migration (Stage 2). |
| Temporary creation | `Source/Editor/ContentBrowserData/Public/ContentBrowserItemData.h`, `FContentBrowserItemDataTemporaryContext` | Store ItemData rather than aggregate Item in the source context; validate and finalize through source-owned callbacks returning ItemData. Aggregate only in Item/UI. Preserve single finalization and activation/mount revalidation. Remove current aggregate context callbacks in Stage 3. |
| Operation dispatch | Asset/File `Private/ContentBrowser*DataCore.cpp`; `Source/Developer/AssetTools/Private/AssetViewUtils.cpp`, `CopyFolders`, `MoveFolders`, `RenameFolder`, `DeleteFolders` | Item dispatches contributions; bulk commands partition by source. Asset source delegates package operations to AssetTools; File source owns ordinary filesystem operations. No concrete-source cast or generic item-kind executor. Remove `CanHandleNativeBatch`, `FContentBrowserRequestContext` and `DContentBrowserNativeDataSource` in Stage 3 after native callers migrate. |
| Committed updates | Asset source `Initialize`/`Shutdown`, registry add/remove/rename/update/path subscriptions and `QueueItemDataUpdate`; File source directory watcher registration, discovery and `Tick` | Asset provider subscribes to registry changes; File provider owns filesystem invalidation/reconciliation. Publish committed effects once, including partial completion. Subsystem aggregates updates and retains revision coverage for missed-frame reconciliation. Remove broad host enumeration dependencies and duplicate mutation publication in Stage 4. |
| Membership/bootstrap | `Source/Editor/ContentBrowserData/Private/ContentBrowserDataSubsystem.cpp`, modular-feature subscriptions; provider plugin module implementations | Data module registers its editor subsystem. Each provider module registers an editor-owned bootstrap subsystem that initializes/registers its source after the data service and shuts it down before service retirement. Register providers in editor project manifests, independently of opening a browser. Remove `Builtins` creation from generic data subsystem in Stage 4. |
| Service ticking | Same UE subsystem, core ticker and modal-loop subscription | Durin has no core ticker and its game tickables require a world tick. Add the UE-equivalent editor tickable contract in DurinEd, driven once by `DEditorEngine::Tick` even without worlds/views. Bind each data service to its owning editor, fence shutdown, and do not tick through MainFrame. Remove query `Tick` and `LastBrowserFrame` in Stage 4. Validate two editors and zero-browser operation. |

UE module manifests were inspected: `ContentBrowserData.Build.cs` and both
provider `*.Build.cs` files. UE's Asset plugin privately depends on ContentBrowser
for presentation contributions; Durin keeps those contributions in UI/feature
modules because the selected generic provider contract must work without UI.
This is a dependency adaptation, not an assertion that UE has no UI dependencies.

Stage 5 manifests are selected as follows: ContentBrowserData publicly depends
on Core/CoreDObject/DurinEd (editor subsystem ownership), with no concrete source,
Mona, RHI or RenderCore dependency. Each provider publicly depends on
ContentBrowserData and privately on its native services: AssetRegistry/AssetTools/
Engine for Asset; Core filesystem plus AssetRegistry/AssetTools companion-ownership
queries for File. ContentBrowser UI depends on ContentBrowserData and keeps its
presentation/rendering dependencies. Provider bootstrap modules are loaded by the
Engine editor manifest; Sandbox and RoadWeaver inherit the editor graph. No new
combined native module is selected. Existing package staging remains in AssetTools;
ordinary-file staging/recovery moves once into File provider utilities, and only
storage-neutral path/I/O mechanisms may be shared. The File provider's package
ownership query is required to hide/reject package companions as ordinary files;
regression gates cover orphaned, shared and conflicting companions.

### Identity Selection

Follow UE `FContentBrowserItemKey::Initialize`: logical folder key is type plus
normalized virtual path; logical file key also includes source identity. Two
sources at the same folder path merge regardless of physical backing. Files from
different sources remain distinct. Provider contribution keys additionally include
owner identity; captured handles retain activation generation and mount revision
to reject stale operations. Keys never contain physical or package paths.
Withdrawal removes only that source's contributions; reactivation returns new
handles without reviving old clipboard/drag capabilities. A move update carries
old and new virtual paths and migrates only the matching source contribution;
the old merged folder survives when another source remains. Clipboard/drag values
carry contribution keys and captured admission identity and resolve/revalidate at
execution. These rules apply equally to nonphysical sources. Stage 2 removes
`StableId()` only after selection, sorting, details, thumbnail, extension and
transfer consumers use these domains. Tests must cover overlapping folders,
same-path files, rename/move selection and stale transfer rejection.

### Selected Operation Semantics

Observed UE behavior is operation-specific: Asset copy/move partition folder and
file payloads, delegate to native tools, and OR their results. File copy/move
partition the same way; `MigrateDirectoryContents` copies supported files without
replacement, checks cancellation while traversing, deletes only successfully copied
sources on move, and retains earlier effects. File folder deletion currently only
removes empty directories. Asset deletion separately delegates folders to
`AssetViewUtils::DeleteFolders` and files to `ObjectTools::DeleteAssets`; confirmation,
loading and cancellation can return early. Asset folder rename moves assets and
checks whether the source folder became empty. These booleans do not establish a
uniform all-items success or a cross-provider transaction.

Durin selects explicit outcomes instead of reproducing ambiguous booleans.
Keep UE-shaped capability/execute entrypoints and ItemData outputs; add a bounded
operation result containing per-contribution/effect old/new identity, status
(committed, reverted, failed, unattempted), diagnostic and recovery location.
Completion status distinguishes complete, partial, failed and cancelled. Commands
and UI must consume this result; bool compatibility paths retire in Stage 3.
Process source partitions in stable source-name order and items in virtual-path
order. Reject unsupported transfers before writing. Preflight shared structural
errors (invalid mount, self/descendant move, duplicate destination ownership or a
package/ordinary-file/companion collision) before any writes. Revalidate policy
immediately before each native effect. Continue after independent I/O failures;
stop remaining work on cancellation, admission loss or uncertain recovery state.
Do not compensate earlier committed partitions. These ordering/reporting rules
are Durin adaptations for deterministic feedback and confirmed destructive I/O.

| Operation | Selected conflict, failure and cancellation behavior | Before/after and retry gate |
| --- | --- | --- |
| Copy | Existing directories merge; existing files/packages fail without replacement. Companion conflicts fail package-unit preflight. A failed local package/companion write rolls back only its owned staging unit. Cancel leaves earlier copies and marks remaining effects unattempted. | Copy `A/package` and `A/readme` into `B`; if readme already exists, package copy may commit while readme fails. Retry only failed/unattempted effects after resolving conflict; never duplicate an already committed package. |
| Move | Same destination rules. A package and its companions form one integrity unit. Ordinary file move commits only after destination publication and source removal; failed source removal reports retained destination/source and recovery details, never hides the duplicate or deletes uncertain ownership. | Move two files; first commits and second fails. First stays at destination, second stays at source unless reported recovery state says otherwise. Cancel preserves committed first move. Retry resolves current identities and excludes committed effects. |
| Rename | Dispatch each contributing source with new virtual name; reject destination ownership conflicts before writes. A later provider failure does not undo an earlier rename. Return successful new ItemData plus failed old contributions. | Rename a merged folder from `A` to `B`; Asset may move while File fails. Both logical folders remain with their actual contributors; selection follows successful contributions and exposes failure. Retry targets only remaining old contributors. |
| Delete | Preserve confirmed exact scope and fingerprint revalidation across all sources before destruction. Consume the capability once destruction begins. Delete provider-owned descendants, then remove empty folders; never remove another source's files or unknown directory contents. Continue independent failures; cancellation stops remaining effects without restoring committed deletions. | Asset deletion succeeds and ordinary-file removal fails: deleted assets remain deleted, file remains selected with diagnostic. Retry requires a fresh preview/fingerprint confirmation for surviving content. Stale preview replacement performs no destructive write. |

For every operation, publish only actual committed updates, including effects
committed before cancellation/failure; reconcile source/destination subtrees when
coverage is uncertain. Report local reverts separately from committed effects and
retain recovery paths. Replace whole mixed-directory rollback assertions with
per-operation failure-injection, conflict, cancellation, partial-success and retry
tests while retaining package/companion integrity tests. Save flags and redirector
semantics stay unchanged. No asset-format or serialization change is selected.

## Implementation Stages

### Stage 0: Verify UE reference and freeze the migration map

Outcome: executable interface and implementation mapping with bounded consumers.

- [x] Verify UE source identity and record the reference symbols/files described
  above. Distinguish observed source behavior from proposed Durin adaptations.
- [x] Inventory affected symbols across every source/test root declared by
  `Durin.dworkspace`, including MainFrame, feature editors, thumbnail consumers,
  extension contributions, Engine, Sandbox and RoadWeaver.
- [x] Freeze UE-aligned declarations and implementation ownership for ItemData,
  Item keys/attributes, filters, provider enumeration, operations, temporary
  contexts, updates, registration and editor-driven ticking.
- [x] Select logical aggregation and provider-contribution identity rules. Include
  overlapping folders, nonphysical items, rename/move, withdrawal/reactivation
  and clipboard/drag identities; physical paths must not define generic identity.
- [x] Resolve mixed-directory execution, partial-success and rollback semantics,
  deletion confirmation adaptation,
  async discovery/cache ownership, service bootstrap and module manifests.
  Record each necessary deviation with its reason and regression gates.
- [x] Query current test registrations and record applicable baseline receipts;
  identify the source symbols and transitional paths to remove in each stage.

Acceptance: every changed responsibility has a selected UE-backed destination,
consumer set and old-path removal condition. Remaining source-dependent decisions
block their implementation stages rather than being hidden behind fallback APIs.

### Stage 1: Move discovery and filtering into providers

Depends on Stage 0. Outcome: providers originate data queries and own native work.

- [ ] Move AssetRegistry captures to the Asset provider and filesystem discovery
  to the File provider, using the selected UE enumeration/filter/cache structure.
- [ ] Implement per-source compiled filter data with appropriate invalidation;
  preserve recursion, visibility, folder, asset and ordinary-file behavior.
- [ ] Migrate list/tree queries to virtual paths and provider enumeration. Remove
  panel-side native catalogs and completed-snapshot transport through
  `FNativeContentBrowserFilter`; retain only provider-neutral view request state.
- [ ] Preserve immutable worker captures and owner-thread publication with source,
  mount, project and cancellation checks. Avoid live reflected sources on workers.

Acceptance: normal list/tree browsing and search do not pre-scan native storage in
the view layer. A nonphysical fixture source participates through the same query
path. Validate async teardown, stale results, independent views and filter parity.

### Stage 2: Converge item data, payloads, attributes and identity

Depends on Stage 1. Outcome: generic items no longer encode native storage layouts.

- [ ] Implement the frozen UE-aligned ItemData/Item/key and attribute interfaces;
  move package, redirector and physical-file metadata into their owning payloads.
- [ ] Replace the item-kind-dependent `StableId()` scheme with selected logical
  and contribution keys; keep distinct identity domains explicit and documented.
- [ ] Migrate presentation, sorting, details, thumbnails, extensions, selections,
  temporary items, clipboard and drag/drop across all workspace consumers.
- [ ] Remove legacy public fields and adapters after migration. Do not retain a
  parallel native snapshot as a second authoritative generic item model.

Acceptance: generic selection and commands work without physical/package paths;
merged folders preserve contributor ownership. Validate key stability, metadata
parity, rename/move selection migration and stale transfer identities. Compile and
test affected extension and thumbnail consumers, not only provider fixtures.

### Stage 3: Separate provider operations and retire the combined executor

Depends on Stage 2. Outcome: native business capabilities belong to their sources.

- [ ] Align provider capability and execution signatures with the frozen UE map,
  including bulk operations, returned ItemData and temporary-context behavior.
- [ ] Move asset save/duplicate/redirector/editing behavior into the Asset provider
  and its selected utilities; move ordinary-file behavior into the File provider.
- [ ] Implement the selected UE-aligned mixed-folder execution and failure strategy
  using narrow native mechanisms. Preserve companion integrity and necessary local
  staging/recovery; report each committed, reverted, failed and unattempted effect.
  Update command results, notifications, selection and refresh for partial success.
- [ ] Move confirmation and recovery state to explicit operation lifetimes.
  Preserve stale-confirmation replacement, single-use deletion and diagnostics.
- [ ] Remove `FContentBrowserRequestContext` and the combined behavior of
  `DContentBrowserNativeDataSource` after their callers migrate. Keep only shared
  mechanisms justified in Stage 0, not renamed versions of the same broad facade.
- [ ] Replace concrete-provider special cases for native batch routing and folder
  aggregation with the selected source/transfer contracts; reject unsupported
  transfers before physical mutation.

Acceptance: no asset/file business dispatch remains in a generic executor or
subsystem. Validate mixed-directory full/partial success, stop/continue and rollback rules,
save flags, redirectors,
companion ownership, policy races, confirmed deletion, and third-party rejection.
No provider owns a duplicate copy of the old staging/recovery implementation.

### Stage 4: Converge service lifecycle, registration and update flow

Depends on Stage 3. Outcome: the data service operates independently of browser UI.

- [ ] Move service ticking to the appropriate editor lifecycle entrypoint; remove
  browser-driven service ticking and `LastBrowserFrame` coordination.
- [ ] Align provider registry/filesystem event subscriptions and update delivery
  with the reference. Publish real committed effects and reconcile missed coverage.
- [ ] Move built-in source creation/registration out of the generic subsystem;
  preserve per-editor ownership, available/active separation and shutdown ordering.
- [ ] Remove broad browser host requirements from provider query APIs; retain
  operation policy/feedback and external mutation notification only where needed.

Acceptance: mutations and provider membership reconcile with zero open browsers;
opening a later browser sees current data. Validate no double tick, two editors,
hidden views, withdrawal/reactivation, publication fences and project shutdown.

### Stage 5: Enforce module boundaries and close legacy paths

Depends on Stage 4. Outcome: build dependencies enforce the selected responsibilities.

- [ ] Split the selected data, Asset provider, File provider and UI modules;
  migrate exports, reflection, manifests, loading/registration and test ownership.
- [ ] Migrate all Engine/Sandbox/RoadWeaver consumers. Remove private-include
  injections and UI/concrete-provider dependencies from generic data contracts.
- [ ] Audit all workspace source/test roots for removed symbols, concrete-source
  casts in the subsystem, panel-native discovery and old physical-path dispatch.
  Remove transitional adapters, duplicate notifications and unused old fixtures.
- [ ] Update implemented contracts and module ownership documentation with the
  final behavior, UE mapping and justified Durin differences.
- [ ] Complete final-input validation and record receipts and remaining skips.

Acceptance: the intended module dependency direction is enforced; a new fixture
provider needs no generic production edits. No migrated workflow can execute a
parallel legacy path. Every retained adaptation has documented semantics/tests.

## Validation And Completion

Follow the [build workflow](../Agents/BuildAndRun.md) and
[test workflow](../Agents/Testing.md). Use focused owning/consumer targets during
each stage; inspect affected selection and reuse passing final-input evidence.
Verify test target names before executing them. Candidate coverage includes
ContentBrowserDataTests, ContentBrowserWorkflowTests, EditorAssetWorkflowTests,
EditorHostToolTests, AssetEditorFamilyTests, PythonScriptPanelTests, SceneImportTests
and affected thumbnail targets. Select by changed behavior and consumers rather
than running every candidate after every edit.

Final acceptance requires one editor `all` build across Engine, Sandbox and
RoadWeaver for the final public API/reflection/module graph: split DLL exports,
registration and cross-project consumers cannot be established by private provider
tests alone. Require a bounded editor startup/shutdown smoke after module/bootstrap
migration, following application-host guidance before execution; no broad startup
characterization suite or GPU qualification is implied. Record unexecuted explicit
gates as outstanding. Documentation-only planning requires document/plan validation
and diff review, not native tests.

- [ ] All stage acceptance conditions passed against their final relevant inputs.
- [ ] Nonphysical provider, overlapping folders, two independent browsers and
  headless/no-browser service operation validated through ordinary APIs.
- [ ] Unchanged native guarantees retained; approved mixed-directory semantic changes
  documented and covered by failure injection, partial-success and retry regressions.
- [ ] Final cross-project all build and bounded startup/shutdown smoke passed.
- [ ] Removed-symbol/old-path audit and documentation validation passed.
- [ ] Actual implemented contracts and justified UE deviations documented;
  completion evidence recorded before changing this plan's lifecycle status.
