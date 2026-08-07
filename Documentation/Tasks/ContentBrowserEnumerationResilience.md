# Make Content Browser Enumeration Resilient

## Outcome

A failure to inspect one filesystem entry does not silently truncate the entire
Content Browser snapshot or directory tree.

This is a bounded enumeration-error task, not an asynchronous scanning redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

`FContentBrowserModel::RefreshItemsSnapshot` shares one `std::error_code`
between recursive iterator progress and per-entry classification. The loop also
requires that code to remain clear. A status failure on one inaccessible or
invalid entry can stop traversal before unrelated later entries are captured,
with no diagnostic surfaced to the panel. `GetDirectoryChildren` has the same
silent-failure shape for the tree cache.

## Required Changes

1. Separate iterator-progress errors from per-entry status, size, and timestamp
   errors in recursive item enumeration and directory-tree enumeration.
2. Continue after an isolated entry failure when traversal itself remains safe.
3. Capture bounded, actionable diagnostics for skipped entries without flooding
   the UI or logs on every frame.
4. Keep reparse-point behavior explicit and do not follow a link merely to make
   enumeration continue.
5. Add deterministic tests that inject or construct an entry-level failure and
   verify that later valid siblings remain visible.

## Protected Invariants

- Permission-denied entries and reparse points never expand the browser's
  traversal authority.
- Snapshot and directory-child ordering remains deterministic.
- Hidden-content and type-filter behavior remains unchanged for successfully
  inspected entries.
- Enumeration failures do not throw through the editor frame loop.

## Likely Working Set

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- One failed entry is skipped with a bounded diagnostic while later valid
  siblings remain in the snapshot and tree.
- A traversal-level failure terminates safely and remains distinguishable from
  an empty directory.
- Run the focused Content Browser tests:

  ```powershell
  .\DevTool.bat test --target EditorAssetWorkflowTests --filter FContentBrowserModelTests.* --agent
  ```

- Complete a successful full editor build:

  ```powershell
  .\DevTool.bat build --target all --agent
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
