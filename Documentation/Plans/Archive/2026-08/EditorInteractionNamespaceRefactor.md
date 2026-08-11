# Editor Interaction Namespace Refactor Plan

Summary: Move shared editor notification, asset-picker, and play-session value contracts into `Durin::Editor` while preserving the reflected editor-engine boundary.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

All stages are complete. Notification and asset-picker contracts now live in
`Durin::Editor` behind `Notification.h` and `AssetPicker.h`; redundant `Editor`
stems were removed, and stateless picker helpers live in
`Durin::Editor::AssetPicker`. Play-session enums and request values also live in
`Durin::Editor`, while reflected `DEditorEngine` and `GEditor` remain in `Durin`.

`EditorShellTests` passes 34 tests, `WorldTests` passes 79 tests, and
`StaticMeshTests` passes 52 tests. The complete native-test suite passes at the
repository-qualified 14-job baseline, the `Win64-Debug-DurinEditor` full `all`
build passes, and the hidden Sandbox startup/exit smoke succeeds. An initial
aggregate run exposed stale dynamically loaded editor DLLs before the required
full build; both affected Launch cases passed after that build. A later isolated
Core concurrency soak retry also passed before the final aggregate succeeded.

## Goal

- Complete the `Durin::Editor` ownership boundary for shared interaction services.
- Remove namespace-redundant `Editor` stems from notification, asset-picker,
  and play-session value types.
- Preserve notification threading/lifetime behavior, asset-picker UI identity,
  and play-in-editor lifecycle behavior.
- Keep the reflected editor-engine ABI and generated-code boundary intact.

## Scope

- `DurinEd` notification and asset-picker public contracts and implementations.
- `DEditorEngine` notification ownership and ordinary play-session value types.
- LevelEditor, MaterialEditor, Launch diagnostics, and native-test consumers.
- Direct public include names, active documentation, and test names that encode
  the migrated public vocabulary.

## Non-Goals

- Moving `DEditorEngine`, `GEditor`, or reflected engine/object types into a C++ namespace.
- Changing notification behavior, toast identity, picker ImGui IDs, filtering,
  assignment behavior, or play-session state transitions.
- Migrating asset retention, preview scenes, source management, thumbnails, or
  concrete editor-module types.
- Providing aliases or forwarding headers for the old root-namespace APIs.

## Design Decisions and Invariants

- Notification types live directly in `Durin::Editor`; qualified names use
  `FNotificationManager`, `FNotificationDesc`, and related concise forms.
- Asset-picker value types live directly in `Durin::Editor`; stateless drawing
  and filtering helpers live in `Durin::Editor::AssetPicker`.
- Play-session value types live directly in `Durin::Editor`; `DEditorEngine`
  remains in `Durin` and qualifies those contracts explicitly.
- `Notification.h/.cpp` and `AssetPicker.h/.cpp` replace the old
  `EditorNotification` and `EditorAssetPicker` filenames. No forwarding files remain.
- Existing enum values, defaults, callback signatures, strings, ImGui labels,
  and state-machine ordering remain unchanged.
- All repository consumers migrate atomically; root-namespace compatibility
  aliases would defeat the ownership boundary and are not permitted.

## Current Foundations and Gaps

- Transactions, property editing, workspaces, and asset compatibility audits
  already use `Durin::Editor`.
- `EditorEngine.h` already forward-declares `Editor::FTransactionManager`, but
  notification ownership and all play-session value contracts remain in `Durin`.
- Notification and asset-picker headers physically reside under `Public/Editor`
  while their APIs remain in the root namespace.

## Implementation Stages

### Stage 0: Select the reflected boundary and migration vocabulary

- [x] Inventory all source, test, and active-document consumers.
- [x] Keep `DEditorEngine` and `GEditor` in `Durin`.
- [x] Select concise names under `Durin::Editor` and reject compatibility aliases.

#### Acceptance Gate

- The selected mapping has one unambiguous owner for every migrated symbol.
- Reflected declarations and generated-header placement do not change.

### Stage 1: Notification and asset-picker services

- [x] Rename the notification and asset-picker source/header pairs.
- [x] Move their public contracts and implementations into `Durin::Editor`.
- [x] Shorten namespace-redundant type and helper namespace names.
- [x] Update DurinEd, feature-editor, and native-test consumers.
- [x] Confirm old filenames and symbols have no non-archived consumers.

#### Acceptance Gate

- Notification and asset-picker tests pass through the smallest owning native-test target.
- Existing notification and picker behavior remains covered by the migrated tests.

### Stage 2: Play-session value contracts

- [x] Move ordinary play-session enums and request values into `Durin::Editor`.
- [x] Update `DEditorEngine` to use the qualified contracts while remaining in `Durin`.
- [x] Update LevelEditor, Launch diagnostics, and native-test consumers.
- [x] Confirm root namespace contains no migrated play-session symbols.

#### Acceptance Gate

- The smallest native-test target covering editor play lifecycle passes.
- Generated reflection output remains valid and `DEditorEngine` remains root-owned.

### Stage 3: Documentation and final validation

- [x] Confirm lasting editor architecture documentation does not name the migrated contracts.
- [x] Search source, tests, and non-archived documentation for old filenames and symbols.
- [x] Run the affected native-test targets.
- [x] Run the complete native-test suite because the public migration crosses test targets.
- [x] Complete a full editor `all` build and hidden Sandbox startup/exit smoke.
- [x] Validate all plans and record final evidence.

#### Acceptance Gate

- Every validation matrix entry passes.
- The full editor build and startup smoke use the same Agent Build Profile.
- No compatibility alias, forwarding header, or stale active reference remains.

## Validation Matrix

| Surface | Validation |
| --- | --- |
| Notification, picker, and PIE contracts | Smallest affected native-test target |
| Cross-target public API migration | Complete native-test suite |
| Generated reflection and editor integration | Full editor `all` build |
| MainFrame, default workspace, and editor startup | Hidden Sandbox startup/exit smoke |
| Plan lifecycle and links | All-plan validator |

## Definition of Done

- Notification, asset-picker, and play-session value contracts live in `Durin::Editor`.
- Redundant `Editor` stems and old filenames are removed without aliases.
- `DEditorEngine` and `GEditor` remain valid reflected/root contracts.
- All validation gates pass and the plan records their evidence.

## Deferred Follow-ups

- Asset retention, preview-scene, source-management, and compatibility services.
- Thumbnail contract namespace migration and public-header decomposition.
- Module-specific namespace reviews for concrete editor implementations.

## Related Documentation

- [Editor Common Namespace Refactor](EditorCommonNamespaceRefactor.md)
- [Editor Workspace Namespace Refactor](EditorWorkspaceNamespaceRefactor.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/Notification.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/AssetPicker.h`
- `Engine/Source/Editor/LevelEditor`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Source/Runtime/Launch/Private/Diagnostics/EditorPIELifecycleSmoke.cpp`
- `Engine/Tests/Native/EngineTests`
