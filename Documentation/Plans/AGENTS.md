# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- Select work with `.\DevTool.bat doc plan list`; do not bulk-read unrelated plans.
- Continue a stage with `.\DevTool.bat doc plan context "<title-or-filename>"`.
  Follow required references and read additional sections only to resolve scope,
  dependencies, acceptance conditions, or conflicts with code.
- Find historical plans with
  `.\DevTool.bat doc plan list --scope archive --query "<title-or-filename>"`.
  Unfiltered `--all-results` is only for user-requested archive browsing or audits.

## Minimal Template

Start with only the lifecycle metadata and working structure every plan needs:

```markdown
# <Feature> Plan

Summary: <One line describing the plan's primary scope.>

Last reviewed: YYYY-MM-DD

Status: Active
Completed:

## Current Status

## Goal

## Implementation Stages

### Stage 0: ...

- [ ] ...
```

Add other sections only when needed for execution or review; omit empty headings.

## Writing Rules

- Keep the one-line `Summary:` below the title useful after status changes.
- Use the template's lifecycle metadata for new plans. Legacy plans without it
  remain valid and active until completed; no general retrofit is required.
- Titles must be unique within active/archive scope and filenames within their
  directory. Adding an active plan normally adds only its Markdown file; do not
  maintain active or archive index files.
- Define scope and selected technical decisions before implementation; put
  unresolved decisions in Stage 0.
- Give each stage an observable outcome, concrete tasks, dependencies, and
  completion conditions.
- Link build and test guidance instead of copying commands.

## Status Maintenance

- With each substantive implementation change, update `Last reviewed`,
  `Current Status`, and evidence-backed checklists.
- Record a changed decision and rationale before continuing when implementation diverges from the plan.
- Move implemented long-lived rules to the owning documentation domain rather
  than leaving the plan as a competing specification.
- Run `.\DevTool.bat doc plan validate --scope all`
  when a plan is added, renamed, completed, archived, or removed; CI must run
  the same validation. A successful DurinDevTool document mutation already
  reports this validation receipt; rerun only after later edits or for an
  explicit audit.

## Archive Workflow

Completion and physical archival are separate operations. When every required
acceptance gate is satisfied:

1. Record completion evidence in `Current Status`, update `Last reviewed`, and
   close only passed checks.
2. Set `Status: Completed` and `Completed: YYYY-MM-DD`. Completed plans remain
   in place temporarily but disappear from the default active-plan listing.
3. Confirm lasting behavior is documented in the owning domain and run the
   all-plan validator.

Periodically batch completed plans by completion month:

1. Archive the batch with
   `.\DevTool.bat doc plan archive YYYY-MM`; use `--dry-run` only when a preview
   is needed.
2. It immediately moves matching plans to `Archive/YYYY-MM/`, sets `Archived`,
   repairs references, and validates transactionally.
3. Review the generated diff, especially the referencing files reported by the
   script.

Archive validation follows [Documentation Rules](../AGENTS.md#maintenance).

Use `.\DevTool.bat doc plan list --scope completed` to inspect the
pending archive queue. The completion date, not the batch date, owns the archive
month and is never changed by later maintenance. Do not maintain a shared
archive index; listings remain generated.

Archived plans are historical evidence and are not default implementation instructions.
