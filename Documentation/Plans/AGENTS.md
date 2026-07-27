# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- From the repository root, run
  `.\Tools\DurinDevTool\DevTool.bat plan list` for
  the compact active index. For named historical provenance, run
  `.\Tools\DurinDevTool\DevTool.bat plan list --scope archive --query
  "<title-or-filename>"`, then open only the selected result. Do not scan,
  bulk-read, or sample unrelated plans; use the standard below.
- Humans run `.\Tools\DurinDevTool\DevTool.bat` without arguments for an interactive
  shell whose `list` command defaults to readable terminal output with automatic
  ANSI color. Direct agent routing, generated Markdown, and piped output use the
  default Markdown format.
- Unfiltered archive listings require the explicit `--all-results` option and
  are allowed only when the user asks to browse or audit the archive as a
  collection. Never use them merely to locate one historical plan.

## Standard Structure

New plans use this structure unless the topic requires additional sections:

```markdown
# <Feature> Plan

Summary: <One line describing the plan's primary scope.>

Last reviewed: YYYY-MM-DD

Status: Active
Completed:

## Current Status
## Goal
## Scope
## Non-Goals
## Design Decisions and Invariants
## Current Foundations and Gaps
## Implementation Stages
### Stage 0: ...
- [ ] ...
#### Acceptance Gate
- ...
## Validation Matrix
## Definition of Done
## Deferred Follow-ups
## Related Documentation
## Related Code
```

## Writing Rules

- Keep the one-line `Summary:` below the title useful after status changes.
- New plans declare `Status: Active` and an empty `Completed:` date after
  `Last reviewed`. Existing plans without lifecycle metadata remain valid and
  are treated as active until they are completed.
- Titles must be unique within active/archive scope and filenames within their
  directory. Adding an active plan normally adds only its Markdown file; do not
  maintain active or archive index files.
- Narrow scope before implementation. Goals, scope, and non-goals must be objectively distinguishable.
- State selected ownership, thread, failure, format, and ordering decisions before stage tasks.
- Put unresolved decisions in Stage 0 instead of presenting conflicting approaches as simultaneous work.
- Each stage needs a coherent outcome, concrete tasks, dependencies, and an
  acceptance gate. Separate implementation from unit, integration, rendering,
  and end-to-end validation.
- Reference root build and test instructions instead of copying commands that can become stale.
- Avoid vague requirements such as "improve," "support well," or "handle as appropriate."

## Status Maintenance

- With each substantive implementation change, update `Last reviewed`,
  `Current Status`, and evidence-backed checklists.
- Record a changed decision and rationale before continuing when implementation diverges from the plan.
- Move implemented long-lived rules to the owning documentation domain rather
  than leaving the plan as a competing specification.
- Run `.\Tools\DurinDevTool\DevTool.bat plan validate --scope all`
  when a plan is added, renamed, completed, archived, or removed; CI must run
  the same validation. Legacy plans need not be retrofitted to the current
  structure.

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

1. Preview the batch with
   `.\Tools\DurinDevTool\DevTool.bat plan archive YYYY-MM`.
2. Apply it with
   `.\Tools\DurinDevTool\DevTool.bat plan archive YYYY-MM --apply`.
   The tool moves matching plans to `Archive/YYYY-MM/`, changes their status
   to `Archived`, and repairs direct Markdown links and repository-relative
   plan paths, then runs the all-plan validator.
3. Review the generated diff, especially the referencing files reported by the
   script.

Use `.\Tools\DurinDevTool\DevTool.bat plan list --scope completed` to inspect the
pending archive queue. The completion date, not the batch date, owns the archive
month and is never changed by later maintenance. Do not maintain a shared
archive index; listings remain generated.

Archived plans are historical evidence and are not default implementation instructions.
