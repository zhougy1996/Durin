# Native Graybox Scene Authoring Expansion Investigation

Summary: Determine the user workflows, ownership model, identity, persistence,
and publication boundaries required before introducing automated graybox or
scene authoring beyond the existing editor placement tools.

**Status:** Deferred pending a concrete end-to-end authoring workflow and a
replacement architecture that can extend beyond name-managed StaticMesh Actors.

**Last reviewed:** 2026-09-02

## Verified Current Behavior

Durin supports one repository-native structural authoring boundary delivered by
the former Native Graybox Scene Authoring plan.

At revision `4d5b123c`, the StaticMesh Level mutation service became the reusable
LevelEditor boundary for bounded create, update, rename, and remove batches over
ordinary unattached `AStaticMeshActor` graphs. Planning is mutation-free and
stale-aware; execution uses one structural transaction with rollback, Undo/Redo
collision checks, saved-revision integration, exact package reporting, and
no-op suppression. The Scene Viewport and relevant World Outliner paths use the
service. Focused tests cover atomic batches, failed live steps, document and
read-only transitions, unsupported graphs, saved state, and history behavior.

The former create-only arena command was removed because its fixed layout did
not justify a dedicated hidden-Editor publication workflow at the engine's
current stage. Its removal did not change the interactive StaticMesh mutation
boundary or the general asset save, load, deletion, and relocation facilities.

The earlier `ThirdPersonTest` bootstrap at revision `f12c9113` used a temporary
one-use executable. That target was removed after generation and is not a model
for a permanent scene-authoring workflow.

## Observable Impact

There is no known correctness defect in the delivered StaticMesh mutation path.
Users can place and transactionally edit supported StaticMesh Actors through
ordinary LevelEditor surfaces. There is intentionally no dedicated automated
graybox Level generator.

The unresolved problem is product and architecture scope. The former plan
combined a dedicated Graybox panel, a triangular-prism asset, presets, a YAML
recipe, name-derived managed identity, incremental reconciliation, pruning, and
non-interactive apply into one presumed progression. That progression is not a
credible basis for a complete scene-authoring solution without first deciding
which real workflows must survive arbitrary Actor types, attachments, manual
editing, duplication, rename, composition, replacement, and collaboration.

Continuing directly from the former stage list would harden first-slice
constraints into public workflow decisions before those requirements are known.

## Ranked Findings

### P1: The supported object model is too narrow for a general solution

The existing service intentionally accepts only exact `AStaticMeshActor`
objects with their default StaticMesh component, no instance components, and no
attachment parent or children. A broader authoring system must decide whether
it owns arbitrary reflected Actor graphs, prefab-like composition, sublevels,
or only specialized placement operations. The current snapshot contract cannot
answer that question by extension alone.

### P1: Persistent identity and manual-edit ownership are unresolved

The former recipe design derived managed Actor names from a namespace and entry
ID. Manual rename intentionally detached an Actor from the recipe, while later
apply could update or prune other managed Actors. No user evidence establishes
whether a recipe, the Level, or a merge of both should be authoritative.
Persistent GUIDs, serialized provenance, rename preservation, duplication, and
conflict recovery remain unselected.

### P1: The intended user-facing authoring workflow is unproven

A dedicated primitive/preset panel and source-controlled YAML were selected
before representative users or levels demonstrated that this is preferable to
ordinary placement improvements, reusable Level fragments, prefabs, procedural
tools, or another editor-native composition model. Without that evidence, the
panel and recipe are features in search of a stable ownership boundary.

### P2: Safe automated publication has no selected contract

Creating or updating a Level through automation requires a recoverable contract
spanning live registry state, dirty documents, candidate publication, save and
audit failure, cancellation, crash, and process exit. The general asset
relocation path does not by itself define that product-level contract.

### P2: Geometry and gameplay expectations are not one concern

Primitive geometry, CSG or editable topology, snapping, materials, grouping,
collision, walkable slopes, character steps, and camera obstruction have
different runtime and editor owners. A single graybox plan should not silently
treat those as one feature boundary.

## Candidate Directions

Treat the StaticMesh Level mutation service as a proven narrow substrate rather
than the architecture of a generalized authoring system. Keep automated scene
generation out of the product until concrete workflows establish a broader
ownership and publication model.

When evidence justifies new work, compare at least these models before selecting
an implementation plan:

- editor-native manual placement plus reusable groups, prefabs, or Level
  fragments;
- a source recipe with explicit persistent identity and a defined merge model;
- specialized procedural placement tools that emit ordinary Level content but
  do not retain source ownership;
- a hybrid in which generated groups retain bounded provenance while ordinary
  Actors remain Level-owned.

Any new plan should begin with one complete vertical workflow and its ownership
rules. It should not assume YAML, name-derived identity, a dedicated Graybox
panel, pruning, or arbitrary scene automation until the selected workflow makes
those choices necessary.

This is a candidate direction, not an adopted architecture.

## Replanning Triggers

Create a new implementation plan only when all of the following are available:

- at least one concrete Level-authoring workflow that current editor placement
  cannot satisfy economically;
- representative content covering the required Actor/component and attachment
  shapes;
- an explicit decision about whether generated source or the edited Level owns
  subsequent changes;
- persistent identity, rename, duplication, conflict, deletion, Undo/Redo, and
  save semantics for that workflow;
- a bounded interactive or non-interactive first slice with objective acceptance
  evidence.

Broader goals such as “complete graybox tooling” or “recipe-driven scenes” are
not sufficient triggers without those inputs.

## Evidence and Validation Gaps

- No consumer inventory ranks actual graybox or scene-authoring operations by
  frequency, scale, or maintenance cost.
- No workflow prototype compares a dedicated panel, reusable content grouping,
  recipe reconciliation, and procedural generation.
- No identity experiment proves correct behavior across arbitrary rename,
  duplication, attachment, copy/paste, merge, or partial manual edits.
- No qualified operation safely replaces an occupied Level while preserving the
  prior package through save, audit, cancellation, crash, and process failure.
- No representative content establishes whether triangular-prism geometry,
  custom mesh generation, materials, collision, snapping, or gameplay
  qualification belongs in the same first slice.
- No end-to-end evidence establishes a stable machine-readable preview/apply
  contract or a current need for one.

## Related Documentation

- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Static Mesh Level Mutations](../Editor/Architecture/StaticMeshLevelMutations.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Sandbox Gameplay](../Runtime/Gameplay/SandboxGameplay.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelMutations.h`
- `Engine/Source/Editor/LevelEditor/Private/Operations/StaticMeshLevelMutations.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/StaticMeshLevelMutationTests.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
