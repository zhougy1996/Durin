# Viewport Editing Architecture

Summary: Define editor viewport ownership, selection, gizmos, interaction, and scene mutation.

Modules: LevelEditor, DurinEd, Engine

The Level Editor separates shared camera navigation from contextual content
editing. A registry describes available edit modes, while each Scene Viewport
owns its own manager, active mode, selection view, and transform interaction.
The ordinary C++ types described below are owned by
`Durin::Editor::Level`; Actors, Components, Worlds, and other reflected
runtime types retain their `Durin` identities.

## Ownership

```text
FLevelViewportEditModeRegistry
  owns descriptors and factories only
            |
            v
FSceneViewportPanel (one workspace viewport)
  owns FLevelViewportEditModeManager
  owns FLevelEditorViewportClient and FTransformGizmo
  owns FViewportPickingService through its viewport client
            |
            +--> active ILevelViewportEditMode instance
            +--> shared FLevelEditorContext selection
            +--> shared FLevelEditorContext picking scene index
```

The registry is process-wide but contains no live document state. A descriptor
has a stable ID, display name, priority, activation predicate, and factory.
Duplicate/invalid registrations are rejected. `GetAvailable()` filters against
the current workspace context and sorts by priority. Unregistration invalidates
an active instance at its next synchronization.

Every Scene Viewport owns an independent `FLevelViewportEditModeManager`.
`Select` is always registered and is the safe default. Mode instances are
created per manager and must not store state in component visualizers or the
global registry.

## Shared Selection

`FLevelEditorContext` is the authority for selected Actors, the selected
component, and component-owned sub-elements. Details, visualization, and edit
modes read and update the same state.

A selected component must belong to the primary selected Actor. A sub-element
must belong to that component and is identified by a typed handle containing an
element kind, stable GUID, and optional secondary index. Current kinds are
point, segment, arrive tangent, and leave tangent. Stable identity, not an array
index, is the transaction and selection address.

Changing world or level clears the entire selection. Actor changes and
component invalidation clear descendants. Component changes clear sub-elements.
Modes additionally repair domain identity—for example, Spline mode discards
handles whose point GUID no longer exists. No panel keeps a competing private
component or point selection.

Native-construction `Generated` components remain visible in the Details
component tree for diagnostics, but the tree labels them read-only and disables
rename, duplicate, delete, reorder, reparent, drag/drop, and reflected property
editing. Their owning Actor remains the authoring authority. `ASplineMeshActor`
Details reports the generated-segment count and construction diagnostic and its
**Edit Spline** action selects the native spline root and explicitly activates
Spline mode. Viewport curve edits therefore mutate persistent spline points;
the generated segment set is reconciled synchronously from stable point GUIDs.

## Semantic Picking Contract

Each `FLevelEditorViewportClient` owns one `FViewportPickingService`. A mode
submits an immutable `FViewportPickRequest` in the prepared `FSceneView` output
pixel rectangle and receives a non-zero per-viewport ticket plus an immediate
or pending completion. Empty space is `Completed` with no hit; invalid input,
cancellation, stale ownership, and backend failure remain distinct statuses.
Modes poll rather than receiving callbacks, so a mode can cancel its ticket on
exit without leaving a callback into a retired instance.

The public result is semantic and backend-independent. It retains hit kind,
weak Actor and exact component identity, optional typed sub-element,
non-negative world distance, semantic priority/depth policy, stable tie key,
and `FPrimitiveSceneId` for scene primitives. Ordinary geometry clicks still
select the Actor; visualization clicks select their exact component or
sub-element. `FLevelEditorContext`, not the service or backend, performs those
mutations.

For each scene-geometry request, the service captures a request-local table
from a non-zero numeric token to `FPrimitiveSceneId`, weak component/Actor, and
the component registration generation. The built-in reference backend performs
LOD0, double-sided surface queries for StaticMesh and current-pose SkeletalMesh
components and returns only a token and world distance. Before exposing a completion, the service verifies client/Level
generation, weak identity, Level membership, ownership, registration cycle,
visibility, and primitive identity. World or Level replacement, client reset or
destruction, mode exit, cancellation, and a newer click retire old work;
camera movement alone does not reinterpret the immutable clicked view.

