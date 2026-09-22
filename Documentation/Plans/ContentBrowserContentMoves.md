# Content Browser Content Moves Plan

Summary: Support moving ordinary files, folders, and mixed selections through the content browser.

Last reviewed: 2026-09-22

Status: Completed
Completed: 2026-09-22

## Current Status

Implemented unified physical preflight and journaled ordinary-file moves, batched asset relocation, folder rename reuse, and owned path drag payloads for mixed selections. Single-asset payload compatibility is retained. The owning [Content Browser contract](../Editor/Architecture/ContentBrowser.md#operations) documents the behavior and failure limits.

Validation on 2026-09-22: `DevTool test affected --report` passed ContentBrowserWorkflowTests, EditorAssetWorkflowTests, StaticMeshThumbnailTests, and ThumbnailTests. `DevTool build --target ContentBrowser` passed. New tests cover mixed relocation, ordinary multi-selection, empty directories, duplicate targets, ownership protection, restoration, failed restoration without overwriting replacement content, committed projection failure, and real ImGui payload production. Two existing symlink cases are skipped because Windows does not grant symlink creation privileges. Native UI tests run without an OS window; an interactive editor smoke test was not run.

The projection-failure test initially reused a resident package name from an earlier case; an isolated run established the test-order dependency, and a unique package name fixed the batch run. Changed-document and all-plan validation passed. Changes are committed with this plan and Stage 0 provenance.

## Goal

Provide one browser operation for moving content items, preserving asset relocation semantics and companion ownership. Folder rename uses the same operation. Dragging a selection supports files, folders, and assets; single-asset drag remains compatible with asset consumers elsewhere in the editor.

The operation preflights the whole selection, collapses selected descendants, rejects destination collisions, mount roots, read-only mounts, reparse points, unknown packages, independently selected owned companions, and moves into a source subtree. Folder moves preserve empty directories and relocate registered packages in one asset batch. Ordinary content stays within the same scanned writable mount; cross-device copying and clipboard UI are outside this change.

Ordinary file moves are journaled and reversed on failure. Asset relocation retains its existing partial-effect and backup diagnostics; this work does not promise crash atomicity or invent asset rollback. Cleanup after committed asset relocation can leave redirectors or source directories and reports a warning. See [asset mutation](../Runtime/Assets/AssetCatalogAndMutation.md#synchronous-relocation).

## Implementation Stages

### Stage 0: Implement and validate content moves

- [x] Add unified selection preflight and execution, and reuse it for folder rename.
- [x] Connect file, folder, and mixed-selection dragging while preserving single-asset payload compatibility.
- [x] Cover ordinary and mixed moves, ownership, collisions, invalid destinations, rollback, and publication in native tests.
- [x] Document the implemented behavior in the owning editor contract.
- [x] Pass relevant native tests and editor compilation following [testing](../Agents/Testing.md) and [build guidance](../Agents/BuildAndRun.md).
- [x] Pass changed-document and all-plan validation, review the diff, and commit with plan/stage provenance.
