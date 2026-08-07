# Use Authoritative Content Browser Companion Ownership

## Outcome

Content Browser file operations classify asset-managed companions from
AssetCore's authoritative ownership data instead of inferring ownership from a
shared filename stem.

This is a bounded ownership-classification task, not a general asset relocation
redesign. Complete the required changes and validation as one outcome, then
delete this file in the implementation commit.

## Evidence

`FContentBrowserOperations::IsManagedCompanion` treats every regular file in an
asset package's directory with the same stem as managed. Folder rename preflight
uses the same heuristic. An unrelated `Foo.txt` beside `Foo.dasset` is therefore
blocked from independent rename or deletion, and folder relocation may approve
it as managed even though AssetCore's relocation token does not own it.
AssetCore already derives exact companion files through registered delete and
relocation contributors.

## Required Changes

1. Add or reuse a narrow AssetCore query that reports exact companion ownership
   without duplicating contributor logic in LevelEditor.
2. Replace same-directory/same-stem checks in standalone file operations and
   folder rename preflight with the authoritative query.
3. Treat unclaimed same-stem files as ordinary files and preserve the existing
   behavior for genuinely owned companions.
4. Ensure folder relocation never reports complete success while leaving an
   approved-but-unmoved file or stale directory tree behind; propagate or
   compensate cleanup failures according to the relocation transaction
   contract.
5. Add tests covering unrelated same-stem files, real companions, ambiguous
   ownership, and folder relocation behavior.

## Protected Invariants

- Asset-managed companions cannot be independently renamed, moved, or deleted.
- Companion providers and AssetCore remain the source of truth for ownership.
- Do not broaden an asset relocation token after its analysis and revalidation.
- Preserve reference-aware relocation and the
  [Content Browser architecture](../Editor/Architecture/ContentBrowser.md).

## Likely Working Set

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
- AssetCore companion or relocation tests selected by the implementation

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- An unrelated same-stem file remains independently operable.
- A provider-owned companion remains protected and moves only with its owner.
- Folder relocation either moves its complete analyzed scope or fails without
  claiming complete success.
- Run the focused editor asset workflow and affected AssetCore tests:

  ```powershell
  .\DevTool.bat test --target EditorAssetWorkflowTests --filter FContentBrowserModelTests.* --agent
  .\DevTool.bat test --target AssetPackageTests --agent
  ```

- Complete a successful full editor build:

  ```powershell
  .\DevTool.bat build --target all --agent
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
