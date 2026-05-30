# Local machine config

This file is intended as a template for a gitignored `LOCAL_ENV.md` file in the repository root.

## CMake

- Use: `D:\Programs\JetBrains\CLion 2025.2.3\bin\cmake\win\x64\bin\cmake.exe`
- Preferred editor build directory: `\Build\Win64-Debug-DurinEditor`
- Third-party build directory: `\Build\ThirdParty\Win64-Debug-assimp`
- Third-party install directory: `\Engine\External\Install\Win64\Debug\assimp`

## Visual Studio environment

- Run configure and build commands from `x64 Native Tools Command Prompt for VS 2022`, or call:
  `D:\Programs\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat`
- `cl.exe` may be discoverable outside that environment, but standard library and SDK include paths are not reliable there.

## Build

- Prefer building only the target needed for the current change instead of `--target all`.
- Example target-focused build commands:
  `& "<cmake path above>" --build "<build dir above>" --target MonaCore -j 18`
- Example configure presets:
  `& "<cmake path above>" --preset Win64-Debug-DurinEditor`
  `& "<cmake path above>" --preset Win64-Debug-DurinGame`
- Use `--target all` only when a full rebuild is actually needed.
- Verified working from a shell initialized by `VsDevCmd.bat`.

## Codex notes

- In the Codex shell, writing to `\Build\Win64-Debug-DurinEditor`, `\Build\ThirdParty\...`, or `\Engine\External\...` may require escalated execution even though source-file edits in the workspace do not.
- If a build is run from Codex, prefer invoking `VsDevCmd.bat` first and then calling the CLion CMake binary.

## Notes

- Keep the real `LOCAL_ENV.md` limited to machine-local paths, environment quirks, and non-portable commands.
