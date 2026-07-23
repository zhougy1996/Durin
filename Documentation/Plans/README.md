# Implementation Plan Documentation Guide

This directory contains implementation plans whose scope has been narrowed enough to execute directly. Read this document before locating or creating a plan; there is no need to read unrelated plans to infer the documentation style.

## Document Index

| Plan | Primary Scope |
| --- | --- |
| [SkyBoxComponent](SkyBoxComponent.md) | Complete vertical slice for the first static cubemap sky background |
| [Texture Support](TextureSupport.md) | Texture2D assets, platform data, material sampling, and validation |
| [Material System](MaterialSystem.md) | Material editing, surface models, shader maps, and runtime materials |
| [Material Parameter Domain Refactor](MaterialParameterDomainRefactor.md) | Stable parameter identity, schema-driven material values, instance overrides, and generic editing |
| [Reflected Property Editing](ReflectedPropertyEditing.md) | Reflected property editing, transactions, notifications, and customization |
| [Multithreading System](MultithreadingSystem.md) | Task system, thread boundaries, and rendering concurrency evolution |
| [Editor Workspace Refactor](EditorWorkspaceRefactor.md) | Editor workspaces, panels, and document lifecycles |
| [Editor Icon Atlas](EditorIconAtlas.md) | Offline atlas pipeline for editor visualization icons |
| [Asset Registry and Thumbnail Cache](AssetRegistryAndThumbnailCache.md) | Persistent asset discovery metadata and rebuildable Content Browser thumbnails |
| [Shader Cache Hardening](ShaderCacheHardening.md) | Shader cache integrity, warm-path manifests, request coalescing, and bounded retention |
| [Logging Pipeline Hardening](LoggingPipelineHardening.md) | Deterministic session history, reliable sink delivery, bounded console consumption, and logging validation |

## Archived Plans

Archived plans preserve implementation decisions, validation evidence, and incomplete historical checks after their active execution path has ended.

| Plan | Outcome |
| --- | --- |
| [Editor World Grid V2](Archive/EditorWorldGridV2.md) | Landed screen-space ray-plane reconstruction; its later renderer-phase follow-up is also archived below |
| [Scene Post-Processing and Editor Assistance Boundary](Archive/ScenePostProcessEditorAssistanceBoundary.md) | Landed post-process-first, depth-aware editor assistance for Present and Offscreen viewport outputs |

Update this index whenever a plan document is added, renamed, or deleted.

## Archive Workflow and Reading Policy

When a plan reaches its acceptance gates:

1. Record the completion evidence in `Current Status`, update `Last reviewed`, and close only the checklists that actually passed.
2. Move the file to `Documentation/Plans/Archive/` without rewriting its goal, scope, decisions, stages, validation matrix, or other historical body text.
3. Move its index entry from **Document Index** to **Archived Plans** and summarize the landed outcome.
4. Update direct links that must continue resolving after the move. Long-lived implementation rules must already live in Architecture documentation rather than depending on the archived plan.

Archived plans are historical evidence, not default task instructions. Do not read or sample archived plan bodies for ordinary implementation work, plan formatting, or repository orientation. Read an archived plan only when the user explicitly names it, an active task-relevant document identifies it as required context, or decision provenance is necessary to resolve the current task. The archive index is sufficient when only the existence or outcome of prior work matters.

## Boundaries Between Plans and Other Documentation

- `Documentation/Issues` records verified, unresolved problems and supporting evidence before an implementation path has been selected or completed.
- `Documentation/Architecture` records adopted architectural constraints that require long-term maintenance.
- `Documentation/Plans` records executable paths and acceptance gates from the current state to a target state.

Keep informal research, external examples, and candidate approaches outside version control under the ignored `Documentation/Local/` directory or in an external knowledge base. Repository documents must not link to those local notes. Create a plan only after the scope, non-goals, and key technical decisions are clear. Once implementation is complete, move long-lived requirements into Architecture instead of allowing the plan to become a second architecture specification.

## Standard Document Structure

New plans use the following structure by default. Topic-specific sections may be added, but scope, stage acceptance, and the definition of done must not be omitted.

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

### 1. Narrow the Scope First

- Use `Goal` to describe in one paragraph what the user will ultimately be able to see or use.
- Use `Scope` to list the end-to-end paths that must be completed.
- Use `Non-Goals` to explicitly exclude capabilities that could easily expand the work incidentally.
- Avoid statements such as "improve," "support well," or "handle as appropriate" that cannot be accepted objectively.

### 2. State Decisions and Invariants First

The plan should state the selected input formats, ownership model, thread boundaries, failure fallbacks, and rendering order. If an item still requires a decision, make resolving it a required Stage 0 task instead of listing conflicting candidate approaches as simultaneous implementation tasks.

### 3. Make Every Stage Independently Acceptable

Each stage contains:

- The stage deliverable, not merely the files to modify.
- Concrete, checkable tasks.
- An `Acceptance Gate` describing the evidence required before proceeding to the next stage.
- Explicit dependencies on earlier stages.

Prefer to order stages as "low-level contracts -> resource lifecycle -> scene data -> rendering result -> editor workflow -> end-to-end validation," while following the actual dependency graph when it differs.

### 4. Separate Implementation Tasks From Validation Tasks

- Unit tests cover data constraints, boundaries, and failure paths.
- Integration tests cover assets, reflection, serialization, threading, and module boundaries.
- Rendering features list both real-backend validation and visible-result checks.
- Final build, test, and run procedures follow the root `AGENTS.md` and Setup documentation. Do not duplicate a potentially stale command set in every plan.

### 5. Maintain Status After Implementation Begins

- Check off tasks as they land instead of updating the entire plan only after it is complete.
- Update `Last reviewed` and `Current Status` with every substantive change.
- If implementation diverges from the plan, update the decision and its rationale before continuing to check off tasks.
- After every `Definition of Done` condition is satisfied, move long-lived rules into Architecture and then mark the plan complete or move it into a history area. Do not remove its index entry without recording its destination.
