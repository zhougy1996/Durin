# Agent Build And Run Workflow

## Routine Commands

Run from the repository root. These are alternatives, not a command sequence:

```powershell
.\DevTool.bat build <Target>
.\DevTool.bat run [-- arguments...]
.\DevTool.bat status
```

Use the selected host profile's default preset. Add presets only when the
change requires another runtime or configuration; see
[routine coverage](../Development/Build/BuildAndRun.md#routine-coverage).

`build` defaults to `all`; prefer the smallest sufficient `<Target>`.
Build and executable test selections configure automatically when needed.
`test` also builds its selected targets and dependencies; do not precede it with
an equivalent build unless a separate build gate or artifact is required.
An `all` build is incremental, not a clean rebuild. Explain which uncovered risk
requires broader coverage before expanding the target selection.
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

On Windows, sandboxed MSVC builds can fail with `C1902` because of PDB service
compatibility. When this sandbox-only failure is already established, request
authorized execution of the DevTool build outside the sandbox from the outset;
do not reproduce it on every task. If it also fails outside the sandbox,
investigate the toolchain rather than assuming a sandbox issue.

After cancellation, external termination, or loss of the controlling DevTool
process, wait for its build process tree to exit, then run `.\DevTool.bat status`
and follow the reported recovery command. If clean, fix the cause and retry.
Ordinary build, test, or application failures do not require recovery or rebuild.

Follow [Testing](Testing.md) for risk-based validation. Use `all` only for an
explicit gate or integration risk that bounded consumer builds cannot cover,
including for editor changes. When handing off a runnable editor, build its
required targets and link the verified
executable from the same Agent Build Profile.

## macOS Application Smoke

Do not run macOS application smoke tests in a Codex sandbox by default. Build
the target when useful and report execution as not run. Run a smoke only when
the user explicitly requests it and macOS application services are already
authorized; `--exit-after-ticks` cannot recover a pre-loop startup stall.

## Read the Complete Guide

Read [Build And Run](../Development/Build/BuildAndRun.md) for environment setup,
changes to build/run behavior, or diagnosis beyond routine failure handling.
