# Decouple Mounted-Content Refresh From Editor Object Transactions

## Outcome

Make Content Browser and asset-registry reconciliation respond only to mounted
filesystem or live registry changes, so scene/component edits such as Spline
control-point operations never scan asset mounts while asset authoring,
Undo/Redo, manual refresh, and external-change workflows remain coherent.

## Evidence

A committed Spline control-point edit currently produces an incremental asset
registry scan such as:

```text
Scanned 17 asset package(s) in 15.245 ms: 17 reused, 0 reparsed,
0 header read(s), 0 reference payload read(s), 0 removed, 0 failed.
```

The scan reuses every cached package and reads no package data, but it still
enumerates all automatically scanned mounts and reconciles persistent registry
and reference snapshots. The cost is unrelated to the edited level object and
will grow with mounted content.

The trigger is semantic coupling rather than Spline evaluation:

- Spline snapshot and transform-gizmo transactions report
  `IEditorTransaction::MutatesContent()`.
- `FEditorTransactionManager` advances one global
  `ContentMutationRevision` for Execute, CommitApplied, Undo, and Redo.
- `FContentBrowserPanel::SynchronizeContentMutation()` treats either that
  revision or the live asset-registry revision as requiring `Refresh(true)`.
- `Refresh(true)` calls
  `FAssetRegistry::ScanMountedContent(Incremental)`.

`MutatesContent` currently conflates in-memory package/object edits with
mounted filesystem mutations. Package dirty state and per-package editor
revision tracking already have separate transaction-manager machinery and must
not depend on a Content Browser scan. See
[Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md),
[Content Browser](../Editor/Architecture/ContentBrowser.md), and
[Asset Packages](../Runtime/Assets/AssetPackages.md).

## Required Changes

1. Replace the ambiguous transaction-level content-mutation signal and revision
   with an explicitly named mounted-content/discovery mutation contract. An
   enum or flags are acceptable if required by existing operations, but the
   contract must distinguish at least:
   - ordinary in-memory object/package edits; and
   - mutations that can change automatically scanned files, directories, or
     registry discovery identity.
2. Classify every current `MutatesContent()` implementation. Spline edits,
   transform-gizmo edits, and other scene/property transactions must not publish
   mounted-content invalidation. Content Browser filesystem transactions and
   asset relocation transitions must publish it for Execute, Undo, and Redo
   when their successful transition changes mounted content.
3. Split Content Browser synchronization behavior by source:
   - a mounted-content revision change performs one incremental registry
     reconciliation followed by mount/item snapshot refresh and selection
     repair;
   - a live asset-registry revision change refreshes derived Content Browser
     snapshots without rescanning the filesystem, because the registry has
     already published the metadata change;
   - manual Refresh and the established external-filesystem change path retain
     an explicit incremental scan.
4. Coalesce self-originating Content Browser operations with revision
   observation so one successful create/import/rename/move/delete/Fix Up or
   Undo/Redo transition cannot cause both an immediate scan and a second scan
   on the next panel draw.
5. Define failure acknowledgement explicitly. A failed automatic
   reconciliation must surface its error, must not claim the affected revision
   was synchronized, and must not retry every frame. Manual Refresh or a later
   mounted-content revision must provide a bounded retry path.
6. Update durable editor architecture documentation to name the separated
   revision/invalidation semantics and remove the implication that unrelated
   editor transactions require registry reconciliation.

Do not solve this task by suppressing the `AssetRegistry` log, adding only a
timer/debounce, or optimizing the no-op scan while retaining the incorrect
trigger. Those may reduce visibility or frequency but preserve the scaling and
semantic defect.

## Protected Invariants

- Ordinary reflected edits, Spline edits, and transform-gizmo edits still
  create exactly their established transaction entries, package revision
  transitions, dirty-state changes, and Undo/Redo behavior.
- Asset create/import/reimport/rename/move/delete/relocation/Fix Up operations
  and their Undo/Redo transitions remain visible in every Content Browser
  panel without reopening the editor.
- The live asset-registry revision continues to advance only when published
  asset or reference metadata changes.
- Manual Refresh still reconciles all registered auto-scan mounts, and a
  surviving current directory and selection are preserved or repaired under
  the existing Content Browser rules.
- Failed filesystem or registry work remains observable and never publishes a
  partially synchronized view.
- Content Browser synchronization must not retain panels or transaction
  objects beyond their established lifetimes or steal focus after unrelated
  global history commands.

## Likely Working Set

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorTransaction.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- Current mounted-content transaction implementations, including Spline,
  transform gizmo, Content Browser operations, and asset relocation
- Focused editor/native tests covering transaction revisions and Content
  Browser synchronization
- `Documentation/Editor/Architecture/ContentBrowser.md`

Expand this set only when a current mutation producer or required test seam is
outside it.

## Acceptance

- A committed Spline point move, point-property edit, topology action, Actor or
  component transform, and their Undo/Redo transitions invoke
  `ScanMountedContent` zero times and emit no `AssetRegistry` scan diagnostic.
- Ordinary in-memory edits retain their existing history count, package dirty
  transitions, cancellation behavior, and save-checkpoint behavior.
- Each successful mounted-content operation and each successful Undo/Redo
  transition causes at most one incremental scan per synchronization cycle;
  all open Content Browser panels converge on the new registry and filesystem
  view.
- A live registry publication with no unobserved mounted-filesystem mutation
  refreshes Content Browser items without invoking `ScanMountedContent`.
- Manual Refresh and a simulated external mounted-file change still perform an
  incremental scan and publish added, changed, and removed items correctly.
- A forced reconciliation failure reports the error, avoids a per-frame scan
  loop, and succeeds after the documented retry trigger without losing the
  pending invalidation.
- Focused automated tests assert scan call counts for unrelated editor
  transactions, mounted-content Execute/Undo/Redo, registry-only publication,
  duplicate-scan prevention, and failure/retry behavior.
- Relevant transaction, Content Browser, asset-registry, viewport, and Spline
  tests pass, followed by a successful full `all` build.
- Durable documentation describes the final invalidation ownership and the task
  file is deleted in the implementation commit.
