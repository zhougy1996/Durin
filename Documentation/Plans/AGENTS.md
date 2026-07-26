# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- Run `python Documentation/Plans/list_plans.py` from the repository root
  to generate the current compact active index; do not scan or open every plan.
- Run `python Documentation/Plans/list_plans.py --scope archive` for
  named historical provenance. The generated archive index is grouped by
  completion month; do not scan or open every archived plan.
- Do not sample unrelated active or archived plans to infer structure; use the standard below.
- Read an archived plan only for named or required historical provenance. Do
  not bulk-read other months.

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

- Keep `Summary:` on one line directly below the title block. It is discovery
  metadata, not a status report, and must remain useful after status changes.
- Give every plan a unique title within its active or archive scope and a
  unique filename within its directory. Adding an active plan normally adds
  only its own Markdown file; do not add a hand-maintained active or archive
  index to the repository.
- Narrow scope before implementation. Goals, scope, and non-goals must be objectively distinguishable.
- State selected ownership, thread, failure, format, and ordering decisions before stage tasks.
- Put unresolved decisions in Stage 0 instead of presenting conflicting approaches as simultaneous work.
- Each stage must deliver a coherent outcome, list concrete tasks, identify dependencies, and define an acceptance gate.
- Keep implementation tasks separate from unit, integration, rendering, and end-to-end validation.
- Reference root build and test instructions instead of copying commands that can become stale.
- Avoid vague requirements such as "improve," "support well," or "handle as appropriate."

## Status Maintenance

- Update `Last reviewed`, `Current Status`, and checklists with every substantive implementation change.
- Check off only work whose acceptance evidence exists.
- Record a changed decision and rationale before continuing when implementation diverges from the plan.
- Move long-lived implemented rules into the owning `Runtime`,
  `Editor/Architecture`, `Editor/Design`, `Development`, or `Workspace` domain
  rather than leaving the plan as a competing specification.
- Run `python Documentation/Plans/list_plans.py --scope all --validate`
  whenever a plan is added, renamed, completed, archived, or removed. CI must
  run the same command so discovery metadata, titles, dates, archive placement,
  and the required current-status section cannot drift. The validator
  deliberately does not retrofit every legacy plan to the current standard
  structure.

## Archive Workflow

When every required acceptance gate is satisfied:

1. Record completion evidence in `Current Status`, update `Last reviewed`, and close only checks that passed.
2. Move the plan to `Documentation/Plans/Archive/YYYY-MM/`, using the month of
   the archive completion date recorded in `Current Status`, without rewriting
   its historical body. Later maintenance does not move it to another month.
3. Do not create or update a shared archive index. Moving the file out of the
   active directory and into its month makes both generated indexes update
   automatically.
4. Repair direct links to the archived location.
5. Confirm lasting behavior is documented in the owning domain and run the
   all-plan validator.

Archived plans are historical evidence and are not default implementation instructions.
