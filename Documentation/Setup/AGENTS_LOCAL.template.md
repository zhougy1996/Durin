# Local machine config

This template is for a gitignored `AGENTS_LOCAL.md` file in the repository root. Keep the real file machine-local and avoid putting team-wide instructions here.

## Tool paths

- Preferred CMake: `D:\Programs\JetBrains\CLion 2025.2.3\bin\cmake\win\x64\bin\cmake.exe`
- Visual Studio env script: `D:\Programs\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat`

## Preferred build trees

- Main editor preset: `Win64-Debug-DurinEditor`
- Main editor build directory: `Build/Win64-Debug-DurinEditor`
- Main game preset: `Win64-Debug-DurinGame`
- Main game build directory: `Build/Win64-Debug-DurinGame`
- Example third-party build directory: `Build/ThirdParty/Win64-Debug-assimp`
- Example third-party install directory: `Engine/External/Install/Win64/Debug/assimp`

## Windows build environment

- Run configure and build commands from `x64 Native Tools Command Prompt for VS 2022`, or invoke `VsDevCmd.bat` first.
- `cl.exe` may appear on `PATH` outside that environment, but Windows SDK and standard library include paths are not reliable there.

## Known working commands

- Configure editor:
  `& "<cmake path above>" --preset Win64-Debug-DurinEditor`
- Configure game:
  `& "<cmake path above>" --preset Win64-Debug-DurinGame`
- Build a specific target:
  `& "<cmake path above>" --build "<main editor build dir above>" --target MonaCore -j 18`
- Prefer target-focused builds over `--target all`.
- Use `--target all` only when a full rebuild is actually needed.

## Codex notes

- In the Codex shell, writes under `Build/`, `Engine/External/Install/`, or other generated output directories may require escalated execution even though source-file edits in the workspace do not.
- If a build is run from Codex, prefer entering the Visual Studio environment first and then invoking the chosen CMake binary.

## Scope

- Keep the real `AGENTS_LOCAL.md` focused on machine-local paths, environment quirks, and verified non-portable command examples.
- Move reusable setup guidance into `Documentation/Setup/*.md` so the repository stays the source of truth.
