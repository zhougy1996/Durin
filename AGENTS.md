# AGENTS.md

This is the repository entrypoint for Codex-style agents. Read only the docs
that match the task at hand.

## Start Here

- Build, run, third-party bootstrap, and native tests:
  `Documentation/Setup/BuildAndRun.md`,
  `Documentation/Setup/ThirdPartyBootstrap.md`,
  `Documentation/Setup/NativeTests.md`
- Build metadata, profiles, and workspace structure:
  `Documentation/Architecture/BuildSystem.md`,
  `Documentation/Architecture/Profiles.md`,
  `Documentation/Architecture/WorkspaceProjects.md`
- Runtime boot flow and subsystem relationships:
  `Documentation/Architecture/RuntimeArchitecture.md`
- Machine-local notes:
  `AGENTS_LOCAL.md` when present; it is not team policy and is shared across worktrees by setup.

## Repository Rules

- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI`, `VulkanRHI`, and sometimes `MonaImGuiBackend`.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
- `CoreStd.h` already supplies the common standard-library headers used across the codebase; do not add new STL includes unless the compiler requires one in that translation unit.

## When To Open Which Doc

- Open `BuildAndRun.md` for local setup, configure, build, run, and output layout.
- Open `ThirdPartyBootstrap.md` for dependency preparation, worktree sharing, and external layout.
- Open `NativeTests.md` for test presets, test execution, and adding native test targets.
- Open `BuildSystem.md` for generated metadata flow and the CMake entrypoints that own it.
- Open `Profiles.md` for profile semantics, presets, compile definitions, and adding a new profile.
- Open `WorkspaceProjects.md` for workspace/project/module/profile boundaries.
- Open `RuntimeArchitecture.md` for startup flow, module loading, and render/UI subsystem ownership.
