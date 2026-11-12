# Editor Responsibility And UE Interface Alignment Plan

Summary: Align editor service updates, Content Browser data interfaces, shared texture previews, package tools, and toolkit identities with UE responsibilities.

Last reviewed: 2026-11-12

Status: Active
Completed:

## Current Status

Execution started on 2026-11-12 from clean revision `3563d2bbd`.
The intervening startup consolidation already moved session maintenance into
DEditorEngine, made it private, removed MainFrame pumping and added headless
close/shutdown coverage. Stage 1 must reuse that behavior, align the callback
name and update ordering, and validate it rather than recreate it.
Stage 0 is complete: consumer inventory, frozen decisions and focused baseline
receipts are recorded below. Stage 1 is implemented and its focused regression
validation passed (39 session cases and 39 host cases).
Stage 2 is complete: provider/subsystem/payload/operation routing, incremental
updates and all save-flag combinations are implemented and validated.
The first checkpoint's shared item interface remains the presentation boundary;
Stage 3 is complete from `6dcb2ff4b`: every panel uses the data subsystem for
queries and owner-routed commands, retaining per-view capture and confirmation.
Provider source interface version 2 and all Stage 3 consumer gates are validated.
Stages 4-6 have not started.
The review baseline is `656742742dc515e159f1caa2a6e67d66c4324a0d`.
Test target names were queried from the configured native registry for
`Win64-Debug-DurinEditor`; execution evidence is recorded below.

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
| Item ownership | `Source/Editor/ContentBrowserData/Public/ContentBrowserItemData.h` (including its payload, update and sink interfaces) | `FContentBrowserItemData`, `IContentBrowserItemDataPayload`, `FContentBrowserItemDataUpdate`, `IContentBrowserItemDataSink` and an aggregate `FContentBrowserItem`; operations route through the owning source. |
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

### Stage 0 Evidence (2026-11-12)

Baseline revision `3563d2bbd`, Windows `windows-msvc-x64`,
`Win64-Debug-DurinEditor`, with only this plan edited. Native build/test ran
outside the sandbox after the first sandboxed build reproduced MSVC C1902.
The outside-sandbox build passed; no compiler/PDB configuration was changed.
Receipts below are under `Build/.agent-state/logs/`:

| Selection | Result | Receipt |
| --- | --- | --- |
| EditorOperationTests / FAssetEditorSessionTests.* | 38 passed | `20261112-102557-044445-31700-EditorOperationTests.log` |
| ContentBrowserWorkflowTests | 158 passed, 2 skipped | `20261112-102648-395744-27368-ContentBrowserWorkflowTests.log` |
| TextureThumbnailTests | 14 passed | `20261112-102732-539338-7868-TextureThumbnailTests.log` |
| MaterialThumbnailTests | 8 passed | `20261112-102800-949556-23972-MaterialThumbnailTests.log` |
| AssetPackageTests / FAsyncAssetLoadTests.Editor*Unload* | 2 passed | `20261112-102827-608613-2692-AssetPackageTests.log` |

The two browser skips require Windows directory/file symlink privileges and
remain unqualified; they are not failures or passes. All 220 executed baseline
cases passed. The baseline does not qualify later source changes.

Stage 1 affected-selection analysis expands to 20 targets through the shared
DurinEd module label. Focused session and host targets are selected because the
changed private method has no external source consumers and only service/shell
ordering changes; preview/browser/package baselines are not claimed as refreshed
behavior validation. The final combined all-build gate remains in Stage 6.

### Frozen Stage 0 Contracts

The workspace inventory is stored at
`Build/EditorResponsibilityAndUEInterfaceAlignment/stage0-consumers.txt`:
861 matching source/test lines in Engine; no current matching consumers in
Sandbox or RoadWeaver. The search covers all six production/test roots and
module manifests/CMake inputs. Engine owners are DurinEd/MainFrame (session
pump), ContentBrowser (data model/discovery/operations), TextureEditor and
MaterialEditor (shared preview), AssetTools/Engine (package collection), and
the four feature-editor modules (toolkits). Test consumers include both
EngineTests and AssetTests; future migrations must search those roots again.
Public LevelEditor toolkit headers and reflected subsystem declarations are
part of the compile closure even when project consumers currently have no hits.

