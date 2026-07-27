# Level Editor Modularization Plan

Summary: Extract reusable editor destination and import primitives and split oversized LevelEditor panels along stable model, operation, and presentation boundaries without changing user-visible behavior.

Last reviewed: 2026-07-28

## Current Status

Stage 0 is complete on baseline commit `8b750d3c`. The prerequisite thumbnail,
typed-mount, mounted-source, and import-workflow changes are established code,
not work coordinated by this plan. No active overlapping-plan handoff is
required before structural refactoring begins.

The unified Core mount registry already owns virtual-path lookup, typed Content
and SourceAssets resolution, reverse physical-path classification, canonical
containment, and mount-owner metadata. LevelEditor must consume those APIs
rather than add another mount-owner or path-resolution abstraction. The
remaining import duplication is editor-only asset-destination validation,
common callback state, and modal lifecycle/presentation.

`ContentBrowserPanel.cpp` and `SceneViewportPanel.cpp` remain the first
file-size and responsibility hotspots. Stage 1 is next and will extract Scene
Viewport presentation helpers behind the unchanged `FSceneViewportPanel`
interface.

## Goal

- Keep mount lookup and typed virtual/physical conversion authoritative in
  Core while removing repeated editor-side destination decisions.
- Give asset import dialogs one reusable destination-validation and callback
  model while preserving source-type-specific forms and import execution.
- Reduce the largest LevelEditor translation units by moving coherent UI,
  model, and operation responsibilities into independently understandable
  components.
- Separate hierarchy and workflow state from immediate-mode drawing where that
  state has useful deterministic tests.
- Preserve LevelEditor workspace ownership, document lifecycle, session
  settings, layout, asset mutation, undo/redo, and user-visible behavior.
- Leave every stage buildable, independently reviewable, and represented by
  one bounded commit.

## Scope

- `LevelEditor` asset-destination parsing, validation, collision diagnostics,
  and browsing behavior repeated by import workflows.
- StaticMesh, Texture2D, and TextureCube import-dialog destination state,
  validation, callbacks, and popup lifecycle.
- Scene Viewport toolbar and overlay presentation.
- Content Browser snapshot/filter/sort/navigation state, item presentation,
  and asset/folder operations.
- World Outliner hierarchy/filter cache and oversized drawing orchestration.
- Details component-tree presentation where it can be separated without
  changing reflected-property editing.
- Level document modal presentation and LevelEditor composition wiring.
- Focused unit, editor integration, UI-scale, and final full-build validation.

## Non-Goals

- Changing editor appearance, commands, shortcuts, responsive breakpoints, or
  persisted setting formats.
- Redesigning Content Browser navigation, selection, import, rename, move,
  delete, thumbnail, or drag/drop behavior.
- Changing mounted-source, provenance, ingest, reimport, multi-asset
  transaction, or generated-asset contracts already established by their
  owning domains.
- Moving concrete LevelEditor panels into `DurinEd`; reusable abstractions may
  move only when they are independent of LevelEditor types and satisfy the
  workspace dependency direction.
- Replacing ImGui, introducing a retained widget hierarchy, or creating a
  general-purpose UI framework as part of file splitting.
- Rewriting transform-gizmo mathematics, viewport picking, reflected-property
  editing, or editor transaction semantics.
- Applying unrelated formatting, naming, comment, or include cleanup.

## Design Decisions and Invariants

### Ownership and dependency direction

- `LevelEditor` remains the owner of its concrete panels, singleton workspace,
  session settings, level document controller, and level-specific asset
  workflows.
- Path helpers that require only Core path and mount types belong beside the
  authoritative path/mount API, not in a LevelEditor panel.
- Reusable import-dialog state initially remains editor-only. It may move to
  `DurinEd` only if it contains no LevelEditor, concrete asset-class, source
  library implementation, or renderer dependency.
- Extracted Content Browser, Outliner, Details, and Viewport presentation types
  remain private to `LevelEditor` until a second editor has an equivalent
  contract. File length alone is not sufficient reason to publish an API.
- `DurinEd` must not gain a dependency on `LevelEditor`, consistent with the
  workspace framework contract.

### Extraction style

- Prefer composition over an import-dialog inheritance hierarchy. A shared
  destination model and callback bundle supply common behavior; each dialog
  retains its source controls, build settings, validation, and final import
  action.
