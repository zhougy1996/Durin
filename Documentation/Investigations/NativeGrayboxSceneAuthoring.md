# Native Graybox Scene Authoring Expansion Investigation

Summary: Determine the user workflows, ownership model, identity, persistence,
and publication boundaries required before extending the existing StaticMesh
authoring service and create-only graybox command into a broader scene-authoring
solution.

**Status:** Deferred pending a concrete end-to-end authoring workflow and a
replacement architecture that can extend beyond name-managed StaticMesh Actors.

**Last reviewed:** 2026-08-12

## Verified Current Behavior

Durin already supports two repository-native authoring boundaries delivered by
the former Native Graybox Scene Authoring plan.

At revision `4d5b123c`, `FStaticMeshLevelAuthoringService` became the reusable
LevelEditor boundary for bounded create, update, rename, and remove batches over
ordinary unattached `AStaticMeshActor` graphs. Planning is mutation-free and
stale-aware; execution uses one structural transaction with rollback, Undo/Redo
collision checks, saved-revision integration, exact package reporting, and
no-op suppression. The Scene Viewport and relevant World Outliner paths use the
service. Focused tests cover atomic batches, failed live steps, document and
read-only transitions, unsupported graphs, saved state, and history behavior.

At revision `a89f7087`, `DevTool scene graybox-build` added a narrower create-only
workflow. A bounded hidden DurinEditor process builds an open Box arena in an
unpublished candidate Level, saves and reloads it, verifies its Actors, and
publishes it through AssetCore relocation only when the requested output path
is absent. It refuses replacement and concurrent project authoring. The
qualified Sandbox smoke covered persistence, occupied-output refusal, ownership
conflict, deterministic rerun refusal, cleanup, asset compatibility, and process
shutdown.

These shipped contracts are documented by Static Mesh Level Authoring and the
Create-Only Graybox Build guide. They remain supported independently of any
future generalized scene-authoring design.

The earlier `ThirdPersonTest` bootstrap at revision `f12c9113` used a temporary
one-use executable. That target was removed after generation. The permanent
service and create-only command eliminate the need to repeat that technique for
their supported cases, but they do not provide incremental regeneration of an
existing Level.

## Observable Impact

There is no known correctness defect in the delivered StaticMesh mutation or
create-only graybox paths. Users can place and transactionally edit supported
StaticMesh Actors through ordinary LevelEditor surfaces, and automation can
create one new open Box arena without generating a temporary build target or
editing package bytes.

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

### P2: Safe replacement and automation stop at an empty output path

The qualified command publishes only to an absent path. Updating an occupied
Level requires a recoverable contract spanning live registry state, dirty
documents, candidate publication, save and audit failure, cancellation, crash,
and process exit. Reusing the create-only relocation path does not by itself
make destructive replacement safe.

### P2: Geometry and gameplay expectations are not one concern

The delivered arena is visual Box geometry. Triangular prisms, CSG or editable
topology, snapping, materials, grouping, collision, walkable slopes, character
steps, and camera obstruction have different runtime and editor owners. A
single graybox plan should not silently treat those as one feature boundary.

## Candidate Directions

Treat `FStaticMeshLevelAuthoringService` as a proven narrow substrate rather
than the architecture of a generalized authoring system. Preserve
`graybox-build` as a create-only utility while gathering concrete workflows
that the existing LevelEditor cannot express economically.

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

- at least one concrete Level-authoring workflow that the current placement and
  create-only command cannot satisfy;
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
  contract or a need for one beyond the existing create-only command.

## Related Documentation

- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Static Mesh Level Authoring](../Editor/Architecture/StaticMeshLevelAuthoring.md)
- [Create-Only Graybox Build](../Editor/Guides/GrayboxBuild.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Sandbox Gameplay](../Runtime/Gameplay/SandboxGameplay.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Private/Authoring/StaticMeshLevelAuthoring.cpp`
- `Engine/Source/Editor/LevelEditor/Public/GrayboxSceneAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Private/Authoring/GrayboxSceneAuthoring.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/StaticMeshLevelAuthoringTests.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
- `Tools/DurinDevTool/durin_dev_tool/scene.py`
