# Agent Build And Run Workflow

## Routine Commands

Run from the repository root:

```powershell
.\DevTool.bat configure
.\DevTool.bat build --target <Target>
.\DevTool.bat build
.\DevTool.bat run [arguments...]
.\DevTool.bat status
```

Use the selected host profile's default preset. Add presets only when the
change requires another runtime or configuration; see
[routine coverage](../Development/Build/BuildAndRun.md#routine-coverage).

`build` defaults to `all`; prefer the smallest sufficient `--target <Target>`.
Use fresh configuration, clean, rebuild, or purge only for a diagnosed need.

Machine-local tool and build-profile overrides belong in
`.agents/DevTool.user.json`. If it is missing or setup is required, use
`.\DevTool.bat setup` in the main checkout or follow the complete guide for a
linked worktree.

Configure, build, rebuild, and commands that invoke them are long-running.
Where process timeouts are supported, allow at least 10 minutes, or one hour
for `all`, increasing for known longer builds. Polling intervals are not process
timeouts. Continue the same session/cell with blocking waits of 30–60 seconds
where supported, never longer than 60 seconds, until its final result. Avoid
rapid polling; quiet output or a yield does not justify another build or a
recovery inspection.

## Recovery

After cancellation, external termination, or loss of the controlling DevTool
process, wait for its build process tree to exit, then run `.\DevTool.bat status`
and follow the reported recovery command. If clean, fix the cause and retry.
Ordinary build, test, or application failures do not require recovery or rebuild.

Follow [Testing](Testing.md) for risk-based validation. Use `all` only for an
explicit gate or integration risk, including for editor changes. When handing
off a runnable editor, build its required targets and link the verified
executable from the same Agent Build Profile.

## macOS Application Smoke

Do not run macOS application smoke tests in a Codex sandbox by default. Build
the target when useful and report execution as not run. Run a smoke only when
the user explicitly requests it and macOS application services are already
authorized; `--exit-after-ticks` cannot recover a pre-loop startup stall.

## Read the Complete Guide

Read [Build And Run](../Development/Build/BuildAndRun.md) for environment setup,
changes to build/run behavior, or diagnosis beyond routine failure handling.