- Source instances are initialized and registered directly as modular features,
  following UE `UContentBrowserDataSource::Initialize(bool)` and
  `RegisterDataSource()`. There is no second class/factory registration registry.
  The editor subsystem distinguishes available instances from active instances;
  activation attaches its sink and deactivation detaches it. Deactivation first
  closes operation admission and invalidates that activation's item handles.
  Reactivating the same instance gives it a new activation generation, so retained
  items cannot silently become callable again. Shutdown unregisters the instance.
  This replaces the initial class-factory decision after checking UE's concrete
  implementation on 2026-11-12; the extra factory layer served no required behavior.
- Each folder item retains all source contributors under the normalized virtual
  path. The public aggregate forwards capability/operation calls to owned item
  data. Unsupported cross-source moves/copies are rejected for the whole batch
  before any provider writes; independently executable same-source batches
  retain their per-item structured outcomes.
- Discovery workers capture immutable value snapshots, cancellation and the
  editor/project/source generation tuple. Only owner-thread publication resolves
  live providers. Cancellation or provider/project retirement rejects publication
  and releases captured payloads without invoking retired code.
- UE save flags now support all combinations through existing package policy.
  Dirty predicates select `LoadedDirtyPackage`; absent a dirty predicate selects
  `CanonicalResave`. Loaded-only requests require residency and retain exports
  through execution. Unknown bits reject; no redundant AssetTools enum was added.
- `FContentBrowserDeleteContext` is a move-only operation-scoped capability
  binding source activation identity, immutable confirmation and the existing
  retained deletion execution session. `BulkDeleteItems(items, context)` checks
  owner, exact scope, fingerprints and admission again. Panel owns user
  confirmation; provider owns execution. A context cannot be reused after
  completion/cancellation or moved to another source. Existing companion policy,
  stale-plan diagnostics and compensation remain authoritative.
- The private asset service `HandleTicker(float)` runs once after tool/script/
  world work and before MainFrame's frame callback. It retains the existing
  `DispatchEditorCallback` transaction/host scope and rechecks `CanCall` after
  callbacks. Shutdown finishes the current borrowed operation, skips subsequent
  sessions and leaves physical teardown to the outer owner boundary. No ticker
  callback outlives its editor; no second loop is introduced.
- Selected baseline cases: `EditorOperationTests/FAssetEditorSessionTests.*`,
  `ContentBrowserWorkflowTests`, `TextureThumbnailTests`, `MaterialThumbnailTests`,
  and `AssetPackageTests/FAsyncAssetLoadTests.Editor*Unload*`. Query current target
  membership before execution. Stage 4's bounded Vulkan cases are
  `FThumbnailVulkanTests.TexturePreviewPreservesRawChannelsSrgbAndAspectRatio`
  and `FThumbnailVulkanTests.Texture2DThumbnailUsesBuiltNormalAndRejectsReplacedAllocation`;
  backend teardown coverage still requires inspection before that stage closes.

### Stage 0: Freeze interface parity and baseline evidence

Dependencies: none. Outcome: an implementation-ready API inventory, adaptation
list and bounded test baseline, without treating design review as test evidence.

- [x] Recheck UE source signatures and record header/method references for the
  interfaces above. Explicitly distinguish UE behavior from Durin adaptations.
- [x] Inventory changed symbols and consumers in all three workspace projects,
  including tests, reflection inputs, module manifests and exported headers.
- [x] Freeze the source initialization/activation and registration-generation
  contract, folder aggregation, async discovery ownership, save-flag mapping,
  cross-source rejection rules and confirmed-delete context.
- [x] Freeze the host service-update ordering and shutdown/reentrancy behavior.
  UE's asset subsystem ticker currently handles deferred restore; Durin's
  document-close maintenance is a deliberate extension and must run each frame,
  not inherit UE's one-second ticker interval.
