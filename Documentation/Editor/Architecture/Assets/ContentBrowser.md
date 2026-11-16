# Content Browser

Summary: Define browser item commands, capability queries, update notifications, UI synchronization, thumbnails, and permanent deletion.

Modules: ContentBrowser, ContentBrowserData, ContentBrowserAssetDataSource, ContentBrowserFileDataSource, MainFrame, AssetTools, DurinEd, AssetRegistry, Engine, TextureEditor, StaticMeshEditor, LevelEditor

The Content Browser presents the contents of automatically scanned mounted
content roots. It combines registered engine assets and ordinary files in one
directory-oriented view while preserving their different identities and
operations.

Its concrete models, panel body, refresh coordination, source-thumbnail caches,
are owned by the `ContentBrowser` module under
`Durin::ContentBrowser::Private`. MainFrame owns the singleton tool
instance and its docked/drawer presentation. Shared asset
registries, picker contracts, and thumbnail services remain in the flat
`Durin` boundary or their runtime modules.

Create, Import, Details, and Context Menu entries are deterministic scoped
extensions ordered by `(Order, Id)`. LevelEditor contributes Level creation,
and MaterialEditor contributes Material and Material Instance creation.
TextureEditor, StaticMeshEditor, and LevelEditor contribute the existing Import
workflows and own their concrete modal state. ContentBrowser captures the live
Import snapshot for a selected virtual directory; MainFrame neither enumerates
nor dispatches concrete import families. Reimport remains capability-driven
through `FReimportManager` rather than this presentation registry.

MainFrame contributes **Run Python Script** for a single ordinary `.py` file.
It opens the host's Python Scripts presenter and submits the physical file through
the existing runtime session. The action declares `MutatesContent`, follows Play
mutation gating, and is retired with its presenter. Python-specific execution,
job state and errors remain owned by MainFrame/DurinPython rather than the browser.

Releasing a feature handle removes it from future snapshots. The host stops
dispatch and releases captured descriptors before unloading feature code. An extension may also
contribute one host presenter for its feature-owned modal state. MainFrame
draws those presenters through the browser tool without depending on concrete
asset-editor modules, and supplies the current asset-mutation policy to both
command invocation and modal submission. Host presenters share an immutable
snapshot sorted by `(Order, Id)`, rebuilt only after a presenter is registered
or unregistered. Captures remain owned during dispatch; retired registration
serials cannot dispatch even if their old snapshot is still retained. Active
presenters continue to run each frame, including when the browser is hidden.
Commands explicitly declare `ReadOnly`
or `MutatesContent`, independently of their menu category; registration rejects
unspecified policies. Mutating commands are disabled while Play denies content
mutation, and dispatch checks the current policy again. An already-open presenter
still draws with submission disabled. Queued commands retain owned context and
registration identity rather than provider callbacks; unregistering or replacing
an entry prevents an old queued click from invoking it.

Asset definitions supply display names, icons, colors and category paths by full
qualified class identity. The browser resolves those values without loading an
asset. The legacy type-presentation registration table and fixed category enum
are removed. Dynamic category filters use path prefixes; persisted built-in filter
shortcuts resolve the same `Level`, `Meshes`, `Material` and `Texture` roots.
Filtering never infers type from display labels or class-name substrings.
Unknown types use their class leaf, generic icon and OtherAssets.

`RegisterAssetTypeExtras` independently supplies exact-class read-only details and
thumbnail badges. Removing extras preserves definition metadata; removing a
definition preserves applicable extras without inventing display metadata or
category membership. Definition and extras revisions invalidate browser
projections. Opening uses the asset-editor subsystem; thumbnail rendering and
Reimport retain independent feature registrations.

Production item, directory-tree, and background menus consume applicable Context
Menu extensions through the same presenter used by Create and Import. Context owns
the mixed selection (assets, redirectors, ordinary files, and directories), the
primary clicked item, current location, target physical/virtual directories, and
catalog revision. Right-clicking an unselected item replaces the selection;
right-clicking a selected item preserves multi-selection. A directory-tree target
supplies that directory as the selection and destination; a background menu has
an empty selection and targets the current directory. A physical directory need
not have a virtual mount.

