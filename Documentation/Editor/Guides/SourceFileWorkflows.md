# Source File Workflows

Summary: Explain how built-in importers select, persist, reimport, and diagnose ordinary source files.

Modules: DurinEd, AssetForgeBuiltins

Last reviewed: 2026-08-27

Durin imports from ordinary files selected with the platform file chooser.
Source files do not need to live beneath an Asset mount, and the editor does
not copy them into Content. Import, reimport, package move, duplication, and
deletion never move, replace, or delete a source file.

## Choosing A Source

Content Browser exposes a fixed built-in Import menu:

- **Texture...** creates Texture2D, TextureCube, or VolumeTexture assets;
- **Terrain Heightmap...** creates one TerrainHeightmap;
- **Scene Source (FBX/glTF)...** creates a set of peer scene outputs;
- **Static Mesh (Geometry Only)...** creates one geometry asset without Scene
  material or texture outputs.

Choose a physical file, then choose the asset path or output directory. The
source row is read-only; selecting a different file uses the file chooser
rather than editing a virtual source destination.

Files beneath the active project directory are stored project-relative. Files
elsewhere are stored as normalized absolute paths. Project-relative filenames
are portable with the project checkout. Absolute filenames are intentionally
workstation-specific and may need to be reselected on another machine.

## Scene Sources

Scene supports FBX and glTF/GLB creation. Relative external dependencies are
resolved beside the root source and captured with it. Missing dependencies,
path escapes, collisions, unsupported data, and resource limits are reported
before any peer output is published.

The importer creates only populated type directories beneath the selected
destination, such as `Meshes`, `Materials`, `Textures`, `Skeletons`,
`SkeletalMeshes`, and `Animations`. No generated asset is primary. Scene is
creation-only: import a revised source into a fresh destination; there is no
whole-scene reimport, generated-output repair, or reconciliation workflow.

## Reimport

For Texture2D, TextureCube, VolumeTexture, TerrainHeightmap, and standalone
StaticMesh, **Reimport from Current Source** reads the persisted filename and
current family settings. Reimport prepares and validates replacement state
before publishing it. A decode or build failure leaves the existing asset
unchanged.

Missing or corrupt disposable derived data is rebuilt through the owning
family build system. It is not treated as source repair or generic import
recovery.

Moving or duplicating an asset retains its source filename. Deleting an asset
does not touch the source. Asset redirectors resolve only asset identities;
they never redirect source filenames.

## Diagnostics And Recovery

- **Missing file:** restore the checkout file or external file at its recorded
  filename, or select a new source through the family editor where supported.
- **Changed file:** reimport explicitly; Durin does not silently rebuild an
  authored asset because a source timestamp changed.
- **External absolute file on another machine:** relink or reselect it. Durin
  does not guess a mount or copy destination.
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
