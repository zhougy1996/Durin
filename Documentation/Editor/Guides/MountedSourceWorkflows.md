# Mounted Source Workflows

StaticMesh, Texture2D, and TextureCube assets keep source provenance as a
complete virtual `FSourcePath`. Asset packages and authoring files resolve
through the same mount `GetContentDir()`. Their typed identities remain
distinct even when a source file and `.dasset` coexist in one directory.

## Import

Choose one source mode:

- **Reference Existing Source** selects a file already inside a registered
  mount content directory. The referencing asset's mount must depend on the
  source mount. Import records the virtual path and does not copy or rename the
  file.
- **Ingest External Source** selects a file outside every registered source
  mount. Choose a writable target mount and complete virtual destination.
  Equal existing bytes are reused; a different collision is rejected. A failed
  build or publication rolls back the staged copy and package.

Import captures the selected source and its provider-declared dependency
closure into one immutable snapshot. A single-output import stores lightweight
provenance on that asset. A multi-output import publishes peer assets plus an
editor-only `DImportRecord`; selecting an output does not make it the owner of
the others. See [Asset Import Framework](../Architecture/AssetImportFramework.md).

Use **Scene Source (FBX/glTF)** for supported scene documents. Choose one output
directory; the importer creates only populated type directories beneath it,
currently `Meshes`, `Materials`, and `Textures`, and stores
`<SourceName>_Import` at the output root. No generated Mesh is treated as the
primary asset.

Use **Static Mesh (Geometry Only)** to create one StaticMesh from OBJ or another
supported model format without creating scene materials or textures. FBX and
glTF may use either workflow: choose Static Mesh for one geometry asset, or
Scene Source when the complete multi-output scene relationship is required.

Import dialogs share one source-layout convention within the destination
mount. A single model defaults to `Sources/Models/<Name>.<ext>` and a Texture2D
to `Sources/Textures/<Name>.<ext>`. Bundle-like inputs receive a stable source
directory: Scene Source uses `Sources/Models/<Scene>/<File>` and TextureCube
uses `Sources/Textures/<Asset>/<File>`. Single-output imports suggest one asset
in the current Content Browser directory; Scene Source suggests one output
directory there. Changing an automatically suggested destination updates its
dependent source suggestions, while a manually edited path is preserved.

The form displays the asset or output-directory destination and source
destination separately,
along with mount identity, dependency status, availability, containment, and
write policy. A Game asset may reference an Engine or declared plugin/library
source. An Engine asset may not reference Game. Ordinary project authoring
cannot mutate Engine source; an explicit editor import whose asset or output
destination is inside `/Engine/` runs in Engine-authoring context and may
ingest its sources into the same writable Engine mount.

## Existing Assets

- **Reimport** reads the persisted source and current settings. It never copies,
  renames, replaces, or otherwise writes source files.
- **Change Source Reference** rebuilds one asset against another allowed
  mounted source and moves no file.
- **Repair Source Path** is explicit recovery for a missing or changed
  reference; it validates the replacement and its persisted hash.
- **Replace Shared Source** changes source bytes in place only after write
  authorization and a complete affected-asset preview.
- **Relocate Shared Source** stages the destination, updates every clean
  affected package, and removes the original only after all saves succeed.
  Failure restores package bytes and removes the staged destination.

Moving, duplicating, or deleting an asset package never implicitly moves or
deletes its source. Duplication retains the same source path.

## Diagnostics And Recovery

Unavailable content directories, forbidden dependencies,
read-only policy, containment escapes, missing files, and changed hashes are
different conditions and are reported separately. A package with valid warm
derived data may remain usable while its source mount is unavailable.

Restore a missing project-relative checkout or link at the path declared by the
project descriptor, then retry or repair. Authoring sources belong beneath the
selected mount's effective content directory; do not replace a committed mount
with a workstation absolute path.
For a read-only source, keep the no-copy reference or ingest into another
authorized writable mount before changing the asset.

Repository ownership, Git LFS, and linked-checkout policy are documented in
[Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
