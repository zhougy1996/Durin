# Agent Build And Run Workflow

Read this short guide before configuring, building, rebuilding, running, or
recovering repository targets. Once read for the current task, do not reread it
unless the file changes.

## Routine Commands

Use DurinDevTool from the repository root and select the smallest target that
validates the change:

```powershell
.\DevTool.bat configure
.\DevTool.bat build --target <Target>
.\DevTool.bat build
.\DevTool.bat run [arguments...]
.\DevTool.bat status
```

`build` defaults to target `all`; prefer `--target <Target>` when a smaller
target provides sufficient validation. Use `configure --fresh`, `clean`,
`rebuild`, or `purge` only when their specific behavior is required, not as a
routine response to a compiler, linker, configuration, or runtime failure.

Machine-local tool and build-profile overrides belong in
`.agents/DevTool.user.json`. If it is missing or setup is required, use
`.\DevTool.bat setup` in the main checkout or follow the complete guide for a
linked worktree.

Treat configure, build, rebuild, and any command that invokes them transitively
as long-running. Give the execution tool at least 10 minutes
(`timeout_ms: 600000`), and at least one hour (`timeout_ms: 3600000`) for a full
`all` build or rebuild.

If execution yields a running process or cell ID, wait on that same invocation
in intervals no longer than 60 seconds. Stop waiting after its final result.
Quiet output, a heartbeat, a yield, or elapsed UI time does not authorize a
second build or recovery inspection.

## Recovery

Never start another build while an earlier CMake, Ninja, compiler, or linker
process tree may still be running. Use recovery only after an operation was
cancelled, externally terminated, or lost its controlling DurinDevTool process:

1. Wait for the old process tree to exit.
2. Run `.\DevTool.bat status`.
3. If the state is `recover required`, run the reported `recover` command.
4. If the state is `rebuild required`, run the reported rebuild command.
5. If the state is `clean`, fix the original failure and rerun the same command.

Ordinary compiler, linker, configuration, clean, test, assertion, timeout,
process, and application failures do not require recovery or rebuilding.

For a user-visible editor change, complete a successful full `all` build before
handoff and link the verified editor executable from the same Agent Build
Profile. Other changes need no executable link after a partial build.

## Read the Complete Guide

Continue to [Build And Run](../Development/Build/BuildAndRun.md) only when the
task changes or diagnoses setup, toolchain, presets, worktrees, build ownership,
DevTool behavior, output layout, deployment, clean/purge, recovery internals,
runtime launch behavior, IDE integration, or crash analysis.
