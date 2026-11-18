# Asset Editor Ownership and Document Manager Removal Plan

Summary: Align editor ownership and principal interfaces with UE, give each material asset its own Toolkit, and remove the shared asset document manager after migrating its consumers.

Last reviewed: 2026-11-19

Status: Completed
Completed: 2026-11-19

## Current Status

The owner selected one asset per Material Editor Toolkit, separate material and
function editing sessions under a shared Toolkit implementation, a dedicated
instance Toolkit, and workspace-wide removal of `FEditorDocumentManager`.
The source review uses Durin revision `e57cb3afd` and the local UE 5.8.2
installation at `D:/Programs/UE_5.8`, changelist 56702186. Stage 1 uses the local UE 5.8.4 source tree to confirm the separate material
and instance Toolkit factories and feature-owned graph navigation. Durin
consolidates graph writes in its existing `FMaterialGraphDocument` command
interface. That interface serves the graph draft, while the asset editor
subsystem handles opening and locating the Toolkit.

Stage 1 passed on macOS arm64 with `MacOS-arm64-Debug-DurinEditor` on
base revision `595521bd7` plus the Stage 1 changes. `AssetEditorFamilyTests`
passed all 14 cases. `@material-editing` passed its graph editing,
interaction, and persistence targets. The
`FMaterialAsyncCompileTests.EditingSessionAppliesOnlyAcceptedResults` case
passed with pending Apply cancellation coverage, and the persistence target
covered forced close during function publication. The test runtime needed
its TextureCompressor deployment refreshed to match the current Engine build.
Stage 2 passed on macOS arm64 with `MacOS-arm64-Debug-DurinEditor` on
base revision `612ccae56` plus the Stage 2 changes. `AssetEditorFamilyTests`
(15 cases), `MaterialEditingPersistenceTests` (30), `AssetPackageReloadTests`
(22), and `EditorShellTests` (64) passed. The tests verify failed publication,
replacement lookup during the function notification, and Discard with two
editors open on the same texture. The subsystem adds one participant per
affected Toolkit and one for transaction history. A synchronous publication
callback refreshes editor presentation and indexes after runtime commit and
before external notifications. Commit and retirement retain their existing
runtime authority.

Stage 3 passed on macOS arm64 with `MacOS-arm64-Debug-DurinEditor` on
base revision `54c5b2092` plus the Stage 3 changes. `EditorOperationTests`
(402 cases with `GTEST_DEATH_TEST_STYLE=threadsafe`), `EditorHostToolTests`
(57), `EditorShellTests` (63), and `MaterialEditingPersistenceTests` (30)
passed. Coverage includes request order, failed Save and Discard, retry,
cancellation, stale responses, grouped window continuation, and forced close
releasing the source root. MainFrame exit and export use Toolkit operations.
The application startup and shutdown smoke remains at the final integration gate.

Stage 4 passed on macOS arm64 with `MacOS-arm64-Debug-DurinEditor` on
base revision `956d58a66` plus the Stage 4 changes. `EditorHostToolTests`
(60 cases) and `EditorOperationTests` (402 with
`GTEST_DEATH_TEST_STYLE=threadsafe`) passed. The Level tests cover a persistent
map, an empty editor, an untitled map, Save As and first save, cancellation,
map and view closure, and map recovery. Level Tools no longer use an initial
document or an associated document root.

Stage 5 migrated grouped Texture, Static Mesh, Material Parameter Collection,
and generic Toolkit consumers and removed the shared document records,
deferred opens, and compatibility APIs. The source and test audit covered
Engine, Sandbox, RoadWeaver, and SDK packaging references. The Toolkit owns
grouped assets and exposes them to tab presentation. The subsystem holds the
pending request identity and routes decisions back to the Toolkit.

On macOS arm64 with `MacOS-arm64-Debug-DurinEditor`, the Editor and consumer
builds passed (`DurinEditor`, `MainFrame`, `Sandbox`, `RoadWeaver`, and the
Editor modules built by the tests). `AssetEditorFamilyTests` (15),
`EditorOperationTests` (403 with `GTEST_DEATH_TEST_STYLE=threadsafe`),
`AssetPackageReloadTests` (22), `EditorShellTests` (54), `EditorHostToolTests`
(60), `@material-editing` (all three targets), `StaticMeshTests` (202),
`EditorRenderingTests` (93), and the material, mesh, and texture thumbnail
targets passed. `AssetPackageReloadVulkanTests` passed with GPU access outside
the sandbox and `DURIN_RHI_BACKEND=vulkan`.

