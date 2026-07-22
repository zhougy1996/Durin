# Durin

Durin is a game engine project built with C++, CMake, and Vulkan. The primary development environment is currently Windows x64. After cloning the repository, run the root setup script and use the root BuildTool wrapper for all configure, build, test, and cleanup operations.

## Prerequisites

- Windows 10 or 11 x64
- Git
- Python 3.10 or newer, with the Python Launcher enabled or Python added to `PATH`
- Visual Studio 2022 17.14 or newer with MSVC Build Tools 14.44+, the **Desktop development with C++** workload, and the English language pack
- CMake 3.24 or newer
- LunarG Vulkan SDK with `Include/vulkan/vulkan.h`, `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`
- Network access to GitHub and the Python Package Index

The build driver discovers the Visual Studio environment automatically and prefers the Ninja bundled with Visual Studio, so a separate Ninja installation is normally unnecessary. BuildTool enforces `VSLANG=1033` and verifies that MSVC emits English diagnostics; this keeps CMake and Ninja dependency parsing consistent in both interactive terminals and Agent output pipes.

## First-Time Setup

Run the following commands from PowerShell or Command Prompt:

```powershell
git clone <repository-url> Durin
cd Durin
.\Setup.bat
```

`Setup.bat` performs the following steps:

1. Checks all readily detectable prerequisites before modifying the checkout, including tool versions and the required Vulkan SDK files.
2. Creates the repository-local `.venv` using the system Python installation.
3. Installs the pinned dependencies from `requirements.txt`, including the `clang.cindex` bindings, native `libclang` library required by DurinHeaderTool, and Rich terminal support used by BuildTool.
4. Creates the optional machine-local Agent build configuration at `.agents/build-config.json`.
5. Downloads and prepares third-party dependencies including glm, spdlog, glfw, rapidyaml, assimp, Slang, and googletest. Vulkan Memory Allocator is supplied by the Vulkan SDK and is not downloaded separately.

Setup is idempotent and reuses dependencies that are already prepared. If a download is interrupted, restore network access and run `Setup.bat` again. After setup succeeds, all build commands use the Python interpreter from `.venv`, preventing mismatches between the Python bindings and libclang.

## Build and Run

After setup, build the complete editor runtime:

```powershell
.\BuildTool build --target all
```

Run the editor:

```powershell
.\BuildTool run
```

Common commands:

```powershell
.\BuildTool configure
.\BuildTool build --target LevelEditor
.\BuildTool test --target CoreTests
.\BuildTool clean
.\BuildTool purge --preset Win64-Debug-DurinEditor-Tests
.\BuildTool purge --all-presets
.\BuildTool rebuild --target all
.\BuildTool build --preset Win64-Release-DurinEditor --target all
```

Run BuildTool without arguments, or pass `shell`, to enter the interactive command shell:

```powershell
.\BuildTool
.\BuildTool shell
```

Inside the shell, use `/presets`, `/preset`, `/build`, `/rebuild`, `/test`, `/run`, `/status`, `/help`, and `/exit`. The selected preset is session-local. If a build or test operation is interrupted, do not resume with an incremental build. Wait for the previous process tree to exit, then run `rebuild --target all` for the affected preset.

Use `--plain` when BuildTool output is redirected or consumed by tooling and ANSI styling is not wanted. BuildTool also selects plain output automatically for non-interactive terminals and when `NO_COLOR` is set.

## New Git Worktrees

After the main worktree has completed `Setup.bat`, run the same command once in every new linked worktree:

```powershell
.\Setup.bat
```

The script shares `Engine/External` and `.venv` from the main worktree. `Build`, `Engine/Intermediate`, and `Engine/Binaries` always remain local to each worktree. If the prepared dependencies are stored in a non-default worktree, use `Engine\Scripts\Bootstrap\PrepareWorktree.bat --source <prepared-worktree>`.

## Troubleshooting

- **Python was not found:** Install Python 3.10 or newer, verify that `py -3 --version` or `python --version` works, and rerun `Setup.bat`.
- **`.venv` uses an outdated or incorrect Python:** Remove `.venv` and rerun `Setup.bat`. Do not mix system site-packages into the virtual environment.
- **`clang.cindex` or libclang is missing:** Rerun `Setup.bat`. The required version is managed by the root `requirements.txt`.
- **BuildTool reports a missing virtual environment:** Setup has not completed; run `Setup.bat` first.
- **Setup reports an old MSVC toolset:** Update Visual Studio 2022 to 17.14 or newer and install MSVC Build Tools 14.44 or newer.
- **Setup reports a missing `vk_mem_alloc.h`:** Update the Vulkan SDK, or download VulkanMemoryAllocator's `vk_mem_alloc.h` and place it under the SDK's `Include/vma` directory.
- **BuildTool reports localized MSVC diagnostics:** Add the English language pack through Visual Studio Installer, then rerun BuildTool. Existing Ninja trees with a non-English dependency prefix are refreshed automatically by the next `configure`, `build`, or `test` operation.
- **A third-party source directory exists but is incomplete:** Move or repair the directory identified by the error, then rerun setup.

For more information, see [Build and Run](Documentation/Setup/BuildAndRun.md), [Third-Party Bootstrap](Documentation/Setup/ThirdPartyBootstrap.md), and [Native Tests](Documentation/Setup/NativeTests.md).
