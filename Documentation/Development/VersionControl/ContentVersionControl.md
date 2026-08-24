# Content Version Control

Durin keeps authored content reproducible alongside the source revision that consumes it. Do not ignore an entire `Content` directory. Store Durin packages and small metadata in Git, large source assets in Git LFS, and leave regenerable output out of version control.

## Storage Policy

| Content | Storage | Reason |
| --- | --- | --- |
| `.dasset` packages and levels | Git | Runtime asset identity and object state must match the code revision. The files are marked binary. |
| Authored `.dabulk` companions | Git LFS | Immutable generation-named authored payload bytes remain outside the ordinary Git object database. |
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
`/Libraries/StudioArt/Textures/Stone.png`. The mount maps both source and asset
identities through one configured content directory; no package stores a
physical directory prefix, absolute checkout path, or link target. Source art
is authoring input rather than a runtime object asset. Moving or
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
  StaticMeshes/     # Versioned .dasset packages
  Models/           # Shared LFS-backed model authoring inputs
  Textures/         # Shared texture authoring inputs
DerivedDataCache/   # Ignored, rebuildable
Saved/              # Ignored, editor-local state
Cooked/             # Ignored, distribution output
```

Storage is selected by file type in the repository `.gitattributes`, not by
directory name. Plugin and manually scanned external mounts apply the same
policy in their own repositories: `.dasset` stays in ordinary Git, while large
model, image, audio, and font inputs use the matching LFS rules.

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

### Codex Sandbox LFS Storage

Codex `workspace-write` sessions protect the repository `.git` directory
recursively. Git LFS stores objects and temporary files under `.git/lfs` by
default, so a read-only command such as `git status` can fail intermittently
when the LFS clean filter needs to create a temporary file. Adding
`.git/lfs/tmp` as a Codex writable root does not override the recursive `.git`
protection.

Developers who use Codex with this repository should move the workstation-local
LFS storage to a directory beside the checkout and grant Codex write access to
that directory. Run the following once from any Durin worktree in a normal
PowerShell session, outside the Codex sandbox:

```powershell
$repoRoot = (git rev-parse --show-toplevel)
$repoName = Split-Path -Leaf $repoRoot
$storageRoot = Join-Path (Split-Path -Parent $repoRoot) "$repoName-LfsStorage"
$commonGitDir = [System.IO.Path]::GetFullPath(
    (git rev-parse --git-common-dir),
    $repoRoot)
$existingLfsStorage = Join-Path $commonGitDir "lfs"

New-Item -ItemType Directory -Force -Path $storageRoot | Out-Null
if (Test-Path -LiteralPath $existingLfsStorage) {
    Get-ChildItem -LiteralPath $existingLfsStorage -Force |
        Copy-Item -Destination $storageRoot -Recurse -Force
}

$gitStoragePath = $storageRoot.Replace("\", "/")
git config --local lfs.storage $gitStoragePath
```

The local Git configuration is shared by linked worktrees, so all worktrees for
this clone use the same external LFS storage. Do not point unrelated clones or
repositories at the same directory. Avoid `git lfs prune` against shared custom
storage unless every worktree and ref that depends on it has been considered.

Add the resolved `$storageRoot` path to the user-level
`%USERPROFILE%\.codex\config.toml`. Merge the setting into an existing
`[sandbox_workspace_write]` table rather than declaring that table twice:

```toml
sandbox_mode = "workspace-write"

[sandbox_workspace_write]
writable_roots = ["D:\\Path\\To\\Durin-LfsStorage"]
```

Restart Codex or start a new task so the updated sandbox configuration is
loaded, then verify the effective paths and repository state:

```powershell
git lfs env | Select-String "LocalMediaDir|TempDir"
git status --short
```

`LocalMediaDir` and `TempDir` must resolve under the external storage directory,
and `git status --short` must complete without an LFS permission error. If the
cache was not copied, or some required objects are absent, run `git lfs pull`
from a normal network-enabled shell.

See the
[Codex protected-path policy](https://learn.chatgpt.com/docs/agent-approvals-security#protected-paths-in-writable-roots)
and the
[Git LFS `lfs.storage` reference](https://github.com/git-lfs/git-lfs/blob/main/docs/man/git-lfs-config.adoc)
for the underlying constraints.

If an optional source checkout is absent, the mount remains unavailable and
warm DDC-backed editor loading may still succeed. Restore the declared checkout
or link and fetch its LFS objects; do not edit the descriptor to a workstation
absolute path. Read-only mounts remain valid
reference and reimport sources. Ingest, replacement, and relocation must target
an authorized writable mount instead.

Before committing, use these checks:

```powershell
git lfs ls-files
git check-attr filter diff merge text -- Engine/Content/Models/Editor/MaterialPreview/Box.obj
git status
```

## Adding Asset Types

When a new large asset extension is introduced, add an explicit LFS rule to `.gitattributes` in the same change as the first asset. Prefer extension-based policy over individual file rules so every project behaves consistently.

Do not place `.dasset` under LFS by default. Packages are currently compact,
and keeping them in normal Git makes ordinary engine and level changes
self-contained. Explicit DAST v5 packages retain external DABK v1 companions,
so the route does not change this split: `.dasset` remains ordinary Git and
`.dabulk` remains LFS. A submit must include the package and every newly
referenced companion generation. Revisit `.dasset` LFS only if a separately
qualified route begins embedding large render data.

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
