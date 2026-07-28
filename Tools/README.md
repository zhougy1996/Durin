# Repository Tools

Repository-owned developer workflow implementations live under
`DurinDevTool`. The repository-root launcher, `../DevTool.bat`, provides setup,
dependency, worktree, build, test, run, scaffolding, and implementation-plan
commands.

Keep implementation-private data, third-party manifests, and focused tests
beside the owning implementation. Repository-level templates consumed by one
or more workflows live under `../Templates` and are selected through
`DurinDevTool/DevTool.json`. Engine build-time programs and helpers invoked
directly by CMake remain under `Engine`.
