# Build And Run

This document is the primary reference for configuring, building, and running Durin locally.

## Prerequisites

- Run `./Setup.bat` on Windows for initial dependency preparation.
- If present, read `AGENTS_LOCAL.md` for machine-specific tool paths and command examples.
- Run configure and build commands from a Visual Studio developer environment on Windows, or call `VsDevCmd.bat` first.

Recommended bootstrap entrypoints:

- `Engine/Scripts/Bootstrap/Bootstrap.bat`
- `python Engine/Scripts/Bootstrap/setup_third_party.py --all --with-tests`

## Configure

Current main presets:

- `cmake --preset Win64-Debug-DurinEditor`
- `cmake --preset Win64-Debug-DurinEditor-Tests`
- `cmake --preset Win64-Release-DurinEditor`
- `cmake --preset Win64-Debug-DurinGame`
- `cmake --preset Win64-Release-DurinGame`
- `cmake --preset Win64-Shipping-DurinGame`

Main project build trees are profile-specific:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinEditor-Tests`
- `Build/Win64-Debug-DurinGame`
- `Build/Win64-Shipping-DurinGame`

Main Editor/Game presets keep `BUILD_TESTING=OFF`. Use `Win64-Debug-DurinEditor-Tests` when native test targets are needed.

## Build

Prefer building only the target needed for the current change instead of `--target all`.

Examples:

```powershell
cmake --build Build/Win64-Debug-DurinEditor --target DurinLauncher -j 4
cmake --build Build/Win64-Debug-DurinEditor-Tests --target CoreTests -j 4
cmake --build Build/Win64-Debug-DurinEditor --target RenderCore -j 4
```

Use `--target all` only when a full rebuild is actually needed.

## Output Layout

Engine binaries are organized by platform and configuration:

- Runtime launcher and module DLLs: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Runtime third-party DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native test shared binaries: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`
- Native test data and working directories: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/<TestTarget>/`
- Import and static libraries: `Engine/Binaries/<Platform>/<Config>/Lib/<Target>/`
- Debug symbols: `Engine/Binaries/<Platform>/<Config>/Symbols/<Target>/`

Examples for Win64 Debug:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`
- `Engine/Binaries/Win64/Debug/Runtime/DurinGame/DurinGame.exe`
- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor-Core.dll`
- `Engine/Binaries/Win64/Debug/ThirdParty/slang.dll`
- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/CoreTests.exe`

## Run

Run the editor from:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`

The launcher target is `DurinLauncher`, but the output executable name matches the active profile:

- `DurinEditor.exe`
- `DurinGame.exe`

The active profile's app config is copied beside the launcher into the runtime output directory:

- `DurinEditor.yaml`
- `DurinGame.yaml`

At runtime, `Launch.cpp` enters `FEngineLoop`, and `FEngineLoop::Init()` constructs the concrete engine implementation for the active build:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

## Runtime Path Assumptions

Runtime path discovery assumes the executable stays inside the repository-relative binary layout:

- `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

At startup, Durin resolves `Engine/` by walking upward from `LaunchDir()`. Moving the whole repository is fine as long as the relative structure stays intact. Moving only the built runtime tree away from the repository root is not supported by the current path logic.

## Runtime DLL Deployment Notes

- Shared third-party runtime DLLs are deployed to `Engine/Binaries/<Platform>/<Config>/ThirdParty/`.
- `RenderCore` delay-loads `slang.dll`.
- Slang runtime DLLs are currently copied to both:
  - `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
  - `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`

If `RenderCore` fails to load, check both locations first.

## Related Docs

- Profile behavior: `Documentation/Architecture/Profiles.md`
- Third-party bootstrap and dependency layout: `Documentation/Setup/ThirdPartyBootstrap.md`
- Native tests: `Documentation/Setup/NativeTests.md`
- CLion BuildInsight notes: `Documentation/Setup/BuildInsightClion.md`
