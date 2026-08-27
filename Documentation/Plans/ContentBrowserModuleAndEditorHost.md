# Content Browser Module and Editor Host Plan

Summary: Extract Content Browser from LevelEditor into an independent editor feature module and host its shared surfaces in MainFrame.

Last reviewed: 2026-08-28

Status: Completed
Completed: 2026-08-28

## Current Status

All implementation stages and acceptance gates are complete. Stages 1 and 3
were implemented together following the user's direction that a
project-wide browser should never remain Level-owned as a transitional step.
MainFrame now owns the singleton browser and shared host tools directly. Stage
2 is complete: concrete import dialogs and reimport request construction live in
their asset-family modules, while ContentBrowser submits only provider-neutral
requests.

Automated qualification on 2026-08-26 passed `ContentBrowserWorkflowTests`
(59), `EditorAssetWorkflowTests` (35), `TextureTests` (87),
`StaticMeshTests` (74), `EditorHostToolTests` (5), and `EditorShellTests` (44),
for 304 passing tests. `./DevTool build` completed the full `all` target, and a
hidden-window Sandbox launch reached Ready, loaded the default document, ran
eight ticks, and shut down cleanly with no error, assertion, or leaked worker.
On 2026-08-28, the user confirmed the prescribed interactive validation passed
for the global tools across the registered workspaces, closing the final Stage 4
gate.

## Stage 0 Frozen Baseline

### Public boundary

- `IContentBrowser` is an opaque, non-ImGui facade with reveal asset, reveal
  directory, request focus, mounted-content invalidation, and request-admission
  shutdown operations. Callers never receive a model, panel, cache, or Level
  context.
- `FContentBrowserConstructionServices` supplies asset opening, transaction
  execution, mounted-content revision/read notification, import-operation
  notification, asset relocation, the shared thumbnail task scope, and the
  callback gate. Every callback is implementation-neutral.
- Contributions use `FContentBrowserExtensionDescriptor` with a stable string
  ID, category, numeric order, applicability predicate, and deferred invocation
  callback. The registry returns a move-only scoped handle. Invalid descriptors
  and duplicate live IDs fail without changing the registry.
- Contribution order is `(Order, Id)`. Registration and removal become visible
  only when the next immutable frame snapshot is published; callbacks enqueue
  work that executes after the browser finishes traversing that snapshot.
- Admission states are `Accepting`, `Stopping`, and `Stopped`. Stopping rejects
  new reveal, refresh, import, and contribution work while allowing already
  admitted work to drain.

### Independent settings reset

Per user direction on 2026-08-26, the new owner does not read or migrate the
legacy `LevelEditorSession.yaml` browser map. If
`ContentBrowserSettings.yaml` is absent, Content Browser starts from its
defaults and writes the new file. Level Editor stops reading and writing its
old browser keys; viewport, gizmo, and Details settings remain untouched.

### Responsibility and move map

| Current source/responsibility | Selected destination |
| --- | --- |
| `Panels/ContentBrowserModel*`, item view/filesystem, operations, deletion transaction, refresh coordinator | `ContentBrowser/Private/Model` and `ContentBrowser/Private/Operations` |
| `Assets/ContentBrowserThumbnailCache*` and `SourceImageThumbnail*` | `ContentBrowser/Private/Thumbnail` |
| `Panels/ContentBrowserPanel*` | opaque `ContentBrowser` tool implementation and public facade |
| Generic relocation execution in `EditorAssetMoveCoordinator` and `AssetRelocationTransaction` | `ContentBrowser` operation adapter; workspace document remap observer in `DurinEd` |
| Level viewport/session relocation repair | `LevelEditor` observer |
| Level/Scene/Terrain create/import forms | `LevelEditor` contributions |
| Material/MaterialInstance create forms | `MaterialEditor` contributions |
| Texture2D/TextureCube/VolumeTexture import forms | `TextureEditor` contributions |
| StaticMesh import form | `StaticMeshEditor` contribution |
| `LevelEditorSessionSettings` browser fields | `ContentBrowserSettings`; old values intentionally reset |
| `ConsolePanel*`, `ConsoleRecordModel*` | `MainFrame/Private/Tools/Console` |
| `EditorNotificationOverlay*`, Activity History, status bar, toasts, import aggregation | `MainFrame/Private/Tools/Activity` |
| Level drawer selection/geometry and `Ctrl+Space` | `MainFrame` host tool state |
| Level workspace browser reveal API and compatibility-audit detour | direct host-owned `IContentBrowser` facade |

