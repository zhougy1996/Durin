# [Durin](https://github.com/zhougy1996/Durin)

Durin is a game engine built with C++, CMake, and Vulkan. Windows x64 is the
fully supported development environment. Apple Silicon (arm64) macOS has a
qualified native setup and Debug Editor build path while broader runtime and
product qualification remains in progress. Use the repository-root
`DevTool.bat` entrypoint on Windows or `DevTool` launcher on macOS for setup,
dependencies, worktrees, builds, tests, running, and cleanup.

## Prerequisites

Both platforms require Git, CMake 3.24 or newer, Python 3.10 or newer with
`venv`, the LunarG Vulkan SDK, and network access to GitHub and the Python
Package Index during setup.

### Windows

- Windows 10 version 1607 or newer, or Windows 11, with Win32 long paths enabled
- Visual Studio 2022 17.14 or newer with MSVC Build Tools 14.44+, the
  **Desktop development with C++** workload, an x64 Windows SDK, and the English
  language pack
- LunarG Vulkan SDK with `Include/vulkan/vulkan.h`,
  `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`

DurinDevTool discovers the Visual Studio environment automatically and normally
uses the Ninja bundled with Visual Studio. A separate Ninja installation is not
usually needed.

### macOS

- Apple Silicon (arm64) Mac with full Xcode selected through `xcode-select`
- Arm64 CMake, Ninja, Python, and Git; Homebrew installations are supported
- LunarG Vulkan SDK with the Vulkan SDK core and MoltenVK components

Source the Vulkan SDK's `setup-env.sh` before the initial setup so
`VULKAN_SDK` points to its `macOS` directory. DurinDevTool validates Xcode,
Apple Clang, the macOS SDK, and the arm64 Vulkan/MoltenVK libraries before
preparing dependencies. See [Build and Run](Documentation/Development/Build/BuildAndRun.md#macos-prerequisites)
for the qualified tool versions and full SDK layout.

## First-Time Setup

On Windows, run these commands from PowerShell or Command Prompt:

```powershell
git clone https://github.com/zhougy1996/Durin.git Durin
cd Durin
.\DevTool.bat setup
```

On macOS, run:

```bash
git clone https://github.com/zhougy1996/Durin.git Durin
cd Durin
source /path/to/VulkanSDK/setup-env.sh
./DevTool setup
```

Setup checks the prerequisites, confirms the detected CMake and host toolchain,
creates the repository-local `.venv`, installs the pinned Python packages, and
prepares the repository-managed third-party dependencies. It also
creates missing machine-local configuration from the tracked templates:

- `.agents/DevTool.user.json` for toolchain and build-profile overrides;
- `.vscode/settings.json`, `.vscode/extensions.json`, and a generated
  `.vscode/launch.json` for local editor integration.

Existing local configuration is preserved. Setup is idempotent, so after fixing
a prerequisite or interrupted download, rerun the same command. Scripts and CI
can use `setup --non-interactive` after valid settings are already available or
can be detected automatically.

## Build and Run

Build the complete editor runtime, then run it on Windows:

```powershell
.\DevTool.bat build --target all
.\DevTool.bat run
```

On macOS, use the extensionless launcher. The available preset is
`MacOS-arm64-Debug-DurinEditor`:

```bash
./DevTool status
./DevTool build --target all
```

For the current macOS qualification scope and known runtime limitations, see
the detailed [macOS workflow](Documentation/Development/Build/BuildAndRun.md#macos-workflow).

Useful commands include:

```powershell
.\DevTool.bat presets
.\DevTool.bat status
.\DevTool.bat configure
.\DevTool.bat build --target LevelEditor
.\DevTool.bat test CoreConcurrencyTests
.\DevTool.bat test all
.\DevTool.bat clean
.\DevTool.bat recover
.\DevTool.bat rebuild --target all
.\DevTool.bat purge --preset Win64-Debug-DurinEditor
.\DevTool.bat build --preset Win64-Release-DurinEditor --target all
```

`build` configures automatically when needed and defaults to target `all`.
Native-test targets are excluded from that default; `test` requires an explicit
selection, and `test all` builds and runs all registered native tests.
Run `.\DevTool.bat --help` or `.\DevTool.bat <command> --help` to discover the
complete command set and options. On macOS, substitute `./DevTool` for
`.\DevTool.bat` in the examples above.

Use `--plain` when styled output is not wanted. DurinDevTool selects plain output
automatically for non-interactive terminals and when `NO_COLOR` is set. Complete
child-process logs are retained under `Build/.agent-state/logs/`.

## Interactive Shell

Run DurinDevTool without arguments, or use `shell`, to keep a preset and
toolchain session open across commands:

```powershell
.\DevTool.bat
.\DevTool.bat shell
```

On macOS, use `./DevTool` or `./DevTool shell`.

Shell commands do not need a leading slash. Use commands such as `presets`,
`preset`, `build`, `rebuild`, `test`, `run`, `status`, `help`, and `exit`:

```text
DurinDevTool> presets
DurinDevTool> preset 3
DurinDevTool> build --target all
DurinDevTool> run
DurinDevTool> help test
DurinDevTool> exit
```

The selected preset is session-local. Shell commands accept the same named
options as their direct command forms.

## Interrupted Operations

Do not start another build while an earlier CMake, Ninja, compiler, or linker
process may still be running. After a build operation is cancelled or loses its
controlling DurinDevTool process, wait for that process tree to exit and inspect
the affected preset:

```powershell
.\DevTool.bat status
```

Use `./DevTool status` on macOS.

Only run the recovery command reported by `status`. A `recover required` state
uses `.\DevTool.bat recover` to resume the recorded target incrementally. A
`rebuild required` state reports the appropriate rebuild command. Ordinary
compiler, linker, configuration, test, and runtime failures do not require a
rebuild; fix the reported cause and rerun the same command.

## Git Worktrees

After setup succeeds in the main checkout, use DurinDevTool for the complete
linked-worktree lifecycle:

```powershell
.\DevTool.bat worktree add ..\Durin-feature -b feature-branch
.\DevTool.bat worktree prepare ..\Durin-feature
.\DevTool.bat worktree list
.\DevTool.bat worktree open
.\DevTool.bat worktree remove ..\Durin-feature
```

The same commands are available on macOS through `./DevTool`, using POSIX
paths.

`add` creates and prepares a worktree. `prepare` initializes or repairs an
existing one by linking `.agents`, `.vscode`, `.venv`, and `Engine/External`
from the prepared source worktree. Build and binary directories remain local to
each worktree.

Always use `DevTool worktree remove` for prepared worktrees on Windows. It
validates and detaches the shared NTFS junctions before asking Git to remove the
directory. Direct recursive deletion can follow those junctions into the main
worktree. Use `--dry-run` to preview an operation, and `--force` only when local
changes may be discarded intentionally.

## Documentation

- [Troubleshooting](Documentation/Development/Build/Troubleshooting.md) provides
  symptom-first fixes for common setup, build, worktree, and runtime problems.
- [Build and Run](Documentation/Development/Build/BuildAndRun.md) defines the
  complete development workflow and recovery contract.
- [Third-Party Bootstrap](Documentation/Development/Build/ThirdPartyBootstrap.md)
  covers dependency ownership and preparation.
- [Native Tests](Documentation/Development/Build/NativeTests.md) covers native
  test discovery, filtering, isolation, and lifecycle rules.
- [Documentation index](Documentation/README.md) routes other repository topics.
