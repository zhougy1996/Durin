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

- Agents must use `Engine/Scripts/Build/agent_build.py` for editor builds and automated validation; `AgentBuild.ps1` is the Windows compatibility wrapper. The selected registered Agent Build Profile owns its isolated preset, build tree, and binary outputs; do not configure, clean, build, or overwrite human-owned non-Agent build trees or outputs.
- Long Agent builds must be allowed to keep running and observed by waiting on the existing process. Do not use short command timeouts to sample build progress or start a replacement build while the previous `cmake`, `ninja`, compiler, or linker process tree may still be running.
- If an Agent build is interrupted, confirm that its old process tree has exited, then recover with `python Engine/Scripts/Build/agent_build.py Rebuild --target all`. Do not resume with an ordinary incremental build. The driver records interrupted operations and rejects unsafe incremental Build/Test actions until a Rebuild succeeds.
- Build trees and binary outputs are isolated by Agent Build Profile, but DHT metadata under `Engine/Intermediate/Build/<Platform>/<Profile>/` is currently shared by presets using the same platform and runtime profile. Do not concurrently configure or build such presets, including Agent and human `DurinEditor` presets.
- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI`, `VulkanRHI`.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling. A runtime smoke test requires a successful full `all` build of the same Agent Build Profile first; a partial or single-target build is not sufficient. Agents are authorized to execute the editor executable produced by that isolated Agent Build Profile for validation, but must not run or overwrite human-owned outputs.
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
