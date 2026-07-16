# Durin

Durin is a game engine project built with C++, CMake, and Vulkan. The primary development environment is currently Windows x64. After cloning the repository, run the root setup script before invoking CMake directly.

## Prerequisites

- Windows 10 or 11 x64
- Git
- Python 3.10 or newer, with the Python Launcher enabled or Python added to `PATH`
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake
- Vulkan SDK
- Network access to GitHub and the Python Package Index

The build driver discovers the Visual Studio environment automatically and prefers the Ninja bundled with Visual Studio, so a separate Ninja installation is normally unnecessary.

## First-Time Setup

Run the following commands from PowerShell or Command Prompt:

```powershell
git clone <repository-url> Durin
cd Durin
.\Setup.bat
```

`Setup.bat` performs the following steps:

1. Creates the repository-local `.venv` using the system Python installation.
2. Installs the pinned dependencies from `requirements.txt`, including the `clang.cindex` bindings and native `libclang` library required by DurinHeaderTool.
3. Creates the optional machine-local Agent build configuration at `.agents/build-config.json`.
4. Downloads and prepares third-party dependencies including glm, spdlog, glfw, rapidyaml, assimp, Slang, and googletest.

Setup is idempotent and reuses dependencies that are already prepared. If a download is interrupted, restore network access and run `Setup.bat` again. After setup succeeds, all build commands use the Python interpreter from `.venv`, preventing mismatches between the Python bindings and libclang.

## Build and Run

After setup, build the complete editor runtime:

```powershell
.\BuildTool.bat build --target all
```

Run the editor:

```powershell
& ".\Engine\Binaries\Win64\Debug\Runtime\DurinEditor\DurinEditor.exe"
```

Common commands:

```powershell
.\BuildTool.bat configure
.\BuildTool.bat build --target LevelEditor
.\BuildTool.bat test --target CoreTests
.\BuildTool.bat clean
.\BuildTool.bat rebuild --target all
.\BuildTool.bat build --preset Win64-Release-DurinEditor --target all
```

Run `.\BuildTool.bat` without arguments to enter the interactive shell, then use `/presets`, `/preset`, `/build`, `/test`, and `/help`. If a build or test operation is interrupted, do not resume with an incremental build. Wait for the previous process tree to exit, then run `rebuild --target all` for the affected preset.

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
- **`BuildTool.bat` reports a missing virtual environment:** Setup has not completed; run `Setup.bat` first.
- **A third-party source directory exists but is incomplete:** Move or repair the directory identified by the error, then rerun setup.

For more information, see [Build and Run](Documentation/Setup/BuildAndRun.md), [Third-Party Bootstrap](Documentation/Setup/ThirdPartyBootstrap.md), and [Native Tests](Documentation/Setup/NativeTests.md).