- [x] Query current registry cases and capture focused baseline receipts for
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

- [x] Move the public `TickAssetEditors` maintenance implementation behind the
  private `HandleTicker(float)` entrypoint. Let DEditorEngine invoke it through
  narrow host access at the existing owner-controlled update boundary.
- [x] Invoke it once per admitted editor frame, after tool/script/world updates
  and before shell presentation. Preserve subsystem callback/transaction scopes
  and stop subsequent work immediately on shutdown or retirement requests.
- [x] Remove semantic maintenance from normal MainFrame ticking. Route any
  special close pump through the same host boundary, never two independent pumps.
- [x] Migrate tests to exercise the editor frame rather than manually compensating
  with `TickAssetEditors`. Remove the obsolete public entrypoint and consumers.

Validation: focused session cases in `EditorOperationTests`, plus affected host
cases in `EditorHostToolTests`. Verify pending Save/Cancel, grouped close,
deferred restore, no-shell operation, no double advancement, shutdown during a
callback and successive editor lifetimes. Build owning modules and consumers
selected by changed headers. No new global ticker framework is needed.

Acceptance: an editor frame without a MainFrame completes deferred semantic work;
adding a shell does not change its outcome or advance it twice. No public caller
or test requires `TickAssetEditors`.

Stage 1 evidence, 2026-11-12: final source/test inputs based on `3563d2bbd`,
Windows `windows-msvc-x64`, `Win64-Debug-DurinEditor`, outside-sandbox MSVC.
`EditorOperationTests/FAssetEditorSessionTests.*` passed 39 cases, including the
new shell-observes-refreshed-state/one-update regression; `EditorHostToolTests`
passed 39 cases. Receipts under `Build/.agent-state/logs/` are
`20261112-102936-325273-29048-EditorOperationTests.log` and
`20261112-103024-629747-13424-EditorHostToolTests.log`.
The host target build also compiled the concrete feature/editor consumer closure.
No production/test inputs changed after these runs; subsequent edits only record
this evidence. All workspace source/test roots have no `TickAssetEditors` hits.
Changed-document and all-plan validation passed. No GPU or application-hosted
validation was required for this service-ordering stage.

### Stage 2: Introduce UE-shaped browser data contracts and providers

Dependencies: Stages 0-1. Outcome: source ownership and item dispatch exist as
real contracts, with built-in providers using the existing operation substrate.

Scope: ContentBrowser public data headers; data subsystem/source registration;
asset/file provider implementations; private discovery and operation code;
reflection/module manifests and provider fixtures.

- [x] Add the data subsystem, abstract source, item data, payload, filter and
  save-flag contracts from the interface map. Keep class names aligned with UE.
- [x] Register initialized asset/file source instances through modular features;
  retain per-editor instances, separate available and active sources,
  and shut down sources before releasing their service/payload dependencies.
- [x] Move asset capabilities into the asset provider and filesystem capabilities
  into the file provider. Call AssetTools and existing filesystem mechanisms;
  preserve staging, rollback, compensation and mutation notifications.
- [x] Implement logical path routing and folder aggregation. Partition batch work
  by source and reject unsupported transfers before any physical writes.
- [x] Keep worker captures immutable and owned. Resolve live reflected sources
  only at the owner-thread boundary; reject stale generation/project results.
- [x] Preserve the existing confirmed deletion flow through the explicit context
  adaptation. A provider capability query does not authorize later I/O.

Validation: public provider cases in `ContentBrowserDataTests`, plus existing
mechanism cases in `ContentBrowserWorkflowTests`: ownership
dispatch, same-name source replacement, deactivate/reactivate, opaque payload
release, merged folders, mixed-source selection, cross-source rejection,
capability/execution revalidation, save-mode parity and stale confirmed deletion.
Use a small test provider to demonstrate extension without editing built-in
item-kind switches; do not create a new production backend for the test.

