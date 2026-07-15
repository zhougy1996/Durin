# Cross-Platform Third-Party Bootstrap

This document covers dependency preparation, shared external layout, and worktree sharing.

## Entry Points

- Primary script: `Engine/Scripts/Bootstrap/setup_third_party.py`
- Windows wrappers:
  - `Engine/Scripts/Bootstrap/Bootstrap.bat`
  - `Engine/Scripts/Bootstrap/Setup_<Library>.bat`
  - `Engine/Scripts/Bootstrap/PrepareWorktree.bat`

Common commands:

```powershell
python Engine/Scripts/Bootstrap/setup_third_party.py --all --with-tests
python Engine/Scripts/Bootstrap/setup_third_party.py --libs glm,spdlog --config Debug
python Engine/Scripts/Bootstrap/setup_third_party.py --validate-manifests
```

## Worktree Sharing

- `PrepareWorktree.bat` links this worktree's `Engine/External` and `.venv` to a prepared dependency worktree.
- The same helper copies `.agents/build-config.json` from that dependency worktree. It skips the write when the files already match.
- If the source worktree has no Agent build config, the helper creates the target config from `Documentation/Setup/TP_AGENT_BUILD_CONFIG.json` without requiring it to be filled in.
- On Windows, shared directories use directory junctions by default; the Agent build config is always a regular per-worktree file.
- Preview the operation with `Engine\Scripts\Bootstrap\PrepareWorktree.bat --dry-run`.
- By default, linked Git worktrees pull those links from the main worktree root.
- Use `--source` when the prepared dependency worktree is not the main worktree root.
- `Setup.bat` switches to `PrepareWorktree.bat` only when the checkout has a valid linked-worktree `.git` pointer; otherwise it falls back to the normal full bootstrap.

Keep `Build/`, `Engine/Intermediate/`, and `Engine/Binaries/` local to each worktree; never junction or share them. Build ownership and IDE rules are documented in `BuildAndRun.md`.

`Documentation/Setup/TP_AGENT_BUILD_CONFIG.json` is the starter template for optional machine-local build overrides. Toolchain definitions and Agent preset selection belong in the tracked Agent Build Profile manifest instead.

## Directory Layout

- Direct source: `Engine/External/Source/<Library>`
- Prebuilt packages: `Engine/External/Packages/<Library>`
- Shared install: `Engine/External/Install/<Platform>/<Config>/<Library>`
- Shared third-party build tree: `Build/ThirdParty/<Platform>-<Config>-<Library>`
- Runtime deployment: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`

`Shipping` main-project builds import shared-install third-party packages from the `Release` install tree.

## Dependency Models

### Prebuilt SDK

Current example: `slang`

- Source location: `Engine/External/Packages/<Library>`
- Build or install behavior: bootstrap downloads or extracts a prepared package
- Runtime implications: consuming modules deploy required runtime DLLs into `Engine/Binaries/...`

### Direct Source

Current examples: `glm`, `googletest`

- Source location: `Engine/External/Source/<Library>`
- Build or install behavior: bootstrap clones source and the main project consumes it with `add_subdirectory(...)`
- Runtime implications: no shared install tree is produced

### Shared Install

Current examples: `spdlog`, `glfw`, `rapidyaml`, `assimp`

- Source location: `Engine/External/Source/<Library>`
- Build or install behavior: bootstrap clones source, configures it, and installs it into the shared install tree
- Runtime implications: the main project imports the installed targets from `Engine/External/Install/...`

## Runtime DLL Notes

- Use `Engine/Binaries/<Platform>/<Config>/ThirdParty/` for shared third-party runtime DLLs.
- If a library is delay-loaded or path-sensitive on Windows, also copy it beside the consuming runtime binary.
- `RenderCore` currently depends on Slang DLL deployment, and those DLLs may need to exist in both the shared third-party directory and the active runtime directory.

## Manifest Model

Third-party manifests live under `Engine/Scripts/Bootstrap/thirdparty/*.json` and declare:

- library identity
- dependency kind
- source acquisition details
- source directory
- wrapper CMake directory when needed
- required-file checks
- per-config install validation for shared-install packages

## Notes

- `--all` skips test-only dependencies unless `--with-tests` is supplied.
- `googletest` is test-only.
- Main project configure and build still start from `CMakePresets.json`.
- Legacy third-party assets can be inspected with `python Engine/Scripts/Bootstrap/cleanup_legacy_thirdparty.py --dry-run`.