Details extensions use `Details` instead of `Invoke` and must declare `ReadOnly`.
The selection panel prepares owned label/value rows, then draws them without
retaining provider callbacks. For a single asset it first asks the exact-class
details provider, then appends applicable Details extensions in `(Order, Id)`
order. Multi-selection details come from extensions that explicitly accept that
context; the browser does not arbitrarily invoke the first asset's type provider.

Presentation state lives in `ContentBrowserSettings.yaml`. When the file is
absent, the browser uses defaults and writes the new file; the retired Level
Editor browser keys are intentionally neither read nor migrated.

Settings > Editor > Content Browser owns hidden-file visibility, thumbnail size,
and Ctrl-wheel resize locking. The browser view-options popup links to that page
and keeps contextual type/redirector filters. Grid/list switching, selection
details, and unlocked Ctrl-wheel thumbnail resizing remain local browser controls.

## Content Model

`ContentBrowserItem.h` publishes the value-owned `FContentBrowserItem` snapshot
and item-kind vocabulary in `Durin::ContentBrowser`. Discovery, operations and
extension selection contexts use this same type. Extensions retain their own
copies; there is no extension-only item representation or stored identity copy.
Metadata and paths remain presentation values; opaque internal item data selects
the source that handles each operation.

The model caches its directory-tree mount display data by `FMountPaths` registry
revision. `ContentStorage::FContentMountPaths` is a stateless projection over the current mount
service, shared by navigation and operation validation. Operations retain no
mount snapshot or injected path service. Directory conversion handles mount roots
explicitly and resolves other directories directly, without a synthetic child.
The display snapshot never authorizes a mutation. An unchanged registry skips path conversion and snapshot construction,
including during hidden-panel ticks. Publication and test-registry replacement
or restoration invalidate this cache; catalog reconciliation and pending async
result processing retain their independent frame-pump behavior.

- Folders are navigation items and remain visible under every content-type
  filter.
- Every top-level asset record in a registered `.dasset` package appears as its
  own real-asset or redirector item. Items use canonical
  `FTopLevelAssetPath` identity while retaining the owning `FPackagePath` for
  file, save, dependency, and whole-package operations. A multi-asset package
  therefore yields multiple items but only one package file.
- Every other regular file appears as a file. A file may be an import source,
  an asset-managed companion, or an unrelated project document; importability
  is a capability rather than an item kind.
- The default `All content` filter shows real assets and files together while
  redirectors stay hidden. `Show Redirectors` and the redirector-only filter
  reveal aliases explicitly; asset-class filters apply only to real assets.
  Folders remain visible even when all of their contained items are hidden
  redirectors.

Search is recursive beneath the current directory. Ordinary browsing shows
only immediate children. Folders sort before other items, after which the
selected stable sort applies.

Ordinary source item events merge directly into the view's keyed contribution
collection. Added and Modified replace only their source contribution, Moved
removes the old identity before merging the complete new description, and Removed
preserves any other folder contributors. Views bind each update with their own
operation policy and apply the baseline's compiled single-item filter. They do
not translate these events back into physical-path range invalidations.

Published records are immutable shared values. Delta publication copies the
record-pointer collection and replaces only affected records; unrelated item
addresses and retained old ranges remain valid. Frontend filtering and sorting
may rebuild ordered indices over those records without backend enumeration.
Commands rely on committed provider deltas instead of initiating another list
capture after save, transfer, rename or deletion. Explicit full refresh and
navigation still establish a new baseline.

Filesystem enumeration isolates per-entry inspection failures from iterator
progress. An unreadable entry is skipped without truncating later siblings;
traversal failures stop safely and appear as bounded warnings in the status bar.
Reparse points are never followed while building the snapshot or directory tree.

### Discovery Ownership And Shutdown