Acceptance: built-in operations use their provider contracts. Provider withdrawal
cannot leave callable stale payloads or publish retired async results. No parallel
legacy source continues to execute migrated operations.

The implemented [provider contract](../Editor/Architecture/Assets/ContentBrowserData.md)
owns the lasting API/lifetime rules. The old private source is now
`FContentBrowserRequestContext`; existing panel adapters and native providers use
that one operation implementation. Stage 3 still owns replacing the panel's direct
request-context binding with the data service, not a second rewrite of mutations.
UE's payload/update/sink declarations are all in `ContentBrowserItemData.h`;
the earlier separate payload-header reference was incorrect and is corrected here.
New APIs and reflected classes are exported by the existing ContentBrowser module;
no new DLL or DurinGame dependency was introduced.

Stage 2 final-input evidence, 2026-11-12, working inputs based on `f5093a4f3`,
Windows `windows-msvc-x64` / `Win64-Debug-DurinEditor`, outside-sandbox MSVC:

| Selection | Result | Receipt under `Build/.agent-state/logs/` |
| --- | --- | --- |
| ContentBrowserDataTests | 20 passed | `20261112-120148-569758-3068-ContentBrowserDataTests.log` |
| ContentBrowserWorkflowTests | 158 passed, 2 existing symlink-privilege skips | `20261112-112933-874134-22784-ContentBrowserWorkflowTests.log` |
| EditorHostToolTests | 39 passed | `20261112-115220-587763-30912-EditorHostToolTests.log` |
| EditorAssetWorkflowTests | 44 passed | `20261112-113008-154379-35100-EditorAssetWorkflowTests.log` |
| PythonScriptPanelTests | 8 passed | `20261112-113017-076547-35912-PythonScriptPanelTests.log` |
| Editor all, Engine/Sandbox/RoadWeaver closure | passed | `20261112-113229-460750-31816-cmake.log`; final interface incremental `20261112-120149-896789-21740-cmake.log` |

These are 269 executed passes and two unqualified permission skips. The final
source changes tightened batch-save activation checks, registered-instance
GC retention, and invalidation of handles created before activation, with
regressions for each; the provider suite was refreshed afterward. Host lifecycle
coverage was refreshed after the GC change. The unchanged legacy consumer
workflows do not enter this new routing branch; their receipts remain applicable.
The final incremental build covers the updated implementation, with the earlier
all build covering its unchanged header/reflection and cross-project closure.
`Build/EditorResponsibilityAndUEInterfaceAlignment/stage2-inputs.json` records
final production/test hashes; `stage2-consumers.txt` records the consumer search.

Affected selection expands to the whole Engine test project for a new test source.
Focused owning/consumer targets are used instead: only ContentBrowser and its
white-box workflow fixtures changed, with no runtime/rendering algorithm changes.
One broad incremental build was required for public Item layout and reflected
class/module-graph integration across all workspace projects. This does not close
the later Stage 6 final-input build or application-startup gates. No GPU or native
window qualification is claimed for Stage 2.

The first Stage 2 checkpoint exposes `ContentBrowserItem.h` and one
`FContentBrowserItem` snapshot used by discovery, operations and extension
contexts. It deletes `FExtensionItem`, `EExtensionItemKind` and their translation
function, including the separately stored extension stable ID. Consumers call
the shared item's identity operation instead. This is an incremental interface
migration; it does not yet claim UE's ItemData/payload/provider dispatch or close
any of the full-stage checkboxes above.

Checkpoint validation on 2026-11-12, inputs based on `e2be387ed`, Windows
`windows-msvc-x64` / `Win64-Debug-DurinEditor`, outside-sandbox MSVC:

