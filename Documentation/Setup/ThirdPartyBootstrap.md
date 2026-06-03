# Cross-Platform Third-Party Bootstrap

This document describes Durin's third-party bootstrap, install, and runtime deployment workflow.

## Goals

- Prepare third-party dependencies before main-project `cmake --preset ...` runs.
- Keep network/download behavior out of normal project configure.
- Reuse the same dependency layout across `DurinEditor` and `DurinGame`.
- Share one bootstrap entrypoint across Windows, macOS, and Linux.

## Entry Points

Primary bootstrap entry:

- `Engine/Scripts/Bootstrap/setup_third_party.py`

Windows convenience wrappers:

- `Engine/Scripts/Bootstrap/Bootstrap.bat`
- `Engine/Scripts/Bootstrap/Setup_<Library>.bat`
- `Engine/Scripts/Bootstrap/PrepareWorktree.bat`

Recommended examples:

```powershell
python Engine/Scripts/Bootstrap/setup_third_party.py --all --with-tests
python Engine/Scripts/Bootstrap/setup_third_party.py --libs glm,spdlog --config Debug
python Engine/Scripts/Bootstrap/setup_third_party.py --validate-manifests
```

## Worktree Sharing

When using Git worktrees, dependency payloads can be shared by linking the ignored
`Engine/External` directory from a prepared sibling `Durin` worktree. The helper
also copies `AGENTS_LOCAL.md` from that sibling worktree:

```powershell
Engine\Scripts\Bootstrap\PrepareWorktree.bat
```

By default, the script links this worktree's `Engine/External` to
`..\Durin\Engine\External`. On Windows, `PrepareWorktree.bat` creates a
directory junction by default. To preview the operation first, run:

```powershell
Engine\Scripts\Bootstrap\PrepareWorktree.bat --dry-run
```

If the prepared dependency worktree is somewhere else, pass either its root or
its `Engine/External` directory with `--source`.

On Windows, root `Setup.bat` only switches to `PrepareWorktree.bat` when Git
explicitly reports a linked worktree. If `.git` is missing, Git is unavailable,
or the Git query fails, `Setup.bat` falls back to the normal full bootstrap.
The detection compares `git rev-parse --git-dir` and
`git rev-parse --git-common-dir` after expanding both to absolute paths.

Keep `Build/`, `Engine/Intermediate/`, and `Engine/Binaries/` per-worktree.
Those directories contain generated state and outputs that can depend on the
current branch and absolute worktree path.

## Directory Layout

- Direct source: `Engine/External/Source/<Library>`
- Prebuilt packages: `Engine/External/Packages/<Library>`
- Shared install: `Engine/External/Install/<Platform>/<Config>/<Library>`
- Shared build: `Build/ThirdParty/<Platform>-<Config>-<Library>`
- Runtime deployment: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`

`Shipping` main-project builds import shared-install third-party packages from the `Release` install tree.

Platform names are normalized as:

- Windows: `Win64`
- macOS: `MacOS`
- Linux: `Linux`

## Dependency Classes

### Prebuilt SDK

Current example:

- `slang`

Behavior:

- bootstrap downloads/extracts a prepared package into `Engine/External/Packages/<Library>`
- main project imports binaries and headers from that prepared source directory
- runtime deployment is handled by consuming module build rules when DLLs must be copied into `Engine/Binaries/...`

### Direct Source

Current examples:

- `glm`
- `googletest`

Behavior:

- bootstrap clones source into `Engine/External/Source/<Library>`
- main project consumes that source directly with `add_subdirectory(...)`
- no shared install tree is generated

### Shared Install

Current examples:

- `spdlog`
- `glfw`
- `rapidyaml`
- `assimp`

Behavior:

- bootstrap clones source into `Engine/External/Source/<Library>`
- bootstrap configures and installs the library into the shared install tree
- main project imports the installed target from `Engine/External/Install/...`

## Runtime Deployment

Runtime deployment is separate from bootstrap and install trees.

Current runtime-only output locations:

- `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`

Use the third-party directory for shared runtime DLLs that should be available through the process DLL search path.

If a library is delay-loaded or sensitive to Windows DLL search behavior, also copy it beside the consuming runtime binary.

Current example:

- `slang.dll`
- `slang-compiler.dll`
- `slang-glslang.dll`

`RenderCore` currently copies those files into both:

- `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`

## Manifest Model

Library definitions live under:

- `Engine/Scripts/Bootstrap/thirdparty/*.json`

Each manifest declares:

- library name
- dependency kind
- source acquisition details
- source directory
- third-party CMake wrapper directory if needed
- required file checks
- per-config install validation for shared-install libraries

## Notes

- `--all` skips test-only dependencies unless `--with-tests` is supplied.
- `googletest` is marked test-only.
- The root `CMakePresets.json` remains the entry for main-project configure/build.
- Third-party bootstrap is intentionally separate from those presets.
- Legacy assets can be cleaned with `python Engine/Scripts/Bootstrap/cleanup_legacy_thirdparty.py --dry-run` and then rerun without `--dry-run`.
