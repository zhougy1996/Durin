# Editor Workspace Namespace Refactor Plan

Summary: Move the reusable workspace/document framework into `Durin::Editor`, shorten its public vocabulary, split its monolithic contract by responsibility, and remove redundant registration paths.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

All stages are complete. Workspace and document lifecycle contracts now live in
`Durin::Editor` behind responsibility-specific `WorkspaceTypes.h`, `Workspace.h`,
and `WorkspaceManager.h` headers. The root-window and presentation boundaries
use `WorkspaceRootWindow.h` and `WorkspaceUI.h`, and every repository consumer
uses the shortened names without root-namespace compatibility aliases.

The unused one-at-a-time registration wrappers were removed, leaving the atomic
scoped `RegisterBatch()` path. Stable ImGui names and IDs are pinned by tests.
`EditorShellTests` passes 34 tests, all six concrete integration targets pass,
the complete 60-target native test suite passes, the `Win64-Debug-DurinEditor`
full `all` build passes, and the hidden Sandbox startup/exit smoke succeeds.

## Goal

- Establish `Durin::Editor` as the owner of reusable workspace and document
  lifecycle APIs.
- Make qualified names concise, for example `Editor::FWorkspaceManager`,
  `Editor::FDocumentId`, and `Editor::IWorkspace`.
- Separate value/interface contracts from manager and registration ownership so
  concrete workspace headers do not include manager-private API by default.
- Keep one atomic registration path and preserve document open, activation,
  deferred replacement, dirty-close, and scoped unregistration behavior.
- Move workspace presentation helpers under `Durin::Editor::WorkspaceUI` and
  remove namespace-redundant `Editor` stems.

## Scope

- `DurinEd/Public/Editor/EditorWorkspace.h` and its implementation.
- `EditorWorkspaceRootWindow.h/.cpp` and `EditorWorkspaceUI.h/.cpp`.
- MainFrame and all five concrete workspace modules that consume these APIs.
- Native tests and current architecture/plan documents that name the migrated
  contracts or headers.
- Removal of the unused one-at-a-time registration wrappers.

## Non-Goals

- Changing workspace/document lifecycle behavior, layout versions, persisted
  ImGui identities, document keys, workspace type string values, or menu labels.
- Moving `DEditorEngine` or other reflected classes into a C++ namespace.
- Migrating notification, asset-picker, asset-retention, preview-scene,
  thumbnail, source-management, or compatibility APIs; each has a separate
  ownership and validation surface.
- Renaming concrete module-owned types such as `FLevelEditorWorkspace` or
  `FMaterialEditorWorkspace`.
- Introducing a generic service locator, workspace base implementation,
  callback registry, or compatibility aliases in the `Durin` root namespace.

## Public Contract Design

### Header boundaries

| Header | Responsibility |
| --- | --- |
| `Editor/WorkspaceTypes.h` | IDs, document metadata/requests/results, descriptors, and asset routes |
| `Editor/Workspace.h` | `IWorkspace` and only the contracts required by concrete workspace implementations |
| `Editor/WorkspaceManager.h` | registration batch/lease and document/registry manager ownership |
| `Editor/WorkspaceRootWindow.h` | reusable ImGui root-window and per-document host state |
| `Editor/WorkspaceUI.h` | stable window/dock identity and presentation helpers |

`Workspace.h` includes `WorkspaceTypes.h`; `WorkspaceManager.h` includes
`Workspace.h`; root-window/UI headers include the narrowest contract they use.
The old `EditorWorkspace*.h` filenames are removed after all repository callers
migrate. No forwarding headers remain.

### Naming map

| Current | Selected |
| --- | --- |
| `Durin::FEditorWorkspaceTypeId` | `Durin::Editor::FWorkspaceTypeId` |
| `FEditorDocumentId/Tab/Request` | `FDocumentId/Tab/Request` |
| `EEditorDocumentOpenResult` | `EDocumentOpenResult` |
| `EEditorDocumentCloseResult/Response` | `EDocumentCloseResult/Response` |
| `EEditorDocumentPolicy` | `EDocumentPolicy` |
| `FEditorWorkspaceDescriptor` | `FWorkspaceDescriptor` |
| `FEditorAssetEditorRegistration` | `FAssetEditorRegistration` |
| `IEditorWorkspace` | `IWorkspace` |
| `FEditorWorkspaceRegistration/Batch/Handle` | `FWorkspaceRegistration/Batch/Handle` |
| `FEditorWorkspaceManager` | `FWorkspaceManager` |
| `FEditorWorkspaceRootWindow...` | `FWorkspaceRootWindow...` |
| `FEditorWorkspaceDocumentHost` | `FWorkspaceDocumentHost` |
| `Durin::EditorWorkspaceUI` | `Durin::Editor::WorkspaceUI` |

