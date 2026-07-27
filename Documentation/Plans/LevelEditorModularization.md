# Level Editor Modularization Plan

Summary: Extract reusable editor path and import primitives and split oversized LevelEditor panels along stable model, operation, and presentation boundaries without changing user-visible behavior.

Last reviewed: 2026-07-27

## Current Status

Planning is complete and no implementation stage has started.

A static ownership and size review identified two concrete duplicate domains:
mount-owner/path resolution shared by Content Browser and texture import, and
destination/error/lifecycle behavior repeated by the three asset import
dialogs. It also identified two immediate file-size and responsibility
hotspots: `ContentBrowserPanel.cpp` at approximately 2,077 physical lines and
`SceneViewportPanel.cpp` at approximately 1,141 physical lines.

This plan selects incremental extraction behind the existing LevelEditor
interfaces. It does not authorize simultaneous edits to files currently owned
by active thumbnail, source-library, or ready-to-use static-model import
stages. Each stage must first confirm that its recorded overlapping plan has
completed or explicitly handed off the shared working set.

## Goal

- Give mount lookup, mount-owner root resolution, and virtual/physical path
  conversion one authoritative implementation.
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

- `LevelEditor` mount/path helpers currently duplicated by Content Browser and
  import workflows.
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
- Changing source-library, provenance, ingest, reimport, multi-asset
  transaction, or generated-asset contracts owned by other active plans.
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

### Active-plan coordination

- The completed `RenderedAssetThumbnails` plan handed provider-neutral request,
  result, scheduling, persistent cache, and preview-scene ownership to
  `DurinEd`; this plan may move LevelEditor presentation and facade code only
  while preserving the editor
  [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md) contract.
- `SourceLibraryReferences` owns source-library registry, portable source
  location, reference-versus-ingest classification, and import-dialog source
  library presentation. Shared import destination extraction must consume
  those contracts after the relevant Stages 1 through 4, not preserve the
  temporary asset-path-derived source model as a new abstraction.
- `ReadyToUseStaticModelImport` owns multi-asset import/reimport orchestration,
  generated output preview, import progress, cancellation, and generated asset
  policies. This plan owns only generic dialog plumbing and structural
  boundaries; it must not create a competing import transaction.
- If an overlapping plan begins work on a recorded file first, that plan owns
  the file through its stage handoff. This plan then revalidates symbols and
  adjusts the extraction stage from the new baseline.

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

- Mount-owner root resolution has duplicate implementations in
  `ContentBrowserPanel.cpp` and `TextureImportDialog.cpp`; owning-mount lookup
  and virtual/physical conversion are also implemented ad hoc.
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

### Stage 0: Freeze behavior, boundaries, and overlap handoffs

Dependencies: none.

- [ ] Record the baseline commit and exact working set for each later stage.
- [ ] Confirm the current status and file ownership of
  `RenderedAssetThumbnails`, `SourceLibraryReferences`, and
  `ReadyToUseStaticModelImport`.
- [ ] Inventory current ImGui IDs, popup names, drag/drop payloads, persisted
  settings keys, transaction descriptions, and LevelEditor public symbols
  affected by the planned moves.
- [ ] Add or identify focused coverage for mount resolution, import destination
  validation, Content Browser filtering/sorting/navigation, Outliner hierarchy
  filtering, document upgrade decisions, and current panel smoke behavior.
- [ ] Freeze the selected component APIs and dependency locations. Any
  remaining ownership ambiguity is resolved here before moving code.
- [ ] Capture representative screenshots or image baselines for Scene Viewport
  toolbar modes and Content Browser grid/details layouts at the required UI
  scales and themes.

#### Acceptance Gate

- Every later stage has a baseline, bounded working set, non-overlapping active
  owner, frozen observable identities, and executable or recorded baseline
  behavior sufficient to distinguish structural movement from regression.

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

### Stage 2: Centralize mount and asset destination resolution

Dependencies: Stage 0 and the applicable Source Library registry handoff.

- [ ] Add one authoritative owning-mount lookup using the selected
  case/containment semantics.
- [ ] Add one authoritative mount-owner root resolver that handles a physical
  mount rooted at `Content` and a directly owned root.
- [ ] Replace duplicate Content Browser and import-dialog implementations with
  the shared helpers.
- [ ] Define a side-effect-free asset destination validation result containing
  parsed path, owning mount, mounted/unmounted state, existing-asset state, and
  actionable diagnostic.
- [ ] Cover trailing separators, normalized and non-normalized physical roots,
  unknown virtual roots, prefix lookalikes, case behavior, loaded packages,
  registry assets, and source-library containment boundaries.

#### Acceptance Gate

- Content Browser and all import workflows use one mount/path implementation;
  focused tests prove identical successful resolution and deterministic
  rejection across callers without changing source-library ownership.

### Stage 3: Extract common import-dialog state

Dependencies: Stage 2; Source Library Stages 3 and 4 contracts; no concurrent
ownership of the three import-dialog files by `ReadyToUseStaticModelImport`.

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
- [ ] Confirm that the shared model can accept the source-library and
  multi-output extensions owned by the overlapping plans without incorporating
  their domain logic.

#### Acceptance Gate

- The three dialogs share destination, lifecycle, and callback plumbing; each
  retains distinct source behavior; import success, cancellation, invalid
  path, collision, and error reporting match the baseline.

### Stage 4: Split Content Browser presentation from state

Dependencies: Stage 0; completion or explicit handoff of
`RenderedAssetThumbnails` Stage 5.

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

Dependencies: Stages 2 and 4; applicable Source Library and ready-to-use import
file handoffs.

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
  BuildTool workflow.

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

- Mount and asset-destination resolution have one authoritative implementation
  with focused coverage.
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