White-box browser model, item-view, refresh, deletion/relocation, destination,
source-thumbnail, and browser-thumbnail tests move to Content Browser private
source ownership. Drawer/status/Console/Activity shell tests move to MainFrame
private source ownership. Asset-family contribution tests stay with the
contributing feature module.

### Dependency audit

The frozen descriptor graph is `MainFrame -> ContentBrowser -> DurinEd,
AssetCore, AssetForge` plus contribution-only edges from concrete editor
modules to `ContentBrowser`. `ContentBrowser` has no dependency on `MainFrame`
or a concrete editor module. `DurinEd`, `AssetCore`, and `AssetForge` do not
depend on `ContentBrowser`.

## Goal

Make Content Browser a project-wide editor tool that remains available while
any Level, Material, Texture, StaticMesh, or SkeletalMesh document is active.
The implementation must have an independent `ContentBrowser` module, a single
host-owned browser instance, feature-module contributions for asset-family
actions, and no dependency on Level workspace types.

Move the shared bottom status bar, drawer selection, Console, notification
toasts, and Activity History presentation to the editor host so their updates
and visibility do not depend on whether the Level Editor dock tab is submitted.

## Scope

- Add an `Editor/ContentBrowser` shared module and register it in the engine
  project and editor target dependency graph.
- Move Content Browser models, item presentation, refresh coordination,
  filesystem operations, reversible deletion/relocation adapters, and browser
  thumbnail request state out of `LevelEditor`.
- Define scoped Content Browser extension registrations for create, import,
  reimport, details, and context-menu contributions that are genuinely owned by
  an asset-family editor module.
- Give Content Browser independent persistent presentation settings; initialize
  defaults without consulting the retired Level Editor browser keys.
- Make `MainFrame` own the singleton Content Browser instance, global bottom
  status bar, bottom drawer, Console presentation, Activity History window, and
  notification toasts.
- Preserve global asset opening through `FWorkspaceManager`, shared transaction
  history, mounted-content reconciliation, thumbnail provider lifetime, reveal
  requests, drag-and-drop payloads, and Play-In-Editor restrictions.
- Split generic asset relocation execution from Level-specific relocation
  observation so Content Browser never receives a Level context or viewport.
- Move and reclassify native tests with their new source owner, then update the
  implemented editor architecture and module-routing documentation.

## Non-Goals

- Replacing AssetCore catalog, package, redirector, deletion, or relocation
  transactions.
- Replacing AssetForge import execution or changing import/build formats.
- Adding multiple independent Content Browser instances, saved browser tabs, or
  per-workspace browser state.
- Creating a full Undo/Redo stack inspector. Activity History remains the
  notification history plus transaction feedback; it is not renamed to an
  operation-stack browser.
- Redesigning asset thumbnails, changing thumbnail budgets, or moving concrete
  thumbnail providers away from their asset-family modules.
- Redesigning individual asset-editor layouts beyond making the host tools
  consistently available.
- Moving World Outliner, Details, Scene Viewport, rendering diagnostics, or
  Level document controls out of `LevelEditor`.

## Design Decisions and Invariants

### Module and dependency ownership

- `ContentBrowser` is a concrete editor feature module. It owns browser UI and
  workflow coordination; it is not part of `MainFrame`, `DurinEd`, or
  `LevelEditor`.
- `DurinEd` continues to own implementation-neutral editor services and
  contracts: workspaces, transactions, notifications, asset drag payloads,
  asset pickers, and the provider-neutral thumbnail service.
- `MainFrame` owns application chrome and the lifetime of the one browser tool
  instance. It may depend on `ContentBrowser`; `ContentBrowser` must not depend
  on `MainFrame` or any concrete asset editor.
- Concrete editor modules may depend on the narrow public Content Browser
  extension API and return scoped registration handles. Destroying a handle
  removes admission before the contributing module can unload.
- The required dependency direction is:

  `MainFrame -> ContentBrowser -> DurinEd/AssetCore/AssetForge`

  and

  `LevelEditor/MaterialEditor/TextureEditor/StaticMeshEditor -> ContentBrowser`

  for contribution registration only. No reverse edge from `ContentBrowser` to
  a concrete editor is allowed.

