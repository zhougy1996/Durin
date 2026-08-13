# Agent Documentation Workflow

Read this short guide before creating, moving, removing, validating,
completing, or archiving repository documentation. Once read for the current
task, do not reread it unless the file changes.

## Discover Documents

Use DurinDevTool from the repository root and open only the closest result:

```powershell
.\DevTool.bat doc find "<task terms>" --limit 5
.\DevTool.bat doc task list
.\DevTool.bat doc plan list
.\DevTool.bat doc roadmap list
.\DevTool.bat doc plan list --scope completed
.\DevTool.bat doc roadmap list --scope completed
```

Query a named historical plan or roadmap instead of listing an entire archive:

```powershell
.\DevTool.bat doc plan list --scope archive --query "<title-or-filename>"
.\DevTool.bat doc roadmap list --scope archive --query "<title-or-filename>"
```

## Validate Changes

Start with the smallest applicable validation and use the lifecycle validators
when plan or roadmap metadata changes:

```powershell
.\DevTool.bat doc validate --scope changed
.\DevTool.bat doc validate --scope all
.\DevTool.bat doc plan validate --scope all
.\DevTool.bat doc roadmap validate --scope all
.\DevTool.bat doc validate --scope all --include-archive
```

Archive-inclusive validation is an explicit historical audit. Missing local
targets in archived plans and roadmaps are warnings because later repository
evolution may remove them; active and completed documents remain strict.

## Apply Document Operations

Create, move, task removal, and monthly archive commands preview by default.
Review the preview, then repeat the same command with `--apply`:

```powershell
.\DevTool.bat doc create contract Documentation\Runtime\Example.md --title "Example"
.\DevTool.bat doc create contract Documentation\Runtime\Example.md --title "Example" --apply
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md --apply
.\DevTool.bat doc task remove Documentation\Tasks\CompletedTask.md
.\DevTool.bat doc task remove Documentation\Tasks\CompletedTask.md --apply
.\DevTool.bat doc plan archive YYYY-MM
.\DevTool.bat doc plan archive YYYY-MM --apply
.\DevTool.bat doc roadmap archive YYYY-MM
.\DevTool.bat doc roadmap archive YYYY-MM --apply
```

Archive transactions repair direct references, reject newly introduced
diagnostics, and tolerate only pre-existing missing-target warnings from older
archives. Review every reported referencing file after applying an operation.

## Read the Owning Rules

Continue to [Documentation Rules](../AGENTS.md) and the nearest directory
`AGENTS.md` before changing document content or lifecycle state. Use
[Documentation](../README.md) to route to the authoritative domain document.
Read the complete [Build And Run](../Development/Build/BuildAndRun.md) guide
only when changing or diagnosing DurinDevTool's documentation implementation or
command behavior.
