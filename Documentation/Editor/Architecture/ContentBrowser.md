# Content Browser

Summary: Define asset presentation, operations, thumbnails, deletion, undo, and recovery in the Content Browser.

Modules: LevelEditor, DurinEd, AssetCore

The Content Browser presents the contents of automatically scanned mounted
content roots. It combines registered engine assets and ordinary files in one
directory-oriented view while preserving their different identities and
operations.

Its concrete models, panels, refresh coordination, source-thumbnail caches, and
asset-operation helpers are owned by `Durin::Editor::Level`. Shared asset
registries, picker contracts, and thumbnail services remain in the flat
`Durin::Editor` boundary or their runtime modules.

## Content Model

- Folders are navigation items and remain visible under every content-type
  filter.
- Registered `.dasset` packages appear as real assets or redirectors from exact
  registry metadata. Package files are never duplicated as ordinary files.
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
its own source-layout and validation contract. VolumeTexture inspects selected
PNG content to suggest an atlas interpretation, while ambiguous layouts remain
explicit choices and advanced fields stay editable. Import submission failures
remain inline in the open modal so the complete form can be corrected and
retried without re-entry.

Assets open through registered asset editors and use asset-aware rename, move,
duplication, and deletion workflows. **Duplicate** and `Ctrl+D` clone one
selected real asset into the same writable folder, choosing `_Copy`, `_Copy2`,
and later suffixes until both catalog and physical destinations are free. The
complete persistent object graph is copied and published as a clean package;
duplicated import records receive a new record identity and relinquish
management of the original record's outputs. `Ctrl+C` writes one selected real
asset identity to the system clipboard; `Ctrl+V` pastes it into the current
folder, preserving its name when free and otherwise using the same copy suffix
sequence. A folder context menu can paste directly into that folder. Opening a
redirector resolves and opens its final real asset; redirectors are excluded
from ordinary pickers, rename, and drag-move.
Ordinary files open through the operating system and use filesystem operations.
Files reported as owned by AssetCore's registered companion contributors cannot
be renamed or deleted independently; the owning asset operation must be used.
A shared filename stem alone does not establish companion ownership.

Every authorable asset uses AssetCore's package-level save actions. `Save
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

A managed output exposes `Resave Import Record` beside `Reveal Import Record`.
The Content Browser resolves the association through the import-record index
and passes only the record package to the generic service. Reimport, recreate,
repair, detach, and resave remain distinct actions; canonical resave reads no
source and invokes no provider.

Double-click routing is resolved by the live `Editor::FWorkspaceManager` exact-class
route table. `MaterialEditor`, `TextureEditor`, and `StaticMeshEditor` own their
respective asset routes; StaticMesh opens a closable per-resource document in
the user-visible **StaticMesh Inspector** workspace. The Content Browser does
not name or construct a concrete asset editor.

## Thumbnail Requests And Refresh

LevelEditor owns Content Browser item presentation and request admission.
`DurinEd` owns the provider-neutral thumbnail service, source decode/cache,
scheduler, persistence, render/readback/upload pipeline, preview-scene pool,
and budgets. MaterialEditor owns Material and MaterialInstance providers;
TextureEditor owns authored Texture2D source selection and TextureCube rendering;
StaticMeshEditor owns StaticMesh rendering. Unsupported classes keep their
ordinary asset icon and create no thumbnail job. Raw source-file images use the
generic Content Browser path and are not owned by TextureEditor.

Refresh, navigation, move, delete, reimport, panel close, and editor shutdown
cancel obsolete request generations before rebinding the visible snapshot.
Visible work outranks prefetch, duplicate keys coalesce, and all concrete asset
types share the same bounded scheduler and one-rendered-capture-per-frame limit.
Provider removal stops admission and drains that provider's queued or in-flight
leases without affecting providers registered by other modules. During editor
shutdown MainFrame stops Content Browser admission, unregisters StaticMesh,
Texture, Material, and Level integrations in that order, drains and destroys the
shared service caches, and only then permits concrete module unload.

Selection details are presentation snapshots. TextureCube details inspect
serialized package metadata through a bounded cache keyed by asset identity,
registry revision, and file metadata; drawing details never loads the package.
Runtime-only platform or build fields are shown as unavailable when they are not
serialized.

Redirector details show the direct and final targets, complete chain and
terminal state, hard/soft/redirect referencer counts, and reference-index
completeness. Referencer navigation reveals the selected owner. Selection,
folder, and project-wide Fix Up commands call the shared AssetCore transaction;
an empty virtual directory never falls through to project-wide scope. Failed
analysis/publication retains every alias and reports the blocking participant.
The Content Browser prepares an immutable Fix Up summary and calls only
`Commit`; it cannot invoke package/store rewrite, alias deletion, verification,
or compensation phases separately.

Create, import/reimport, duplicate, rename, move, and folder relocation
through the shared publication seam reject a redirector-occupied destination.
The error names the final destination and directs the user to Fix Up or remove
the alias closure rather than treating the path as vacant.

Asset and folder moves retain one opaque AssetCore mutation transaction in the
global editor history. The coordinator prepares and commits it once; the editor
history calls only `Undo` and `Redo`, never AssetCore revalidation, publication,
restore, or compensation phases directly.

