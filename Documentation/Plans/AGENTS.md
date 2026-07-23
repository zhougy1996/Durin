# Implementation Plan Rules

These instructions apply under `Documentation/Plans/`.

## Purpose and Reading Policy

- Active plans turn selected decisions into executable stages and acceptance gates.
- `README.md` indexes only active plans.
- `Archive/README.md` indexes completed plans.
- Do not sample unrelated active or archived plans to infer structure; use the standard below.
- Read an archived plan only for named or required historical provenance.

## Standard Structure

New plans use this structure unless the topic requires additional sections:

```markdown
# <Feature> Plan

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
- Move long-lived implemented rules into Architecture rather than leaving the plan as a competing specification.
- Update `README.md` whenever an active plan is added, renamed, completed, or removed.

## Archive Workflow

When every required acceptance gate is satisfied:

1. Record completion evidence in `Current Status`, update `Last reviewed`, and close only checks that passed.
2. Move the plan to `Documentation/Plans/Archive/` without rewriting its historical body.
3. Remove it from `Plans/README.md` and add its landed outcome to `Plans/Archive/README.md`.
4. Repair direct links to the archived location.
5. Confirm lasting behavior is documented in Architecture.

Archived plans are historical evidence and are not default implementation instructions.