The Sandbox Editor smoke used the Vulkan backend and exited normally after
three completed ticks. The launcher receipt is
`Build/.agent-state/logs/20261119-013903-892476-79259-DurinEditor.log`.
The broader `build all` attempt stopped in unchanged `MetalPipeline.cpp`
(commit `5a885d38a` introduced references absent from the local
`metal-cpp` headers). The owning modules and affected workspace targets passed
the focused build gate. Native Metal startup remains unverified.

## Goal

The Toolkit owns its editing objects and feature state. The asset editor
subsystem indexes the relationship between an asset and its open editor. The
window host handles presentation. The editor world owns the current level.
Each of those owners provides the state its callers need directly. The final
code has no shared asset-document records, lookup IDs, or controller between
these owners.

This plan covers the Toolkit, host, editor-side package reload integration, and
affected feature modules. Material shader compilation, asset formats, and the
runtime package replacement protocol are outside its implementation scope.
Concrete feature Toolkits and session classes remain module-private.

## Ownership and Principal Interfaces

UE paths in this table are relative to the reference installation's
`Engine/Source/Editor`.

| Responsibility | UE source and interface | Durin target |
| --- | --- | --- |
| Toolkit lifetime and presentation | `EditorFramework/Private/Toolkits/ToolkitManager.cpp` registers shared Toolkits. `UnrealEd/Private/Toolkits/AssetEditorToolkit.cpp` initializes the editor and attaches its host. | `FToolkitManager` continues to own registered Toolkit lifetime. `DAssetEditor` supplies the reflected route and factory where the existing family uses it. The host presents, focuses, and detaches the Toolkit window. |
| Edited objects | `UnrealEd/Public/Toolkits/AssetEditorToolkit.h` defines `GetEditingObjects`, `AddEditingObject`, `RemoveEditingObject`, and the Toolkit's GC-reported `EditingObjects`. | Make `GetEditingObjects()` the authoritative public query. Keep one strong editing-object owner inside the Toolkit, in its concrete session when it has one. The base class does not copy a session's source into a second owning list. A family that edits several assets owns its collection in the Toolkit. |
| Open editor lookup | `UnrealEd/Private/Subsystems/AssetEditorSubsystem.cpp` implements `OpenEditorForAsset`, `FindEditorForAsset`, `NotifyAssetOpened`, `NotifyAssetClosed`, and `NotifyEditorClosed`. | Keep those responsibilities in `DAssetEditorSubsystem`. The existing indexes are non-owning projections of admitted Toolkits and their source assets. Publish associations only after successful open or committed replacement. Preserve focus and editor identity rules, including open method and instance role. |
| Close policy and presentation | `UnrealEd/Private/Toolkits/AssetEditorToolkit.cpp` invokes `OnRequestClose(Reason)` from `CloseWindow(Reason)`. `MaterialEditor/Private/MaterialEditor.cpp` decides how to handle its dirty draft. MainFrame owns the application save prompt. | The Toolkit decides close admission, settles active interactions, and performs feature-specific save or discard. The subsystem coordinates close requests across Toolkits. MainFrame presents their decisions and owns application-exit policy. A single pending request record bridges the asynchronous UI to the requesting Toolkit. |
| Package reload | `UnrealEd/Private/Subsystems/AssetEditorSubsystem.cpp`, `HandlePackageReloaded`, finds affected editors and coordinates close and reopen across reload phases. | The subsystem discovers affected Toolkits and coordinates editor participation. Each actual owner supplies its replacement participant. Keep Durin's existing atomic in-place rebind and rollback as the selected reload behavior. The runtime package transaction owns commit and retirement. |
| Current level | `LevelEditor/Private/LevelEditorActions.cpp` reads `GetEditorWorldContext().World()`. New Level and map save decisions are handled by the Level Editor and map workflows. | The existing editor world and level activation services remain authoritative. `FLevelDocumentController` coordinates transitions, transaction checkpoints, and viewport state. The Toolkit derives its presentation and asset association from the current level. |
| Graph navigation | `MaterialEditor/Private/MaterialEditor.cpp` registers an internal `FDocumentTracker` for graph tabs. `MaterialEditor/Private/MaterialEditorModule.cpp` creates a separate Toolkit instance for each material, function, or instance being opened. | Material and function share a graph editor implementation and each Toolkit owns one canvas. Any future subgraph navigation belongs to the feature Toolkit. The current graph path requires only node selection and framing. |