The Content Browser enumerates and navigates only automatically scanned mounts.
Filesystem-backed creation and rename operations additionally require the owning
mount to be authoring-writable. These browsing constraints do not change the
validity of typed source paths or AssetCore's authoring policy.

Its asset rows are derived from owned `FAssetCatalogSnapshot` values. Manual or
automatic reconciliation consumes the single `FAssetCatalogRefreshResult`,
including catalog/reference completeness, prior and resulting revisions,
counters, warnings, and structured errors; the panel never retains a registry
pointer or reads mutable last-error state. A failed refresh may display the
explicitly retained prior snapshot, but it is not presented as a newly complete
catalog.

## Recursive Deletion

Folder and mixed-item deletion is one recursive, reversible editor transaction.
Preflight reads the physical filesystem beneath normalized selected roots rather
than the filtered Content Browser projection. It deduplicates selected
descendants beneath selected ancestors and classifies every entry as a folder,
registered package, ordinary file, or asset-managed companion. An unregistered
`.dasset` is an explicit unknown-package blocker and is never treated as an
ordinary file or silently omitted.

Preflight produces one immutable deletion plan shared by the confirmation modal
and transaction. The plan contains the registry revision, deterministic
physical fingerprints, sorted entries, and the minimal set of maximal roots to
move. File fingerprints include a bounded-memory streamed 128-bit byte identity;
directory identities deterministically aggregate their sorted descendants.
Confirmation and transaction transitions revalidate those identities. If
filesystem or registry state changed, the modal replaces the stale plan,
displays the updated scope, and requires a second confirmation.

The complete operation is blocked before mutation when any target is outside a
single writable authoring mount, is the mount root, requires cross-volume
staging, traverses a reparse point, violates source-control policy, or cannot be
inspected. AssetCore deletion preparation also blocks loading or dirty packages,
references from outside the deletion set, and ambiguous or externally owned
companions. References between assets inside the same deletion set are allowed.
AssetCore assigns each companion to one owner and includes an owned companion
outside a selected folder once as a standalone root.

Deletion never rewrites soft or external-store paths, but it reports them as
explicit dangling-reference warnings and revalidates the warning snapshot.
Alias-only deletion, broken aliases, and target selections missing any
direct/upstream alias are blocked. A target may be deleted only with its complete
alias closure and explicit confirmation; the transaction captures exact
redirector entries and files so Undo/Redo restores their metadata byte-for-byte.

### Transaction and Recovery

The first deletion executes through the global editor transaction manager, so
notification actions, the Edit menu, and keyboard Undo/Redo operate on the same
history entry. The Content Browser transaction is the sole owner of physical
staging. It renames maximal roots into a collision-safe, marked operation
directory under `Saved/ContentBrowserUndo` on the same volume and supplies only
that reversible stage/restore transition to its retained opaque AssetCore
deletion transaction. AssetCore orders revalidation, package unload, catalog
removal/restoration, and compensation around that transition; the editor cannot
sequence those phases and AssetCore never stages the bytes a second time. Undo
restores persisted content and catalog visibility without restoring package
residency.

Each transition revalidates its inputs: Execute checks the captured plan, Undo
requires every original destination to be free, and Redo requires unchanged
staged fingerprints plus current AssetCore safety. A conflict performs no
mutation and leaves the transaction at its current history head. A later edit
after Undo invalidates Redo through the normal shared history rules.

Mutation steps are journaled and compensated in reverse order on ordinary
failure. If compensation also fails, the transaction enters recovery-required
state, reports both original and staged paths, does not enter or advance
history, and retains its marked operation directory. Otherwise, staged data is
owned for exactly the lifetime of the reachable history entry and cleanup
validates the exact marked, unmounted root without traversing reparse points.

Successful relocation and Delete transitions declare that they mutate mounted
content discovery. Execute, Undo, and Redo therefore advance the transaction
manager's monotonic mounted-content mutation revision. Direct Content Browser
filesystem operations and import completion publish the same invalidation
explicitly. Ordinary object, component, reflected-property, Spline, and
transform-gizmo transactions never publish it; their package revision and dirty
state transitions are independent of filesystem discovery.

Every Content Browser panel observes the mounted-content revision separately
from the live asset-registry revision. A new mounted-content revision performs
one incremental reconciliation, then refreshes mount and item snapshots and
repairs selection. Reconciliation acknowledgement and failure suppression are
shared across open panels, so later observers refresh their local snapshots but
do not repeat the scan. A registry-only revision refreshes those derived
snapshots without scanning, because AssetCore has already published the
metadata change.
The initiating panel acknowledges a successful self-originating reconciliation,
so the next draw cannot repeat it. Manual Refresh remains an explicit scan of
all auto-scan mounts and is also the retry path for external filesystem changes.

Failed automatic reconciliation reports the AssetCore error and retains the
unacknowledged mounted-content revision. That exact failed revision is
suppressed on later frames to avoid a scan loop; manual Refresh or a later
mounted-content revision retries it. Snapshot refresh happens only after a
successful reconciliation. Surviving directories and selections are preserved;
if the current directory was deleted, the panel navigates to its nearest
surviving parent. Panels and transaction objects do not retain one another, and
unrelated global history commands do not steal Content Browser focus.

## Related Documentation

- [Asset Import Framework](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md)
- [Canonical Resave](../Guides/CanonicalResave.md)
- [StaticMesh Inspector](../Guides/StaticMeshInspector.md)
