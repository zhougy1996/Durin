# Make Texture Cube Selection Details Side-Effect Free

## Outcome

Drawing Content Browser selection details for a TextureCube reads cached or
inspected metadata without loading the asset package on every frame.

This is a bounded presentation-data task, not a TextureCube editor redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

`FContentBrowserPanel::DrawSelectionDetails` calls `Asset::LoadAsset` while
drawing a selected TextureCube. Merely selecting an item can therefore change
package residency, perform synchronous work on the UI thread, and repeat the
lookup on every frame.

## Required Changes

1. Define a side-effect-free TextureCube details snapshot populated from asset
   registry metadata, package inspection, or a bounded cache outside the ImGui
   row emission path.
2. Remove package loading from `DrawSelectionDetails` and keep drawing limited
   to already captured presentation data.
3. Invalidate the snapshot when the selected asset identity, registry revision,
   or relevant package fingerprint changes.
4. Preserve useful source layout, source dimensions, face override, exposure,
   platform-data summary, and build diagnostic fields when they are available;
   represent unavailable fields explicitly without loading the package.
5. Add focused tests for snapshot derivation and invalidation, including corrupt
   or unavailable package metadata.

## Protected Invariants

- Selection details never create, load, dirty, save, or unload an asset package.
- Opening a TextureCube through its registered editor retains existing behavior.
- Rendering remains deterministic when optional metadata is unavailable.
- Keep presentation-only helpers separate from mutable asset workflows.

## Likely Working Set

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserItemView.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserItemView.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserItemViewTests.cpp`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Repeatedly drawing TextureCube selection details does not change loaded
  package residency or invoke synchronous package loading.
- Details update after package or registry changes and fail gracefully for
  unavailable metadata.
- Run the focused presentation tests:

  ```powershell
  .\DevTool.bat test --target EditorAssetWorkflowTests --filter FContentBrowserItemViewTests.* --agent
  ```

- Complete a successful full editor build:

  ```powershell
  .\DevTool.bat build --target all --agent
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
