# Third-Party / Local Agent Notes Template

Use this as a minimal starting point for a machine-local `AGENTS_LOCAL.md`.
Keep the real file gitignored in the repository root and replace placeholders
with paths that exist on your machine.

## Tool paths

- Preferred CMake: `<cmake-path>`
- Visual Studio env script: `<vsdevcmd-path>`

## Preferred presets

- Main editor preset: `Win64-Debug-DurinEditor`
- Main game preset: `Win64-Debug-DurinGame`

## Known working commands

- Configure editor:
  `& "<cmake-path>" --preset Win64-Debug-DurinEditor`
- Build a specific target:
  `& "<cmake-path>" --build "Build/Win64-Debug-DurinEditor" --target MonaCore -j <jobs>`

## Notes

- Run configure/build commands from a VS developer environment, or invoke `<vsdevcmd-path>` first.
- Keep this file limited to machine-local paths, environment quirks, and verified non-portable commands.