Align the existing Durin entry points with these UE roles. Reuse the current
subsystem indexes and Toolkit registration. Keep layout identity derived from
the existing Toolkit and profile keys, including across rename and host changes.
Retain the existing host UI and package transaction behavior where the UE
implementation uses different mechanics.

### Behavior to Preserve

- A base material applies a successfully compiled current draft. Undo and Redo
  edit that working copy. A material function publishes synchronously, and
  successful history replay can republish the function. Function Discard keeps
  applied, unsaved source changes. Keep the source-conflict and reentrant
  publication checks. Instances continue to edit their source overrides directly.
- A failed Save or Discard leaves the same close request pending. Cancel
  withdraws that request and any enclosing exit operation. Forced teardown and
  asset deletion follow their current reason-specific policies. Async work and
  reentrant callbacks cannot close or save a replacement session.
- Ordinary multi-selection opens one editor per asset in the material,
  function, and instance families. Reject explicit grouped requests for these
  families before partially opening a selection. Keep grouped editing for
  other families that support it. Existing workspace state already restores
  by asset path and open method.
- Distinguish a transient current level from the absence of a level. The former
  displays `Untitled` and follows the existing unsaved-level workflow. The
  latter has no level to save. Closing the current level and closing the Level
  Editor window are separate operations.

## Implementation Stages

### Stage 1: Give each material asset a single Toolkit session

- [x] Route base materials and functions to separate instances of the shared
  `FMaterialEditor` implementation. Split out the private
  `FMaterialInstanceEditor` and its reflected factory. Enforce single-asset
  admission both in the family routes and when a Toolkit is opened directly.
- [x] Move base material and function draft ownership into their concrete
  sessions. Reuse `FMaterialEditingSession` and extract the existing
  `FFunctionDocument` as the function session. Each owns its canvas and preview.
  Hold a local session reference across callbacks that can close the editor.
- [x] Delete `OpenMaterials`, `EditingSessions`, `FunctionDocuments`,
  `MaterialPreviews`, `MaterialGraphCanvases`, `ParameterSearchTexts`, and
  `PendingLayoutResets`. Keep search and layout-reset state as single values.
  Remove resource-key lookups, reverse session lookups, and per-document drawing
  loops. Use the Toolkit window identity for preview names. Keep the host's
  temporary compatibility record as the only document ID use.
- [x] Use the asset editor subsystem to open and find the target function,
  then navigate by node GUID through its sole session. Consolidate graph
  write commands in `FMaterialGraphDocument`. Remove the static
  `FMaterialGraphOperations` command facade after migrating consumers and keep
  catalog search, canvas geometry, and gesture sessions private.
- [x] Validate family admission, graph edits and interactions, Apply and Save,
  history, rename, reopen, and session retirement. Include close during a
  function publication callback and cancellation of a pending material Apply.

### Stage 2: Move edited-object ownership and reload to Toolkits

- [x] Introduce the `GetEditingObjects()` Toolkit query and migrate subsystem
  association, saveable-object discovery, and workspace restore callers. Keep
  one owning reference to each source per Toolkit. During migration, let
  legacy Toolkit queries read their current records. When a family moves
  ownership, its compatibility record derives its object from the new owner
  and releases its corresponding `AssetRoots` entry.
- [x] Move editor-side reload participant discovery to the asset editor
  subsystem. Find affected editors from the existing association index and
  obtain participants from the Toolkit owners. Use one editor-side participant
  assembly path from both explicit Package Reload and editor Discard. Reuse the
  existing transactor participant and deduplicate participants per operation.
- [x] Validate the editor identity, source mapping, and relevant draft revision
  at the reload barrier. Replace object references at commit. Refresh derived
  previews and synchronize indexes after publication, before external
  notifications observe the replacement. On failure retain the original source
  reference, association, and session. Move rename handling to the subsystem
  and Toolkit notifications as each family migrates.
- [x] Verify repeat open and focus, restored editor state, rename, replacement
  admission, rollback, and reopen against the new association. Exercise both
  explicit reload and Discard with an affected editor already open.

