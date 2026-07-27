# Repository Tools

Repository-owned developer tool implementations live here. Their stable,
human-facing batch entrypoints remain at the repository root:

- `BuildTool.bat` runs `BuildTool/durin_build_tool`.
- `DocTool.bat` runs `DocTool/durin_doc_tool`.
- `WorktreeTool.bat` remains backed by the shared bootstrap and worktree
  utilities under `Engine/Scripts` because it owns repository preparation.

Keep tool-specific data and templates beside the owning implementation. Keep
engine build helpers that are invoked by CMake or third-party bootstrap under
`Engine/Scripts`. Focused tool tests that do not belong to an engine program
live under `Tests`.
