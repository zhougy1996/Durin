# Content Version Control

Last reviewed: 2026-08-30

Durin keeps authored content reproducible alongside the source revision that consumes it. Do not ignore an entire `Content` directory. Store Durin packages and small metadata in Git, large source assets in Git LFS, and leave regenerable output out of version control.

## Storage Policy

| Content | Storage | Reason |
| --- | --- | --- |
| `.dasset` packages and levels | Git | Runtime asset identity and object state must match the code revision. The files are marked binary. |
| Authored raw `.dbulk` segments | Git LFS | Stable-name external canonical fields remain outside the ordinary Git object database. |
| Models, textures, audio, and fonts | Git LFS | These files are commonly large, binary, or produce noisy diffs. LFS avoids copying every revision into the main Git object database. |
| Small text metadata and import settings | Git | They are reviewable and should evolve with their assets. |
| `DerivedDataCache`, `Cooked`, and `Saved` | Ignored | These directories contain rebuildable or machine-local output. |

`Cooked` is reproducible and therefore not source-controlled, but its contents
are required distribution artifacts for a particular staged build. They must
not be treated like disposable DDC entries while that build is installed or
running. See [Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md)
for the `.dasset`, DDC `.bin`, and cooked `.dbulk` boundaries.

Standalone imported assets store their decoder-free canonical imported data in
the `.dasset` plus optional raw `.dbulk` closure. Optional schema-2 source hints are explicitly
`AssetRelative`, `ProjectRelative`, or `Absolute` physical paths used only by
explicit Reimport. They are not asset identities, DDC keys, or rebuild
authority. Source art is optional authoring input after a successful package
save. Moving, duplicating, or deleting a package never moves or deletes
potentially shared source art.

Repository packages use concrete family import-data schema 2. Retired mounted
source, filename-only, and source-backed recovery schemas have no current
reader; the repository corpus must remain canonical-resaved before those
compatibility routes are removed.

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

Project-relative junctions or symbolic links may provide a source checkout
whose location differs by workstation. Commit the source repository revision
or submodule ownership, never an arbitrary resolved link target. Git does not
version the contents behind a local link, so the team must define which
repository owns those files and how its revision is acquired.

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

If an optional source checkout is absent, authored assets still load, edit,
rebuild after a cold DDC, and Cook from their validated authored package
closure. Only explicit
Reimport through a hint becomes unavailable. Restore the checkout and its LFS
objects or use Reimport From File to select a replacement. Read-only physical
sources remain valid import and reimport inputs because Durin never writes
them.

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
self-contained. Ordinary DAST v7 packages keep large authored fields in a raw
`.dbulk`; `.dasset` remains ordinary Git and the segment remains LFS. A submit
must include the package and every newly referenced stable companion. Hidden
`.dbulk.durin-backup` and atomic temporary files are
transaction state and must never be submitted. Revisit
`.dasset` LFS only if a separately qualified route begins embedding large
render data.

The package version and Payload Directory, not a suffix scan, select the
canonical closure. Review migration or resave reports and submit the `.dasset`
and its LFS-backed `.dbulk` together whenever either changes. Never submit only
the package or only the companion side.

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
