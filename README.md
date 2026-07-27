# Durin

Durin is a game engine project built with C++, CMake, and Vulkan. The primary
development environment is currently Windows x64. After cloning the repository,
use DurinDevTool for setup, dependency, worktree, build, test, run, and cleanup
operations.

## Prerequisites

- Windows 10 or 11 x64
- Git
- Python 3.10 or newer, with the Python Launcher enabled or Python added to `PATH`
- Visual Studio 2022 17.14 or newer with MSVC Build Tools 14.44+, the **Desktop development with C++** workload, and the English language pack
- CMake 3.24 or newer
- LunarG Vulkan SDK with `Include/vulkan/vulkan.h`, `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`
- Network access to GitHub and the Python Package Index

The build driver discovers the Visual Studio environment automatically and
prefers the Ninja bundled with Visual Studio, so a separate Ninja installation
is normally unnecessary. DurinDevTool enforces `VSLANG=1033` and verifies that
MSVC emits English diagnostics; this keeps CMake and Ninja dependency parsing
consistent in both interactive terminals and Agent output pipes.

## First-Time Setup

Run the following commands from PowerShell or Command Prompt:

```powershell
git clone <repository-url> Durin
cd Durin
.\Tools\DurinDevTool\DevTool.bat setup
```

`DevTool setup` performs the following steps:

1. Creates the optional machine-local Agent build configuration at `.agents/build-config.json` when it does not already exist.
2. Checks all readily detectable prerequisites, including tool versions and the required Vulkan SDK files.
3. Creates the repository-local `.venv` using the system Python installation.
4. Installs the pinned dependencies from `requirements.txt`, including the `clang.cindex` bindings, native `libclang` library required by DurinHeaderTool, and Rich terminal support used by DurinDevTool.
5. Downloads and prepares third-party dependencies including glm, spdlog, glfw, rapidyaml, assimp, Slang, and googletest. Vulkan Memory Allocator is supplied by the Vulkan SDK and is not downloaded separately.

Setup is idempotent, never overwrites an existing local Agent build
configuration, and reuses dependencies that are already prepared. If
prerequisite detection cannot find a machine-specific CMake or Visual Studio
environment, edit `.agents/build-config.json` and run `DevTool setup` again. If
a download is interrupted, restore network access and rerun the same command.
After setup succeeds, dependency-backed commands use the Python interpreter from
`.venv`, preventing mismatches between the Python bindings and libclang.

## Build and Run

After setup, build the complete editor runtime:

```powershell
.\Tools\DurinDevTool\DevTool.bat build --target all
```

Run the editor:

```powershell
.\Tools\DurinDevTool\DevTool.bat run
```

Common commands:

```powershell
.\Tools\DurinDevTool\DevTool.bat configure
.\Tools\DurinDevTool\DevTool.bat build --target LevelEditor
.\Tools\DurinDevTool\DevTool.bat test --target CoreTests
.\Tools\DurinDevTool\DevTool.bat clean
.\Tools\DurinDevTool\DevTool.bat purge --preset Win64-Debug-DurinEditor-Tests
.\Tools\DurinDevTool\DevTool.bat purge --all-presets
.\Tools\DurinDevTool\DevTool.bat rebuild --target all
.\Tools\DurinDevTool\DevTool.bat build --preset Win64-Release-DurinEditor --target all
```

Run DurinDevTool without arguments, or pass `shell`, to enter the interactive
command shell:

```powershell
.\Tools\DurinDevTool\DevTool.bat
.\Tools\DurinDevTool\DevTool.bat shell
```

Inside the shell, use `/presets`, `/preset`, `/build`, `/rebuild`, `/test`, `/run`, `/status`, `/help`, and `/exit`. The selected preset is session-local. If a build or test operation is interrupted, do not resume with an incremental build. Wait for the previous process tree to exit, then run `rebuild --target all` for the affected preset.