### Host presentation

- The status bar is drawn once by `MainFrame` below the host dock space and is
  visible for every active project workspace.
- The bottom drawer is anchored to the host content area, has one selected tool,
  and preserves the current focus-loss and drag-leave dismissal semantics.
- Content Browser and Console are single-instance host tools. Opening a tool in
  the drawer or as a host-level dockable window submits its content body at most
  once per frame.
- "Dock in Layout" targets the host dock space rather than a workspace's
  internal dock space. Resetting an asset editor's layout cannot close or move a
  global host tool; resetting the host layout restores the global tool defaults.
- `Ctrl+Space` is a host command. It toggles Content Browser regardless of the
  active workspace while respecting text-input ownership.
- Content Browser is disabled for mutations during Play-In-Editor exactly as it
  is today, but Console and Activity History remain usable.
- Notification updates, transaction-event publication, Console log polling,
  unread warning/error counts, and toasts execute from host lifetime, not from
  the visibility of a workspace root window.

### Content Browser service and extensions

- The public browser facade exposes reveal asset, reveal directory, request
  focus, mounted-content invalidation, and request-admission shutdown without
  exporting its private models or ImGui implementation.
- Asset opening calls the live `FWorkspaceManager` route table through an
  injected host service; Content Browser never names a concrete workspace.
- Extension descriptors have stable IDs, deterministic ordering, an
  applicability predicate, and an invocation callback protected by the
  contributing module's callback gate. Duplicate IDs and invalid registrations
  fail before mutating the registry.
- Generic folder creation and AssetCore-backed package/file operations remain
  in `ContentBrowser`. Asset-family creation and import forms move to their
  semantic owner:
  - Level and Scene/Terrain contributions: `LevelEditor`.
  - Material and MaterialInstance creation: `MaterialEditor`.
  - Texture2D, TextureCube, and VolumeTexture import: `TextureEditor`.
  - StaticMesh import: `StaticMeshEditor`.
- Reimport actions backed entirely by AssetForge's generic import record may
  remain in `ContentBrowser`. A type-specific form or policy must be supplied by
  its owning extension rather than recognized through concrete runtime-class
  branches in the browser panel.
- Extension callbacks enqueue actions after the current browser snapshot is no
  longer traversed. Registration or removal during drawing becomes visible no
  earlier than the next safe frame boundary.

### State, mutation, and lifetime

- A new Content Browser settings owner persists view mode, icon size and lock,
  tree width, hidden-content visibility, and last directory independently of
  `LevelEditorSession.yaml`.
- On first load without the new settings file, Content Browser defaults are
  saved by the new owner. Level-specific settings retain viewport, gizmo, and
  Details state and never read or write browser presentation state.
- Browser navigation, selection, search, refresh coordination, and thumbnail
  cache survive workspace switches because the singleton instance survives.
- Content Browser prepares and commits AssetCore mutation transactions through
  the global editor transaction manager. It does not call back into a Level
  context to repair state.
- Generic relocation publishes AssetCore mappings. `FWorkspaceManager` observes
  them to remap open document resource IDs, while `LevelEditor` separately
  observes them to move viewport-session keys and capture current-Level state.
- Mounted-content mutation revision and reconciliation acknowledgement remain
  process-wide and are not reset by workspace activation.
- Shutdown order is admission stop, extension unregistration in reverse module
  order, browser async/import drain, thumbnail cache drain, browser destruction,
  provider/service destruction, then concrete module unload.
- Existing deletion recovery directories and journal markers remain compatible;
  source relocation must not orphan or reinterpret pending recovery data.

### Compatibility and naming

- Persisted ImGui IDs for workspace-internal Content Browser windows are not
  reused for the host tool. The host layout version is bumped once when the new
  tool window is introduced, avoiding accidental attachment to the retired
  Level Editor dock node.
- The user-visible names remain **Content Browser**, **Console**, and
  **Activity History**.
- The existing editor asset drag payload contract remains stable so active
  workspace targets accept drags without depending on the new module.

## Current Foundations and Gaps

- `MainFrame` already owns the root window, active-workspace resolution, host
  dock space, shared thumbnail service lifetime, and workspace registration
  order. It is the correct composition root for global tool presentation.
