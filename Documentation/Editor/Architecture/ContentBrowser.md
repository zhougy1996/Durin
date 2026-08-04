# Content Browser

The Content Browser presents the contents of automatically scanned mounted
content roots. It combines registered engine assets and ordinary files in one
directory-oriented view while preserving their different identities and
operations.

## Content Model

- Folders are navigation items and remain visible under every content-type
  filter.
- Registered `.dasset` packages appear only as assets, using registry metadata
  and asset-class behavior. Package files are never duplicated as ordinary
  files.
- Every other regular file appears as a file. A file may be an import source,
  an asset-managed companion, or an unrelated project document; importability
  is a capability rather than an item kind.
- The default `All content` filter shows assets and files together. `Assets`
  and `Files` provide category-only projections, and asset-class filters apply
  only to registered assets.

Search is recursive beneath the current directory. Ordinary browsing shows
only immediate children. Folders sort before other items, after which the
selected stable sort applies.

## Hidden Content

Any item beneath a path component whose name starts with `.` is hidden by
default. The Content Browser settings menu provides an explicit `Show hidden
files and folders` option. Hidden-content visibility is independent of whether
an item is an asset or an ordinary file.

## Operations

Assets open through registered asset editors and use asset-aware rename, move,
and deletion workflows. Ordinary files open through the operating system and
use filesystem operations. Files detected as asset-managed companions cannot
be renamed or deleted independently; the owning asset operation must be used.

The Content Browser enumerates only automatically scanned mounts. This affects
navigation and presentation, not the validity of typed source paths or the
authoring-write policy of a mount.

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
move. Confirmation revalidates that exact plan. If filesystem or registry state
changed, the modal replaces the stale plan, displays the updated scope, and
requires a second confirmation.

The complete operation is blocked before mutation when any target is outside a
single writable authoring mount, is the mount root, requires cross-volume
staging, traverses a reparse point, violates source-control policy, or cannot be
inspected. AssetCore batch analysis also blocks loading or dirty packages,
references from outside the deletion set, and ambiguous or externally owned
companions. References between assets inside the same deletion set are allowed.
AssetCore assigns each companion to one owner and includes an owned companion
outside a selected folder once as a standalone root.

### Transaction and Recovery

The first deletion executes through the global editor transaction manager, so
notification actions, the Edit menu, and keyboard Undo/Redo operate on the same
history entry. The Content Browser transaction is the sole owner of physical
staging. It renames maximal roots into a collision-safe, marked operation
directory under `Saved/ContentBrowserUndo` on the same volume. AssetCore owns
package unload and registry removal/restoration, but never stages those bytes a
second time. Undo restores persisted content and registry visibility without
restoring loaded-package residency.

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

Successful Delete, Undo, and Redo advance the editor transaction manager's
content-mutation revision. Content Browser panels observe that revision without
being retained by history, cancel obsolete thumbnail work, rescan and refresh,
repair selection, and preserve the current directory when it survives. If the
current directory was deleted, the panel navigates to its nearest surviving
parent; unrelated global history commands do not steal Content Browser focus.

## Related Documentation

- [Asset Import Framework](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md)
