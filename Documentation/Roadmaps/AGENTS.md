# Engineering Roadmap Rules

These instructions apply under `Documentation/Roadmaps/`.

## Purpose

- Use a roadmap when one engineering outcome needs multiple independently
  executable implementation plans.
- A roadmap owns milestone ordering, dependencies, entry and exit gates, and
  the boundary between required and conditional work.
- Child plans own selected implementation decisions, file-level work, stage
  checklists, validation evidence, and commit provenance.

## Minimal Template

Start with only the lifecycle metadata and program structure every roadmap
needs:

```markdown
# <Outcome> Roadmap

Summary: <One line describing the cross-plan outcome.>

Last reviewed: YYYY-MM-DD

Status: Active
Completed:

## Current Status

## Outcome

## Milestones

- [ ] P0: <Deliverable; dependencies and completion condition.>
```

Add scope, non-goals, program decisions, foundations, child-plan boundaries,
risks, validation, completion criteria, and related-link sections only when
they clarify the roadmap. Do not add empty headings for completeness.

## Authoring Rules

- Keep the one-line `Summary:` useful as the roadmap status changes.
- New roadmaps declare `Status: Active` and an empty `Completed:` date.
- Name proposed child plans, but create each plan only when its entry gate is
  satisfied and it is ready to become active work.
- Every required milestone must identify its deliverable, relevant
  dependencies, and completion condition. Add an entry gate only when work
  must not begin before a specific condition. Mark optional or evidence-gated
  milestones explicitly.
- Do not duplicate child-plan stages or detailed task lists in the roadmap.
- Record decisions that constrain every child plan here; keep local design
  choices in the owning child plan.
- Reference the root build and test guidance rather than copying commands.

## Lifecycle

- Update `Last reviewed`, `Current Status`, and milestone state whenever a child
  plan is activated, completed, replaced, or deliberately deferred.
- A completed child plan remains linked as historical provenance after it is
  archived by the plan workflow.
- Complete the roadmap only after all required milestones pass their exit
  gates, lasting contracts move to their owning documentation domains, and all
  conditional milestones are either completed or explicitly dispositioned.
- Set `Status: Completed` and `Completed: YYYY-MM-DD` when those conditions are
  met. Completed roadmaps remain in place temporarily but disappear from the
  default active-roadmap listing.
- Run `.\DevTool.bat doc roadmap validate --scope all` when a roadmap is
  added, renamed, completed, archived, or removed. A successful DurinDevTool
  document mutation already reports this validation receipt; do not rerun it
  unless the roadmap changes afterward.

## Archive Workflow

Completion and physical archival are separate operations. Periodically batch
completed roadmaps by completion month:

1. Inspect the pending queue with
   `.\DevTool.bat doc roadmap list --scope completed`.
2. Archive the batch with `.\DevTool.bat doc roadmap archive YYYY-MM`; use
   `--dry-run` only when a preview is needed.
3. The command applies the transaction immediately. The former `--apply`
   spelling remains accepted for compatibility but is unnecessary.

The tool moves matching roadmaps to `Archive/YYYY-MM/`, changes their status to
`Archived`, repairs direct Markdown links and repository-relative paths, and
runs roadmap plus repository-documentation validation transactionally. Review
the generated diff, especially the reported referencing files.

The transaction rejects new broken links introduced by the move but tolerates
pre-existing missing targets in older archives. An explicit
`doc validate --include-archive` audit reports those historical targets as
warnings.

The completion date owns the archive month and is never changed by later
maintenance. Do not maintain a shared active or archive index; listings are
generated from roadmap metadata. Archived roadmaps are historical evidence and
are not default engineering directions.