- `DurinEd` already owns `FWorkspaceManager`, the global transaction manager,
  notification manager, asset picker and drag payload, and provider-neutral
  thumbnail service.
- Asset editor workspaces already forward Undo/Redo to the global transaction
  manager, so transaction ownership is not actually Level-specific.
- Content Browser already opens assets through an injected callback backed by
  `FWorkspaceManager`; the concrete panel does not construct asset editors.
- AssetCore already exposes catalog snapshots, reversible deletion/relocation
  transactions, mounted-content mutation signals, and asset-move observation.
- The browser implementation is nevertheless physically private to
  `LevelEditor`, inherits `ILevelEditorPanel`, accepts `FLevelEditorContext` in
  drawing, reads `FLevelEditorSessionSettings`, and routes typed import dialogs
  through `MLevelEditor`.
- The current asset-move coordinator combines generic transaction execution
  with Level viewport-session repair, creating the strongest unwanted
  dependency.
- Console and Activity History consume global data but are updated only after a
  visible Level Editor root window passes its early visibility checks. Other
  editor tabs therefore lack the bottom bar and can delay polling or transaction
  feedback publication.
- Existing native tests white-box private Level Editor sources. Their ownership,
  include roots, and source lists must move with the implementation rather than
  continuing to claim a Level Editor exception.

## Implementation Stages

### Stage 0: Freeze contracts and characterize behavior

- [x] Inventory every Content Browser source, private-header consumer, native
  test source, settings key, ImGui ID, import dialog, thumbnail dependency,
  mutation observer, reveal caller, and shutdown callback.
- [x] Add characterization coverage for drawer toggle/focus dispositions,
  once-per-frame tool submission, hidden Console polling, unread counts,
  transaction-event publication, and browser reveal behavior.
- [x] Specify the public browser facade, host-construction inputs, extension
  descriptor, scoped registration handle, and shutdown/admission states without
  exposing Level Editor types.
- [x] Specify deterministic extension ordering and the safe frame boundary for
  contribution changes.
- [x] Record the user-approved reset behavior for retired browser settings.
- [x] Produce the final source/test move map and verify the proposed `.dmodule`
  graph has no concrete-editor or `MainFrame` dependency below
  `ContentBrowser`.

#### Acceptance Gate

- Public contracts compile in isolation with no Level Editor include.
- Characterization tests demonstrate the current tool behavior and known
  inactive-workspace update gap.
- Every current browser responsibility and caller has one selected destination;
  no source is assigned to both LevelEditor and ContentBrowser.

### Stage 1: Extract the ContentBrowser module with direct host ownership

- [x] Add `Engine/Source/Editor/ContentBrowser`, its API/export header,
  `.dmodule`, CMake target, and `Engine.dproject` mapping.
- [x] Move browser models, item views, filesystem helpers, refresh coordinator,
  thumbnail cache, operation adapters, and deletion transaction sources into
  the new module.
- [x] Replace `ILevelEditorPanel` inheritance and `FLevelEditorContext` draw
  parameters with a browser-owned window wrapper plus a state-preserving content
  body.
- [x] Inject workspace opening, transaction execution, notifications,
  thumbnail service/task scope, and mutation revision through explicit
  construction services.
- [x] Per user direction, skip the temporary Level owner and construct the
  singleton directly in MainFrame.
- [x] Move white-box Content Browser tests and CMake private-source ownership to
  the new module; retain test names where their behavior is unchanged.
- [x] Remove all moved source lists and private include paths from LevelEditor
  after the new target is authoritative.

#### Acceptance Gate

- The `ContentBrowser` and `LevelEditor` targets build with no duplicate source
  ownership.
- Content Browser model, refresh, item-view, deletion, relocation, thumbnail,
  and shell-characterization tests pass from their new owner.
- Repository search finds no Content Browser implementation file remaining
  under `LevelEditor`; the temporary adapter contains composition only.
- Runtime behavior, visible labels, drawer behavior, and persisted settings are
  unchanged from Stage 0.

### Stage 2: Remove Level-specific browser policy

- [x] Implement the scoped extension registry and immutable per-frame extension
  snapshot.
- [x] Move Level/Scene/Terrain, Material, Texture, and StaticMesh creation/import
  dialogs to their selected owning modules and register their contributions
  during each module's existing integration lifecycle.
- [x] Replace concrete asset-class and import-type branches in Content Browser
  with extension applicability and invocation.
