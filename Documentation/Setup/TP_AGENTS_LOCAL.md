# Third-Party / Local Agent Notes Template

Use this as a minimal starting point for a machine-local `AGENTS_LOCAL.md`.
Keep the real file gitignored in the repository root and replace placeholders
with paths that exist on your machine.

## Tool paths

- Preferred CMake: `<cmake-path>`
- Visual Studio env script: `<vsdevcmd-path>`

## Agent build command

- Entry point: `Engine/Scripts/Build/AgentBuild.ps1`
- Usage: `& "Engine/Scripts/Build/AgentBuild.ps1" <Configure|Build|Test> [-Target <name>] [-Jobs <count>] [-Filter <gtest-filter>]`
- The script always uses the isolated Agent preset, build tree, and `Debug-Agent` outputs.

## Notes

- The Agent build script reads the tool paths above and initializes the Visual Studio developer environment automatically.
- Keep this file limited to machine-local paths, environment quirks, and verified non-portable commands.