- Keep `FContentBrowserPanel` and `FSceneViewportPanel` as stable panel-facing
  facades. Initial splits move implementation behind those interfaces instead
  of changing panel registration or callers.
- Move code by coherent responsibility. Do not create numbered fragment files
  or split a class across translation units solely to reduce a line count.
- A component that owns cached state also owns its invalidation rules.
  Presentation code consumes immutable views or explicit commands rather than
  mutating another component's containers directly.
- Immediate-mode UI IDs, popup names, table column identities, drag/drop
  payload names, and persisted settings keys remain byte-for-byte stable unless
  a separately documented behavior change requires migration.

### Failure and mutation behavior

- Path and destination validation remain side-effect free.
- Asset/folder operations continue to perform complete validation before the
  first mutation and preserve existing rollback, package-dirty, registry, and
  error-reporting behavior.
- Extracting dialogs does not create a second error channel. The existing
  workspace error reporting and imported-asset notification remain the
  observable endpoints.
- Undo/redo transaction descriptions, before/after state, and package dirty
  restoration remain unchanged.

### Established prerequisite contracts

- Core's immutable typed mount registry remains the sole owner of mount lookup,
  Content and SourceAssets resolution, reverse classification, canonical
  containment, dependency checks, and source-write policy.
- `DurinEd` retains provider-neutral thumbnail request, scheduling, persistent
  cache, and preview-scene ownership described by
  [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md). Content Browser
  refactoring may move only LevelEditor state and presentation.
- Existing mounted-source reference/ingest/repair/relocation behavior and the
  three current import forms are the behavioral baseline. Common dialog
  extraction consumes those workflows without moving their domain logic.
- Static-model multi-asset import/reimport orchestration, generated outputs,
  progress, cancellation, and generated-asset policies remain outside this
  structural plan.

## Current Foundations and Gaps

### Foundations

- `FLevelEditorPanel` gives concrete panels a stable draw/open interface.
- `FLevelEditorContext` centralizes active level, selection, play, focus,
  rename, and error callbacks.
- `FEditorWorkspaceRootWindow`, workspace registration, and document lifecycle
  ownership already have established architecture contracts.
- `FAssetStructureUpgradeModel` already separates compatibility decision state
  from most persistence and activation effects.
- `FContentBrowserThumbnailCache`, source/rendered thumbnail services,
  `FEditorAssetMoveCoordinator`, and `FEditorRenameDialog` demonstrate useful
  extracted component boundaries.
- `LevelEditorHelpers` and `MonaImGui` already provide narrow reusable UI
  helpers and shared design metrics.

### Gaps

- The three import dialogs independently combine `FAssetPath` parsing, typed
  mount lookup, destination collision checks, browsing conversion, and
  user-facing diagnostics even though Core already supplies the underlying
  path results.
- Three import dialogs duplicate callbacks, preferred destination state,
  asset-path buffers, open requests, mounted-destination checks, collision
  checks, destination browsing, and completion reporting.
- `ContentBrowserPanel` owns mount snapshots, directory enumeration, item
  snapshots, filtering, sorting, selection, navigation, thumbnails, three
  presentation modes, asset/folder mutation, drag/drop, and dialogs in one
  class and translation unit.
- The first approximately 500 lines of `SceneViewportPanel.cpp` implement
  toolbar icons, split buttons, surfaces, layout helpers, and orientation
  drawing before the panel implementation begins.
- `FWorldOutlinerPanel::Draw` contains hierarchy-row recursion, selection,
  visibility transactions, context menus, drag/drop, rename, shortcuts, and
  delete confirmation in one function; its deterministic hierarchy/filter
  cache is coupled to the view.
- `FDetailsPanel::DrawComponents` combines component-tree construction,
  instance-component mutation, drag/drop, rename, menus, and presentation.
- `FLevelDocumentController` mixes document workflow with unsaved-level and
  structure-upgrade modal rendering.

## Implementation Stages

### Stage 0: Establish the refactoring baseline

Dependencies: none.

- [x] Record baseline commit `8b750d3c` for the first extraction stage.
- [x] Treat current typed-mount, mounted-source, import, and thumbnail behavior
  as established inputs rather than overlapping implementation work.
- [x] Freeze existing panel public interfaces, ImGui identities, popup names,
  drag/drop payloads, persisted settings keys, transaction descriptions, and
  callback behavior as observable compatibility constraints.
