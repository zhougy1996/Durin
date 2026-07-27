# Content Version Control

Durin keeps authored content reproducible alongside the source revision that consumes it. Do not ignore an entire `Content` directory. Store Durin packages and small metadata in Git, large source assets in Git LFS, and leave regenerable output out of version control.

## Storage Policy

| Content | Storage | Reason |
| --- | --- | --- |
| `.dasset` packages and levels | Git | Runtime asset identity and object state must match the code revision. The files are marked binary. |
| Models, textures, audio, and fonts | Git LFS | These files are commonly large, binary, or produce noisy diffs. LFS avoids copying every revision into the main Git object database. |
| Small text metadata and import settings | Git | They are reviewable and should evolve with their assets. |
| `DerivedDataCache`, `Cooked`, and `Saved` | Ignored | These directories contain rebuildable or machine-local output. |

`Cooked` is reproducible and therefore not source-controlled, but its contents
are required distribution artifacts for a particular staged build. They must
not be treated like disposable DDC entries while that build is installed or
running. See [Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md)
for the `.dasset`, DDC `.bin`, and cooked `.dbulk` boundaries.

StaticMesh, Texture2D, and TextureCube `.dasset` files record complete mounted
source paths such as `/Game/Models/Chair.fbx` or
`/Libraries/StudioArt/Textures/Stone.png`. The mount selects a SourceAssets
domain; no package stores `SourceAssets/`, an absolute checkout path, or a link
target. Source art is authoring input rather than runtime Content. Moving or
deleting a package does not implicitly move or delete potentially shared source
art.

Repository packages use only reflected `FSourcePath` provenance. Packages that
retain the former string carrier are rejected; migrate them with an engine
revision that still supports the old format before upgrading.

## Directory Convention

Projects should use a layout similar to:

```text
Content/
  Levels/           # Versioned .dasset packages
  Materials/        # Versioned .dasset packages
  StaticMeshes/     # Versioned .dasset packages; no source models are required here
SourceAssets/
  Models/           # Shared LFS-backed model authoring inputs; not content-mounted
  Textures/         # Shared texture authoring inputs
DerivedDataCache/   # Ignored, rebuildable
Saved/              # Ignored, editor-local state
Cooked/             # Ignored, distribution output
```

Storage is selected by file type in the repository `.gitattributes`, not by
directory name. Plugin and source-only mounts apply the same policy in their
own repositories: `.dasset` stays in ordinary Git, while large model, image,
audio, and font inputs use the matching LFS rules.

Project-relative junctions or symbolic links may provide a declared mount root
whose checkout location differs by workstation. Commit the descriptor path and
the source repository revision or submodule ownership, never the resolved
absolute target. Git does not version the contents behind an arbitrary local
link, so the team must define which repository owns those files and how its
revision is acquired.

## Workstation Setup

Install Git LFS before checking out content, then initialize it for the user account:

```powershell
git lfs install
git lfs pull
```

`git lfs install` is normally required only once per workstation. A clone made without Git LFS may contain small pointer files instead of the actual assets; install LFS and run `git lfs pull` to populate them.

If an optional source checkout is absent, the mount remains unavailable and
warm DDC-backed editor loading may still succeed. Restore the declared checkout
or link and fetch its LFS objects; do not copy files into Content or edit the
descriptor to a workstation absolute path. Read-only mounts remain valid
reference and reimport sources. Ingest, replacement, and relocation must target
an authorized writable mount instead.

Before committing, use these checks:

```powershell
git lfs ls-files
git check-attr filter diff merge text -- Engine/SourceAssets/Models/teapot.obj
git status
```

## Adding Asset Types

When a new large asset extension is introduced, add an explicit LFS rule to `.gitattributes` in the same change as the first asset. Prefer extension-based policy over individual file rules so every project behaves consistently.

Do not place `.dasset` under LFS by default. Packages are currently compact, and keeping them in normal Git makes ordinary engine and level changes self-contained. Revisit that decision if package payloads begin embedding large render data.

## Existing Repository Files

Changing `.gitattributes` does not rewrite history. After these rules are introduced, re-adding an existing matching file converts its current revision to an LFS pointer. Make that conversion in a dedicated commit so reviewers can distinguish storage migration from content edits:

```powershell
git add --renormalize Engine/Content
git lfs ls-files
git status
```

This conversion affects the current tree only. `git lfs migrate import` rewrites repository history and changes commit IDs; do not run it on a shared branch without an explicit team migration plan.

## Binary Coordination

Git cannot merge most assets or `.dasset` packages semantically. Keep asset changes small and avoid editing the same package concurrently. If concurrent binary conflicts become frequent, enable an LFS locking workflow for the affected extensions or evaluate an asset-oriented version-control system such as Perforce rather than moving all content out of version control.
