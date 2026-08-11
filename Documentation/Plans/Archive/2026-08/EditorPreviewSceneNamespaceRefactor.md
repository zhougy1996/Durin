# Editor Preview Scene Namespace Refactor Plan

Summary: Move the shared editor preview-scene lifetime contract into `Durin::Editor` without changing world, renderer, or consumer ownership behavior.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

All stages are complete. `FPreviewScene` now lives in the flat
`Durin::Editor` namespace with its existing type and `PreviewScene.h/.cpp`
filenames. Material, static-mesh, skeletal, and shared rendered-thumbnail
consumers use `Editor::FPreviewScene` directly, and the lasting skeletal-editor
architecture documentation names the new owner. `MaterialTests` passes 78
tests, `StaticMeshTests` passes 52 tests, `SkeletalMeshEditorTests` passes 3
tests, and `ThumbnailTests` passes 53 tests.

The lifecycle implementation changed only its enclosing namespace; construction,
delegation, teardown, renderer flushing, diagnostics, and object names remain
unchanged. The full editor `all` build, complete native-test suite at default
target granularity with serial scheduling, and hidden two-tick Sandbox startup
all pass on `windows-msvc-x64` / `Win64-Debug-DurinEditor`.

Parallel complete-suite attempts at 18, 14, and 8 jobs each exposed the same
unrelated `LaunchProcessBoundaryTests` child-process 15-second timeout. Its
reported case passed in isolation, the complete four-case target passed, and
the final serial complete suite passed without skipping a target. The thumbnail
contract family remains explicitly deferred.

## Goal

- Complete `Durin::Editor` ownership for the shared editor-only preview world and renderer scene.
- Preserve game-thread ownership and renderer/world teardown ordering.
- Migrate material, static-mesh, skeletal, and rendered-thumbnail consumers atomically.
- Remove the root-namespace contract without aliases or forwarding headers.

## Scope

- `DurinEd` `FPreviewScene` public contract and implementation.
- MaterialEditor, StaticMeshEditor, SkeletalMeshEditor, and DurinEd rendered-thumbnail consumers.
- Native tests that exercise preview construction, world/component lifetime,
  rendered-thumbnail scene leasing, and teardown.
- Non-archived editor architecture documentation that names `FPreviewScene`.

## Non-Goals

- Renaming `FPreviewScene` or `PreviewScene.h/.cpp`.
- Moving `DWorld`, `DLevel`, `IScene`, renderer interfaces, or reflected runtime types.
- Moving or decomposing thumbnail request, provider, cache, service, scheduler,
  pipeline, preview-pool, generation-session, or extension contracts.
- Moving concrete material, static-mesh, or skeletal preview implementations into module namespaces.
- Changing preview naming, world type, actor/component ownership, ticking,
  BeginPlay/EndPlay behavior, render-scene creation, render-command flushing, or diagnostics.
- Adding compatibility aliases, deprecated spellings, or forwarding headers.

## Design Decisions and Invariants

- `FPreviewScene` lives directly in `Durin::Editor`; the responsibility-bearing
  type and file names remain unchanged.
- Runtime world, level, and renderer types remain owned by `Durin`; the editor
  contract references those parent-namespace types without relocating them.
- Construction remains game-thread-only. It requires the engine renderer,
  creates one renderer scene, creates an ordinary preview world and level,
  roots the world, sets `EWorldType::Preview`, attaches the renderer scene, and
  installs the current level in the existing order.
- Destruction remains game-thread-only. It ends play, detaches the current
  level and renderer scene, releases and flushes renderer work when required,
  then unroots and garbage-marks the world hierarchy in the existing order.
- `BeginPlay()`, `Tick()`, and `EndPlay()` remain thin world delegates with the
  same null tolerance and arguments.
- Concrete editors and the rendered-thumbnail preview pool keep their present
  ownership, creation frequency, names, actors, components, viewports, and
  release sequencing; only their qualified shared type changes.
- Existing strings, diagnostics, object names, callback order, and thread
  assertions remain byte-for-byte or semantically unchanged as applicable.
- All consumers migrate in one change. A root-namespace alias would obscure the
  selected ownership boundary and is not permitted.

## Current Foundations and Gaps

- Transactions, workspaces, interaction services, asset support services, and
  compatibility audits already establish the flat `Durin::Editor` boundary.
- `FPreviewScene` is editor-only and physically owned by DurinEd, but its public
  contract and implementation still live in the `Durin` root namespace.
- Material, StaticMesh, and SkeletalMesh previews each own one `FPreviewScene`.
  The DurinEd rendered-thumbnail pool also owns the same shared contract.
- Existing native tests exercise concrete preview world/component teardown and
  rendered-thumbnail preview reuse, but their validation must be rerun across
  every affected target after the public namespace migration.

