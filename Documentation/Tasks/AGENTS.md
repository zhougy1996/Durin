# Task Document Rules

These instructions apply under `Documentation/Tasks/`.

## Purpose

- Record bounded implementation work whose direction is already selected and
  whose result does not need a staged plan.
- Treat one task file as one atomic outcome. The file's presence means the work
  is open; do not add status fields, progress journals, completion records, or
  archives.
- Use `Investigations/` when the problem or direction remains unresolved, and
  use `Plans/` when work needs stages, open decisions, or durable handoffs.
  Lasting contracts belong in their owning documentation domain.

## Reading And Selection

- Agents asked to pick work run `.\DevTool.bat doc task list`, select one task,
  then open only that task and its required links.
- Do not scan every task body to choose work. The command derives its compact
  description from each task's Outcome section.
- A task document authorizes only its stated scope. Follow repository build,
  test, coding, and commit rules normally.

## Required Content

- State the outcome, concrete evidence or motivation, required changes,
  protected invariants, likely working set, and acceptance criteria.
- Make failure behavior and deletion conditions explicit. Avoid vague requests
  to clean up, optimize, modernize, or refactor.
- Keep the task self-contained enough for another agent to execute without
  rediscovering the original review, while linking rather than copying durable
  repository contracts.
- Do not add Plan metadata or `Plan:` / `Stage:` commit provenance to task
  work.

Use this minimal template; each section is required by task validation:

```markdown
# <Task>

## Outcome

<One atomic result.>

## Evidence

<Why the change is needed.>

## Required Changes

<What must change.>

## Protected Invariants

<What must remain true.>

## Likely Working Set

<Relevant code and documentation.>

## Acceptance

<Observable completion conditions.>
```

## Lifecycle

- If implementation reveals a material unresolved decision or requires staged
  delivery, replace the task with an Investigation or Plan rather than
  maintaining two sources of truth.
- Completion requires the stated validation and any lasting documentation
  updates. In the same implementation commit, delete the task file.
- Do not move completed or cancelled tasks to an archive or retain a resolved
  index. Git history is the task record.
