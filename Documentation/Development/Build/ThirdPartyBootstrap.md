# Third-Party Dependency Preparation

This document covers dependency preparation, shared external layout, and worktree sharing.

## Entry Points

DurinDevTool owns dependency preparation, manifest validation, setup, and
linked-worktree preparation.

Common commands:

```powershell
.\DevTool.bat dependency prepare --all --with-tests
.\DevTool.bat dependency prepare --all --with-development
.\DevTool.bat dependency prepare --libs tracy,tracy-tools
.\DevTool.bat dependency prepare --libs glm,spdlog --config Debug
.\DevTool.bat dependency validate
```

For a fresh Windows clone, use
`.\DevTool.bat setup`. It creates `.venv`, installs
`requirements.txt`, and prepares test and development dependencies through the
same dependency service.

## Worktree Sharing

- `DevTool worktree prepare` links a linked worktree's `Engine/External` and `.venv` to a prepared dependency worktree.
- The same command links the complete `.agents` directory from that dependency worktree, so machine-local configuration and helper changes are shared immediately.
- When migrating an existing worktree with a real non-empty `.agents` directory, the helper preserves it as `.agents.pre-link-backup` before creating the link.
- On Windows, all three shared directories use directory junctions by default; `.agents/build-config.json` remains a regular file in the source worktree and is reached through that shared directory.
- Preview the operation with `DevTool worktree prepare --dry-run`.
- By default, linked Git worktrees pull those links from the main worktree root.
- Use `--source` when the prepared dependency worktree is not the main worktree root.
- `DevTool setup` initializes only the main checkout and reports an error when invoked from a linked worktree.
- `DevTool worktree add` creates and prepares a linked worktree;
  `DevTool worktree open` opens terminals for every registered worktree.
- `DevTool worktree remove` is the required Windows removal path. It refuses the
  main worktree, locked worktrees, and unexpected directory links, then detaches
  the three shared junctions before invoking Git. Direct recursive deletion can
  follow a junction and remove the shared source directory outside the target
  worktree.

Keep `Build/`, `Engine/Intermediate/`, and `Engine/Binaries/` local to each worktree; never junction or share them. Build ownership and IDE rules are documented in `BuildAndRun.md`.

`Templates/DurinDevTool/build-config.json` is the starter template for optional
machine-local build overrides. Repository paths, enabled command groups, and the
Agent Build Profile manifest location belong in the tracked
`Tools/DurinDevTool/DevTool.json` configuration instead.

## Directory Layout

- Direct source: `Engine/External/Source/<Library>`
- Prebuilt packages: `Engine/External/Packages/<Library>`
- Versioned development tools:
  `Engine/External/Packages/<Tool>/<Version>/<Platform>`
- Shared install: `Engine/External/Install/<Platform>/<Config>/<Library>`
- Shared third-party build tree: `Build/ThirdParty/<Platform>-<Config>-<Library>`
- Runtime deployment: `Engine/Binaries/<Platform>/ThirdParty/<Config>/`

`Shipping` main-project builds import shared-install third-party packages from the `Release` install tree.

## Dependency Models

### Prebuilt SDK

Current example: `slang`

- Source location: `Engine/External/Packages/<Library>`
- Build or install behavior: bootstrap downloads or extracts a prepared package
- Runtime implications: consuming modules deploy required runtime DLLs into `Engine/Binaries/...`

### Direct Source

Current examples: `glm`, `bc7enc_rdo`, `googletest`, `tracy`

- Source location: `Engine/External/Source/<Library>`
- Build or install behavior: bootstrap clones source and the main project consumes it with `add_subdirectory(...)`
- Runtime implications: no shared install tree is produced

`bc7enc_rdo` has no release tags, so its manifest pins an exact upstream commit.
Git sources may define exactly one `tag` or `commit`; commit-pinned sources are
fetched shallowly and checked out detached for reproducible bootstrap results.

### Tool Package

Current example: `tracy-tools`

- Tool packages contain development executables and are not linkable engine
  dependencies.
- Tracy `v0.13.1` Windows tools are installed at
  `Engine/External/Packages/tracy-tools/0.13.1/Win64/`.
- A former manual copy beneath `Build/Tools/Tracy-0.13.1/` is obsolete after
  managed preparation. Bootstrap does not search, migrate, or delete that
  disposable build-tree location.
- Archive platform entries may pin a SHA-256 digest. Bootstrap verifies it
  after download and before extraction, and reports the expected and actual
  digests without publishing a package directory on mismatch.
- A tool package may explicitly allow unsupported host platforms. Such a
  package is skipped with a status message while the remaining selected
  dependencies continue.
- The editor profiling status UI reports Tracy platform support, resolved
  paths, required and missing files, preparation state, version, and the
  focused repair command from the manifests.

### Shared Install

Current examples: `spdlog`, `glfw`, `rapidyaml`, `assimp`

- Source location: `Engine/External/Source/<Library>`
- Build or install behavior: bootstrap clones source, configures it, and installs it into the shared install tree
- Runtime implications: the main project imports the installed targets from `Engine/External/Install/...`

## Runtime DLL Notes

- Use `Engine/Binaries/<Platform>/ThirdParty/<Config>/` for shared third-party
  runtime DLLs. Preset roles such as Profiling share this directory with the
  matching ordinary CMake configuration.
- If a library is delay-loaded or path-sensitive on Windows, also copy it beside the consuming runtime binary.
- `RenderCore` currently depends on Slang DLL deployment, and those DLLs may need to exist in both the shared third-party directory and the active runtime directory.

## Manifest Model

Third-party manifests live under
`Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/*.json` and declare:

- library identity
- dependency kind
- source acquisition details
- source directory
- wrapper CMake directory when needed
- required-file checks
- per-config install validation for shared-install packages
- optional `test_only` and `development_only` selection flags
- optional archive-platform `sha256` integrity pins
- optional `allow_unsupported_platform` behavior for host-specific tools
- optional tool `version` and `repair_command` status metadata

## Notes

- `--all` skips test-only dependencies unless `--with-tests` is supplied.
- `--all` skips development-only dependencies unless `--with-development` is
  supplied. Explicit `--libs <name>` selection remains available.
- `DevTool setup` prepares all ordinary dependencies and supplies the test and
  development selections so a fully prepared checkout includes both dependency
  classes by default.
- `googletest` is test-only.
- Tracy `v0.13.1` source and matching Win64 host tools are development-only and
  licensed under BSD-3-Clause. Setup prepares both by default; use
  `DevTool dependency prepare --libs tracy,tracy-tools` to prepare or repair
  them together.
  The upstream binary archive does not include its license file; the prepared
  Tracy source retains `Engine/External/Source/tracy/LICENSE`, and repository
  documentation accompanying the managed download retains the license and
  copyright reference required for binary redistribution.
- Vulkan Memory Allocator is supplied by the Vulkan SDK rather than this bootstrap. See `BuildAndRun.md` for the required SDK layout and the older-SDK fallback.
- Main project configure and build still start from `CMakePresets.json`.
