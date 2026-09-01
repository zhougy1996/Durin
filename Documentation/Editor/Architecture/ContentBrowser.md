# Content Browser

Summary: Define asset presentation, operations, thumbnails, forward-only mutation jobs, and permanent deletion in the Content Browser.

Modules: ContentBrowser, MainFrame, AssetTools, DurinEd, AssetRegistry, Engine, TextureEditor, StaticMeshEditor, LevelEditor

The Content Browser presents the contents of automatically scanned mounted
content roots. It combines registered engine assets and ordinary files in one
directory-oriented view while preserving their different identities and
operations.

Its concrete models, panel body, refresh coordination, source-thumbnail caches,
and asset-operation helpers are owned by the `ContentBrowser` module under
`Durin::Editor::ContentBrowser::Private`. MainFrame owns the singleton tool
instance and its docked/drawer presentation. Shared asset
registries, picker contracts, and thumbnail services remain in the flat
`Durin::Editor` boundary or their runtime modules.

Create, Import, Details, and Context Menu entries are deterministic scoped
extensions ordered by `(Order, Id)`. LevelEditor contributes Level creation,
and MaterialEditor contributes Material and Material Instance creation.
TextureEditor, StaticMeshEditor, and LevelEditor contribute the existing Import
workflows and own their concrete modal state. ContentBrowser captures the live
Import snapshot for a selected virtual directory; MainFrame neither enumerates
nor dispatches concrete import families. Reimport remains capability-driven
through `FReimportManager` rather than this presentation registry.

Releasing a feature handle removes
admission before its module callback gate retires. An extension may also
contribute one host presenter for its feature-owned modal state. MainFrame
draws those presenters through the browser tool without depending on concrete
asset-editor modules, and supplies the current asset-mutation policy to both
command invocation and modal submission. Create and Import are both mutation
categories: invocation is rejected while Play denies asset mutation, while an
already-open presenter still draws with submission disabled.

Presentation state lives in `ContentBrowserSettings.yaml`. When the file is
absent, the browser uses defaults and writes the new file; the retired Level
Editor browser keys are intentionally neither read nor migrated.

## Content Model

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

Filesystem enumeration isolates per-entry inspection failures from iterator
progress. An unreadable entry is skipped without truncating later siblings;
traversal failures stop safely and appear as bounded warnings in the status bar.
Reparse points are never followed while building the snapshot or directory tree.

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

Any item beneath a path component whose name starts with `.` is hidden by
default. The Content Browser settings menu provides an explicit `Show hidden
files and folders` option. Hidden-content visibility is independent of whether
an item is an asset or an ordinary file.

## Operations

Texture asset creation enters through the `Import > Texture...` action. Its
explicit asset-type selection creates Texture2D, TextureCube, or VolumeTexture
without inferring asset identity from a source extension; each type retains
its own source-layout and validation contract. Texture2D accepts a direct
physical filename without a source-mount destination. VolumeTexture inspects selected
PNG content to suggest an atlas interpretation, while ambiguous layouts remain
explicit choices and advanced fields stay editable. Import submission failures
remain inline in the open modal so the complete form can be corrected and
retried without re-entry.

Assets open through registered asset editors. ContentBrowser owns selection,
dialogs, clipboard, mixed ordinary-file coordination, physical deletion
staging, refresh, and reveal; it sends package-backed duplicate, save,
relocation, deletion-preflight, and Fix Up requests to `IAssetTools` rather
than sequencing Engine mutation phases. **Duplicate** and `Ctrl+D` clone one
selected real asset into the same writable folder, choosing `_Copy`, `_Copy2`,
and later suffixes until both catalog and physical destinations are free. The
  complete persistent object graph is copied and published as a clean package.
  `Ctrl+C` writes the selected canonical top-level asset identity to the system
clipboard; `Ctrl+V` pastes it into the current
folder, preserving its name when free and otherwise using the same copy suffix
sequence. A folder context menu can paste directly into that folder. Opening a
redirector resolves and opens its final real asset; redirectors are excluded
from ordinary pickers, rename, and drag-move.
Ordinary files open through the operating system and use filesystem operations.
Files reported as owned by Engine's registered companion contributors cannot
be renamed or deleted independently; the owning asset operation must be used.
A shared filename stem alone does not establish companion ownership.

Every authorable asset uses AssetTools editor save actions over Engine's sole
package persistence authority. `Save
Package` is enabled only for a resident Dirty package, whether newly created or
already published. Closing or rolling back unsaved work must explicitly select
the discard-unsaved unload policy. `Resave Package` may be
used on a clean compatible current-format package and is labeled recommended
when live load observed a registered legacy reflected identity. Multi-selection
routes `Resave Selected Packages` through the same deterministic planner; exact
preflight failures are reported without writing other selected packages.
Presentation never turns a resave recommendation into an unsaved-change
decoration or close prompt. Registry and item snapshots refresh only after
verified publication.

Double-click routing is resolved by the live `Editor::FWorkspaceManager` exact-class
route table. `MaterialEditor`, `TextureEditor`, and `StaticMeshEditor` own their
respective asset routes; StaticMesh opens a closable per-resource document in
the user-visible **StaticMesh Inspector** workspace. The Content Browser does
not name or construct a concrete asset editor.

