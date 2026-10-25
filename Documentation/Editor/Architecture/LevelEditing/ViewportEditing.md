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
calls `PickViewport` with an immutable `FViewportPickRequest` in the prepared
`FSceneView` output pixel rectangle and consumes the result in that input event.
Empty space is `Completed` with no hit; invalid input or ownership is `Invalid`,
and rendering failure is `Failed`. There are no pending tickets, polling,
cancellation, callbacks, or deferred selection state.

The public result is semantic and backend-independent. It retains hit kind,
weak Actor and exact component identity, optional typed sub-element,
non-negative world distance,
and `FPrimitiveComponentId` for scene primitives. Ordinary geometry clicks still
select the Actor; visualization clicks select their exact component or
sub-element. `FLevelEditorContext`, not the service or backend, performs those
mutations.

The service captures a request-local table mapping nonzero `FHitProxyId` values
to weak Actor/component identity, registration generation, and optional typed
sub-element. Zero identifies background. The Renderer receives only numeric
primitive IDs, view values, and detached overlay geometry; reflected objects
remain on the game thread. The immediate result verifies weak identity,
ownership, registration cycle, visibility, and primitive identity before a mode
applies selection. The identity table is local to the call. The backend accepts
only a render-scene pointer and numeric `FHitProxyRenderRequest` values, returning
the complete immutable CPU map. Pixel sampling, ID/distance decoding, and weak
identity resolution belong to the picking service; no weak identity table or
mouse position crosses the backend boundary.

`IRendererModule::RenderHitProxies` renders an on-demand integer ID/depth pass
using the renderer's current scene visibility, selected LOD, mesh vertex
factories, transforms, spline deformation, and material culling. Masked materials
apply their opacity mask; translucent surfaces are treated as solid selectable
surfaces. The pass excludes lighting, post-processing, and antialiasing.
Unmapped visible surfaces still occlude and write background ID. There is no
Level-owned picking observer, editor scene spatial index, or physics dependency.
Currently implemented mesh factories cover StaticMesh and SplineMesh. Restoring
Terrain or SkeletalMesh requires their renderer mesh-batch/vertex-factory path;
the Hit Proxy framework does not restore removed geometry modules. Unsupported
renderer resources fail the request explicitly.

`FHitProxyPixel` defines the `RG32_UINT` ID/distance layout shared by CPU sampling
and renderer target creation. The second integer word preserves the IEEE 754
float distance bits; callers consume typed ID and distance fields. `FHitProxyMap`
can only be created through its validating factory, which checks nonzero extent,
dimension/pixel limits, and exact byte count before exposing immutable pixels.
Pixel reads only check coordinates. Invalid readback payloads never enter the
cache. Overlay priority and foreground policy remain render inputs rather than
returned hit metadata. Forward and
reversed depth conventions are supported. The viewport submits an ordered
render command and flushes it before returning. On a cache miss,
`ReadTexture2D` synchronously copies the complete target and waits for that copy's
GPU completion. The limit is 8,388,608 pixels / 64 MiB, with dimensions no larger
than 8192; requests exceeding it fail.
Each viewport backend retains one full-map cache. Ordered render-thread lookup
checks view/projection and pixel extent, scene publication revision, accepted
material and texture-reference publication epochs, renderer generation, numeric
ID mapping, and overlay geometry/priority. A valid map shares an immutable CPU
pixel allocation; another query does not render or read back that map again.
Only successful reads enter the cache, and failed requests can be retried.
Scene changes retire the reusable map without altering previously returned CPU
snapshots. Cache lookup, publication, and invalidation are render-thread operations;
the key, map, and frame-dependence policy form one private entry. Statistics are
read-only snapshots and remain cumulative across invalidation. Viewport reset or
Level replacement invalidates the entry before the next map lookup. Live
view-dependent material programs conservatively expire each render frame unless
material time is frozen; render-target texture inputs expire each frame.
Region-only copies and box-selection UI are not implemented.

Visualization lines become screen-expanded quads, and icons/boxes include their
pixel hit padding. World overlays depth-test against scene geometry. Foreground
overlays render afterward without depth testing, in ascending semantic priority,
so the highest priority wins overlapping handles. Equal-priority overlays use
collector order. Selection uses this GPU result; the existing CPU visualization
hit test remains available for immediate hover feedback.

Transform gizmos retain immediate analytic handle tests and ray/axis/plane drag
constraints. Their active interaction consumes input before scene selection.
These interaction rules are independent of scene-mesh selection; the shared
Hit Proxy overlay API is available for tools requiring rendered handle IDs.

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

The gizmo snapshots valid targets and package edit revisions at drag start.
Preview and restoration use package-dirty suppression. Completion uses one
`StoreUndo(Change)` call; the transactor owns recording and package transitions.
Suppressed preview revisions let it distinguish the gesture from unrelated writes.
The document controller owns save admission for both commands and direct calls:
saving and save-producing rename require an idle transactor and no active gesture
or unfinished restore.
Cancel, target invalidation, mode exit and net-zero completion restore starting
transforms without a history entry. Initial clean flags are restored only if the
package edit revision did not change; unrelated writes during the gesture remain
dirty. Target capability masks prevent unsupported operations before mutation.

Each target also retains its last preview value. An outside transform write
ends ownership of that target; cancellation and release preserve its live value
and restore the remaining targets. Failed preview writes restore the complete
selection, including a setter that partially mutates before failing. A failed
restore stays active for retry and blocks saving and level changes. History
replay validates every live source transform before writing and compensates the
complete selection on failure; incomplete compensation requires recovery.
Restoration verifies the actual values and defers while another recorder owns
`GUndo`, so it cannot publish a false clean state or join another transaction.

The Actor adapter preserves translation, rotation, scale, shared-pivot,
hierarchy filtering, local/world axes, snapping, Undo/Redo, and dirty-state
behavior. Spline point and tangent adapters expose translation only, convert
between component-local and world coordinates, and resolve every update by
point GUID. A tangent selection produces one tangent target; a point
multi-selection produces one target per selected point.

## Actor Structure Commands

DurinEd's `DEditorActorSubsystem` owns editor Actor addition, batch deletion,
duplication, renaming and attachment/detachment. Outliner commands and Select-mode
shortcuts resolve this boundary through their document's editor host;
StaticMesh asset placement prepares its mesh and final transform before first
registration. Actor structural edits use one application history entry per
command and package revision transitions, without manual saved-state invalidation.

Membership records retain exact Actor identities and complete Outer trees across
absence. Removal unregisters components and Tick functions, detaches boundary
attachments with world transforms intact, and excludes absent graphs from package
capture through the Actor's transient flag. Restoration preserves Level ordering,
primary camera selection, authored attachments and descendant transforms, and
the previous component registration state. Terminal runtime destruction remains
separate: reversible editor removal does not route `OnActorDestroyed`, EndPlay,
or garbage marking. Inactive graphs become collectible when history releases them.

Duplication uses ordinary owned-graph duplication and remaps editable references
and attachments between selected graphs. Derived component data is rebuilt before
publication. Rename retains identity and exact before/after names; initial name
allocation includes inactive history-owned siblings. No-op renames create no entry.
Replay validates membership, names, camera selection, and the complete proposed
attachment graph before writes. Conflicts preserve history for a repaired retry.
Play transitions and read-only commands are rejected. Prepared objects from
rejected initial commands retire without reserving names for later edits.

## Built-In Modes

`Select` is always available. It owns Actor picking, Ctrl toggling, component
visualization selection, and Actor transform targets.
It also routes Actor duplicate/delete shortcuts through the structure command
boundary; domain modes retain authority over their own structural shortcuts.

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
