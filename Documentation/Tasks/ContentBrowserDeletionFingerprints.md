# Strengthen Content Browser Deletion Fingerprints

## Outcome

Content Browser deletion confirmation detects a file whose bytes changed after
preflight even when its path, kind, size, and modification timestamp are
unchanged.

This is a bounded stale-plan safety task, not a general content-addressed storage
system. Complete the required changes and validation as one outcome, then delete
this file in the implementation commit.

## Evidence

`FContentDeletionFingerprint` and `CalculateFingerprintDigest` cover only the
normalized path, entry kind, file size, and modification timestamp. A same-size
rewrite or replacement that preserves the timestamp passes
`IsDeletionPlanCurrent`, allowing deletion without the required second
confirmation for the changed bytes.

## Required Changes

1. Add an `FXxHash128` byte identity for ordinary files, managed companions,
   redirector/package files, and directory descendant digests.
2. Compute the identity by streaming every file through `FXxHash128Builder` in
   fixed-size chunks so large package and bulk files use bounded memory. Extract
   or reuse a shared file-hash helper rather than retaining a Content
   Browser-specific whole-file reader.
3. Revalidate the same identity before first execution and for staged content
   before Redo.
4. Keep the immutable plan shared by the confirmation modal and transaction.
5. Add tests for same-size, timestamp-preserving rewrites before Execute and
   before Redo, plus unchanged-content success.

## Protected Invariants

- Preflight and revalidation do not load or deserialize asset objects.
- Fingerprinting failures block deletion before mutation with an actionable
  diagnostic.
- Directory fingerprints remain deterministic and independent of traversal
  order.
- Preserve the recursive deletion and recovery contract in
  [Content Browser architecture](../Editor/Architecture/ContentBrowser.md).

## Likely Working Set

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentDeletionTransaction.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
- AssetCore fingerprint helpers or tests if an existing package identity is reused

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- A same-size, timestamp-preserving byte change invalidates the deletion plan
  and requires fresh confirmation.
- A staged byte change invalidates Redo without moving or publishing content.
- Unchanged plans retain existing Execute, Undo, Redo, and cleanup behavior.
- Run the focused deletion transaction tests:

  ```powershell
  .\DevTool.bat test --target EditorAssetWorkflowTests --filter FContentBrowserModelTests.*Deletion* --agent
  ```

- Complete a successful full editor build:

  ```powershell
  .\DevTool.bat build --target all --agent
  ```

- Update the lasting Content Browser contract if the selected fingerprint
  strategy changes its documented guarantees.
- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
