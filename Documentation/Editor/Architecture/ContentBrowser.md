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

## Related Documentation

- [Asset Import Framework](AssetImportFramework.md)
- [Asset Thumbnails](AssetThumbnails.md)
- [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md)
