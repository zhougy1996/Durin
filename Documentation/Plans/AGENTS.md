# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- `README.md` is the stable entrypoint for active-plan discovery. Run
  `python Documentation/Plans/list_active_plans.py` from the repository root to
  generate the current compact index; do not scan or open every plan.
- `Archive/README.md` is a stable navigation index containing only archive month
  links. Each `Archive/YYYY-MM/README.md` indexes the plans completed in that
  month.
- Do not sample unrelated active or archived plans to infer structure; use the standard below.
- Start archive discovery at `Archive/README.md`, enter only the relevant monthly
  index, and read an archived plan only for named or required historical
  provenance. Do not bulk-read other months.

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
- Give every active plan a unique filename and title. Adding an active plan
  normally adds only its own Markdown file; do not add a hand-maintained active
  plan table or generated index to the repository.
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
  `Editor/Systems`, `Development`, or `Workspace` domain rather than leaving
  the plan as a competing specification.
- Run `python Documentation/Plans/list_active_plans.py --validate` whenever an
  active plan is added, renamed, completed, or removed. CI must run the same
  command so discovery metadata, titles, dates, and the required current-status
  section cannot drift. The validator deliberately does not retrofit every
  legacy plan to the current standard structure.

## Archive Workflow

When every required acceptance gate is satisfied:

1. Record completion evidence in `Current Status`, update `Last reviewed`, and close only checks that passed.
2. Move the plan to `Documentation/Plans/Archive/YYYY-MM/`, using the month of
   the archive completion date recorded in `Current Status`, without rewriting
   its historical body. Later maintenance does not move it to another month.
3. Append its title and link to that month's `README.md`, and add the month link
   to `Plans/Archive/README.md` if it is new. Moving the file out of the active
   directory removes it from the generated active index automatically.
4. Repair direct links to the archived location.
5. Confirm lasting behavior is documented in the owning domain and run the
   active-plan validator.

Keep an open monthly index deliberately lightweight: list archived plan links,
but do not maintain a running plan count, per-plan outcome summaries, or a
month-wide synthesis. After the calendar month has ended, finalize that monthly
index during the next archive maintenance pass by adding the final plan count
and one concise monthly summary. Once finalized, do not revise the index for
routine later maintenance. The individual archived plans remain the source for
detailed outcomes and validation evidence.

Do not add counts, landed-area summaries, or other frequently changing metadata
to `Archive/README.md`; its purpose is month discovery only.

Archived plans are historical evidence and are not default implementation instructions.
