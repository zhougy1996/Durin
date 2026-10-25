# Static Mesh Level Mutations

Summary: Plan and execute reversible StaticMesh actor commands with compact outcomes.
Modules: LevelEditor, DurinEd, Engine

`FStaticMeshLevelMutations` is the reusable planned-batch boundary for ordinary
`AStaticMeshActor` instances. Interactive Outliner and viewport structure commands
use the general [Actor command boundary](ViewportEditing.md#actor-structure-commands).

## Supported boundary

The first boundary accepts exact `AStaticMeshActor` objects with their default
`DStaticMeshComponent`, no instance components, and no Actor attachment parent
or children. Derived classes, additional component graphs, and attached Actors
remain explicit unsupported cases for this compact batch API. They are supported
by the general Actor structure commands.

Each detached Actor state contains the exact name, StaticMesh reference,
transform, and visibility. A request contains one or more create, update,
rename, or remove mutations against one Level package. Planning is
mutation-free and returns before/after deltas with an error classification,
owned presentation message and failing mutation index when available.
Plan and execution success derive only from `Error`; UI callers display `Message`
directly. Actor-support queries return a boolean. Detailed transaction validation,
replay and rollback causes stay inside the implementation and history contracts.

## Optimistic planning and execution

`CaptureTarget` records the Level package path and monotonic package edit
revision. `Plan` validates the supported graph, unique names, finite transforms,
available assets, and exact source state. The resulting plan also records the
Actor hierarchy revision.

`Execute` requires the caller's current open Level and read-only state. It
rejects a replaced document, a Level that entered PIE/read-only mode, a changed
package or hierarchy, and a plan executed off the game thread. Undo and Redo
repeat source-state and destination-collision validation before changing live
objects. Validation precedes the structural mutation phase; a live-operation
journal restores updates, creations, renames, removals, and the prior dirty
flag if a defensive runtime failure still occurs.

## Transactions and saved state

One changed batch creates one `Durin::FCommandChange`; an unchanged
update creates none. `DTransBuffer` owns and executes the change and restores the target Level package's editor
revision relative to its saved checkpoint. Interactive callers never save as
part of execution.

Creation/removal delegates to the shared identity-preserving Actor membership
record exported by DurinEd's `MakeActorPresenceChange` composition seam. SkyBox
placement uses the same seam. Specialized owners clean up abandoned unpublished
actors and retain their own policy and compound history entry. The batch retains
exact participants, including their component graphs;
undo, redo and injected-failure compensation restore the same instances rather
than spawning replacements. Source-state validation also requires exact identity.

Create and rename callers receive the exact resulting Actor names and publish
selection through `FLevelEditorContext`. Deletion clears invalid selection.
The service itself does not own workspace selection, document saving, widgets,
or asset loading.

## Current integrations

- Programmatic planned batches retain compact state validation and atomic rollback.
- StaticMesh and SkyBox placement, Outliner commands, and Select-mode shortcuts
  share identity-preserving Actor membership rather than terminal destruction.
## Related code

- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelMutations.h`
- `Engine/Source/Editor/LevelEditor/Private/Operations/StaticMeshLevelMutations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/StaticMeshLevelMutationTests.cpp`