## Thumbnail Requests And Refresh

ContentBrowser owns item presentation, ordinary-file image decode/cache, and
request admission. Asset cards retain lightweight `FAssetThumbnail` values and
submit the exact `FTopLevelAssetPath`, owning package fingerprint, and
visible/prefetch priority to the
`DThumbnailManager`-owned shared `FAssetThumbnailPool`. Cards do not select a
renderer or production path. `DurinEd` owns scheduling, persistence,
render/readback/upload, preview scenes, reference pinning, texture reuse, and
budgets. Feature editor modules own their exact-class renderers. Unsupported
classes keep their ordinary asset icon and create no pool job.

Refresh, navigation, move, delete, reimport, panel close, and editor shutdown
cancel obsolete request generations before rebinding the visible snapshot.
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

Selection details are presentation snapshots. TextureCube details inspect
serialized package metadata through a bounded cache keyed by asset identity,
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

Asset and folder moves use one opaque forward-only Engine mutation job.
AssetTools calls `ResumeForward`; authored relocation never enters global
object-edit Undo/Redo history.

The Content Browser enumerates and navigates only automatically scanned mounts.
Filesystem-backed creation and rename operations additionally require the owning
mount to be content-writable. These browsing constraints do not change the
validity of typed source paths or Engine's authoring policy.

Its asset rows are derived from owned `FAssetCatalogSnapshot` values. Manual or
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

Preflight produces one immutable deletion plan shared by the confirmation modal
and command. The plan contains the registry revision, deterministic
physical fingerprints, sorted entries, and the minimal set of maximal roots to
remove. Its destructive AssetTools operation is move-only, so copying a plan
cannot create another handle to the same deletion job. File fingerprints include
a bounded-memory streamed 128-bit byte identity;
directory identities deterministically aggregate their sorted descendants.
Confirmation and execution revalidate those identities. If
filesystem or registry state changed, the modal replaces the stale plan,
displays the updated scope, and requires a second confirmation.

The complete operation is blocked before mutation when any target is outside a
single writable authoring mount, is the mount root, traverses a reparse point,
violates source-control policy, or cannot be
inspected. AssetTools deletion preflight over Engine safety mechanisms also
blocks loading or dirty packages,
references from outside the deletion set, and ambiguous or externally owned
companions. References between assets inside the same deletion set are allowed.
Engine assigns each companion to one owner and includes an owned companion
outside a selected folder once as a standalone root.

Deletion never rewrites soft or external-store paths, but it reports them as
explicit dangling-reference warnings and revalidates the warning snapshot.
Alias-only deletion, broken aliases, and target selections missing any
direct/upstream alias are blocked. A target may be deleted only with its complete
alias closure and explicit confirmation.

### Permanent deletion boundary

The confirmation dialog states that Delete permanently removes local content,
cannot be undone, and must be restored through version control. ContentBrowser
revalidates every confirmed byte identity and rejects new descendants before it
calls AssetTools. Engine then revalidates Registry/reference safety, unloads
eligible resident packages, and invokes one destructive maximal-root callback.
No quarantine directory, pre-image, Restore/Purge API, editor history entry, or
automatic retention policy exists.

If deletion stops after removing an earlier root, it does not recreate that
root. The result is forward-pending, affected Registry paths are fenced against
stale resolution, and the same operation can retry remaining content. Mutation
disposition is structured state rather than an English diagnostic prefix. A Registry
failure after physical removal is `ContentCommittedProjectionPending`; manual
or automatic Registry reconciliation converges from mounted authored files.

Successful relocation and Delete declare that they mutate mounted content
discovery and advance the monotonic mounted-content mutation revision once.
Direct Content Browser
filesystem operations and import completion publish the same invalidation
explicitly. Ordinary object, component, reflected-property, Spline, and
transform-gizmo transactions never publish it; their package revision and dirty
state transitions are independent of filesystem discovery.

Every Content Browser panel observes the mounted-content revision separately
from the live AssetRegistry revision. A new mounted-content revision calls
`RefreshAssetRegistry` for one incremental reconciliation, then refreshes mount
and item snapshots and repairs selection. Reconciliation acknowledgement and failure suppression are
shared across open panels, so later observers refresh their local snapshots but
do not repeat the scan. A registry-only revision refreshes those derived
snapshots without scanning, because AssetRegistry has already published the
metadata change.
The initiating panel acknowledges a successful self-originating reconciliation,
so the next draw cannot repeat it. Manual Refresh remains an explicit scan of
all auto-scan mounts and is also the retry path for external filesystem changes.

Failed automatic reconciliation reports the AssetRegistry error and retains the
unacknowledged mounted-content revision. That exact failed revision is
suppressed on later frames to avoid a scan loop; manual Refresh or a later
mounted-content revision retries it. Snapshot refresh happens only after a
successful reconciliation. Surviving directories and selections are preserved;
if the current directory was deleted, the panel navigates to its nearest
surviving parent. Panels and mutation jobs do not retain one another, and
unrelated global history commands do not steal Content Browser focus.

## Related Documentation

- [Asset Import Architecture](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Source File Workflows](../Guides/SourceFileWorkflows.md)
- [Canonical Resave](../Guides/CanonicalResave.md)
- [StaticMesh Inspector](../Guides/StaticMeshInspector.md)
