# Editor Responsibility And UE Interface Alignment Plan

Summary: Align editor service updates, Content Browser data interfaces, shared texture previews, package tools, and toolkit identities with UE responsibilities.

Last reviewed: 2026-11-11

Status: Active
Completed:

## Current Status

The selected direction and Stage 0 UE-source interface review are recorded;
implementation has not started. Runtime baseline capture and all implementation
and runtime acceptance gates remain open.
The review baseline is `656742742dc515e159f1caa2a6e67d66c4324a0d`.
Test target names were queried from the configured native registry for
`Win64-Debug-DurinEditor`; no native validation was executed for this plan.

The UE source reference is the locally installed UE 5.8.0 release, changelist
53629095, branch `++UE5+Release-5.8`. Source locations below are relative to
UE's `Engine/` directory and do not require that machine's installation path.
Stage 0 must verify this baseline against the implementation checkout and
record any subsequent source changes before beginning migration.

## Goal

Make the principal public interfaces recognizable to a UE developer, including
their ownership, dispatch, lifetime and result semantics. Use UE names where
they identify the same abstraction; map `U` object types to Durin's `D` prefix
and retain `F`, `I` and `E` vocabulary. Use Durin containers, reflection, GC,
module registration, Mona presentation and existing mutation mechanisms.

Keep `DEditorEngine` as the editor/PIE coordinator. Keep asset toolkits as
application controllers that coordinate documents, commands, Apply and Save.
UE's `UEditorEngine::LoadDefaultEditorModules`, `PlayLevel.cpp` and
`FMaterialEditor::SaveAsset_Execute` support these responsibilities. This plan
does not move all editor bootstrap into Launch, split PIE into a new subsystem,
or replace working controllers solely because their files are large.

## UE Reference And Interface Decisions

The following source observations, rather than inferred ideal layering, own
the reference comparison:

| Responsibility | UE 5.8 source reference | Selected Durin interface and adaptation |
| --- | --- | --- |
| Asset editor service updates | `Source/Editor/UnrealEd/Private/Subsystems/AssetEditorSubsystem.cpp`: `Initialize`, `HandleTicker`, `Deinitialize` | Retain `DAssetEditorSubsystem`; move frame maintenance behind a private `HandleTicker(float)` callback driven by its editor host, independent of MainFrame. |
| Browser data service | `Source/Editor/ContentBrowserData/Public/ContentBrowserDataSubsystem.h`: `UContentBrowserDataSubsystem` | `DContentBrowserDataSubsystem : DEditorSubsystem, IContentBrowserItemDataSink` owns source discovery, activation, path/filter queries and update distribution per editor. |
| Browser providers | `Source/Editor/ContentBrowserData/Public/ContentBrowserDataSource.h`: `UContentBrowserDataSource` | Abstract `DContentBrowserDataSource : DObject, IModularFeature`, with concrete `DContentBrowserAssetDataSource` and `DContentBrowserFileDataSource`. |
| Item ownership | `Source/Editor/ContentBrowserData/Public/ContentBrowserItemData.h` and `ContentBrowserItemDataPayload.h` | `FContentBrowserItemData`, `IContentBrowserItemDataPayload` and an aggregate `FContentBrowserItem`; operations route through the owning source. |
| Shared texture preview | `Source/Editor/UnrealEd/Public/Texture2DPreview.h`, used by TextureEditor viewport code | Shared preview interpretation/rendering belongs in DurinEd `Preview/`, outside the concrete TextureEditor feature. Preserve Mona/RHI ownership rather than copying UE's canvas rendering implementation. |
| Package editing policy | `Source/Editor/UnrealEd/Public/PackageTools.h`: `UPackageTools`, `FUnloadPackageParams`; `Source/Runtime/CoreUObject/Public/UObject/PackageReload.h` | Editor-side `DPackageTools` exposes UE-shaped static unload overloads and options. Engine exposes only the narrow collection mechanism required by that policy. |
| Toolkit identity | `Source/Editor/MaterialEditor/Private/MaterialEditor.h`, `TextureEditor/Private/TextureEditorToolkit.h`, `StaticMeshEditor/Private/StaticMeshEditor.h` | Use `FMaterialEditor`, `FTextureEditorToolkit` and `FStaticMeshEditor` for toolkit controllers; keep actual Mona widgets under `Widgets/`. |

