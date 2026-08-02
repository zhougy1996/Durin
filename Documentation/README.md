# Documentation

Use this file only when the task needs repository-specific guidance and the
owning document is not already known. Read the first matching topic below; do
not open the other rows or scan an entire directory.

| Task trigger | Read first |
| --- | --- |
| Repository-owned C++ change | [C++ coding standards](Development/Standards/CodingStandards.md) |
| Configure, build, run, output layout, or interrupted-build recovery | [Build and run](Development/Build/BuildAndRun.md) |
| CMake metadata, target generation, or module binaries | [Build system](Development/Build/BuildSystem.md) |
| Runtime variants or build presets | [Runtime variants](Development/Build/RuntimeVariants.md) |
| Tracy or CPU profiling | [CPU profiling](Development/Build/Profiling.md) |
| IDE code model or debugging | [IDE code model](Development/Tooling/IDECodeModel.md) |
| Dependencies, bootstrap, or worktrees | [Third-party bootstrap](Development/Build/ThirdPartyBootstrap.md) |
| Native tests | [Native tests](Development/Build/NativeTests.md) |
| Workspace, project, module, or runtime-variant ownership | [Workspace projects](Workspace/WorkspaceProjects.md) |
| Runtime physical paths or atomic byte publication | [File I/O](Runtime/Core/FileIO.md) |
| Runtime startup, shutdown, or frame lifecycle | [Runtime lifecycle](Runtime/Core/RuntimeLifecycle.md) |
| CPU tasks, dependencies, cancellation, waiting, or worker-thread ownership | [CPU task system](Runtime/Core/TaskSystem.md) |
| Scene viewport or window-backed rendering | [Viewport rendering](Runtime/Rendering/ViewportRendering.md) |
| Editor architecture, design, or user workflow | Use a targeted search under `Editor/Architecture/`, `Editor/Design/`, or `Editor/Guides/` |
| Bounded implementation task or task selection | Run `.\DevTool.bat doc task list`, then open only the selected task |
| Active implementation plan | Humans run `.\DevTool.bat` for the interactive shell; agents run `.\DevTool.bat doc plan list` for the compact Markdown index, then open only the matching plan |
| Completed plans awaiting monthly archive | Run `.\DevTool.bat doc plan list --scope completed` |
| Named historical plan or required provenance | Run `.\DevTool.bat doc plan list --scope archive --query "<title-or-filename>"`, then open only the selected archived plan |
| Verified unresolved engineering problem | [Open investigations](Investigations/README.md) |

If no row matches, use `rg --files Documentation` or a targeted `rg` content
query. Open only the closest result and follow its direct references only when
the task requires them. Never load `Documentation/` as one corpus or maintain a
repository-wide file index.

Authoring and lifecycle rules are in the nearest `AGENTS.md`.
