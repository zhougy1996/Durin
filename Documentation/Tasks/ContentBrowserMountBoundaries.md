# Enforce Content Browser Mount Boundaries

## Outcome

Content Browser navigation and every filesystem-backed authoring operation stay
inside automatically scanned mounts, and mutations are rejected unless the
owning mount is authoring-writable.

This is a bounded policy-enforcement task, not a mount-system redesign. Complete
the required changes and validation as one outcome, then delete this file in the
implementation commit.

## Evidence

`FContentBrowserModel::PhysicalToVirtualDirectory` classifies paths against all
registered asset mounts, while `NavigateToPhysical` accepts any classified
directory. Session restoration and `FContentBrowserPanel::RevealDirectory` can
therefore enter a mount excluded from the Content Browser's `bAutoScan` mount
snapshot. Once there, ordinary folder creation and filesystem rename paths do
not consistently enforce `bAuthoringWritable`.

## Required Changes

1. Make Content Browser navigation accept only directories owned by the current
   automatically scanned mount snapshot.
2. Centralize Content Browser mount lookup so navigation and filesystem-backed
   operations use the same normalized containment and mount-identity rules.
3. Require an authoring-writable owning mount before creating or renaming an
   ordinary file or folder, including folder relocation paths.
4. Ensure stale saved directories and reveal requests targeting excluded mounts
   fail safely and leave or recover to a valid Content Browser directory.
5. Add focused tests for `bAutoScan == false`, `bAuthoringWritable == false`,
   stale session paths, and valid writable auto-scan mounts.

## Protected Invariants

- Preserve the navigation history behavior for valid Content Browser paths.
- Preserve read-only browsing of auto-scan mounts while rejecting authoring
  mutations there.
- Do not weaken AssetCore mount dependency or authoring checks.
- Preserve the contract in
  [Content Browser architecture](../Editor/Architecture/ContentBrowser.md).

## Likely Working Set

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Non-auto-scan mounts cannot become the current Content Browser directory
  through direct navigation, reveal, or saved-session restoration.
- Filesystem mutations in non-writable mounts fail before changing disk state
  and return an actionable diagnostic.
- Valid writable auto-scan mounts retain existing navigation and authoring
  behavior.
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
