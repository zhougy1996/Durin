# Static Mesh Level Authoring

Modules: LevelEditor, DurinEd, Engine

`FStaticMeshLevelAuthoringService` is the reusable structural-editing boundary
for ordinary `AStaticMeshActor` instances. The Scene Viewport and World
Outliner use it instead of directly spawning, renaming, or destroying supported
StaticMesh Actors.

## Supported boundary

The first boundary accepts exact `AStaticMeshActor` objects with their default
`DStaticMeshComponent`, no instance components, and no Actor attachment parent
or children. Derived classes, additional component graphs, and attached Actors
remain explicit unsupported cases; their legacy Outliner behavior is not
presented as transaction-backed authoring.

Each detached Actor state contains the exact name, StaticMesh reference,
transform, and visibility. A request contains one or more create, update,
rename, or remove mutations against one Level package. Planning is
mutation-free and returns typed diagnostics plus before/after deltas.

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

One changed batch creates one `IEditorTransaction`; an unchanged update creates
none. The transaction reports only the target Level package, so
`FEditorTransactionManager` advances and restores that package's editor
revision relative to its saved checkpoint. Interactive callers never save as
part of execution.

Create and rename callers receive the exact resulting Actor names and publish
selection through `FLevelEditorContext`. Deletion clears invalid selection.
The service itself does not own workspace selection, document saving, widgets,
or asset loading.

## Current integrations

- Dropping a StaticMesh asset in the Scene Viewport plans its final transform,
  creates it transactionally, and selects the resulting Actor.
- Creating, renaming, or deleting supported StaticMesh Actors in the World
  Outliner uses the same service.
- SkeletalMesh placement and unsupported Actor/component graphs retain their
  existing explicit non-structural-transaction behavior.

## Related code

- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Private/Authoring/StaticMeshLevelAuthoring.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/StaticMeshLevelAuthoringTests.cpp`