Other `Editor` stems are removed when the enclosing namespace supplies exactly
the same meaning: `MakeEditorRootWindowName()` becomes `MakeRootWindowName()`,
`MakeEditorHostDockSpaceId()` becomes `MakeHostDockSpaceId()`, and
`SubmitEditorHostDockSpace()` becomes `SubmitHostDockSpace()`. Generic dock,
panel, and window-class helper names already remain specific and do not change.

### Behavioral invariants

- Workspace type strings, document keys, root keys, labels, layout versions,
  and ImGui hash inputs remain byte-for-byte stable across the migration.
- `RegisterBatch()` validates the whole batch before mutation and its lease
  continues to unregister routes and documents in one operation.
- Deferred singleton replacement never discards the current active document
  before the replacement succeeds.
- Exactly one dirty-close request may be pending, and failed Save/Discard keeps
  that request pending.
- Scoped unregistration preserves the current deterministic fallback document
  activation and never leaves an active ID referring to removed metadata.
- Per-resource root-window drawing continues to defer manager mutation until
  document iteration completes.

## Stages

### Stage 1: Core contract and manager boundary

- [x] Add `WorkspaceTypes.h`, `Workspace.h`, and `WorkspaceManager.h` under
  `Durin::Editor` with the selected names.
- [x] Rename `EditorWorkspace.cpp` to `WorkspaceManager.cpp` and migrate its
  registry/detail state into `Durin::Editor`.
- [x] Remove `RegisterWorkspace()` and `RegisterAssetEditor()`; retain only the
  validated, scoped `RegisterBatch()` entry.
- [x] Migrate MainFrame, LevelEditor, MaterialEditor, TextureEditor,
  StaticMeshEditor, SkeletalMeshEditor, and tests to the narrowest new header.
- [x] Remove `EditorWorkspace.h` without adding root-namespace aliases or a
  forwarding header.
- [x] Run `EditorShellTests` and the smallest concrete editor test targets that
  compile registration integrations touched by this stage.

### Stage 2: Workspace presentation boundary

- [x] Rename the root-window and UI source/header pairs to
  `WorkspaceRootWindow` and `WorkspaceUI`.
- [x] Move root-window/document-host types into `Durin::Editor` and shorten
  their redundant names.
- [x] Move helpers to `Durin::Editor::WorkspaceUI`, shorten only redundant
  `Editor` function stems, and verify stable names/IDs against pre-migration
  expectations.
- [x] Migrate MainFrame and concrete editor panel/window consumers.
- [x] Extend `EditorShellTests` to pin host dock-space, root-window, document
  root, dock-class, dock-space, and panel-window identity strings/IDs.

### Stage 3: Documentation and qualification

- [x] Update `WorkspaceFramework.md`, current related architecture documents,
  active plans, and `DurinEd` public include references.
- [x] Search source, tests, and non-archived documentation for old symbols and
  `EditorWorkspace*.h`; retain only test-suite names when they remain useful.
- [x] Run `EditorShellTests` after the final source state.
- [x] Run `test --target all` because the source-breaking public migration spans
  multiple native-test targets.
- [x] Complete a `Win64-Debug-DurinEditor` full `all` build.
- [x] Run a hidden Sandbox editor startup/exit smoke to qualify MainFrame
  registration, default workspace activation, and default-document opening.
- [x] Validate all plans and mark this plan completed with final evidence.

## Validation

- `\.\DevTool.bat test --target EditorShellTests`: 34/34 tests pass.
- `StaticMeshTests`, `SkeletalMeshEditorTests`, `MaterialThumbnailTests`,
  `TextureThumbnailTests`, `StaticMeshThumbnailTests`, and
  `EditorRenderingTests`: all pass.
- `\.\DevTool.bat test --target all`: all 60 native-test targets pass.
- `\.\DevTool.bat build --target all`: `Win64-Debug-DurinEditor` passes.
- `\.\DevTool.bat run --project Sandbox\Sandbox.dproject --args --hidden-window --exit-after-ticks=2`: passes.
- `\.\DevTool.bat doc plan validate --scope all`: passes.

## Risks and Controls

- Namespace and header changes are source-breaking across every editor module.
  Keep each stage buildable, migrate all repository consumers in the owning
  stage, and do not mask omissions with compatibility aliases.
- Renaming UI helpers can accidentally change persisted ImGui identities.
  Preserve every input string and layout version, and pin representative
  outputs before removing the old implementations.
- Splitting the public header can reveal accidental transitive includes. Add
  direct includes only where the consumer uses the corresponding contract; do
  not restore the monolithic dependency through an umbrella include.
- Removing registration wrappers is safe only while the repository has no
  caller. Re-run the call-site search immediately before deletion.
