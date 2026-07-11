# Content Version Control

Durin keeps authored content reproducible alongside the source revision that consumes it. Do not ignore an entire `Content` directory. Store Durin packages and small metadata in Git, large source assets in Git LFS, and leave regenerable output out of version control.

## Storage Policy

| Content | Storage | Reason |
| --- | --- | --- |
| `.dasset` packages and levels | Git | Runtime asset identity and object state must match the code revision. The files are marked binary. |
| Models, textures, audio, and fonts | Git LFS | These files are commonly large, binary, or produce noisy diffs. LFS avoids copying every revision into the main Git object database. |
| Small text metadata and import settings | Git | They are reviewable and should evolve with their assets. |
| `DerivedDataCache`, `Cooked`, and `Saved` | Ignored | These directories contain rebuildable or machine-local output. |

Static-mesh `.dasset` files currently record a project-relative source path, while CPU and GPU render data is rebuilt from the copied Content source. Therefore, commit the referenced source model together with its `.dasset`; committing only the package does not produce a reproducible checkout.

## Directory Convention

Projects should use a layout similar to:

```text
Content/
  Levels/           # Versioned .dasset packages
  Materials/        # Versioned .dasset packages
  StaticMeshes/     # Versioned .dasset packages
  SourceAssets/     # LFS-backed models, textures, audio, and fonts
DerivedDataCache/   # Ignored, rebuildable
Saved/              # Ignored, editor-local state
Cooked/             # Ignored, distribution output
```

Existing specialized source directories such as `Content/SourceMeshes` may remain in use. Storage is selected by file type in the repository `.gitattributes`, not by directory name.

## Workstation Setup

Install Git LFS before checking out content, then initialize it for the user account:

```powershell
git lfs install
git lfs pull
```

`git lfs install` is normally required only once per workstation. A clone made without Git LFS may contain small pointer files instead of the actual assets; install LFS and run `git lfs pull` to populate them.

Before committing, use these checks:

```powershell
git lfs ls-files
git check-attr filter diff merge text -- Engine/Content/SourceMeshes/teapot.obj
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