- [x] Identify existing mount-registry and document-upgrade model coverage and
  record the missing destination, Content Browser model, Outliner model, and
  panel-focused coverage in the stages that introduce those seams.
- [x] Keep all extracted panel, dialog, and presenter types module-private;
  reuse Core path APIs and existing `DurinEd` services at their current
  dependency boundaries.

#### Acceptance Gate

- The current repository is the behavioral baseline, prerequisite contracts
  have no unresolved ownership dependency, and Stage 1 can begin as a bounded
  structural extraction.

#### Stage 0 Handoff

- Baseline: `8b750d3c`.
- Stage 1 working set:
  `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.h`,
  `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`, and
  new module-private files under
  `Engine/Source/Editor/LevelEditor/Private/Viewport/` for toolbar and overlay
  presentation.
- Key symbols:
  `FSceneViewportPanel::CalculateToolbarLayout`,
  `FSceneViewportPanel::DrawToolbar`,
  `FSceneViewportPanel::DrawOrientationOverlay`, and
  `FSceneViewportPanel::DrawFPSOverlay`.
- Decisions: retain the panel constructor, public API, viewport clients,
  renderer/world/play ownership, all ImGui IDs and popup names, and input
  exclusion rectangles; use composition rather than publishing a reusable UI
  framework.
- Open questions: none.
- Validation: the baseline `Win64-Debug-DurinEditor-Tests` `all` build
  completed successfully on 2026-07-28.

### Stage 1: Extract Scene Viewport presentation helpers

Dependencies: Stage 0.

- [ ] Move viewport toolbar icon drawing, surfaces, selection indicators,
  ordinary buttons, split buttons, and runtime controls into a private
  `ViewportToolbar` component.
- [ ] Move toolbar layout calculation and its responsive state into the same
  component or a paired immutable layout type.
- [ ] Move orientation and FPS overlay drawing into narrowly named private
  helpers without moving camera, world, renderer, or play-session ownership.
- [ ] Keep the existing panel constructor, viewport clients, frame
  finalization, input routing, camera preview, and public panel API unchanged.
- [ ] Verify all full, compact, narrow, play, pause, snap, transform-mode, and
  view-mode toolbar states against the Stage 0 baseline.

#### Acceptance Gate

- `SceneViewportPanel.cpp` contains panel orchestration rather than private
  toolbar implementation, all toolbar identities and behavior remain stable,
  and focused editor/UI validation passes in both themes and required scales.

### Stage 2: Extract shared asset-destination validation

Dependencies: Stage 0.

- [ ] Introduce one side-effect-free editor asset-destination result built from
  `FAssetPath` and Core's existing typed mount APIs.
- [ ] Carry the parsed asset path, owning mount, mounted/unmounted state,
  resolved physical destination, existing-package/registry state, and
  actionable diagnostic without adding another path taxonomy.
- [ ] Replace repeated validation and destination-browse conversion in the
  three import dialogs with the shared result.
- [ ] Keep Content Browser on `FindMountForVirtualPath`,
  `ResolveContentPath`, and `ClassifyContentPath`; factor only a genuinely
  shared editor decision rather than wrapping Core APIs mechanically.
- [ ] Cover trailing separators, normalized and non-normalized physical roots,
  unknown virtual roots, prefix lookalikes, case behavior, loaded packages,
  registry assets, and mounted-domain containment boundaries.

#### Acceptance Gate

- Core remains the only mount/path implementation, the three import workflows
  share one editor destination decision, and focused tests prove deterministic
  success and rejection without changing mounted-source behavior.

### Stage 3: Extract common import-dialog state

Dependencies: Stage 2.

- [ ] Introduce a callback bundle for clearing/reporting errors and reporting a
  successfully imported asset.
- [ ] Introduce a composed destination model for preferred directory, asset
  path text, suggestion tracking, browsing, and Stage 2 validation.
- [ ] Centralize modal open-request lifecycle and common destination-row/error
  presentation only where labels and ImGui identities remain explicit inputs.
- [ ] Convert StaticMesh, Texture2D, and TextureCube dialogs one at a time,
  retaining their source-specific state, validation, output preview, settings,
  and import execution.
- [ ] Replace repeated import-dialog construction callbacks in
  `MLevelEditor::Construct` with one bounded factory/helper.
- [ ] Keep mounted-source actions and multi-output import orchestration behind
  their existing dialog-specific interfaces rather than incorporating their
  domain logic.

#### Acceptance Gate

