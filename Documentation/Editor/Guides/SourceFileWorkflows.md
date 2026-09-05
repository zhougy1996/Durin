# Source File Workflows

Summary: Explain how built-in importers select, persist, reimport, and diagnose ordinary source files.

Modules: AssetTools, DurinEd, AssetForgeBuiltins

Last reviewed: 2026-08-31

Durin imports from ordinary files selected with the platform file chooser.
Source files do not need to live beneath an Asset mount, and the editor does
not copy them into Content. Import, reimport, package move, duplication, and
deletion never move, replace, or delete a source file.

## Choosing A Source

Content Browser exposes a fixed built-in Import menu:

- **Texture...** creates Texture2D, TextureCube, or VolumeTexture assets;
- **Scene Source (FBX/glTF)...** creates a set of peer scene outputs;
- **Static Mesh (Geometry Only)...** creates one geometry asset without Scene
  material or texture outputs.

Choose a physical file, then choose the exact top-level asset destination or
output directory. Package/file ownership is derived separately from that
destination. The
source row is read-only; selecting a different file uses the file chooser
rather than editing a virtual source destination.

For every single-output choice, the editor creates the final package and final
asset object directly through the selected concrete Factory. Import is not a
temporary-asset publish transaction. A capture, decode, or build failure
discards that new unsaved package; a save failure leaves the complete live
asset available for an explicit retry. Scene is the multi-output exception and
retains its all-or-nothing package-set transaction.

A successful import may retain an optional source hint. Project-local sources
normally use `AssetRelative`, resolved from the physical parent of the owning
`.dasset`; a project-local source whose owner is outside the project uses
`ProjectRelative`; external sources use normalized `Absolute` hints. The base
is stored explicitly and is never guessed from the text. Absolute hints are
intentionally workstation-specific and may need to be reselected on another
machine. None of these hints is asset validity or build authority.

## Scene Sources

Scene supports FBX and glTF/GLB creation. Relative external dependencies are
resolved beside the root source and captured with it. Missing dependencies,
path escapes, collisions, unsupported data, and resource limits are reported
before any peer output is published.

The importer creates only populated type directories beneath the selected
destination, such as `Meshes`, `Materials`, and `Textures`. Sources containing
skins or animation channels are rejected before publication. No generated asset is primary. Scene is
creation-only: import a revised source into a fresh destination; there is no
whole-scene reimport, generated-output repair, or reconciliation workflow.

## Reimport

For Texture2D, TextureCube, VolumeTexture, and standalone
StaticMesh, **Reimport** resolves the complete retained hint set and reuses the
current family import interpretation. The action is available only when every
required role has a hint. **Reimport From File...** is always available for a
successfully loaded supported asset and selects a complete replacement source
set even when no hint exists. Both actions capture once, prepare a detached
candidate, and commit canonical imported data, settings, derived result, and
hints together. A resolution, decode, build, or cancellation failure leaves
both live and persisted state unchanged. A later save failure preserves the
prior canonical DAST v9 main/raw-bulk closure and leaves the complete new live
state Dirty for retry.

These actions are class-aware capabilities supplied by the reflected factory
for the loaded asset; the Content Browser does not maintain a separate family
switch. Panorama TextureCube replacement selects one file, while a six-face
TextureCube replacement selects all six faces before the manager invokes the
handler. Unsupported or ambiguous handlers are reported without mutating the
asset.

Missing or corrupt disposable derived data is rebuilt through the owning
family build system. It is not treated as source repair or generic import
recovery.

Moving or duplicating an asset copies `HintBase + Hint` byte for byte and never
touches the source. `AssetRelative` therefore rebinds from the destination;
`ProjectRelative` and `Absolute` retain their physical meaning. Deleting an
asset does not touch the source. Asset redirectors resolve exact object
identities; they never redirect source hints.

## Diagnostics And Recovery

- **Missing hint:** the asset remains valid and rebuildable; use **Reimport From
  File...** only when new imported content is wanted.
- **Missing file for Reimport:** restore the hinted file or use **Reimport From
  File...** to select and adopt a replacement.
- **Changed file:** reimport explicitly; Durin does not silently rebuild an
  authored asset because a source timestamp changed.
- **External absolute file on another machine:** use **Reimport From File...**
  to select it again. Durin does not guess a mount or copy destination.
- **Read-only source:** import and reimport may read it normally; source writes
  are not part of the workflow.
- **Scene dependency escape:** place the dependency within the accepted root
  closure and update the scene reference, then retry.

For package persistence, dirty state, source metadata stripping, and cooked
runtime behavior, see
[Asset Data Lifecycle And Storage](../../Runtime/Assets/AssetDataLifecycle.md).
For importer ownership and Scene transaction semantics, see
[Asset Import Architecture](../Architecture/AssetImportFramework.md).

Repository ownership and Git LFS behavior are documented in
[Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
