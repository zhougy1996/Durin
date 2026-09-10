# Agent Documentation Workflow

Read this short guide before creating, moving, removing, validating,
completing, or archiving repository documentation. Once read for the current
task, do not reread it unless the file changes.

## Discover Documents

When the owning document is unknown, follow [Documentation](../README.md)
for topic routing and compact discovery. Known documents can be opened directly.
For stage continuation, use the compact plan context:

```powershell
.\DevTool.bat doc plan context "<title-or-filename>"
```

## Validate Changes

For content edits, start with changed-document validation:

```powershell
.\DevTool.bat doc validate --scope changed
```

For lifecycle changes, use the validator required by the owning rules below.
Use `doc validate --scope all` for a repository-wide documentation audit; add
`--include-archive` only for an explicit historical audit.
Successful mutating documentation commands report the validation they already
completed transactionally. Do not immediately rerun an equivalent validator;
validate again only after a later edit or when an explicit audit is required.

## Apply Document Operations

Document move, task removal, and monthly archive commands apply immediately and
validate transactionally. Pass `--dry-run` only when a preview is needed.
Create specialized files directly from the minimal template in the nearest
`AGENTS.md`, then run the applicable validator. Review the generated diff,
including every reported referencing file after structural operations.

Read only the rules for the operation being performed:

- [Task lifecycle](../Tasks/AGENTS.md#lifecycle)
- [Plan completion and archive](../Plans/AGENTS.md#archive-workflow)
- [Roadmap lifecycle and archive](../Roadmaps/AGENTS.md#lifecycle)
- [Move commands and transaction behavior](../Development/Tooling/DurinDevTool.md#documentation-commands)

## Read the Owning Rules

Continue to [Documentation Rules](../AGENTS.md) and the nearest directory
`AGENTS.md` before changing document content or lifecycle state. Use
[Documentation](../README.md) to route to the authoritative domain document.
For documentation command changes or diagnosis, start with
[DurinDevTool documentation commands](../Development/Tooling/DurinDevTool.md#documentation-commands).
Read [Build And Run](../Development/Build/BuildAndRun.md) only if the issue also
involves setup, build ownership, or recovery.
