# Agent Documentation Workflow

## Discover Documents

When the owning document is unknown, follow [Documentation](../README.md)
for topic routing and compact discovery; otherwise open it directly.

## Validate Changes

For content edits, start with changed-document validation:

```powershell
.\DevTool.bat doc validate --scope changed
```

For lifecycle changes, use the validator required by the owning rules below.
Use `doc validate --scope all` for a repository-wide documentation audit; add
`--include-archive` only for an explicit historical audit.
Successful document mutations include a validation receipt; reuse it unless
later edits or an explicit audit require validation again.

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

Before editing, read [Documentation Rules](../AGENTS.md) and the nearest
directory `AGENTS.md`.
For documentation command changes or diagnosis, start with
[DurinDevTool documentation commands](../Development/Tooling/DurinDevTool.md#documentation-commands).
Read [Build And Run](../Development/Build/BuildAndRun.md) only if the issue also
involves setup, build ownership, or recovery.