- The three dialogs share destination, lifecycle, and callback plumbing; each
  retains distinct source behavior; import success, cancellation, invalid
  path, collision, and error reporting match the baseline.

### Stage 4: Split Content Browser presentation from state

Dependencies: Stage 0.

- [ ] Extract grid metrics, transparency background, type badge, thumbnail,
  icon, label, file-size, and file-time presentation into a private item-view
  component.
- [ ] Extract directory-tree, grid, details-table, selection-details, toolbar,
  status, and context-menu drawing into bounded views that consume explicit
  panel state and return commands.
- [ ] Keep thumbnail request identity, visibility priority, resource lifetime,
  and cache/provider ownership unchanged.
- [ ] Preserve table sort specs, selection anchors, rename focus, responsive
  layout, item hover, popup, drag/drop, and deferred-action ordering.
- [ ] Validate mixed source-file and asset grids at minimum, default, and
  maximum icon sizes, including cold, warm, failed, and invalidated
  thumbnails.

#### Acceptance Gate

- Item and panel presentation no longer occupy the Content Browser orchestration
  unit, all thumbnail and interaction regressions are excluded by focused and
  visual validation, and no thumbnail-service contract moved modules.

### Stage 5: Split Content Browser model and operations

Dependencies: Stages 2 and 4.

- [ ] Introduce a Content Browser model that owns mount snapshots, current
  location, navigation history, directory/item snapshots, filtering, sorting,
  and cache invalidation.
- [ ] Expose immutable item views and explicit navigation/filter/sort commands
  to the panel.
- [ ] Introduce an operations component for folder creation, asset creation,
  rename, move/drop, delete analysis, deletion, and Explorer/clipboard actions.
- [ ] Keep selection and rename UI state in the panel unless tests demonstrate
  a model-level invariant that requires moving it.
- [ ] Preserve preflight-before-mutation ordering, managed companion behavior,
  rollback, focus-after-operation, registry rescan, selection repair, and
  error messages.
- [ ] Add focused tests for navigation history, recursive filtering, stable
  sorting, rename collisions, unmanaged folder content, move failure, delete
  blockers, and refresh after mutation.

#### Acceptance Gate

- `FContentBrowserPanel` coordinates model, views, and commands without owning
  filesystem/registry algorithms; all existing Content Browser workflows pass
  focused and editor smoke validation with unchanged observable behavior.

### Stage 6: Extract Outliner and Details submodels

Dependencies: Stage 0.

- [ ] Extract an Outliner hierarchy model that owns node construction, cycle
  defense, parent/child indices, traversal intervals, depth, revision
  invalidation, and filter visibility.
- [ ] Add deterministic tests for roots, nested ordering, cycle defense,
  descendant queries, filter ancestor retention, revision changes, and deleted
  actors.
- [ ] Replace the recursive lambda and operation lambdas in
  `FWorldOutlinerPanel::Draw` with named row, context-menu, drag/drop,
  visibility, rename, shortcut, and delete methods or components.
- [ ] Extract the Details component-tree view/model only after freezing
  instance-component ordering, attachment, duplicate, rename, removal,
  selection, and active property-edit interactions.
- [ ] Do not move `FReflectedPropertyView` ownership or change its edit
  completion/cancellation contract.

#### Acceptance Gate

- Outliner hierarchy behavior is testable without ImGui, World Outliner and
  Details draw functions are bounded by named responsibilities, and actor/
  component selection, hierarchy, mutation, undo/redo, and property editing
  pass focused integration tests.

### Stage 7: Separate document dialogs and finish composition cleanup

Dependencies: Stages 1 through 6 as applicable.

- [ ] Move unsaved-level and structure-upgrade rendering into document-dialog
  presenters that derive their content from controller/model state and return
  explicit decisions.
- [ ] Keep `FAssetStructureUpgradeModel` as the owner of pending compatibility
  decisions and preserve retry, cancel, unload, activation, and deferred-open
  completion behavior.
- [ ] Reduce `MLevelEditor::Construct` to clear service/panel construction and
  callback wiring helpers without changing ownership or destruction order.
- [ ] Review extracted APIs and keep module-private types private; promote only
  path/import primitives with demonstrated cross-editor consumers.
- [ ] Move lasting ownership and lifecycle contracts into the applicable
  Editor Architecture document and leave this plan as implementation history.
- [ ] Run plan validation, focused native/editor suites, a successful full
  `all` build, and the hidden-window editor smoke test through the repository
  DurinDevTool workflow.