| Selection | Result | Receipt under `Build/.agent-state/logs/` |
| --- | --- | --- |
| ContentBrowserWorkflowTests | 158 passed, 2 symlink-privilege skips | `20261112-103827-232648-33588-ContentBrowserWorkflowTests.log` |
| EditorHostToolTests | 39 passed | `20261112-103853-171413-30760-EditorHostToolTests.log` |
| PythonScriptPanelTests | 8 passed | `20261112-103901-602051-32944-PythonScriptPanelTests.log` |
| EditorAssetWorkflowTests | 44 passed | `20261112-103912-385675-32832-EditorAssetWorkflowTests.log` |
| StaticMeshThumbnailTests | 10 passed | `20261112-104041-983423-17696-StaticMeshThumbnailTests.log` |
| ThumbnailTests | 74 passed | `20261112-104057-564443-34292-ThumbnailTests.log` |

All 333 executed cases passed; two Windows permission skips remain unqualified.
The thumbnail white-box targets require the ContentBrowser Public include
directory now that their shared item lives there; no DLL dependency was added.
Only test indentation/line endings and documentation changed after the receipts.
All workspace production/test roots were checked for removed extension item
names and the old private-qualified item name; no remaining hits exist.
This checkpoint uses focused affected consumers rather than the final all-build
gate: it changes a value type's ownership and extension representation, not GPU
behavior, provider lifecycle or package operations. The full Stage 2 gates remain
open and these receipts cannot qualify their future implementation.

### Redundancy Review

| Observation | Decision and retained behavior |
| --- | --- |
| Browser item versus extension-only item and Folder/Directory enum translation | Removed in the first Stage 2 checkpoint. Extensions retain value-owned selection/primary snapshots, including ordinary-file paths; no asset loading is introduced. |
| Proposed registration-of-source-classes plus activation factory | Removed from the selected design after checking UE's direct instance registration. Preserve available/active separation and activation generations; they protect different lifetimes. |
| Private concrete source and public provider appearing to own the same role | Private implementation renamed to request context and reused as one mutation/recovery substrate. Generic dispatch is source-owned; no forwarding aliases or second mutation engine were retained. |
| New coarse source-change notification | Replaced with UE-shaped `FContentBrowserItemDataUpdate`/`QueueItemDataUpdate`, sourced from committed effects rather than operation attempts. |
| Native Asset/File sources both contributing the same physical folder | Stage 3 collapses the duplicate executable identity. Mixed native selections share one atomic backend; unrelated providers retain separate contributors and require explicit transfer support. |
| Discovery preparation retained in the mutation request context | Stage 3 separates per-view owned captures into the service-bound data query. Panels hold no request context; query completion uses ordinary provider enumeration. |
| Panel-specific asset/file command branches and physical-path drag reconstruction | Stage 3 replaces them with item capabilities, virtual destinations and retained owner identities. No parallel legacy panel operation path remains. |
| `NotifyMountedContentMutation` versus `NotifyScopedContentMutation` host callbacks | Retained at the native adapter for external create/import and registry reconciliation consumers. Provider frame updates drive browser state; the unscoped fallback still expresses unknown external coverage and is not a second mutation engine. |
| Virtual path, physical path and package identity stored on an item | Not proven redundant: unmounted files, logical folder navigation and package-backed operations use different identity domains. Provider payload migration should move ownership rather than delete required identities. |

### Stage 3: Migrate browser presentation and external consumers

Dependencies: Stage 2. Outcome: every browser uses the data service; panel state
and user confirmation remain local to each browser.

Implementation decisions, 2026-11-12: extract pure discovery preparation from the
mutation request context into per-view query state. Pass completed native
snapshots through the provider filter/enumeration contract on the owner thread;
never invoke a reflected provider on a worker. Keep a weak data-subsystem binding
in the browser host. Native Asset/File adapters explicitly support mixed native
batches through their one existing atomic backend. Collapse their duplicate
physical-folder operation identity while retaining unrelated providers' logical
folder contributions. Expose immutable provider-neutral deletion previews, with
the validated execution capability still private to its provider. Creation/import
and definition registration remain independent of provider membership.

Scope: ContentBrowser Panels/Model/Query/refresh coordinator/operations/public
host contracts; MainFrame; AssetForgeBuiltins; LevelEditor, MaterialEditor,
TextureEditor, StaticMeshEditor; RoadWeaverEditor contributions and all tests.

