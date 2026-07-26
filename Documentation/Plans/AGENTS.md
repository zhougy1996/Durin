# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- From the repository root, run `python Documentation/Plans/list_plans.py` for
  the compact active index or add `--scope archive` for named historical
  provenance. Do not scan, bulk-read, or sample unrelated plans; use the
  standard below.

## Standard Structure

New plans use this structure unless the topic requires additional sections:

```markdown
# <Feature> Plan

Summary: <One line describing the plan's primary scope.>

Last reviewed: YYYY-MM-DD

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
- Run `python Documentation/Plans/list_plans.py --scope all --validate`
  when a plan is added, renamed, completed, archived, or removed; CI must run
  the same validation. Legacy plans need not be retrofitted to the current
  structure.

## Archive Workflow

When every required acceptance gate is satisfied:

1. Record completion evidence in `Current Status`, update `Last reviewed`, and close only passed checks.
2. Move the plan, without rewriting its historical body, to
   `Documentation/Plans/Archive/YYYY-MM/` using the completion month recorded
   in `Current Status`; later maintenance never changes that month.
3. Do not maintain a shared archive index; the move updates generated indexes.
4. Repair direct links to the archived location.
5. Confirm lasting behavior is documented in the owning domain and run the
   all-plan validator.

Archived plans are historical evidence and are not default implementation instructions.
