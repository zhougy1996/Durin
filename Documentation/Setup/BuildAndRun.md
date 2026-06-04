# Build And Run

This is the operational guide for configuring, building, and running Durin locally.

## Prerequisites

- Run `.\Setup.bat` on Windows before the first configure in a worktree.
- Run configure and build commands from a Visual Studio developer environment on Windows, or call `VsDevCmd.bat` first.
- If present, use `AGENTS_LOCAL.md` only for machine-local tool paths and command variants.

## Configure

Common presets:

- `cmake --preset Win64-Debug-DurinEditor`
- `cmake --preset Win64-Debug-DurinEditor-Tests`
- `cmake --preset Win64-Debug-DurinGame`
- `cmake --preset Win64-Shipping-DurinGame`

Main build trees stay profile-specific under `Build/`, for example:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinEditor-Tests`

Use the `*-Tests` preset when native test targets are needed. Normal editor/game presets keep `BUILD_TESTING=OFF`.

## Build

Prefer building only the target needed for the current change instead of `--target all`.

```powershell
cmake --build Build/Win64-Debug-DurinEditor --target DurinLauncher -j 4
cmake --build Build/Win64-Debug-DurinEditor --target RenderCore -j 4
cmake --build Build/Win64-Debug-DurinEditor-Tests --target CoreTests -j 4
```

## Run And Output Layout

Run the editor from:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`

Key output locations:

- Runtime launcher and module DLLs: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Shared runtime third-party DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

The launcher target is `DurinLauncher`, but the executable name matches the active profile such as `DurinEditor.exe` or `DurinGame.exe`.

Runtime path discovery assumes the executable remains inside the repository-relative output layout above.

## Runtime DLL Troubleshooting

- Shared third-party runtime DLLs are deployed to `Engine/Binaries/<Platform>/<Config>/ThirdParty/`.
- `RenderCore` delay-loads `slang.dll`; if startup fails, check both `ThirdParty/` and the active runtime directory first.
- Moving only the built runtime tree away from the repository root is not supported by the current path logic.

## Related Docs

- `Documentation/Setup/ThirdPartyBootstrap.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