- [x] Add the independent Content Browser settings owner with default fallback
  and idempotent restart behavior, without reading Level Editor session state.
- [x] Split relocation execution from Level-specific observation. Route open
  document remapping through workspace-level observation and retain only
  viewport/session repair in LevelEditor.
- [x] Route import progress and completion through global notification and
  mounted-content services rather than an `MLevelEditor` overlay pointer.
- [x] Make reveal requests target the browser facade directly; remove the
  compatibility-audit detour through `FLevelEditorModule`.

#### Acceptance Gate

- `ContentBrowser` has no source or link dependency on LevelEditor, MaterialEditor,
  TextureEditor, StaticMeshEditor, SkeletalMeshEditor, or MainFrame.
- Registering, invoking, unloading, and duplicate-ID rejection are covered for
  every extension category.
- Creation, import, reimport, open, rename, move, duplicate, delete, fix-up,
  reveal, and notification flows preserve their existing transaction and error
  behavior.
- New settings survive restart and do not alter Level viewport/session settings;
  retired browser settings are intentionally ignored.
- Moving an open asset remaps document identity; moving the active Level also
  preserves its viewport-session state through separate observers.

### Stage 3: Move shared tools into the editor host

- [x] Add a MainFrame-owned host tool state containing status-bar selection,
  drawer geometry/state, host tool-window visibility, and global shortcuts.
- [x] Construct and own the singleton Content Browser from MainFrame using the
  live WorkspaceManager, notification/transaction services, and shared
  thumbnail service.
- [x] Move Console presentation and record state out of LevelEditor; poll logs
  and update unread counts once per host frame even when its window and drawer
  are closed.
- [x] Move Activity History, transaction-event publication, import-progress
  aggregation, status notifications, and toasts to host lifetime.
- [x] Reserve status-bar height before submitting the host dock space, anchor
  the drawer to the remaining host content region, and add a host-level tool
  window path for "Dock in Layout".
- [x] Route `Ctrl+Space`, status-bar buttons, Window-menu recovery actions, host
  layout reset, Content Browser reveal/focus, and drawer dismissal through the
  host state.
- [x] Remove Content Browser, Console, notification overlay, drawer state,
  status-bar drawing, and corresponding layout roles from `MLevelEditor`.
- [x] Bump the host layout version and remove the retired Level Editor panel IDs
  from its default/reset layout without clearing unrelated user settings.

#### Acceptance Gate

- Content Browser, Console, status notifications, toasts, and Activity History
  remain visible and live while each registered workspace is active and while
  the Level Editor tab is hidden.
- Each single-instance tool content body is submitted at most once per frame;
  toggling its selected drawer button closes it, and selecting a docked tool
  focuses it.
- Console warnings/errors generated in a non-Level workspace update unread
  state immediately; transaction feedback from that workspace reaches Activity
  History without first activating LevelEditor.
- Content Browser state, search, selection, navigation, and thumbnail requests
  survive workspace switches.
- Asset drag from the drawer can dismiss the drawer and complete on compatible
  Level, Material, Texture, StaticMesh, and SkeletalMesh workspace targets.
- Play-In-Editor keeps browser mutations disabled without disabling Console or
  Activity History.

### Stage 4: Qualify lifecycle and publish the new contract

- [x] Exercise partial registration failure and reverse-order shutdown for
  extensions, imports, browser request admission, task scopes, thumbnail
  providers/caches, workspaces, and concrete modules.
- [x] Verify pending deletion recovery and reachable Undo/Redo entries survive
  the ownership move without retaining unloaded module callbacks.
- [x] Run the repository-prescribed targeted native tests, editor-module builds,
  full editor build, and interactive editor scenarios from the standard agent
  build/test workflows.
- [x] Update the Content Browser and Workspace Framework architecture documents
  from implemented evidence, including module ownership, host presentation,
  settings migration, extension lifecycle, and shutdown order.
- [x] Update Code Modules routing and any direct source links or test ownership
  descriptions that still identify LevelEditor as the browser owner.
- [x] Remove temporary adapters, legacy settings writes, obsolete layout roles,
  unused LevelEditor dependencies, and stale private-source test exceptions.

#### Acceptance Gate

- Clean startup, default-document opening, workspace switching, project
  relaunch, and shutdown pass with no leaked registration, task, thumbnail,
  import, or callback lease.