`FLevelEditorContext` owns one game-thread `FViewportPickingSceneIndex` for its
active Level and shares it with every attached viewport service. Engine emits
monotonic editor-only primitive mutation batches for registration, retirement,
transform, owner visibility, mesh/proxy replacement, and skeletal pose-bound
publication. Subscription begins with a complete snapshot. A missing revision,
invalid batch, Level replacement, or an index build that cannot satisfy its
64 MiB budget makes the complete request use reference discovery; a stale
partial candidate table is never queried.

The index admits registered visible StaticMesh and SkeletalMesh primitives
with non-zero identity and finite current world bounds. It uses deterministic
centroid splits and 10 percent fat update bounds with a 0.01 world-unit minimum.
An update inside its fat bound changes only the exact leaf; an escape, add, or
remove triggers one deterministic synchronization rebuild ordered by primitive
identity. The measured double-precision layout is bounded at 384 bytes per
admitted primitive. Ray traversal returns candidates only; the established
world-distance epsilon and stable key remain the semantic resolver.

StaticMesh render data owns one immutable ray-query BVH per valid LOD, built
from the matching CPU positions and uint32 triangle triplets. Nodes use
deterministic longest-axis centroid partitioning, triangle ordinal as the final
tie-break, and no more than eight triangles per leaf. Viewport picking queries
LOD 0, transforms the ray into local space, traverses near-first, and converts
hits back to world distance. Instances and viewports reuse the same asset-owned
allocation. Missing, malformed, over-budget, or replaced data uses the exact
reference triangle loop for that component.

Private backend policy selects `Reference`, `Accelerated`, or `Compare`.
Compare runs both against the same immutable request target table; a status,
token, hit-presence, or distance mismatch increments the parity counter and
returns the reference completion. Skeletal triangles retain the exact M2
provider: only their current-pose world bounds are accelerated because measured
skinning work did not meet the Stage 3 activation threshold.

For a skeletal candidate, the backend acquires one immutable
`FSkeletalPosePalette` from the component on the game thread. The pose must have
a non-zero revision, the mesh's non-empty skeleton compatibility identity, one
finite matrix per LOD0 palette entry, and finite valid current local bounds.
CPU positions, indices, sections, influences, and palette shapes must agree.
Influence bone indices are skeleton bone indices and resolve through a unique
request-local bone-to-palette-slot map; each referenced vertex is deformed once
as the weighted sum of its palette-matrix-transformed bind position. Missing,
incomplete, incompatible, malformed, or non-finite data makes only that
component a non-candidate. There is no bind-pose or bounds-only fallback.

The current pose bounds reject work before deformation. Bounds-surviving
skeletal candidates share request-wide limits of 250,000 skinned referenced
vertices and 500,000 tested triangles. Reaching either limit is permitted;
exceeding a limit fails the complete geometry request with no partial winner.
Private diagnostics record applicable/invalid targets, bounds rejects, budget
failures, skinned vertices, and tested triangles without expanding the public
result or producing per-click logs. Local hits are transformed back to world
space before distance comparison, preserving ordering under rotation,
non-uniform scale, and mirroring; singular transforms are skipped.

Geometry and the prepared visualization candidate enter one resolver. A
depth-independent visualization wins first. Otherwise the nearest finite world
distance wins; within `1e-8`, scene geometry wins over a depth-tested
visualization, then higher semantic priority and the lowest stable key break
remaining ties. The prepared collector is hit-tested once at submission and is
never retained by pending state.

## Visualization And Hit Identity

Actor and component visualizers are stateless draw producers. Their primitives may carry
the exact Actor, component, and optional typed sub-element handle. Hover and
hit-testing preserve that identity through viewport presentation and mode
dispatch.

Render visualization is submitted through the Engine-owned
`FPrimitiveDrawInterface`. Visualizers copy lines, translucent lines, points,
and sprites into their viewport's bounded submission and retain no interface or
submission pointer after the traversal. Built-in camera, light, player-start,
spline, selection-bound, and collision visuals use the same
contract. Their icon meaning is resolved to atlas UVs before submission, and
XRay+Visible helpers emit an explicit translucent `Foreground` primitive
followed by its `World` primitive. The viewport seals the list when traversal
ends, so later tool callbacks cannot mutate an enqueued frame.

