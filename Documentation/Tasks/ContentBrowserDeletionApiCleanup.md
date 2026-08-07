# Remove Legacy Content Browser Deletion Paths

## Outcome

The reversible `FContentDeletionTransaction` workflow is the only production
Content Browser deletion path, with no public helper that can bypass recursive
preflight, immutable-plan revalidation, history, or staging.

This is a bounded API cleanup task, not a deletion behavior redesign. Complete
the required changes and validation as one outcome, then delete this file in the
implementation commit.

## Evidence

`FContentBrowserOperations` still publicly exposes the legacy `Delete` method
and its private `DeleteEmptyFolder` helper. Production UI currently uses
`BuildDeletionPlan` and `FContentDeletionTransaction`, but the legacy method can
irreversibly delete assets, files, or empty folders without the new transaction
contract. Its remaining known caller is an obsolete direct-delete test.

## Required Changes

1. Remove the public legacy `Delete` API and `DeleteEmptyFolder` implementation,
   or make any retained low-level primitive unreachable from Content Browser
   production callers and incapable of bypassing the transaction owner.
2. Replace obsolete tests with assertions against deletion-plan blockers and
   transaction behavior.
3. Search production and test code for direct filesystem or `Asset::DeleteAsset`
   calls initiated by Content Browser deletion and route them through the shared
   transaction.
4. Keep delete commands disabled when editor history is unavailable.

## Protected Invariants

- Recursive deletion continues to inspect hidden and filtered descendants.
- Asset reference, redirector closure, companion ownership, mount, reparse, and
  source-control blockers remain pre-mutation gates.
- Delete, Undo, and Redo continue to publish content mutation revisions.
- The transaction remains the sole owner of physical staging and cleanup.

## Likely Working Set

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Repository search finds no production Content Browser deletion route that
  bypasses `FContentDeletionPlan` and `FContentDeletionTransaction`.
- Former direct-delete coverage is expressed through preflight and transaction
  tests without reducing failure coverage.
- Run the focused Content Browser deletion tests:

  ```powershell
  .\DevTool.bat test --target EditorAssetWorkflowTests --filter FContentBrowserModelTests.*Deletion* --agent
  ```

- Complete a successful full editor build:

  ```powershell
  .\DevTool.bat build --target all --agent
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
