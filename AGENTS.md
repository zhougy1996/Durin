# AGENTS.md

Repository entrypoint for Codex-style agents. Read only task-relevant docs.

## Task Documentation

- Setup, build, run, and outputs: `Documentation/Setup/BuildAndRun.md`; IDE code models/debugging: `Documentation/Setup/IDECodeModel.md`; dependencies/worktrees: `Documentation/Setup/ThirdPartyBootstrap.md`; native tests: `Documentation/Setup/NativeTests.md`.
- Build metadata/CMake: `Documentation/Architecture/BuildSystem.md`; profiles/presets: `Documentation/Architecture/Profiles.md`; workspace boundaries: `Documentation/Architecture/WorkspaceProjects.md`; runtime/rendering/UI: `Documentation/Architecture/RuntimeArchitecture.md`.
- Plan index, required structure, and writing rules: `Documentation/Plans/README.md`. Read this entrypoint before creating or restructuring an implementation plan; do not sample unrelated plan files to infer the format.
- Open engineering issue index and issue-writing rules: `Documentation/Issues/README.md` and `Documentation/Issues/AGENTS.md`.
- Machine-local build overrides belong in optional `.agents/build-config.json`; create it with `Setup.bat` when needed.

## Repository Rules

- Each checkout has one source/build writer; use separate worktrees for concurrency. Run all Agent build operations through root `BuildTool.bat` on Windows or `Engine/Scripts/Build/durin_build_tool/__main__.py` elsewhere.
- IDEs observing an Agent checkout use `Win64-Debug-DurinEditor-FastConfigure` only for code model/debugging. Set `VSLANG=1033`, disable build-before-launch, never build from the IDE, and do not Configure/Reload while `BuildTool` runs.
- Treat `BuildTool build` and `rebuild` as long-running: set `timeout_ms >= 600,000` (longer for a measured full build). Call `wait` only when the command explicitly yields a running cell ID, and wait on that same cell in intervals of at most 60 seconds. Once `exec` or `wait` returns a final exit result, stop waiting and act on it. BuildTool prints a 30-second heartbeat while a child command is still alive; quiet output or a yielded cell alone is not an interruption.
- Run `rebuild --target all` only when a configure/build process was interrupted or externally terminated, its process tree has exited, and BuildTool still reports a recovery marker. Ordinary compiler/linker errors, failed assertions, test timeouts, and runtime exits do not require `rebuild`; fix the cause and rerun the same `build` or `test` command.
- Generated metadata is part of the source of truth. If a module looks incomplete, inspect `Engine/Intermediate/Build/...` and DHT outputs before assuming files are missing.
- Runtime-loaded module binaries must keep the `<Profile>-<Module>` naming convention from `CMake/Project/ProjectTargets.cmake`.
- Rendering changes usually span `RHI` and `VulkanRHI`. Validate UI/rendering changes by building and running `DurinEditor`; smoke tests require a successful full `all` build of the same preset. Runtime smoke tests must pass `--hidden-window` so no editor or secondary viewport window is displayed; `Start-Process -WindowStyle Hidden` alone is insufficient. For an interactive shell run, use `BuildTool run --args --hidden-window` and stop it with Ctrl+C. For a timed Windows smoke test, use PowerShell `Start-Process -ArgumentList '--hidden-window' -WindowStyle Hidden -PassThru`, retain the returned process, monitor it, then stop it after verification.
- `CoreStd.h` supplies common STL headers; add one only when the translation unit requires it.
- Preserve concise comments that explain non-obvious constraints, invariants, or tradeoffs; update or remove them only when the design changes.

## Agent Handoff

- After completing and validating a functional change, or generating/updating documentation, directly issue one command request that stages only the task files and creates one commit. Use the command execution approval as the user's authorization; do not ask separately in conversation and do not commit if approval is denied. Inspection-only, advice-only, and unchanged tasks need no request.
- Use one subject: `<type>(<scope>): <imperative summary>`, with a short lowercase scope, no trailing period, and preferably `feat`, `fix`, `refactor`, `perf`, `build`, `test`, `docs`, or `chore`. Describe the outcome, not file edits. Example: `refactor(rhi): centralize swapchain lifetime management`.
- Add a concise commit body that records the delivered behavior or documentation outcome, the important implementation details or constraints, and the validation performed. Prefer a few short paragraphs or bullets over a file-by-file change list; omit only sections that genuinely do not apply.
- When the task implements or updates an active implementation plan, end the commit body with explicit plan provenance using `Plan: Documentation/Plans/<Plan>.md` and `Stage: Stage <N>: <stage title>`. Use the exact plan path and stage heading, list multiple stages when the commit spans them, and update the plan's status/checklists in the same commit when required. If no task-relevant plan exists, do not invent a Plan or Stage reference.
- After a successful full build, the final response may link the verified executable from the same Agent Build Profile; partial builds need no link.
