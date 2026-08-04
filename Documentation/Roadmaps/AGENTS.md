# Engineering Roadmap Rules

These instructions apply under `Documentation/Roadmaps/`.

## Purpose

- Use a roadmap when one engineering outcome needs multiple independently
  executable implementation plans.
- A roadmap owns milestone ordering, dependencies, entry and exit gates, and
  the boundary between required and conditional work.
- Child plans own selected implementation decisions, file-level work, stage
  checklists, validation evidence, and commit provenance.

## Standard Structure

New roadmaps use this structure unless the topic requires an additional
section:

```markdown
# <Outcome> Roadmap

Summary: <One line describing the cross-plan outcome.>

Last reviewed: YYYY-MM-DD

Status: Active
Completed:

## Current Status
## Outcome
## Scope
## Non-Goals
## Program Decisions and Invariants
## Current Foundations and Gaps
## Milestone Map
## Child Plan Boundaries
## Program Validation Matrix
## Risks and Control Gates
## Completion Criteria
## Related Documentation
## Related Code
```

## Authoring Rules

- Keep the one-line `Summary:` useful as the roadmap status changes.
- New roadmaps declare `Status: Active` and an empty `Completed:` date.
- Name proposed child plans, but create each plan only when its entry gate is
  satisfied and it is ready to become active work.
- Every required milestone must identify its dependencies, deliverable, entry
  gate, and exit gate. Mark optional or evidence-gated milestones explicitly.
- Do not duplicate child-plan stages or detailed task lists in the roadmap.
- Record decisions that constrain every child plan here; keep local design
  choices in the owning child plan.
- Reference the root build and test guidance rather than copying commands.

## Lifecycle

- Update `Last reviewed`, `Current Status`, and the child-plan table whenever a
  child plan is activated, completed, replaced, or deliberately deferred.
- A completed child plan remains linked as historical provenance after it is
  archived by the plan workflow.
- Complete the roadmap only after all required milestones pass their exit
  gates, lasting contracts move to their owning documentation domains, and all
  conditional milestones are either completed or explicitly dispositioned.
- Set `Status: Completed` and `Completed: YYYY-MM-DD` when those conditions are
  met. Completed roadmaps remain discoverable here; Git preserves prior
  revisions.