Public API cross-checks:
[asset editor subsystem](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UAssetEditorSubsystem),
[browser data source](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ContentBrowserData/UContentBrowserDataSource),
[package unloading](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UPackageTools/UnloadPackages).

### Browser Interface Surface

Align names and argument roles with UE, adapting `TArrayView`/`TFunctionRef` to
the existing Durin span/callback conventions:

- Data service: `ActivateDataSource`, `DeactivateDataSource`,
  `EnumerateItemsUnderPath`, `EnumerateItemsAtPath`,
  `EnumerateItemsMatchingFilter`, and `Tick`.
- Source: `Initialize`, `Shutdown`, `RegisterDataSource`,
  `UnregisterDataSource`, `SetDataSink`, `CompileFilter`,
  `EnumerateItemsAtPath` and `EnumerateItemsMatchingFilter`.
- Operations: `CanSaveItem`/`BulkSaveItems`, `CanRenameItem`/`RenameItem`,
  `CanCopyItem`/`BulkCopyItems`, `CanMoveItem`/`BulkMoveItems`, and
  `CanDeleteItem`/`BulkDeleteItems`. Retain distinct creation and duplicate
  operations; do not reinterpret copying as package duplication.
- Item data: `GetOwnerDataSource` plus provider-owned payload; selection-facing
  `FContentBrowserItem` dispatches to its constituent item data rather than
  inferring execution policy from a central item-kind switch.
- Use logical/virtual destination paths at the public operation boundary.
  Providers resolve physical paths and package identities. Define
  `EContentBrowserItemSaveFlags` with an explicit mapping from current save modes.

Source registration uses the existing modular-feature mechanism and scoped
module registrations, not a second custom provider registry. The subsystem
retains active reflected source instances and enumerates their GC references;
item handles identify both a source instance and its registration generation.
Expired handles cannot silently dispatch to a replacement source with the same
name. Provider payloads remain opaque outside their source.

Merge logical folders for presentation while preserving all contributing source
items. Batch operations partition by source and preserve per-item outcomes;
cross-source operations require an explicitly supported transfer contract and
otherwise reject before mutation. Never convert a partial batch failure into
unqualified success. Existing structured recovery results remain available even
where UE's outer operation returns `bool`.

Deletion is an intentional signature adaptation: retain the current prepared,
confirmed deletion operation and require its validated request context at the
provider `BulkDeleteItems` boundary. Do not add an unconfirmed destructive
shortcut to imitate UE's signature. Freeze the exact context type and mapping
to existing deletion plans in Stage 0, retaining stale-confirmation checks,
companion-file policy, cancellation and recovery.

## Scope And Existing Plan Boundaries

- [UE Asset Editor Architecture](UEAssetEditorArchitecture.md) continues to own
  definition dispatch, reflected sessions, toolkit/host authority, presentation
  consolidation and its remaining retirement/interaction qualification. This
  plan owns only the service-update migration and toolkit identity changes
  described here. Shared acceptance evidence may be reused, but neither plan
  closes the other's unchecked gates.
- [Runtime Startup And Scene Activation](RuntimeStartupAndSceneActivation.md)
  retains project/scene readiness and the one-frame-loop contract. Service
  updates integrate with its current safe points; no second startup loop or
  readiness owner is introduced.
- [Restart-Based Plugin System](RestartBasedPluginSystem.md) owns plugin
  enablement and loading. This plan adds no Python/MCP enablement policy or
  hot-reload guarantee.
