# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Documentation

- Setup, build, run, and outputs: `Documentation/Setup/BuildAndRun.md`; dependencies/worktrees: `Documentation/Setup/ThirdPartyBootstrap.md`; native tests: `Documentation/Setup/NativeTests.md`.
- Build metadata/CMake: `Documentation/Architecture/BuildSystem.md`; profiles/presets: `Documentation/Architecture/Profiles.md`; workspace boundaries: `Documentation/Architecture/WorkspaceProjects.md`; runtime/rendering/UI: `Documentation/Architecture/RuntimeArchitecture.md`.
- TODO plan index, required structure, and writing rules: `Documentation/Todo/README.md`. Read this entrypoint before creating or restructuring a TODO plan; do not sample unrelated TODO files to infer the format.
- Machine-local build overrides belong in optional `.agents/build-config.json`; create it with `Setup.bat` when needed.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency. Run all Agent build operations through root `BuildTool.bat` on Windows or `Engine/Scripts/Build/durin_build_tool/__main__.py` elsewhere.
- IDEs observing an Agent checkout use `Win64-Debug-DurinEditor-FastConfigure` only for code model/debugging. Set `VSLANG=1033`, disable build-before-launch, never build from the IDE, and do not Configure/Reload while `BuildTool` runs.
- Treat `BuildTool build` and `rebuild` as long-running: set `timeout_ms >= 600,000` (longer for full builds) and wait on the same yielded cell in intervals of at most 60 seconds until its final result. Quiet output or a yield is not an interruption. Run `rebuild --target all` only after the final result reports termination/timeout and the interruption marker remains after the process tree exits.
- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI` and `VulkanRHI`. Validate UI/rendering changes by building and running `DurinEditor`; smoke tests require a successful full `all` build of the same preset. On Windows, use PowerShell `Start-Process -WindowStyle Hidden -PassThru`, monitor it, then stop it after verification.
- `CoreStd.h` supplies common STL headers; add one only when the translation unit requires it.
- Preserve concise comments that explain non-obvious constraints, invariants, or tradeoffs; update or remove them only when the design changes.

## Agent Handoff

- After completing and validating a functional change, or generating/updating documentation, directly issue one command request that stages only the task files and creates one commit. Use the command execution approval as the user's authorization; do not ask separately in conversation and do not commit if approval is denied. Inspection-only, advice-only, and unchanged tasks need no request.
- Use one subject: `<type>(<scope>): <imperative summary>`, with a short lowercase scope, no trailing period, and preferably `feat`, `fix`, `refactor`, `perf`, `build`, `test`, `docs`, or `chore`. Describe the outcome, not file edits. Example: `refactor(rhi): centralize swapchain lifetime management`.
- After a successful full build, the final response may link the verified executable from the same Agent Build Profile; partial builds need no link.