Picking remains a separate prepared visualization contract: render PDI values
do not carry Actor/component identity and are not retained for hit testing.
Solid transform Gizmos remain in their specialized mesh and interaction path;
wire boxes and eligible debug geometry are emitted as PDI lines.

Select mode may use a visualization hit to select the owning Actor and exact
component/element, but a normal visualization click never activates a
contextual mode. Spline mode accepts hits only from its selected spline
component, preventing an actor with multiple spline components from editing the
wrong target.

## Mode Lifecycle

Activation is explicit through the viewport mode selector or a domain Details
action. The manager checks the descriptor predicate, calls `Exit()` on the old
mode, constructs the new instance, then calls `Enter()`.

Before every tick, synchronization verifies that the descriptor still exists
and its predicate still accepts the current context. Invalid, unregistered, or
read-only targets force `Exit(..., true)` and fall back to Select. Manager
shutdown uses the same forced-exit path. Document/world replacement and PIE
read-only transitions therefore cannot retain a contextual tool from the prior
editable document.

An edit mode must cancel active manipulation during forced exit before it
releases transient selection or targets. Active mode identity is workspace
state only and is never serialized into a level. A new document and a return
from read-only/PIE synchronize to Select unless a valid mode is explicitly
activated again.

## Input Arbitration

The Scene Viewport resolves input in this order:

1. shared camera navigation and viewport focus;
2. the active mode's transform gizmo and domain interaction; and
3. Select-mode Actor/component picking when Select is active.

Toolbar and popup interaction does not become scene selection. A hovered or
dragged gizmo consumes the relevant pointer input. Text entry suppresses
viewport shortcuts. `W`, `E`, and `R` choose transform operations inside the
current edit mode; they do not select edit modes.

Escape is progressive for contextual editing: it cancels an active drag first,
then clears spline sub-element selection, then exits Spline mode on a later
press. Camera navigation remains outside mode ownership so switching tools does
not replace the established fly, orbit, pan, dolly, or focus path.

## Transform Gizmo Targets

`FTransformGizmo` operates on `ITransformGizmoTarget` instances rather than
Actor storage. A target supplies stable identity, validity, transform,
parent-relative rotation, affected package, label, and capability mask. A
target set supplies the current collection and transaction label.

The gizmo snapshots valid targets at drag start, applies preview transforms,
and commits one generic transaction at completion. Cancel, target invalidation,
mode exit, and net-zero completion restore starting values and package saved
state without a history entry. Target capability masks prevent unsupported
operations before mutation.

The Actor adapter preserves translation, rotation, scale, shared-pivot,
hierarchy filtering, local/world axes, snapping, Undo/Redo, and dirty-state
behavior. Spline point and tangent adapters expose translation only, convert
between component-local and world coordinates, and resolve every update by
point GUID. A tangent selection produces one tangent target; a point
multi-selection produces one target per selected point.

## Built-In Modes

`Select` is always available. It owns Actor picking, Ctrl toggling, component
visualization selection, and Actor transform targets.

`Spline` is available only when the selected component is an editable
`DSplineComponent`. It seeds the first point when entered without a point
selection, owns point/tangent/segment hits, Ctrl point multi-selection, blank
click clearing, selected-target focus, structural point shortcuts, segment
insertion, and spline translation targets. Its structural actions use complete
spline-authoring snapshots; continuous transforms use the generic gizmo
transaction lifecycle.

## Registration Contract

A module registering a new mode must:

- provide a unique stable ID and user-facing display name;
- make `CanActivate` reject missing, invalid, or read-only targets;
- return a new mode instance from the factory;
- keep live state inside that instance;
- return only valid, capability-correct gizmo targets;
- cancel active work from `Exit`, including forced exit; and
- unregister its descriptor during module shutdown.

Tests for a new mode should cover independent managers, activation filtering,
input priority, exact hit identity, target invalidation, cancellation,
unregistration, document/PIE transitions, and shutdown.

## Related Code

```text
Engine/Source/Editor/LevelEditor/Public/LevelEditorSelection.h
Engine/Source/Editor/LevelEditor/Public/LevelEditorViewportEditing.h
Engine/Source/Editor/LevelEditor/Public/LevelEditorTransformTargets.h
Engine/Source/Editor/LevelEditor/Public/LevelEditorViewportPicking.h
Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportEditing.cpp
Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.cpp
Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp
Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.h
Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp
```