- [DurinEditor SDK Packaging](DurinEditorSdkPackaging.md) owns SDK distribution.
  Export/include changes must be communicated there without claiming SDK
  packaging acceptance here.
- The current [Content Browser contract](../Editor/Architecture/Assets/ContentBrowser.md#operations)
  deliberately specifies a private concrete source with no provider registry.
  Stages 2-3 explicitly replace that selected design with UE-style modular
  sources; this is a new decision, not a claim about existing behavior.
- Start within existing modules: shared browser contracts and service in
  ContentBrowser; concrete providers in separate responsibility folders in that
  module; shared preview in DurinEd; package policy in AssetTools; collection
  mechanism in Engine. Additional UE-equivalent DLLs are not required for API
  alignment and are outside this plan.

Search and migrate shared symbols across production and test roots of every
project declared in [Durin.dworkspace](../../Durin.dworkspace): Engine, Sandbox
and RoadWeaver. Include RoadWeaverEditor registrations and project-defined
editors. Complete each interface migration without permanent aliases, forwarding
headers or a second legacy dispatch path. Temporary migration helpers must be
removed before the owning stage closes.

## Implementation Stages

### Stage 0: Freeze interface parity and baseline evidence

Dependencies: none. Outcome: an implementation-ready API inventory, adaptation
list and bounded test baseline, without treating design review as test evidence.

- [x] Recheck UE source signatures and record header/method references for the
  interfaces above. Explicitly distinguish UE behavior from Durin adaptations.
- [ ] Inventory changed symbols and consumers in all three workspace projects,
  including tests, reflection inputs, module manifests and exported headers.
- [ ] Freeze the source initialization/activation and registration-generation
  contract, folder aggregation, async discovery ownership, save-flag mapping,
  cross-source rejection rules and confirmed-delete context.
- [ ] Freeze the host service-update ordering and shutdown/reentrancy behavior.
  UE's asset subsystem ticker currently handles deferred restore; Durin's
  document-close maintenance is a deliberate extension and must run each frame,
  not inherit UE's one-second ticker interval.
- [ ] Query current registry cases and capture focused baseline receipts for
  sessions, browser operations, preview consumers and package unloading. Record
  existing failures/skips separately with a reproducing baseline.

Acceptance: every principal interface has an owner, UE counterpart or documented
adaptation, consumer inventory, and behavior-based validation selection. Keep
unresolved signature decisions in this stage until resolved; do not begin the
affected migration with an implicit competing contract.

### Stage 1: Advance asset editor sessions independently of MainFrame

Dependencies: Stage 0. Outcome: service-only and windowed hosts advance the same
asset editor semantics through their editor frame.

Scope: DurinEd `Subsystems/AssetEditorSubsystem`, `AssetEditorSessions`,
`Editor/EditorEngine`; MainFrame frame/close pumping; session/host test fixtures.

- [ ] Move the public `TickAssetEditors` maintenance implementation behind the
  private `HandleTicker(float)` entrypoint. Let DEditorEngine invoke it through
  narrow host access at the existing owner-controlled update boundary.
- [ ] Invoke it once per admitted editor frame, after tool/script/world updates
  and before shell presentation. Preserve subsystem callback/transaction scopes
  and stop subsequent work immediately on shutdown or retirement requests.
- [ ] Remove semantic maintenance from normal MainFrame ticking. Route any
  special close pump through the same host boundary, never two independent pumps.
- [ ] Migrate tests to exercise the editor frame rather than manually compensating
  with `TickAssetEditors`. Remove the obsolete public entrypoint and consumers.

Validation: focused session cases in `EditorOperationTests`, plus affected host
cases in `EditorHostToolTests`. Verify pending Save/Cancel, grouped close,
deferred restore, no-shell operation, no double advancement, shutdown during a
callback and successive editor lifetimes. Build owning modules and consumers
selected by changed headers. No new global ticker framework is needed.

Acceptance: an editor frame without a MainFrame completes deferred semantic work;
adding a shell does not change its outcome or advance it twice. No public caller
or test requires `TickAssetEditors`.

### Stage 2: Introduce UE-shaped browser data contracts and providers

Dependencies: Stages 0-1. Outcome: source ownership and item dispatch exist as
real contracts, with built-in providers using the existing operation substrate.

Scope: ContentBrowser public data headers; data subsystem/source registration;
asset/file provider implementations; private discovery and operation code;
reflection/module manifests and provider fixtures.

- [ ] Add the data subsystem, abstract source, item data, payload, filter and
  save-flag contracts from the interface map. Keep class names aligned with UE.
- [ ] Register asset/file source types through the modular-feature mechanism;
  create/retain per-editor instances, separate available and active sources,
  and shut down sources before releasing their service/payload dependencies.
- [ ] Move asset capabilities into the asset provider and filesystem capabilities
  into the file provider. Call AssetTools and existing filesystem mechanisms;
  preserve staging, rollback, compensation and mutation notifications.
- [ ] Implement logical path routing and folder aggregation. Partition batch work
  by source and reject unsupported transfers before any physical writes.
- [ ] Keep worker captures immutable and owned. Resolve live reflected sources
  only at the owner-thread boundary; reject stale generation/project results.
- [ ] Preserve the existing confirmed deletion flow through the explicit context
  adaptation. A provider capability query does not authorize later I/O.

Validation: provider-focused cases in `ContentBrowserWorkflowTests`: ownership
dispatch, same-name source replacement, deactivate/reactivate, opaque payload
release, merged folders, mixed-source selection, cross-source rejection,
capability/execution revalidation, save-mode parity and stale confirmed deletion.
Use a small test provider to demonstrate extension without editing built-in
item-kind switches; do not create a new production backend for the test.

Acceptance: built-in operations use their provider contracts. Provider withdrawal
cannot leave callable stale payloads or publish retired async results. No parallel
legacy source continues to execute migrated operations.

### Stage 3: Migrate browser presentation and external consumers

Dependencies: Stage 2. Outcome: every browser uses the data service; panel state
and user confirmation remain local to each browser.

Scope: ContentBrowser Panels/Model/Query/refresh coordinator/operations/public
host contracts; MainFrame; AssetForgeBuiltins; LevelEditor, MaterialEditor,
TextureEditor, StaticMeshEditor; RoadWeaverEditor contributions and all tests.

- [ ] Replace per-panel concrete source ownership with a weak/service-bound data
  subsystem reference. Keep selection, search, navigation, cancellation cursors
  and presentation settings per panel; do not put them in shared providers.
- [ ] Migrate queries, menu capabilities, shortcuts, clipboard and drag/drop to
  `FContentBrowserItem`/`FContentBrowserItemData` and virtual destination paths.
- [ ] Preserve independent create/import/details extensions and metadata-driven
  asset definitions. Do not merge browser provider registration with definitions.
- [ ] Delete obsolete concrete-source APIs and central item-kind execution
  branches; retain item kind only where it describes presentation metadata.
- [ ] Update the implemented Content Browser contract and affected consumer docs
  after the production migration is complete.

Validation: `ContentBrowserWorkflowTests` and affected cases in
`EditorAssetWorkflowTests`, `EditorHostToolTests`, `AssetEditorFamilyTests` and
thumbnail targets that consume browser items. Verify two independent browsers,
refresh after committed operations, ordinary files, assets, redirectors, folder
operations, import/reimport, async teardown and project retirement. Compile
RoadWeaverEditor and affected Sandbox/Engine consumers.

Acceptance: one source-routing path serves every browser and contributed action;
a test provider works through ordinary panel commands. Existing file/asset
workflows and destructive-operation guarantees remain covered. Search all
workspace source/test roots for removed APIs before closing the stage.

### Stage 4: Place shared texture preview mechanisms in DurinEd

Dependencies: Stage 0; execute after Stage 3 in the default sequence. Outcome:
MaterialEditor no longer links TextureEditor solely for a shared preview helper.

Scope: TextureEditor `TexturePreview.h`, its implementation and shared resource
shutdown; DurinEd `Public/Preview` and `Private/Preview`; MaterialEditor graph
previews; texture thumbnails; module dependencies and shader/resource discovery.

- [ ] Move `FTexturePreview`, options/channel/interpretation types and
  `RenderTexturePreview` into the shared Durin namespace and Preview folders,
  exporting them from DurinEd. Use `Preview/TexturePreview.h` consistently.
- [ ] Keep texture editor toolbars, source/built selection, asset-specific
  settings and volume inspection UI in TextureEditor. Do not move GPU algorithms
  or change their pixel interpretation as part of the ownership migration.
- [ ] Transfer shared render-resource teardown to the owning shared lifecycle;
  closing/unloading TextureEditor must not invalidate MaterialEditor previews.
- [ ] Migrate all consumers and remove the MaterialEditor-to-TextureEditor
  dependency once no other real use remains. Do not introduce a replacement
  feature-to-feature dependency or compatibility namespace alias.

Validation: affected cases in `TextureThumbnailTests`, `MaterialThumbnailTests`
and `MaterialEditorInteractionTests`; compile every preview consumer. Run a
bounded existing case from `ThumbnailVulkanTests` to verify real texture display
output/resource lifetime across the export/resource-owner change. Stage 0 must
identify the case covering channel/normal interpretation and backend teardown;
extend meaningful coverage if missing. Full GPU qualification is not required
for a mechanical move with unchanged rendering behavior.

Acceptance: MaterialEditor builds without TextureEditor as a dependency, both
features retain correct preview output, and shared resources are released once
after their consumers retire and before the render thread stops.

### Stage 5: Expose UE-shaped package tools through an explicit engine bridge

Dependencies: Stage 0; execute after Stage 4 in the default sequence. Outcome:
editor policy consumes a declared mechanism rather than Engine's private tree.

Scope: AssetTools `PackageUnloading`, its public API and callers; Engine
`Private/Asset/Tools/PackageCollection.h` and implementation; reflected header
registration where required; package and asset-workflow fixtures.

- [ ] Introduce the editor-side abstract reflected `DPackageTools` facade,
  matching UE's `UPackageTools` role, with static `UnloadPackages` overloads for
  packages, packages/error/dirty policy, and `FUnloadPackageParams` options.
  Map containers and errors to Durin conventions; add reflection only for
  supported scripting signatures, not arbitrary span-based functions.
- [ ] Preserve the AssetTools policy/Engine mechanism split. Define a narrow
  exported package-collection bridge under Engine Public/Asset, with explicit
  admission, candidate identity and collection-result semantics. Do not expose
  unrelated Engine internals or transfer GC ownership to AssetTools.
- [ ] Align the outer bool with UE: true means the loaded-package set changed,
  not merely that candidates were submitted. Preserve detailed errors for dirty
  or still-referenced packages and use the mechanism's actual outcome.
- [ ] Migrate `PackageTools::UnloadPackages` consumers and remove the superseded
  namespace API. Delete the private Engine include-directory injection and
  direct `AssetPrivate` usage from AssetTools.

Validation: `AssetPackageTests`, `AssetRuntimeLifecycleTests` and affected
`EditorAssetWorkflowTests` cases. Cover clean/dirty packages, still-referenced
exports, no-op requests, partial success, collection admission during callbacks,
object retention and accurate bool/error results. Compile Engine, AssetTools
and each affected workspace consumer; inspect the Game dependency closure for
accidental editor imports without adding an unrelated Game runtime gate.

Acceptance: no cross-module private include remains in this bridge; policy
results reflect actual collection outcomes and no package schema/format changes
are needed.

### Stage 6: Align toolkit identities and close integrated validation

Dependencies: Stages 1-5. Outcome: type names/folders express the controller/view
distinction, with final evidence covering the complete interface migration.

Scope: toolkit controllers, their definitions/factories, preferences, public
LevelEditor includes, module manifests, Engine/Sandbox/RoadWeaver consumers and
owning architecture/module documentation.

- [ ] Rename `MMaterialEditor` to `FMaterialEditor`, `MMaterialFunctionEditor`
  to `FMaterialFunctionEditor`, `MTextureEditor` to `FTextureEditorToolkit`,
  `MVolumeTextureEditor` to `FVolumeTextureEditorToolkit`, and
  `MStaticMeshInspector` to `FStaticMeshEditor` while preserving read-only policy.
- [ ] Rename `MLevelEditor` to `FLevelEditorToolkit`. This is an explicit Durin
  adaptation: UE's `SLevelEditor` is a Slate surface/host, while this Durin type
  is a `FBaseAssetToolkit` controller, not a Mona widget.
- [ ] Move these controllers from `Widgets/` to matching `Toolkits/` source
  folders and migrate public include paths where they already exist. Keep
  feature namespaces and current public/private visibility; do not export
  private controllers just to imitate a name.
- [ ] Retain toolkit command/Apply/Save coordination and existing editing
  sessions; separate actual widgets only when their responsibility requires it.
  Keep serialized toolkit IDs, layout profile IDs, session snapshots and user
  settings stable through C++ renames; add no persistence migration or aliases.
- [ ] Update implemented contracts in the owning Framework, Assets and Materials
  documents and the module ownership map. Record UE parity and every deliberate
  adaptation, and reconcile shared evidence with the existing asset-editor plan.
- [ ] Complete final integrated build, behavior checks and documentation
  validation, recording remaining failures/skips without converting them to passes.

Validation: affected cases from `EditorOperationTests`, `EditorHostToolTests`,
`EditorShellTests`, `AssetEditorFamilyTests`, `MaterialEditorInteractionTests`
and `ContentBrowserWorkflowTests`. Verify open/focus/close/restore, host transfer,
material Apply/Save, read-only mesh inspection, stable layouts/settings and
provider retirement. A bounded normal-editor startup/shutdown smoke covers
service ordering, source activation and shared preview teardown; it does not
replace the other plan's still-unqualified interactive gates.

Final build gate: one incremental default-profile Editor `all` build. The risk
is the combined exported-header, reflection-input and dependency-graph migration
across Engine, Sandbox and RoadWeaver; focused owner builds alone do not cover
that combined closure. Do not repeat this broad gate after each rename.

Acceptance: all five review items have the selected responsibilities and public
interfaces; removed symbols/headers have no workspace consumers; relevant final
inputs have passing evidence or explicitly unresolved gates. The plan is not
complete while a required gate remains open.

## Validation And Handoff Policy

Follow the [build workflow](../Agents/BuildAndRun.md) and
[test selection/result reuse workflow](../Agents/Testing.md); query the registry
again before execution instead of assuming target membership remains unchanged.
Run affected-selection explanation for behavior changes and justify any narrower
case selection. Test commands build their selected dependencies; avoid redundant
owner builds when the same closure is already covered.

Keep raw receipts, logs and inventories under ignored
`Build/EditorResponsibilityAndUEInterfaceAlignment/`. For each completed stage,
record the tested revision, relevant input changes, profile/environment, case
selection, pass/failure/skip counts and receipt paths here. Reuse evidence only
when its relevant inputs and environment remain applicable. Known baseline
failures require reproduction and a disposition; they are not acceptance passes.

Document-only authoring needs changed-document validation, all-plan validation
and diff review; it does not require native builds or runtime tests. During
implementation, update this plan and owning lasting contracts in the same commit.
Use exact `Plan` provenance `Documentation/Plans/EditorResponsibilityAndUEInterfaceAlignment.md`
and the corresponding `Stage N: <stage title>` trailer; do not borrow stage
completion or provenance from the related asset-editor plan.