Providers own native discovery and compiled filters as described in the
[data provider contract](ContentBrowserData.md#items-payloads-and-filters).
`FContentBrowserDataQuery` routes virtual list/tree scopes to those providers and
prepares value-owned capture work. The model schedules that work through the
existing task runtime and owns navigation, cancellation, request replacement,
tree presentation caches and publication versions. It retains no asset catalog
or native collector. Source-provided root enumeration also exposes nonphysical
folders in the directory tree; navigation and breadcrumbs use virtual paths.

Workers retain immutable source-prepared values and cancellation tokens. Live
source dispatch and payload binding stay on the owner thread. Publication checks
the captured source/mount identity and request cancellation; stale
results cannot become actionable rows. Directory requests compile current
provider inputs independently of earlier list captures. Views preserve their
selection, search, filtering, sorting and presentation state independently.

Stopping a panel closes its query admission, releases temporary contexts and
confirmed deletion capabilities, revokes publication rights, cancels captures and
releases thumbnail references. Completed tasks are still collected after admission
stops; their results cannot publish. Destruction drains worker bodies before module
code may unload. Synchronous discovery fixtures use the same owned capture boundary.

### Directory Tree Snapshots

Directory drawing queues missing child snapshots without performing filesystem
enumeration. The model publishes those requests before a later frame draws either
browser pane. A published child vector remains alive and unchanged for the complete
recursive tree traversal, so nodes may retain non-owning spans into it. Inserting a
distinct cache entry is allowed because map growth does not invalidate existing element
references; clearing, erasing, replacing, or mutating an observed entry is not.

Tree navigation and directory context actions commit after recursive tree drawing and
before the content pane, preserving same-frame navigation without invalidating ancestor
spans. Filesystem mutations delivered by drag and drop commit after both panes finish
using their frame snapshots.

## Hidden Content

Assets, folders, and ordinary source, configuration, and documentation files are
visible by default. `.dbulk` files are internal package payloads and never appear
as ordinary file rows, including orphan payloads and case variants of the
extension. This presentation filter applies to browsing, recursive search, and
the Files filter; enabling hidden files does not expose package internals.
Asset size includes the registered package's `.dasset` and `.dbulk` bytes.
The hover tooltip shows the total and, when bulk data exists, both file sizes;
the list Size column and size sort use the same total.
Physical snapshots and recursive mutation scope remain complete. No ownership
is inferred from a shared filename stem, so ordinary source files remain visible.
Redirectors remain opt-in through `Show redirectors` or the Redirectors filter.

Any item beneath a path component whose name starts with `.` is hidden by
default. The Content Browser settings menu provides an explicit `Show hidden
files and folders` option. Hidden-content visibility is independent of whether
an item is an asset or an ordinary file.

## Operations

Texture creation enters through `Import > From File...`, currently restricted
to Texture2D. TextureEditor opens the native file picker and delegates naming,
factory invocation, save, and retry to its UI-independent file-import service.
The current browser directory and a collision-free filename-derived name select
the destination. AssetForgeBuiltins infers first-import texture settings from
filename hints and decoded source content. Errors use host notifications;
successful persistence refreshes and reveals the asset. A failed save exposes
`Import > Retry Texture Saves` while retaining the complete live asset. See
[Source File Workflows](../../Guides/SourceFileWorkflows.md) for automatic defaults.

Assets open through registered asset editors. The factory receives
`FContentBrowserHostContext` once, separately from presentation settings and
persistence. That host provides a weak `DContentBrowserDataSubsystem` binding;
each panel owns only view/query state and borrows the shared data service. The
[provider contract](ContentBrowserData.md) owns source registration, opaque payloads,
item capabilities, batching and incremental notifications. Native providers retain
request-local policy and explicit deletion requests, with provider-owned
package/file operations and shared native recovery mechanisms.

Panel captures intent, confirms destructive scope and presents results. Menus,
shortcuts and deferred commands use `FContentBrowserItem` capabilities; batch save,
copy, move and confirmed delete route through the data subsystem. Destinations
remain virtual until the native adapter resolves them. There is no panel-level
asset/file execution dispatch. Clipboard and Explorer remain presentation/platform
actions. Asset-specific redirector maintenance uses optional host callbacks wired by MainFrame to the native asset provider;
creation/import/details extensions and asset definitions remain independently
registered. All queued execution rechecks current caller and provider admission.

Operations return bool for any contribution success and reveal explicit outputs.
Duplicate returns the created copy even when saving fails; the dirty copy remains
available for ordinary Save, with the save error reported separately.
Folder creation returns a source-owned temporary context without writing. Its
validation/finalization callbacks retain the original directory, validate inline
names without repeated global diagnostics and recheck current policy before
creating content. Each panel owns its context and inline editor. Failure retains
the editor; Escape, outside clicks, navigation and shutdown discard it without
writing. Only successful creation publishes an added item and reveals the folder.
The temporary item never enters the Model snapshot, selection or drag payloads.

The Panel logs diagnostics to Console and displays a brief error or warning;
it owns no diagnostic history. Providers publish actual changes through
`FContentChangeBatch`, with full refresh for unknown scope. Panels consume source
updates and refresh after attempted commands; they do not republish mutations or
interpret native recovery details. Explicit outputs drive reveal and selection.
See the [provider contract](ContentBrowserData.md#operations-and-request-state)
for result, binding and execution semantics.
The data source never refreshes Model. Each view subscribes to item updates and explicit refresh events.
The data service owns source ticking independently of views. Asset query subscriptions and File background reconciliation deliver updates with no open browser. RefreshCoordinator additionally consumes external host/catalog notifications and supports explicit reconciliation. **Duplicate** and `Ctrl+D` clone one
selected real asset into the same writable folder, choosing `_Copy`, `_Copy2`,
and later suffixes until both catalog and physical destinations are free. The
complete persistent object graph is copied into live memory, then the browser
explicitly requests saving. Duplication and save results are consumed separately.
Save failure reports the save error and retains the copy, reveals its identity,
and notifies the host about the live content change; ordinary Save retries it.
`Ctrl+C` and the Copy context action accept ordinary files, folders, and mixed
selections. A single real asset retains its canonical top-level identity on the
system clipboard. Other selections retain an owned in-process snapshot shared
by browser panels, guarded by a system clipboard token; replacing that token
invalidates the selection. This is an editor clipboard, not OS file-transfer
interoperability. `Ctrl+V` and Paste target the current folder; a folder context
menu can paste directly into that folder. Paste keeps source names, merges existing directories and fails existing files/packages without replacement. Inspect actual results before retrying a mixed batch; there is no public per-item
retry ledger. Duplicate retains suffix selection.

Each provider validates its own copy scope, collapses selected descendants under
selected folders and preserves empty directories and hidden ordinary files. Sources
execute independently; there is no all-providers-before-write preflight.
It rejects mount roots, nested mount boundaries, reparse points, missing sources,
redirectors, unknown packages, and pasting a folder into itself or a descendant.
Assets use AssetTools duplication with new identities; ordinary files retain
identical bytes, including embedded path references. Bulk payloads are recreated
by asset persistence, never copied independently; orphan `.dbulk` files block
copy even inside folders. Other contributor-owned companion types currently
block folder copy because no generic companion duplication contract exists.

Copy retains earlier successful writes after independent failures. Native
providers report retained/uncertain recovery locations and protect their own
remaining writes. A provider failure does not cancel independent sources. Asset
copies may retain a dirty live copy after persistence failure; diagnostics describe
that state. There is no generic transfer cancellation or cross-provider rollback.
Background query cancellation and deletion confirmation dismissal remain supported.

Opening a
redirector resolves and opens its final real asset; redirectors are excluded
from ordinary pickers, rename, and drag-move.
Ordinary files open through the operating system and use filesystem operations.
Files reported as owned by Engine's registered companion contributors cannot
be renamed, moved, or deleted independently; the owning asset operation must be used.
A shared filename stem alone does not establish companion ownership.

Every authorable asset uses AssetTools editor save actions over Engine's sole
package persistence authority. `Save
Package` is enabled only for a resident Dirty package, whether newly created or
already published. Closing or rolling back unsaved work must explicitly select
the discard-unsaved unload policy. `Resave Package` may be
used on a clean compatible current-format package and is labeled recommended
when live load observed a registered legacy reflected identity. Multi-selection
routes `Resave Selected Packages` through source bulk save; independent successes
remain saved when other requested packages fail.
Presentation never turns a resave recommendation into an unsaved-change
decoration or close prompt. Registry and item snapshots refresh only after
verified publication.

Double-click routing is resolved by the live `FEditorDocumentManager` exact-class
route table. `MaterialEditor`, `TextureEditor`, and `StaticMeshEditor` own their
respective asset routes; StaticMesh opens a closable per-resource document in
the user-visible **StaticMesh Inspector** workspace. The Content Browser does
not name or construct a concrete asset editor.

## Thumbnail Requests And Refresh

ContentBrowser owns item presentation, ordinary-file image decode/cache, and
request admission. Cards pass an item and visible/prefetch priority to
`FContentBrowserThumbnailReferences`. This service checks live renderer
availability and obtains exact asset and package metadata from AssetRegistry,
then retains lightweight `FAssetThumbnail` references and submits requests to the
`DThumbnailManager`-owned shared `FAssetThumbnailPool`. Cards do not select a
renderer or production path. `DurinEd` owns scheduling, persistence,
render/readback/upload, preview scenes, reference pinning, texture reuse, and
budgets. Feature editor modules own their exact-class renderers. Unsupported
classes keep their ordinary asset icon and create no pool job. Item snapshots
retain identity, paths, class, display size and modification time; they carry no
thumbnail identity, source override or package-cache fingerprint. Supported
ordinary images use their physical identity and existing file display metadata
through the source-image cache. Operation results do not initialize preview data.

Refresh, navigation, move, delete, reimport, panel close, and editor shutdown
cancel obsolete owned requests before rebinding the visible snapshot.
Ordinary item updates retain matching thumbnail references across delta
publication. Metadata/residency notifications are not pixel invalidations;
content/dependency changes are handled by the shared pool. Scoped regeneration
retains a valid old preview until replacement, while withdrawn resources and
unknown full invalidation revoke their pixels immediately.
Visible work outranks prefetch, duplicate keys coalesce, and all concrete asset
types share the same bounded scheduler and one-rendered-capture-per-frame limit.
Renderer removal stops admission and drains that renderer generation's queued
or in-flight leases without affecting renderers registered by other modules. During editor
shutdown MainFrame first stops Content Browser admission, then unregisters
feature integrations in reverse composition order. That retires extension
callbacks and cancels feature-owned dialogs and thumbnail renderer leases. The
browser has already released its asset-thumbnail references; destruction then
drains any admitted import and ordinary-file cache before workspace state and host
notification surfaces disappear. Concrete modules unload only after this
host-owned teardown.

Selection details are presentation snapshots. TextureEditor owns TextureCube
metadata inspection and its bounded cache keyed by asset identity,
registry revision, and file metadata; drawing details never loads the package.
Runtime-only platform or build fields are shown as unavailable when they are not
serialized.

Redirector details show the direct and final targets, complete chain and
terminal state, hard/soft/redirect referencer counts, and reference-index
completeness. Referencer navigation reveals the selected owner. Selection,
folder, and project-wide Fix Up commands call the shared AssetTools operation;
an empty virtual directory never falls through to project-wide scope. Failed
analysis/publication retains every alias and reports the blocking participant.
AssetTools prepares and resumes the opaque Engine job without adding editor
history; ContentBrowser cannot invoke package/store rewrite, alias deletion,
verification, or recovery phases separately.

Create, import/reimport, duplicate, rename, move, and folder relocation
through the shared publication seam reject a redirector-occupied destination.
The error names the final destination and directs the user to Fix Up or remove
the alias closure rather than treating the path as vacant.

Content-item drag and drop supports ordinary files, folders, and mixed selections.
Dragging an already selected item preserves the selection. A single real asset
keeps the shared asset payload used by other editors; other selections carry
owned physical paths and execute after view traversal. Both payloads submit
items and a destination directory through `MoveItems`; the browser no longer
exposes a package-mapping move entrypoint. Registered packages in physical-path
payloads recover their asset identity from the current catalog. Asset, file, and
folder rename build a new destination name and use the same internal move commit
path. Asset object names survive package rename; ordinary file rename retains the
existing extension when none is supplied. Selected descendants are collapsed
under selected folders; empty directories are preserved.

Capability queries inspect the selected item, name and final destination without
recursive planning. Queries do not reserve destinations: each executing provider
reads current mounts and validates its recursive scope before writing. Planned item
notifications are published only through the commit path, never after a rejected
preflight. Individual assets may relocate across writable scanned mounts through
AssetTools; ordinary files and folders stay within their source mount.

Provider preparation validates mounts, source/destination ancestry, reparse paths,
unknown packages and companion ownership before writing. Shared directories merge;
files and packages never overwrite existing destinations. Selected descendants
collapse beneath selected folders. Asset rename/move runs synchronously through RenameAssets with exact object
paths. Normal rename changes the selected object name and its package leaf;
multi-asset packages retain unselected siblings. Dirty and unsaved asset items
use live identity rather than requiring a preliminary save. Ordinary file moves copy without
replacement, verify byte identities, then remove only the verified source. Failed
source removal retains the destination and reports both paths for recovery.
Independent failures retain prior commits; admission loss or uncertain native
recovery stops remaining writes inside that provider. Registry publication failure retains
committed content and requests reconciliation. Source cleanup removes only empty
directories; retained redirectors produce a warning. Authored relocation never
enters global object-edit Undo/Redo. See
[asset mutation](../../../Runtime/Assets/Packages/AssetCatalogAndMutation.md#rename-and-private-byte-transformations).

The browser's navigation roots come from automatically scanned mounts. Providers
may contribute logical items beneath those roots without physical storage.
Filesystem-backed creation and rename operations additionally require the owning
mount to be content-writable. These browsing constraints do not change the
validity of typed source paths or Engine's authoring policy.

Its asset rows are a UI value projection of `CaptureAssetQuerySnapshot`.
ContentBrowser performs no live-object enumeration and never manufactures disk
package metadata for unsaved assets. GameThread captures filenames and display
metadata until explicit invalidation; workers receive those owned UI values.
Import and creation completion callbacks only reveal/open assets; they do not
report a mounted-filesystem mutation or trigger a Registry rescan. Filesystem
notifications remain reserved for actual filesystem changes.
Providers consume `OnAssetQueryChanged` and publish item-data events. Views
invalidate affected caches and cancel obsolete captures; pickers share the same
creation, rename, save, unload and discard events. Unsaved assets and their implied folders can be browsed
and revealed without writing directories or packages. Manual or
automatic reconciliation consumes the single `FAssetCatalogRefreshResult`,
including catalog/reference completeness, prior and resulting revisions,
counters, warnings, and structured errors; the panel never retains a registry
pointer or reads mutable last-error state. A failed refresh may display the
explicitly retained prior snapshot, but it is not presented as a newly complete
catalog.

## Recursive Deletion

Folder and mixed-item deletion is one recursive, irreversible command.
Preflight reads the physical filesystem beneath normalized selected roots rather
than the filtered Content Browser projection. It deduplicates selected
descendants beneath selected ancestors and classifies every entry as a folder,
registered package, ordinary file, or asset-managed companion. An unregistered
`.dasset` is an explicit unknown-package blocker and is never treated as an
ordinary file or silently omitted.

AssetTools constructs an `FAssetDeleteModel` for the captured selection. Its
private state owns the confirmation snapshot, registry admission, sorted physical
fingerprints and maximal roots. Browser providers hold no independent Plan,
Operation or versioned confirmation handle. File fingerprints include a streamed
128-bit byte identity; final validation also checks directory membership. A
changed filesystem, registry or reference scope rejects execution on that model.
A later user attempt constructs a fresh model and confirms its newly analyzed
scope; no mutable handle is replaced or automatically retried.

The complete operation is blocked before mutation when any target is outside a
single writable authoring mount, is the mount root, traverses a reparse point,
violates source-control policy, or cannot be
inspected. AssetTools deletion preflight over Engine safety mechanisms also
blocks loading packages and unselected dirty content that cannot be preserved,
references from outside the deletion set, and ambiguous or externally owned
companions. References between assets inside the same deletion set are allowed.
Engine assigns each companion to one owner and includes an owned companion
outside a selected folder once as a standalone root.

Deletion never rewrites soft or external-store paths, but it reports them as
explicit dangling-reference warnings and revalidates the warning snapshot.
Redirectors are selected by exact export, without resolving the selection to its
destination. Target deletion requires the necessary incoming alias closure;
external references remain blockers. Selected unsaved changes remain subject to
explicit loss policy and the model's final checks.

### Permanent deletion boundary

The confirmation dialog states that Delete permanently removes local content,
cannot be undone, and must be restored through version control. ContentBrowser
revalidates every confirmed byte identity and rejects new descendants through
the file-validation callback invoked by AssetTools before destructive work. AssetTools revalidates the confirmed selection, companion
ownership, warnings, and external-reference snapshots, asks Engine to release
the eligible resident package batch, and invokes the host deletion phase, which removes confirmed provider-owned files and then empty directories individually. Engine then publishes the checked catalog removals. The model owns confirmation and execution state; no separate deletion job
is wrapped or exposed to ContentBrowser.
No quarantine directory, pre-image, Restore/Purge API, editor history entry, or
automatic retention policy exists.

`BulkDeleteItems` captures exact provider item scope and invokes one model-backed
command. Stale content before destruction records unattempted entries and rejects
execution. The Panel neither retries automatically nor expands the selection;
a new command analyzes and confirms a fresh model.

`StaleData` means confirmed content or dependency snapshots changed, including
companion ownership and external reference-store snapshots. Read failures retain
`IoError`; loading and live-reference blockers retain their original errors.
Failed validation performs no destructive I/O. A retry constructs a new model;
neither a previous confirmation nor a completed model bypasses final checks.

A model cannot execute again once attempted. Partial deletion never recreates
removed content; it publishes the committed removals, requests conservative
reconciliation when needed, and retains failed/unattempted facts for diagnostics.
Registry fences and recovery obligations remain at their owning boundaries.
There is no Browser last-result cache or executable confirmation handle.

If physical removal completes but Registry publication fails, the data source
reports a warning and committed removals plus failed projection in partial-result
facts. RefreshCoordinator reconciles from mounted authored files. The Panel
never inspects `EAssetWriteEffect`, parses diagnostic prefixes or repeats I/O.

Mounted-content mutations publish structured events. Panels subscribe and coalesce
reconciliation across views, preserving selection and directory remapping. An
AssetRegistry item event updates view caches without another filesystem scan.
Ordinary object/property edits do not request mounted-content reconciliation.

A failed reconciliation is suppressed until an explicit Refresh or another
mutation event. New events raised during callbacks remain pending for the next
batch. Explicit Refresh scans auto-scan mounts and invalidates the view on success.
Surviving navigation/selection is preserved; a removed current directory falls
back to its nearest surviving parent. Panels retain no catalog/mutation cursor.

## Related Documentation

- [Asset Import Architecture](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Source File Workflows](../../Guides/SourceFileWorkflows.md)
- [Canonical Resave](../../Guides/CanonicalResave.md)
- [StaticMesh Inspector](../../Guides/StaticMeshInspector.md)