- [x] Replace panel-facing direct request-context dispatch with a weak/service-bound data
  subsystem reference. Keep selection, search, navigation, cancellation cursors
  and presentation settings per panel; do not put them in shared providers.
- [x] Migrate queries, menu capabilities, shortcuts, clipboard and drag/drop to
  `FContentBrowserItem`/`FContentBrowserItemData` and virtual destination paths.
- [x] Preserve independent create/import/details extensions and metadata-driven
  asset definitions. Do not merge browser provider registration with definitions.
- [x] Delete panel-level execution dispatch by item kind. Keep family-specific
  validation inside native request mechanics where required for atomic mixed
  package/file folder operations; do not duplicate staging/rollback by provider.
- [x] Update the implemented Content Browser contract and affected consumer docs
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

Stage 3 final-input evidence, 2026-11-12, working inputs based on `6dcb2ff4b`,
Windows `windows-msvc-x64` / `Win64-Debug-DurinEditor`, outside-sandbox MSVC:

| Selection | Result | Receipt under `Build/.agent-state/logs/` |
| --- | --- | --- |
| ContentBrowserWorkflowTests | 163 passed, 2 existing symlink-privilege skips | `20261112-134854-575041-36068-ContentBrowserWorkflowTests.log` |
| ContentBrowserDataTests | 22 passed | `20261112-134910-132385-19040-ContentBrowserDataTests.log` |
| EditorAssetWorkflowTests | 44 passed | `20261112-134918-525672-10836-EditorAssetWorkflowTests.log` |
| EditorHostToolTests | 39 passed | `20261112-134922-041302-33424-EditorHostToolTests.log` |
| AssetEditorFamilyTests | 7 passed | `20261112-134924-780864-21720-AssetEditorFamilyTests.log` |
| ThumbnailTests | 74 passed | `20261112-134939-647465-3232-ThumbnailTests.log` |
| StaticMeshThumbnailTests | 10 passed | `20261112-134944-607146-35652-StaticMeshThumbnailTests.log` |
| Editor all, Engine/Sandbox/RoadWeaver closure | passed | `20261112-134947-900791-36124-cmake.log` |

These are 359 executed passes and two unqualified permission skips. New tests
exercise a provider with no physical item storage through ordinary panel commands,
two independent browsers and selection migration, target policy rechecks during
clipboard/drop routing, withdrawal/reactivation, native confirmation replacement
and retry, mixed native batching and deferred creation. Unrelated source updates
do not restart another view; source revisions cannot bypass a publication fence.
Existing coverage retains ordinary files, packages, redirectors, companion policy,
folder rollback, import/reimport, asynchronous cancellation and editor retirement.

All source/test roots of Engine, Sandbox and RoadWeaver were searched. MainFrame
now supplies the weak service binding. Feature-editor and RoadWeaver contributions
already use the shared item/extension contracts and needed no separate operation
adapter or registration migration. Panel dispatch no longer references the native
mutation request context or its save-mode/physical-destination APIs. Native
maintenance commands remain explicit asset-provider utilities, independently of
generic item routing. The lasting contracts are in
[Content Browser](../Editor/Architecture/Assets/ContentBrowser.md) and
[data providers](../Editor/Architecture/Assets/ContentBrowserData.md).

The affected selector expands to the entire Engine test project because a test
CMake declaration changed and private query sources were added. The named owning
and consumer targets above bound the changed behavior; unrelated rendering,
physics and transport suites were not run. The all build covers public host/API
layout, provider vtables/delegates and consumers across all workspace projects.
No rendering algorithm changed, and no GPU qualification or full application
startup is claimed. Stage 6 retains its later final-input/application gates.
Raw inventories and final input hashes are under
`Build/EditorResponsibilityAndUEInterfaceAlignment/stage3-consumers.txt` and
`stage3-inputs.json`. Changed-document and all-plan validation passed; subsequent
changes after these native receipts are documentation only.

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