## Implementation Stages

### Stage 0: Select ownership and validation boundary

- [x] Inventory source, test, and non-archived documentation consumers.
- [x] Select direct `Durin::Editor` ownership without renaming the type or files.
- [x] Record game-thread and renderer/world teardown invariants.
- [x] Defer the thumbnail contract family to a separate plan.

#### Acceptance Gate

- The migration has one selected public owner and spelling.
- Existing tests cover every concrete preview consumer and shared rendered-thumbnail usage.
- No reflected or persistent contract requires a compatibility layer.

### Stage 1: Move the shared preview-scene contract

- [x] Move the `FPreviewScene` declaration and implementation into `Durin::Editor`.
- [x] Keep `DWorld`, `DLevel`, `IScene`, and renderer contracts in their existing owners.
- [x] Preserve construction, delegation, teardown, flushing, and diagnostics exactly.
- [x] Confirm the public header remains self-contained under the new namespace.

#### Acceptance Gate

- DurinEd compiles with `Editor::FPreviewScene` as the only public spelling.
- Searches find no root-namespace declaration, definition, alias, or forwarding header.
- The lifecycle implementation diff contains no behavioral reordering.

### Stage 2: Migrate consumers and lifecycle coverage

- [x] Update MaterialEditor, StaticMeshEditor, and SkeletalMeshEditor consumers with direct qualification.
- [x] Update the DurinEd rendered-thumbnail preview-scene pool with direct qualification.
- [x] Update non-archived architecture documentation to name `Editor::FPreviewScene`.
- [x] Run the focused preview and rendered-thumbnail validation targets.

#### Acceptance Gate

- `MaterialTests`, `StaticMeshTests`, `SkeletalMeshEditorTests`, and `ThumbnailTests` pass.
- Material, static-mesh, skeletal, and rendered-thumbnail preview creation and teardown remain covered.
- No runtime module or unrelated editor module consumes the migrated contract.

### Stage 3: Final validation and plan completion

- [x] Search source, tests, and non-archived documentation for stale root-namespace references.
- [x] Run the complete native-test suite because the public API migration crosses test targets.
- [x] Complete a full editor `all` build because the public DurinEd DLL boundary changes.
- [x] Run a hidden Sandbox startup/exit smoke using the same Agent Build Profile.
- [x] Validate all plans and record final evidence.

#### Acceptance Gate

- Every validation-matrix entry passes.
- The full editor build and startup smoke pass from the same profile.
- No compatibility alias, forwarding header, stale documentation reference, or behavioral change remains.

## Validation Matrix

| Surface | Validation |
| --- | --- |
| Material preview and rendered-thumbnail scene lifetime | `MaterialTests` |
| Static-mesh preview world/component lifetime | `StaticMeshTests` |
| Skeletal preview world/component lifetime | `SkeletalMeshEditorTests` |
| Shared rendered-thumbnail preview pool | `ThumbnailTests` |
| Cross-target public API migration | Complete native-test suite at default target granularity |
| DurinEd and concrete editor DLL consumers | Full editor `all` build |
| MainFrame, default workspace, and startup integration | Hidden Sandbox startup/exit smoke |
| Plan lifecycle and links | All-plan validator |

## Definition of Done

- `FPreviewScene` lives only in `Durin::Editor` with its existing type and filenames.
- World, level, renderer, thread, teardown, and consumer ownership behavior remain unchanged.
- All concrete and shared-thumbnail consumers use the new owner directly without compatibility surfaces.
- Focused tests, complete native tests, full editor build, startup smoke, and
  plan validation pass with recorded evidence.

## Deferred Follow-ups

- Move the shared thumbnail contract family into `Durin::Editor`.
- Decompose thumbnail public headers by request/view, provider registration,
  generation session, scheduling, cache/persistence, and preview-pool responsibility.
- Review module-specific namespaces for concrete preview implementations after
  shared DurinEd contracts are complete.

## Related Documentation

- [Editor Asset Support Namespace Refactor](EditorAssetSupportNamespaceRefactor.md)
- [Editor Common Namespace Refactor](EditorCommonNamespaceRefactor.md)
- [Asset Thumbnails](../../../Editor/Architecture/AssetThumbnails.md)
- [Skeletal Asset Editor](../../../Editor/Architecture/SkeletalAssetEditor.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Preview/PreviewScene.h`
- `Engine/Source/Editor/DurinEd/Private/Preview/PreviewScene.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp`
- `Engine/Source/Editor/StaticMeshEditor/Private/Widgets/StaticMeshPreview.cpp`
- `Engine/Source/Editor/SkeletalMeshEditor/Private/Widgets/SkeletalAssetPreview.cpp`
- `Engine/Tests/Native/EngineTests`