- Targeted and aggregate native tests pass, all affected editor targets build,
  and interactive validation covers every global tool from every workspace.
- Documentation validation passes after the lasting contracts and module routes
  reflect the implemented ownership.
- No production or test source outside the new module includes a private
  Content Browser header.

## Validation Matrix

| Area | Automated evidence | Interactive evidence |
| --- | --- | --- |
| Module graph | Target generation/build proves exports and link dependencies; dependency audit rejects concrete-editor edges from ContentBrowser | Editor reaches Ready with every workspace registered |
| Browser model | Existing model, item-view, filesystem, refresh, selection, filtering, and snapshot tests under the new owner | Navigate mounts, search, change filters/views, reopen the tool, and switch workspaces |
| Asset workflows | Existing create/import/reimport, duplicate, rename, move, deletion, fix-up, recovery, and transaction tests plus extension-registry tests | Execute each contributed menu action and confirm errors/progress appear globally |
| Settings | Default fallback, invalid-value fallback, idempotent reload, and independent-save tests | Reset once on upgrade, then preserve browser layout across restart without changing viewport state |
| Host drawer | Drawer disposition, geometry, once-per-frame submission, shortcut routing, and host layout tests | Toggle, dismiss, drag out, dock, focus, close, recover, and reset host layout in every workspace |
| Console and activity | Bounded record-model, hidden polling, unread-count, notification-history, and transaction-event tests | Generate logs and Undo/Redo feedback outside LevelEditor and observe immediate status/history updates |
| Workspace integration | Asset-open routing, reveal, document remap, drag payload, active-workspace, and PIE-policy tests | Open assets from the global browser, switch editor tabs, drag compatible assets, and enter/leave PIE |
| Lifetime | Registration rollback, callback-gate, task/import cancellation, cache/provider drain, and shutdown-order tests | Close the editor with active imports and after reversible asset operations; relaunch and verify consistency |

Build and native-test selection/execution follow the repository workflows in
[Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md); this plan does not duplicate their commands.

## Definition of Done

- `ContentBrowser` is a registered shared editor module with no concrete-editor
  or MainFrame dependency.
- MainFrame owns one browser and one set of global bottom tools for the complete
  project-editor host lifetime.
- LevelEditor contains no Content Browser, Console, Activity History, status bar,
  or bottom-drawer implementation and exposes no browser reveal API.
- Asset-family modules contribute their owned create/import policy through
  scoped, unload-safe registrations.
- Browser settings are independent; retired Level browser state is ignored and
  the new state persists without repeated writes.
- Asset operations retain their AssetCore and global transaction semantics;
  document and Level viewport state remain coherent through separate observers.
- Cross-workspace behavior, async lifetime, shutdown, and recovery satisfy every
  stage acceptance gate and validation-matrix row.
- Lasting architecture and module-routing documents describe the implemented
  boundary, all documentation validators pass, and the completed plan records
  exact validation evidence before archival.

## Deferred Follow-ups

- Multiple Content Browser instances or saved browser tabs.
- A searchable, package-filtered Undo/Redo stack inspector distinct from
  Activity History.
- A general plugin-facing editor-tool framework if more application-wide tools
  later need the same host drawer/docking contract.
- Per-project customization of bottom-bar tool ordering or keyboard shortcuts.
- Detaching global tools into separate native windows.

## Related Documentation

- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Code Modules](../Workspace/CodeModules.md)

## Related Code

- [MainFrame host composition](../../Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp)
- [Workspace framework](../../Engine/Source/Editor/DurinEd/Public/Editor/Workspace.h)
- [Workspace manager](../../Engine/Source/Editor/DurinEd/Public/Editor/WorkspaceManager.h)
- [Level Editor composition](../../Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp)
- [Content Browser panel](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.h)
- [Content Browser model](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserModel.h)
- [Content Browser operations](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserOperations.h)
- [Asset move coordinator](../../Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp)
- [Console panel](../../Engine/Source/Editor/MainFrame/Private/Panels/ConsolePanel.h)
- [Notification overlay](../../Engine/Source/Editor/MainFrame/Private/Widgets/EditorNotificationOverlay.h)
- [Level Editor session settings](../../Engine/Source/Editor/LevelEditor/Private/Settings/LevelEditorSessionSettings.h)
- [Editor transaction manager](../../Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h)