#### Acceptance Gate

- Document workflows preserve every workspace lifecycle outcome, LevelEditor
  construction and destruction order remain explicit, lasting contracts are
  documented in their owning domain, and the final full build and runtime smoke
  validation succeed.

## Validation Matrix

| Area | Representative coverage | Required result |
| --- | --- | --- |
| Path/mount | project/engine/custom mounts, unknown roots, lookalike prefixes, separators, case and containment | One deterministic owner/root result or actionable rejection |
| Import dialogs | open, cancel, browse, invalid path, existing asset, import failure, success notification | Same visible state and callbacks before and after extraction |
| Scene Viewport | full/compact/narrow, transform modes, snap popup, play/pause/step/stop, camera preview | No layout, ID, command, tooltip, or visual regression |
| Content model | rescan, navigation history, recursive mode, search, type filter, stable multi-column sort | Deterministic item and navigation state |
| Content UI | grid/details, resize, icon zoom, selection/range selection, rename, context menus, drag/drop | Existing interactions and responsive behavior remain intact |
| Thumbnails | cold/warm, pending/ready/failed, scrolling priority, invalidation, restart | No ownership, scheduling, cache, or visual regression |
| Content operations | create, rename, move, rollback, delete analysis, blocked delete, refresh/reveal | Same preflight, mutation ordering, recovery, and errors |
| Outliner | nesting, filtering, expansion, range selection, visibility, attach/detach, rename, delete | Stable hierarchy and transaction behavior |
| Details | component add/duplicate/attach/detach/rename/remove and active property edits | No selection, undo/redo, dirty-state, or edit-lifecycle regression |
| Documents | dirty open/close, compatibility save, data-loss acknowledgement, open without saving, retry/cancel | Same deferred-open and package lifetime outcomes |
| UI system | both themes; 75%, 100%, 125%, 150%, and 200% scale | Readable, clickable, non-overlapping controls |
| Final integration | focused tests, applicable editor suites, full `all` build, hidden-window smoke | All required validation succeeds from one Agent Build Profile |

Build, test, and runtime validation use
[Build and Run](../Development/Build/BuildAndRun.md); this plan does not copy
commands or output paths that may change.

## Definition of Done

- Core remains the authoritative mount/path implementation, and editor
  asset-destination validation has one composed implementation with focused
  coverage.
- StaticMesh, Texture2D, and TextureCube import dialogs share composed
  destination/lifecycle/callback plumbing without sharing source-specific
  domain logic.
- `ContentBrowserPanel.cpp` and `SceneViewportPanel.cpp` no longer combine all
  identified responsibilities in monolithic translation units.
- Outliner hierarchy/filter logic is independently testable, and oversized
  Outliner and Details draw paths are decomposed into named responsibilities.
- Document workflow state is separated from modal presentation without
  changing workspace lifecycle outcomes.
- No extracted private type is exposed merely to satisfy a line-count target,
  and `DurinEd` remains independent of concrete LevelEditor code.
- Every stage lands as an independent commit with its plan checklist and
  handoff updated.
- Lasting ownership and lifecycle contracts are published in the owning Editor
  Architecture documentation.
- Plan validation, required focused tests, final full build, and hidden-window
  editor smoke test all pass.

## Deferred Follow-ups

- A generic editor toolbar/painter API, after at least one non-LevelEditor
  consumer demonstrates the same semantic button and split-control contract.
- Shared tree-view infrastructure for Outliner and component hierarchies after
  their selection, drag/drop, and mutation contracts converge in practice.
- Public file-size/time formatting or Explorer/clipboard utilities if another
  editor needs the same platform behavior.
- Transform-gizmo geometry or actor-transform transaction extraction when a
  second manipulation surface requires the same transaction contract.
- Further `MLevelEditor` service-container or dependency-injection changes;
  construction helpers in this plan are intentionally bounded.
- Performance changes beyond preserving current snapshots, invalidation, and
  thumbnail scheduling behavior.

## Related Documentation

- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Editor UI Style](../Editor/Design/UIStyle.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Rendered Asset Thumbnails Plan](Archive/2026-07/RenderedAssetThumbnails.md)
- [Source Library References Plan](SourceLibraryReferences.md)
- [Ready-to-Use Static Model Import Plan](ReadyToUseStaticModelImport.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.h`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/AssetStructureUpgradeModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