Use `--plain` when output is redirected or consumed by tooling and ANSI styling
is not wanted. DurinDevTool also selects plain output automatically for
non-interactive terminals and when `NO_COLOR` is set. Child output defaults to
`--output auto`: interactive terminals update routine Ninja progress in place,
while redirected and Agent invocations show compact stage/results output.
Complete logs are saved under `Build/.agent-state/logs/`. Pass
`--output progress`, `--output full`, or `--output compact` to override that
selection; progress falls back to compact without an interactive terminal.

## Git Worktrees

After the main worktree has completed `DevTool setup`, use the `worktree`
command family for the complete linked-worktree lifecycle:

```powershell
.\Tools\DurinDevTool\DevTool.bat worktree add ..\Durin-feature -b feature-branch
.\Tools\DurinDevTool\DevTool.bat worktree prepare ..\Durin-feature
.\Tools\DurinDevTool\DevTool.bat worktree list
.\Tools\DurinDevTool\DevTool.bat worktree remove ..\Durin-feature
```

Use `worktree open` to open every registered worktree in Windows Terminal, with
up to four panes per tab:

```powershell
.\Tools\DurinDevTool\DevTool.bat worktree open
.\Tools\DurinDevTool\DevTool.bat worktree open --dry-run
```

`add` creates the Git worktree and prepares it automatically. `prepare` is also
available explicitly for worktrees created manually, for repairs, and for
previewing changes with `--dry-run`. Preparation links `.agents`,
`Engine/External`, and `.venv` from the main worktree, so machine-local Agent
configuration and prepared dependencies are shared immediately.
`Build/`, `Engine/Intermediate/`, and `Engine/Binaries` remain local to each
worktree.

`DevTool setup` initializes only the main checkout. Running it from a linked
worktree reports an error and directs the caller to `DevTool worktree prepare`.

Always use `DevTool worktree remove` for prepared worktrees on Windows. It verifies
that the target is a registered linked worktree, refuses the main or a locked
worktree, checks for unexpected directory links, and detaches the three shared
NTFS junctions before asking Git to remove the directory. Direct recursive
deletion can traverse those junctions and delete the corresponding directories
in the main worktree. Pass `--force` only when modified and untracked files may
be discarded, or `--dry-run` to validate without changing anything.

When upgrading an existing worktree that already has a real `.agents` directory,
DurinDevTool preserves it as `.agents.pre-link-backup` before creating the link.
If the prepared dependencies are stored in a non-default worktree, use
`DevTool worktree prepare --source <prepared-worktree>`.

## Troubleshooting

- **Python was not found:** Install Python 3.10 or newer, verify that `py -3 --version` or `python --version` works, and rerun `DevTool setup`.
- **Setup cannot find CMake or the Visual Studio environment:** Set the corresponding override in `.agents/build-config.json` and rerun `DevTool setup`; the existing configuration is preserved.
- **`.venv` uses an outdated or incorrect Python:** Remove `.venv` and rerun `DevTool setup`. Do not mix system site-packages into the virtual environment.
- **`clang.cindex` or libclang is missing:** Rerun `DevTool setup`. The required version is managed by the root `requirements.txt`.
- **DurinDevTool reports a missing virtual environment:** Run `DevTool setup` in the main checkout, or `DevTool worktree prepare` in a linked worktree.
- **Setup reports an old MSVC toolset:** Update Visual Studio 2022 to 17.14 or newer and install MSVC Build Tools 14.44 or newer.
- **Setup reports a missing `vk_mem_alloc.h`:** Update the Vulkan SDK, or download VulkanMemoryAllocator's `vk_mem_alloc.h` and place it under the SDK's `Include/vma` directory.
- **DurinDevTool reports localized MSVC diagnostics:** Add the English language pack through Visual Studio Installer, then rerun the command. Existing Ninja trees with a non-English dependency prefix are refreshed automatically by the next `configure`, `build`, or `test` operation.
- **A third-party source directory exists but is incomplete:** Move or repair the directory identified by the error, then rerun setup.

For more information, see [Build and Run](Documentation/Development/Build/BuildAndRun.md), [Third-Party Bootstrap](Documentation/Development/Build/ThirdPartyBootstrap.md), and [Native Tests](Documentation/Development/Build/NativeTests.md).
