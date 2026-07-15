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
- Machine-local Agent build overrides:
  `.agents/build-config.json`; run `Setup.bat`, `Engine/Scripts/Bootstrap/InitializeAgentConfig.bat`, or the cross-platform `initialize_agent_config.py` to create it from the tracked template when missing. It is optional machine data, not team policy.

## Repository Rules

- Agents must run in a dedicated checkout/worktree. On Windows, all Agent Configure, Build, Clean, Rebuild, and Test actions must use the root-level `BuildTool.bat`; it establishes the stable MSVC environment before forwarding to `Engine/Scripts/Build/agent_build.py`. On other hosts, invoke the Python driver directly. The worktree owns its build tree, generated metadata, and binary outputs. Do not run an Agent build in a checkout concurrently owned by a human or another workflow.
- Long Agent builds must be allowed to keep running and observed by waiting on the existing process. Do not use short command timeouts to sample build progress or start a replacement build while the previous `cmake`, `ninja`, compiler, or linker process tree may still be running.
- If an Agent build is interrupted, confirm that its old process tree has exited, then recover with `.\BuildTool Rebuild --target all` on Windows or `python Engine/Scripts/Build/agent_build.py Rebuild --target all` on other hosts. Do not resume with an ordinary incremental build. The driver records interrupted operations and rejects unsafe incremental Build/Test actions until a Rebuild succeeds.
- Generated metadata lives under the current worktree's `Engine/Intermediate/Build/<Platform>/<Profile>/`. DHT serializes commands that share the same build identifier, platform, and profile, but this does not make concurrent CMake or Ninja operations on one build tree safe. Do not bypass the registered Agent workflow inside an Agent-owned worktree.
- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI`, `VulkanRHI`.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling. A runtime smoke test requires a successful full `all` build of the same Agent Build Profile first; a partial or single-target build is not sufficient. Agents are authorized to execute the editor executable produced in their own worktree for validation.
- `CoreStd.h` already supplies the common standard-library headers used across the codebase; do not add new STL includes unless the compiler requires one in that translation unit.
- Preserve or add concise comments where non-obvious reasoning, constraints, invariants, or tradeoffs materially shape the design. Comments should explain why the design exists and what must remain true; do not merely restate the code. Do not remove such comments during refactoring unless the underlying design no longer applies, and update them whenever that design changes.

## Agent Handoff

- After completing a substantial change, include exactly one proposed commit subject in the final response. A change is substantial when it alters behavior, architecture, build flow, or several related files; trivial inspection or advice-only tasks do not require one.
- Format the subject as `<type>(<scope>): <imperative summary>`, using a short lowercase scope and an imperative summary with no trailing period. Prefer one of `feat`, `fix`, `refactor`, `perf`, `build`, `test`, `docs`, or `chore` for `type`.
- Keep the subject concise and describe the user-visible or architectural outcome rather than the editing process. Example: `refactor(rhi): centralize swapchain lifetime management`.
- When a commit contains several related changes, use one subject that summarizes their shared outcome and optionally follow it with a short bullet-list body describing the individual results. Do not enumerate every edit in the subject. If the changes have no coherent shared outcome, recommend splitting them into separate commits instead.
- When a change has been validated with a successful full build, the final response may include a direct local link to the resulting executable so it can be launched conveniently. Confirm that the linked artifact exists and belongs to the Agent Build Profile used for validation. A partial or single-target build does not require an executable link.

## When To Open Which Doc

- Open `BuildAndRun.md` for local setup, configure, build, run, and output layout.
- Open `ThirdPartyBootstrap.md` for dependency preparation, worktree sharing, and external layout.
- Open `NativeTests.md` for test presets, test execution, and adding native test targets.
- Open `BuildSystem.md` for generated metadata flow and the CMake entrypoints that own it.
- Open `Profiles.md` for profile semantics, presets, compile definitions, and adding a new profile.
- Open `WorkspaceProjects.md` for workspace/project/module/profile boundaries.
- Open `RuntimeArchitecture.md` for startup flow, module loading, and render/UI subsystem ownership.