### Stage 3: Give closing a single request owner

- [x] Route window close, close-all, asset removal, and exit through
  `CloseWindow(Reason)` and Toolkit `OnRequestClose(Reason)`. The Toolkit
  supplies the operation to resolve a dirty edit. Store pending requests once in
  the subsystem, keyed by Toolkit identity and ordered by request arrival.
  MainFrame renders the next request and submits its decision.
- [x] Move exit and export preparation and save traversal to Toolkit-facing
  operations. Keep package save handling at its existing service boundary.
  Resume a pending close from its result, after checking that the Toolkit and
  targeted edit still match. Clear the request on cancellation or retirement.
- [x] Remove the document-level pending close ID, sequence, and cancellation
  revision, and the Toolkit retry state that mirrors them. Retain the current
  guards for read-only editors, busy edits, save failure, and reentrant close.
- [x] Verify one confirmation per request, cancellation, retry after save
  failure, closing the last asset, application exit, export, and forced
  shutdown. Check that closed Toolkits release their asset references once.

### Stage 4: Give the current level a dedicated lifecycle

- [x] Read the current level from the existing editor world context. Keep
  transient level, active persistent level, and no-level presentation derived
  from that context. Keep prompts, transaction activation and viewport
  restoration in the Level Editor path. Update asset association only after a
  successful level transition or Save As.
- [x] Replace the `InitialDocuments` placeholder used to start or restore the
  Level Editor. Implement close-level, close-window, and focus using the
  existing controller and host. Retire `AssociateDocumentAsset` once no
  consumers remain.
- [x] Verify startup with and without a current level, new transient level,
  open and switch, Save As, unsaved cancellation, close-level, and host teardown.

### Stage 5: Migrate remaining consumers and delete the manager

- [x] Migrate Texture Editor, Static Mesh Editor, Material Parameter Collection
  Editor, the generic asset editor, and remaining MainFrame and host consumers.
  Preserve supported grouped asset editing with Toolkit-local presentation
  state and the shared Toolkit lifecycle. Remove the temporary compatibility
  path after the last consumer switches.
- [x] Remove `FEditorDocumentManager`, its stored document records and
  `GetDocumentManager()` entry points, and the document close UI adapter.
  Remove `FDocumentId`, `FEditorDocument`, `FDocumentRequest`, and deferred
  document-open state once their remaining consumers are migrated or retired.
  Audit deferred-open use in production and tests before removing it. Preserve
  asset-load or graph readiness behavior through its owning feature.
- [x] Audit source, tests, and SDK consumers in every project in
  `Durin.dworkspace`. Build the owning Editor modules and affected project
  consumers. Run focused editor and reload coverage and one bounded Editor
  startup and shutdown smoke, since this stage changes host initialization
  and exit across asset families.
- [x] Update the lasting asset editor, material editor, and Level Editor
  contracts. Record acceptance evidence and close the plan when all gates pass.

## Acceptance

- Each material Toolkit edits one source asset and owns one session. The
  instance editor owns no graph session. The material graph has one shared
  write interface.
- The Toolkit is the sole owner of its edited objects. The subsystem indexes
  current associations and owns pending close requests. The UI presents those
  requests without storing another close state machine.
- Editor Discard and explicit package reload share participant assembly, and
  the runtime reload transaction remains the commit authority.
- `Untitled` is derived from the current level. The Level Editor can have
  no current asset and no document-manager placeholder.
- No production or test compatibility path still creates or queries
  `FEditorDocumentManager`.

Use the repository [build](../Agents/BuildAndRun.md) and
[testing](../Agents/Testing.md) workflows. The starting test targets are
`AssetEditorFamilyTests`, `MaterialGraphEditingTests`,
`MaterialEditorInteractionTests`, `MaterialEditingPersistenceTests`,
`EditorOperationTests`, `AssetPackageReloadTests`, `EditorShellTests`, and
`EditorHostToolTests`. Select cases by stage and add coverage for a specific
behavior gap when the existing cases do not exercise it.

The existing [asset editor contract](../Editor/Architecture/Framework/AssetEditors.md)
and [material editor lifecycle](../Editor/Architecture/Materials/MaterialEditorLifecycle.md)
remain authoritative until their implementations migrate. Coordinate shared
public header removals with the [DurinEditor SDK packaging plan](DurinEditorSdkPackaging.md).
