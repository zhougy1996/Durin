# AGENTS.md

This is the repository entrypoint for Codex-style agents. Read only the docs
that match the task at hand.

## Task Documentation

- Local setup, configure/build/run, and outputs: `Documentation/Setup/BuildAndRun.md`; dependencies and worktree sharing: `Documentation/Setup/ThirdPartyBootstrap.md`; native tests: `Documentation/Setup/NativeTests.md`.
- Generated build metadata and CMake ownership: `Documentation/Architecture/BuildSystem.md`; profiles and presets: `Documentation/Architecture/Profiles.md`; workspace/project/module boundaries: `Documentation/Architecture/WorkspaceProjects.md`; startup, modules, rendering, and UI ownership: `Documentation/Architecture/RuntimeArchitecture.md`.
- Machine-local build overrides belong in `.agents/build-config.json`. Run `Setup.bat` to create it when needed; it is optional machine data, not team policy.

## Repository Rules

- Each checkout has one source/build writer; concurrent workflows require separate worktrees. Use root-level `BuildTool.bat` for all Agent `configure`, `build`, `clean`, `purge`, `rebuild`, and `test` actions on Windows, or `Engine/Scripts/Build/durin_build_tool/__main__.py` on other hosts.
- An IDE observing an Agent-owned checkout uses `Win64-Debug-DurinEditor-FastConfigure` for code model and debugging only. Set `VSLANG=1033`, remove build-before-launch, never invoke IDE build actions, and do not Configure/Reload while `BuildTool` is active.
- Treat every `BuildTool configure`, `build`, `clean`, `purge`, `rebuild`, and `test` invocation as long-running. When the shell tool exposes `timeout_ms`, set it to at least `3600000` (longer for a full build); never use the tool default. A tool `yield`/cell ID only means the invocation is still running: repeatedly wait on that same cell, using waits of at most 60 seconds, until it returns a final exit result. Do not launch a replacement command, inspect recovery state, or infer interruption merely from quiet output, a yield, or an elapsed UI window. Run `rebuild --target all` only when the shell's final result explicitly says the original command was terminated or timed out and the interruption marker remains after its process tree exits.
- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI` and `VulkanRHI`. Validate UI or rendering changes by building and running `DurinEditor`; a runtime smoke test requires a successful full `all` build of the same preset first. Agents may run the editor produced in their owned checkout.
- `CoreStd.h` already supplies the common standard-library headers used across the codebase; do not add new STL includes unless the compiler requires one in that translation unit.
- Preserve or add concise comments where non-obvious reasoning, constraints, invariants, or tradeoffs materially shape the design. Comments should explain why the design exists and what must remain true; do not merely restate the code. Do not remove such comments during refactoring unless the underlying design no longer applies, and update them whenever that design changes.

## Agent Handoff

- After completing a substantial change, include exactly one proposed commit subject in the final response. A change is substantial when it alters behavior, architecture, build flow, or several related files; trivial inspection or advice-only tasks do not require one.
- Format it as `<type>(<scope>): <imperative summary>` with a short lowercase scope, no trailing period, and preferably a `feat`, `fix`, `refactor`, `perf`, `build`, `test`, `docs`, or `chore` type. Describe the shared user-visible or architectural outcome, not the edits; use an optional short body for related details, or recommend separate commits when no coherent subject exists. Example: `refactor(rhi): centralize swapchain lifetime management`.
- When a change has been validated with a successful full build, the final response may include a direct local link to the resulting executable so it can be launched conveniently. Confirm that the linked artifact exists and belongs to the Agent Build Profile used for validation. A partial or single-target build does not require an executable link.
