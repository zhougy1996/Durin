# UE 5.8 Asset Operations Alignment Plan

Summary: Align editor asset operation interfaces, data structures, ownership, and behavior with local UE 5.8, migrate every workspace consumer, and remove superseded operation protocols.

Last reviewed: 2026-11-16

Status: Completed
Completed: 2026-11-16

## Current Status

Planning baseline: `4f09ea83cf179c7cf331971c7792aa1c28af7a41`.
Stage 0 is complete against audit baseline
`b3f347136ed5398a9cf4c0e205922aac9f38a31d` on 2026-11-16. The reference contract,
supported surface, dependency decisions, workspace consumer inventory and test
mapping are frozen below. The intervening toolkit ownership/preview changes are
included in that baseline. Stage 1 is complete on 2026-11-16 against
`394237cb6176aecc8b6e4b898a4851231557b336` plus its implementation working tree.
Stage 2 is complete on 2026-11-16 against
`793934ef450be2b3f46bf5b49777d92553fb5ddd` plus its implementation working tree.
Acceptance evidence for both stages is recorded below. Stage 3 is complete on
2026-11-16 against `21af11c9207f5111023158b05558192349cf7c3c` plus its implementation
working tree. File-list and scene execution now share managed tasks, and the
superseded import protocols are removed. Stage 4 is complete on 2026-11-16
against `d570674d8f9ccd7446c96aad63a379c71b493270` plus its implementation working
tree. ObjectTools and the delete model now own deletion policy; Browser deletion
uses independent context-free provider dispatch. Stage 5 is complete on
2026-11-16 against `30ee8234ab909a60356dbab0da558c8b05d8e5cb` plus the
implementation inputs in `stage5-receipts/tested-inputs.json`. All removal,
contract, workspace-build, selected-regression and bounded lifecycle gates passed.
Final evidence is recorded in Stage 5 below.
Stage 0 itself contained no implementation change or native execution.

Stage 0 planning-document checks on 2026-11-16: changed-document validation passed for
this file; all-plan validation passed for 11 active, 8 completed and 457 archived
plans. No native build or test was run for this documentation-only change.

The Stage 0-2 reference installation was `D:/Programs/UE_5.8`, version 5.8.2,
CL 56702186, compatible CL 55116800, branch `++UE5+Release-5.8`, verified from
`Engine/Build/Build.version`. Reference paths below are relative to its `Engine/`.
Stage 0 reverified this identity. Later implementation must refresh inventory
when the reference or relevant workspace inputs change.

The user supplied the current reference at `E:/Programs/Epic Games/UE_5.8`.
On 2026-11-16 its `Engine/Build/Build.version` reports **5.8.0**, CL 53629095,
compatible CL 0, branch `++UE5+Release-5.8`. Stage 3 rechecked
`Source/Editor/UnrealEd/Public/AssetImportTask.h` and
`Source/Developer/AssetTools/Private/AssetTools.cpp` against that source: task
defaults, managed factory/options/results, null-task skipping, successful object
paths and optional package saving retain the selected roles. Earlier source
locations and validation receipts remain historical evidence for their recorded
baseline; this reference change does not retroactively revalidate other operations.

This plan supersedes the retained public confirmed-deletion protocol selected by
the completed [operation simplification plan](ContentBrowserUEOperationSimplification.md)
and the asset-operation adaptations in the completed
[editor alignment plan](EditorResponsibilityAndUEInterfaceAlignment.md).
Their independent provider dispatch, opaque item payloads, and incremental
updates remain the baseline. Their completed evidence is historical, not evidence
for the behavior changes here. The
[asset editor architecture plan](UEAssetEditorArchitecture.md) retains toolkit
and document ownership; this plan migrates its operation consumers only.

## Goal

A UE developer should recognize each supported operation's owner, argument roles,
data structure, result meaning, object lifetime, identity changes, and saving
behavior. Alignment must change the actual execution model where necessary;
renaming Durin requests or wrapping unchanged behavior is insufficient.

Use Durin's `DObject`, `DClass`, `DPackage`, strong/weak object pointers,
`FName`, typed asset paths, standard containers, and existing module system.
Map UE containers to `std::vector`/`std::span` and text to supported Durin text
types without weakening path identity. Do not introduce UE container emulation
or reproduce its entire API surface. Implement supported features completely;
do not publish inert flags, fabricated success, or placeholder async methods.

Scope includes create, duplicate, unique naming, import, rename/move, redirector
fix-up, save, unload/reload, deletion, Browser adapters, notifications, script/tool
consumers, and affected native tests across Engine, Sandbox, and RoadWeaver.
No package-format migration, GC redesign, DDC scheduler rewrite, new source-control
backend, localization system, force-delete capability, or reference-consolidation
feature is required. Existing capabilities in these areas must not regress.

## Reference And Selected Architecture

### Stage 0 verified decisions

These decisions replace provisional choices elsewhere in this plan where more
specific. They are selected implementation requirements, not claims that the
current engine already implements UE behavior.

#### Verified UE behavior and required changes

References in this table use the local source root and line locations at the
audited installation. A SHA-256 manifest of all 23 reference files is retained
with the Stage 0 artifacts; refresh locations if that manifest changes.

| UE source and symbol | Verified behavior | Selected Durin change |
| --- | --- | --- |
| `AssetTools.cpp:1743`, `UAssetToolsImpl::CreateAsset` | Mutable factory; class may be inferred from factory; factory receives CallingContext; flags are Public/Standalone/Transactional; successful creation publishes AssetCreated and marks Dirty | Separate asset name and directory, infer class when appropriate, preserve fixed creation flags internally, route CallingContext to factory |
| `AssetTools.cpp:2161`, `PerformDuplicateAsset` | Explicit target, delegates to ObjectTools; normally Dirty, but source-control-enabled branch also saves and branches package | Source control is absent in Durin: select the disabled-provider branch, retain Dirty copies and remove unconditional Browser copy/duplicate saves. Do not describe UE duplication as universally save-free |
| `AssetTools.cpp:3122`, `CreateUniqueAssetName` | Sanitizes names, increments trailing integers with zero padding, handles all-numeric names, queries existing object paths | Replace implicit `_Copy`, `_Copy2` policy with the UE naming algorithm. Add conservative resident/physical occupancy checks; outputs do not reserve a destination |
| `AssetRenameManager.cpp:138,154`, `RenameAssets`, `RenameAssetsWithDialog` | Ordinary rename rejects outstanding discovery; dialog overload may return Pending and resume later | Implement synchronous RenameAssets only; no dialog/deferred rename overload or EAssetRenameResult in this increment. Discovery/unavailable catalog rejects before mutation; no fabricated Pending or cancellation enum |
| `AssetRenameManager.cpp:98`, private rename-data normalization | Fills old path from live object, or resolves live object from old path; fills missing new path from directory/name | Use the same constructor/normalization roles; contradictory valid fields reject rather than guess |
| `AssetRenameManager.cpp:386,1748`, `FixReferencesAndRename`, `PerformAssetRename` | Gathers persistent referencers and dirty loaded soft references, leaves redirectors as needed, renames live objects, schedules renamed/reference packages for save; ReportFailures determines rename bool | Implement live object rename and reference policy, not package-byte relocation with new names. Preserve successful peers on failure. Bool reports absence of per-item rename failure, not OR-success or proof all saves persisted; persistence failures remain diagnostics/native facts |
| `ObjectTools.cpp:4278`, `RenameSingleObject` | Operates on selected object and target outer/package; sibling assets are not implicitly selected | Move selected top-level object graph into destination package, preserve source siblings and their package, and preserve subobject suffixes. Extend CoreDObject's checked rename/reparent mechanism; do not invoke transaction replay as a rename shortcut |
| `AssetFixUpRedirectors.cpp:534,562`, `FixupReferencers`, `ExecuteFixUp` | Void entrypoint, weak captures if discovery defers, separate in-progress query and mode-dependent redirector deletion | Implement synchronous void FixupReferencers plus IsFixupReferencersInProgress; reject unavailable catalog with feedback; expose delete/leave modes only |
| `AssetTools.cpp:2955,3106`, task and file imports; `AssetImportTask.cpp:17,29` | Tasks skip null entries; report successful outputs, not positional null placeholders; bSave saves result packages; GetObjects waits for actual async results, polling does not | Task state owns managed results; file-list import returns successful outputs only. Async-capable scene adapter may defer; ordinary factories may finish synchronously |
| `FileHelpers.h:482,490`; `FileHelpers.cpp:5960,5996,6019` | Prompt enum distinguishes decline/cancel/failure; SavePackages filters invalid/non-dirty packages and returns true for empty filtered work | Replace strict loaded-dirty SaveAssets rejection with selected UE filtering. Preserve committed-file evidence independently of command success |
| `PackageTools.h:19,85,101,132,155`; `PackageTools.cpp:361,848` | Unload/reload return whether resident package set changed; dirty reload interaction has Interactive/AssumePositive/AssumeNegative | Add corresponding editor policy over native atomic reload, retain native operation while retirement is pending; do not turn it into a second public request API |
| `ObjectTools.h:323,325,334,374`; `ObjectTools.cpp:2759,3077` | DeleteObjects/DeleteAssets return counts; FDeleteItemsParameters/Result already represent objects and folders separately | Add UE-shaped structures and DeleteItems, instead of replacing ContentDeletion with another custom generic result |
| `AssetDeleteModel.cpp:94,280,285` | Owner-thread scanning advances through states; CanDelete reports reference admissibility; DoDelete executes when admissible and count is a separate fact | Preserve bool action/count distinction. Durin additionally requires completed analysis and current native validation before execution; no force-delete or unchecked public bypass |
| `ContentBrowserDataSource.cpp:437,442,492,512` | Default single delete returns false; default bulk delete/copy/move calls each single override and ORs results | Restore DeleteItem and context-free BulkDeleteItems, including extensible default fallbacks; concrete native bulk overrides may batch their own work |
| `IAssetRegistry.h:942`, `OnAssetRenamed`; `ObjectMacros.h:741,756`, reference information values | Registry rename event carries new FAssetData and old object path; reference reports separate internal/external owners and property occurrences | Adopt those roles below AssetTools for identity notification and delete analysis rather than leaking editor request types into lower modules |

#### Frozen supported declarations

Use `namespace Durin` unless an existing Browser namespace owns the item type.
The public header paths are `AssetTools/IAssetTools.h`, `AssetTools/AssetRenameData.h`,
`AssetImportTask.h` (DurinEd), `ObjectTools.h`, `AssetDeleteModel.h`,
`PackageTools.h`, and `FileHelpers.h`. Avoid forwarding headers for removed names.
The new names map to UE roles; they do not require UE container wrappers.

```cpp
// IAssetTools: required arguments and defaults; no old Context/Flags/Warn tail.
virtual DObject* CreateAsset(std::string_view AssetName,
    std::string_view PackagePath, DClass* AssetClass, DFactory* Factory,
    FName CallingContext = {}, bool bOverwriteExisting = false) = 0;
virtual DObject* DuplicateAsset(std::string_view AssetName,
    std::string_view PackagePath, DObject* OriginalObject) = 0;
virtual void CreateUniqueAssetName(std::string_view InBasePackageName,
    std::string_view InSuffix, std::string& OutPackageName,
    std::string& OutAssetName) = 0;
virtual bool RenameAssets(std::span<const FAssetRenameData> AssetsAndNames) = 0;
virtual std::vector<DObject*> ImportAssets(std::span<const std::string> Files,
    std::string_view DestinationPath, DFactory* ChosenFactory = nullptr,
    bool bSyncToBrowser = true,
    std::vector<std::pair<std::string, std::string>>* FilesAndDestinations = nullptr,
    bool bAllowAsyncImport = false, bool bSceneImport = false) const = 0;
virtual void ImportAssetTasks(std::span<DAssetImportTask* const> ImportTasks) = 0;
virtual void FixupReferencers(std::span<DAssetRedirector* const> Objects,
    bool bCheckoutDialogPrompt = true,
    ERedirectFixupMode FixupMode = ERedirectFixupMode::DeleteFixedUpRedirectors) const = 0;
virtual bool IsFixupReferencersInProgress() const = 0;
virtual TMulticastDelegate<void(std::span<const FAssetRenameData>)>&
    OnAssetPostRename() = 0;

struct FAssetRenameData
{
    TWeakObjectPtr<DObject> Asset;
    std::string NewPackagePath, NewName;
    FSoftObjectPath OldObjectPath, NewObjectPath;
    bool bOnlyFixSoftReferences = false;
    FAssetRenameData() = default;
    FAssetRenameData(TWeakObjectPtr<DObject>, std::string_view NewPackagePath,
        std::string_view NewName, bool bOnlyFixSoftReferences = false);
    FAssetRenameData(const FSoftObjectPath& OldObjectPath,
        const FSoftObjectPath& NewObjectPath, bool bOnlyFixSoftReferences = false);
};
enum class ERedirectFixupMode
{ DeleteFixedUpRedirectors, LeaveFixedUpRedirectors };

// ObjectTools: editor entrypoints; native safety checks remain mandatory.
namespace ObjectTools {
enum class EAllowCancelDuringDelete : uint8 { AllowCancel, CancelNotAllowed };
struct FDeleteItemsParameters
{
    std::vector<DObject*> ObjectsToDelete;
    std::vector<std::string> FoldersToDelete;
    bool bShowConfirmation = true;
    EAllowCancelDuringDelete AllowCancelDuringDelete = EAllowCancelDuringDelete::AllowCancel;
};
struct FDeleteItemsResult { int32 ObjectsDeleted = 0, FoldersDeleted = 0; };
void DuplicateObjects(std::span<DObject* const> SelectedObjects,
    std::string_view SourcePath = {}, std::string_view DestinationPath = {},
    bool bOpenDialog = true, std::vector<DObject*>* OutNewObjects = nullptr);
int32 DeleteObjects(std::span<DObject* const> ObjectsToDelete,
    bool bShowConfirmation = true,
    EAllowCancelDuringDelete AllowCancelDuringDelete = EAllowCancelDuringDelete::AllowCancel);
int32 DeleteAssets(std::span<const FAssetData> AssetsToDelete,
    bool bShowConfirmation = true);
FDeleteItemsResult DeleteItems(const FDeleteItemsParameters& Options);
}
```

`FAssetRenameData` is owned value data; delegate spans live only for synchronous
broadcast. Normal form renames package and asset together. Explicit paths allow
non-leaf names and soft-subobject reference rewrites; ordinary physical asset
rename requires a top-level object and does not reparent an arbitrary subobject.
`FAssetData` is the existing exact-asset record, not FPackageData.

Freeze the remaining structures and overloads as follows:

- `DAssetImportTask : DObject` belongs to DurinEd, the same lower layer as
  DFactory, matching UE's UnrealEd task/factory placement. All five booleans
  (`bReplaceExisting`, `bReplaceExistingSettings`, `bAutomated`, `bSave`, `bAsync`)
  are false by default. Filename/destination strings are empty,
  Factory/Options null, paths/results empty, and a new task is synchronously
  complete until submitted. `GetObjects() const` returns
  `const std::vector<DObject*>&`; `IsAsyncImportComplete() const` returns bool.
  Reflected/strong owners retain Factory, Options and results; task-private
  completion storage is independent of AssetTools implementation types.
- `DFactory` execution becomes mutable in Stage 1. Use UE-shaped
  `FactoryCreateNew(DClass*, DObject* InParent, FName, EObjectFlags, DObject* Context,
  FFeedbackContext*, FName CallingContext = {}) -> DObject*` and
  `FactoryCreateFile(DClass*, DObject* InParent, FName, EObjectFlags,
  std::string_view Filename, std::string_view Parms, FFeedbackContext*,
  bool& bOutOperationCanceled) -> DObject*`. Stage 1 migrates both signatures and
  every override/caller; Stage 3 adds task-aware options and file dispatch.
  Discovery remains read-only metadata over CDOs; execution instantiates and
  retains a transient factory unless an explicit configured instance is supplied.
  Do not mutate or const-cast discovered CDOs. Keep typed domain causes internal
  and format through feedback at the factory boundary.
- Add `FScopedFeedbackContext(FFeedbackContext&)` in Core in Stage 1, with lexical
  owner-thread override of `FFeedbackContext::Get()`, nested restoration and no
  implicit worker propagation. This preserves existing per-command diagnostics
  after removing trailing Warn arguments without rebuilding structured errors
  from prose or adding a new operation envelope. Existing feedback remains the
  fallback; permission context is unaffected.
- `DPackageTools` keeps three UnloadPackages overloads: packages; packages plus
  `std::string& OutErrorMessage` and `bool bUnloadDirtyPackages=false`; and
  `FUnloadPackageParams&`. Params own an immutable vector of DPackage pointers,
  `bUnloadDirtyPackages=false`, `bResetTransBuffer=true`, and error text.
  ReloadPackages has packages-only and packages/error-text/
  `EReloadPackagesInteractionMode=Interactive` overloads; enum values are
  Interactive, AssumePositive, AssumeNegative. Omit the deprecated bool overload.
  Reload waits/drains native retirement at the existing safe point before the
  synchronous call returns; native operation APIs remain available to actual
  asynchronous/resource owners, not through IAssetTools.
- `DEditorLoadingAndSavingUtils` exposes static bool SavePackages(packages,
  bOnlyDirty), SavePackagesWithDialog(packages, bOnlyDirty), SaveDirtyPackages(
  bSaveMapPackages, bSaveContentPackages), and its WithDialog counterpart.
  All package lists are `std::span<DPackage* const>`. `FEditorFileUtils` exposes
  `enum EPromptReturnCode { PR_Success, PR_Failure, PR_Declined, PR_Cancelled }`,
  `FPromptForCheckoutAndSaveParams&`, and UE's short bool-parameter overload:
  packages, bCheckDirty, bPromptToSave, optional failed-package vector pointer,
  bAlreadyCheckedOut=false, bCanBeDeclined=true. Params use UE defaults:
  bCheckDirty=false, bPromptToSave=false, bAlreadyCheckedOut=false,
  bCanBeDeclined=true, bIsExplicitSave=false, Title/Message default save prompt,
  OutFailedPackages=nullptr. The title/message overload delegates to Params.
- `FPendingDelete` keeps a strongly retained Object, DiskReferences as
  `std::vector<FPackagePath>`, `FReferencerInformationList MemoryReferences`, RemainingDiskReferences and
  RemainingMemoryReferences counts, IsInternal, and CheckForReferences.
  `FAssetDeleteModel` has the five UE states, object-span constructor,
  AddObjectToDelete, GetPendingDeletedAssets (const view), GetState,
  Tick(float), CanDelete/DoDelete, GetDeletedObjectCount, GetProgress() returning float,
  GetProgressText() returning std::string, and OnStateChanged(EState). Scanning is
  owner-thread
  incremental work. Own retention is excluded from outside-reference blockers;
  it is released only at the validated executor handoff.
- Add CoreDObject reference-information values in Stage 4: FReferencerInformation
  has weak Referencer (Durin lifetime adaptation), int32 TotalReferences and
  `std::vector<const FProperty*> ReferencingProperties`;
  FReferencerInformationList owns InternalReferences/ExternalReferences vectors.
  Derive them through read-only native traversal, with property descriptors valid
  only while their owner module is retained. Native function/strong-owner blockers
  remain explicit facts even when they have no DObject/FProperty referencer;
  never fabricate counts or clear those blockers to populate the UE model.
  Distinguish the model's exact own retention from other strong registrations
  during final validation; an asset-wide exemption is not acceptable.
- Browser DeleteItem(item) and BulkDeleteItems(span of const item data) return
  bool, without context parameters. Base bulk delegates to single actions and
  ORs results. The Data service and Item aggregate dispatch sources independently;
  one provider's confirmation rejection does not roll back another's work.

#### Supported policy and deliberate deviations

| Concern | Frozen decision and owning stage |
| --- | --- |
| Creation replacement | Stage 1 implements bOverwriteExisting, default false. True prepares a candidate and safely replaces a compatible existing asset via the object replacement/reload participant mechanisms; failed preparation preserves the old object. Protected/live owners must migrate through a participant or reject before replacement. Cross-class replacement rejects explicitly; no forced destruction. Saved bytes change only through save policy |
| CallingContext and flags | FName CallingContext is not the old DObject Context. Public creation fixes UE's three flags; specialized native construction/tests use object primitives. Preserve factory settings through invocation instances, not an arbitrary public flags tail |
| Import replacement/settings | Stage 3 implements same-class replacement for supported families. bReplaceExisting=false preserves an existing destination and diagnoses conflict in automated mode; true permits replacement, with settings retained unless bReplaceExistingSettings=true. Options apply to the invocation. Headless runs never prompt and reject ambiguous/unsupported replacement. Cover all flag combinations |
| File-list import options | FilesAndDestinations maps source filename to logical destination; bSceneImport selects the registered scene adapter. bSyncToBrowser is optional host synchronization; absent UI is a no-op. Async scene import uses the task pipeline; legacy ChosenFactory is not silently ignored merely to imitate Interchange. This explicit adaptation differs from UE's documented Interchange factory bypass |
| Reusing import tasks | Reject concurrent submission of an active task; clear old per-run results on a completed task's resubmission. GetObjects retains UE waiting behavior; UI polls first. No public cancel field: host/session cancellation and drain remain internal |
| Rename/reference/save boundary | Implement object and subobject path remapping, including dirty loaded soft references. Rename may automatically save renamed assets, required redirectors and eligible referencers; no universal pre-save gate. Leave valid redirectors for unrewritable/read-only participants. Partial saves never cause repeat rename or discard unsaved live state |
| Multi-asset packages | Rename/move selects exact top-level asset, preserves siblings and updates source/destination package metadata and references. Stage 4 object deletion similarly preserves unselected siblings; persist the surviving package and remove companions only when no surviving package owns them. Folder selection explicitly expands assets; no hidden sibling expansion |
| Dirty deletion | Remove the blanket selected-Dirty-package blocker when safe object deletion can explicitly abandon selected content. Confirmation covers that loss; headless caller explicitly requests deletion. Preserve unselected dirty siblings. Outside references, active documents/levels, loading packages and unsafe files still block; no force-delete |
| Redirector deletion | An unused redirector may be selected alone without selecting its real target. Deleting a target still checks incoming alias/reference usage; do not retain the old unconditional target/complete-alias-selection rule when the model proves the selected objects safe. Never delete a still-needed redirector to force success |
| Source control | No provider exists in the scanned modules. Use UE's disabled-provider branches with physical writeability checks. No fake checkout, branch/revert or source-control success. bAlreadyCheckedOut/bCheckoutDialogPrompt retain compatible roles but do not authorize read-only writes. Do not expose new source-control settings |
| Dialogs and automated callers | MainFrame registers typed prompt/selection/sync services into AssetTools; AssetTools does not link MainFrame or Browser. Missing required confirmation host rejects, never auto-approves. Explicit noninteractive SavePackages and DeleteAssets(false) need no UI; interactive reload without a host uses negative response for Dirty loss. AssumePositive/AssumeNegative are explicit caller choices |
| Deferred rename, localized variants, Interchange | Omit RenameAssetsWithDialog, EAssetRenameResult, bAlsoRenameLocalizedVariants, Interchange handles/pipelines, and async creation overloads. Existing Durin task/format adapters supply supported async import. No placeholder methods or unsupported success flags |
| Fix-up modes | Implement delete/leave; omit PromptForDeletingRedirectors until its project policy and host dialog exist. FixupReferencers is void; UI/console reports handled/admitted independently of backend diagnostics and observes native events, rather than interpreting a bool as persistence success |
| Deletion model limits | No CanForceDelete/DoForceDelete, reference consolidation or public DeleteObjectsUnchecked. CanDelete requires finished scan plus native protections, an intentional stricter adaptation. Use actual committed object/folder counts; DoDelete bool means accepted execution, not proof of full deletion or persistence |
| Package writer/reload | Keep FAssetWriteResult, typed errors, byte identities and atomic reload internally/native. Keep DurinEd document Save/Discard using Engine mechanisms: these are existing lower-layer callers, not missing IAssetTools migrations |

#### Consumer inventory and dependency closure

Audited all source/native-test roots declared by `Durin.dworkspace`: Engine,
Sandbox and RoadWeaver. Also searched program sources, NativeTestSupport, Tools,
Templates, CMake, descriptors, reflection inputs and script/tool bindings.
`rg --files`-based inventory scanned 3,221 text/code/metadata files and retained
2,685 grouped matching lines, including declarations, fixtures, include-only and
diagnostic-string hits. These are lexical counts, not counts of semantic callers.
No source/test-local AGENTS.md was found in those project roots.

Raw reproducible inventory is under ignored `Build/UE58AssetOperationsAlignment/`:
`stage0_inventory.py`, `scanned-files.txt`, `consumer-hits.tsv`,
`consumer-summary.json`, `module-graph.json`, `ue-reference-manifest.json`, and
`test-registry-*.txt`. Reproduce the lexical inventory with
`python Build/UE58AssetOperationsAlignment/stage0_inventory.py`; the patterns are
also embedded in consumer-summary.json. The following committed mapping is the
reviewed migration authority; implementation must refresh raw matches, not rely
on line numbers or these initial counts remaining unchanged.

Paths below are relative to each module root registered in Engine.dproject unless
the row gives a project/program/test root. Entries sharing a stage and replacement
are grouped, but include-only users are distinguished from execution callers.

| Current callers / declarations | Replacement and closure | Stage |
| --- | --- | --- |
| AssetTools `Public/AssetTools/IAssetTools.h`, `AssetToolsModule.h`, `Private/AssetTools/AssetTools.cpp`, `AssetOperations.cpp`, `PackageTools.cpp` | Implement frozen facades; split canonical resave; remove old create/duplicate/save/recover/discard shapes; migrate internal declaration users in AssetDeletion/ContentDeletionRequest | 1, then 2-4 |
| DurinEd `Public/Factories/Factory.h`, `Private/Factories/Factory.cpp`, `Private/Import/EditorReimportHandler.cpp`, `Public/DurinEd.h`; AssetForgeBuiltins StaticMesh/Texture2D/TextureCube/VolumeTexture factory headers/implementations and SceneDirectImport | Stage 1 mutable execution/renamed FactoryCreateFile and invocation lifetime; Stage 3 task/options support. Reflection list and shared PCH include closure must migrate with the base API | 1, 3 |
| ContentBrowserAssetDataSource `ContentBrowserAssetActions.cpp`, `ContentBrowserAssetSave.{h,cpp}`, `ContentBrowserAssetTransfer.cpp`, `ContentBrowserAssetDataSource.cpp` | ObjectTools naming/duplicate, editor save facades, RenameAssets and delete model; remove provider-owned package orchestration | 1, 2, 4 |
| MaterialEditor `Private/MaterialEditorModule.cpp`; LevelEditor `Private/LevelEditorModule.cpp` | CreateAsset argument roles and scoped feedback. Failed-construction cleanup becomes owner-local/PackageTools unload; user Discard stays native reload. Direct SavePackage remains native where physical evidence is required | 1 |
| TextureEditor `Private/Import/TextureFileImport.{h,cpp}`; StaticMeshEditor `Private/Import/StaticMeshImportDialog.cpp` | Save facade first, then tasks/options and result ownership. Single-file API remains only a UI/feature helper over shared execution | 1, 3 |
| RoadWeaverEditor `Private/Dialogs/RoadNetCreateDialog.{h,cpp}` | CreateAsset, flags/feedback migration, explicit safe cleanup of failed newly created package; preserve existing native save evidence | 1 |
| `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp` | Material creation branches at current lines 1171/1319 and cleanup at 1180/1187 migrate; retain headless operation, typed exit/failure handling and no MainFrame dependency | 1 |
| LevelEditor `Private/Assets/EditorAssetMoveCoordinator.{h,cpp}`; MainFrame `Private/Panels/ConsolePanel.cpp`, `Private/MainFrameModule.cpp`; Browser `Public/ContentBrowser/ContentBrowserContracts.h`, `Private/Panels/ContentBrowserPanel.cpp` | Object-based rename, void fix-up dispatch and host bindings; action handled status differs from write success | 2 |
| DurinEd `Private/Documents/EditorDocumentManager.cpp`, corresponding public header, `Private/Subsystems/AssetEditorLifecycle.cpp`; LevelEditor move coordinator | Existing IAssetMoveObserver assumes package prefix replacement. Migrate to lower-layer old/new object identity notifications, preserving subobject suffixes, recent assets and pending/open document keys | 2 |
| Engine `AssetMutationRegistry.cpp`, `AssetRelocationExtensions.cpp`, `Public/Asset/MutationExtensions.h`; LevelEditor ProjectDefaultLevelReferenceStore and MaterialEditor MaterialEditorProfileSettings | Native identity notifications and shared reference-store capture/rewrite remain Engine-owned; preserve callback lifetime and external configuration paths; remove package-only public editor workflow mapping after callers migrate | 2 |
| MaterialEditor MaterialEditorModule and MaterialEditorProfileSettings; Texture import, StaticMesh import | Existing relocation/deletion companion registrations and material profile external stores are extension consumers, not obsolete mutation commands; retain/split capability headers | 2, 4 |
| AssetForgeBuiltins `GltfAssetImportDialog.{h,cpp}`, `SceneDirectImport.cpp`, `Public/AssetForge/Builtins/AssetImport.h`; LevelEditor `Private/Assets/SceneImportDialog.{h,cpp}` | Task-owned execution; feature preview controller may retain decoded snapshots without publishing imports through another API | 3 |
| ContentBrowserData `Public/ContentBrowserDataSource.h`, `ContentBrowserDataSubsystem.h`, `ContentBrowserDeleteRequestLease.h`, corresponding source files; both provider headers/implementations; FileTransfer/FileUpdates; Browser Panel header/implementation | Remove context/preview/validation/lease protocol and restore single/bulk UE deletion; retain File ownership and private native evidence | 4 |
| AssetTools all ContentDeletion headers/implementations plus AssetDeletion, AssetDeletionInternal, AssetDeletionExtensions | Split retained contributor/companion capabilities from private native executor; public ObjectTools/model and UE DeleteItems data replace workflow protocol | 4 |
| NativeTestSupport `Public/NativeDObjectTestSupport.h`; 59 Engine Stage 1 lexical test/fixture files before factory expansion | Replace exported package-leaf helpers in shared fixtures first, then direct factory/Browser/package/editor tests. A common fixture hit does not require rewriting every unrelated test body | 1, 3 |
| Engine tests: FactoryTests; ContentBrowserDataTests/ModelTests/OperationFixtures/PanelExtensionTests; PackageTests; AssetEditorFamily/Lifecycle; AssetPackageReload; ProjectDefaultLevelReferenceStore; MaterialFunction/GraphInteraction and texture/mesh import fixture consumers | Migrate old contract assertions in owning stage; preserve native fault-injection/lifetime coverage. Exact per-file matches remain in consumer-hits.tsv | 1-4 |
| RoadWeaver `Tests/Native/Private/RoadSceneIntegrationTests.cpp` | Fixture helper dependency and native SavePackage use; validate RoadSceneIntegrationTests after Stage 1 | 1 |
| Sandbox source and `Tests/Native`; RoadWeaver runtime source | No obsolete operation API match. Still include these projects in final build; do not invent migration edits for unchanged native/runtime users | 5 build |
| DurinMCP/DurinPython, Tools/Templates/CMake | No direct obsolete command consumer found. Generic tool routing remains; check reflection/export and compile consumers when new types arrive | 1-5 audit |
| ContentBrowser `Private/ContentBrowserTool.cpp`, MainFrameModule IAssetTools include | Include-only hits: remove unnecessary includes as owning stage changes dependencies; not extra command execution paths | 1-2 |
| Engine `Private/Asset/AssetRuntime.cpp` UnloadPackages diagnostic string; DurinEd document Save/Discard; AssetMaintenance canonical save; native Runtime/primary-asset tests | Classify as native/diagnostic-only, retain correct mechanisms. RecoverPackages has only service declaration/implementation, no execution consumer; remove wrapper without inventing call migrations | 1 audit |

Current dependency edges were read from every scanned dmodule. AssetTools publicly
depends on Core/CoreDObject/Engine/AssetRegistry and privately on
DurinEd/AssetMaintenance. DurinEd depends on Engine/AssetRegistry/Projects and
render/UI substrate, not AssetTools. AssetForgeBuiltins depends on DurinEd/Engine
and privately on AssetTools/ContentBrowser/MeshBuilder. Browser providers privately
depend on AssetTools/Engine/AssetRegistry and publicly on ContentBrowserData;
ContentBrowserData depends on DurinEd/Core/CoreDObject. MainFrame, feature editors
and RoadWeaverEditor are upper-layer tool callers.

Resolve the three potential cycles without new DLLs:

1. Put task data and factory-facing task access in DurinEd; task completion uses
   owned values/private callbacks, not AssetTools classes. AssetTools registers
   dispatch and AssetForgeBuiltins supplies adapters; no reverse link.
2. Keep OnAssetPostRename in AssetTools for UE-shaped upper-layer consumers.
   Add AssetRegistry's UE-shaped OnAssetRenamed delegate, using
   `TMulticastDelegate<void(const FAssetData&, const FSoftObjectPath& OldObjectPath)>`
   (typed old path is the Durin text adaptation). Engine's live identity publisher
   supplies the new record after committing object identity, with no registry lock
   held during callbacks; disk reconciliation must not repeat that identity event.
   Replace IAssetMoveObserver and its package-only mapping with this event in
   DurinEd/LevelEditor. Preserve subobject suffixes. Package-only workspace keys
   change only if the whole package identity actually moved; derive that fact
   from current package residency rather than blindly replacing a prefix.
   Do not expose FAssetRenameData through Engine or AssetRegistry.
3. MainFrame registers optional prompt/sync/delete-model presentation callbacks
   into AssetTools. The services and native model live in AssetTools; MainFrame
   may include them, DurinEd must not. Keep document Save/Discard on Engine's
   native mechanism instead of routing them up through the new facades.

#### Test registry and missing-regression mapping

Registry queries `Asset`, `Import`, `ContentBrowser`, `EditorOperation`, `Startup`,
`Road`, `Sandbox`, `SlowTask`, `MaterialGraph`, and `MaterialEditorInteraction`
succeeded for `Win64-Debug-DurinEditor`. No test executable
or build was run. Existing tests are migration inputs, not evidence for the new
semantics. The mapping below covers the stage acceptance categories; cases added
in later stages must assert observable behavior and native failure boundaries.

| Gate | Registered owning/direct consumer selection | Existing basis and required new/changed coverage |
| --- | --- | --- |
| Stage 1 creation/factories/feedback | EditorOperationTests, SlowTaskTests | FactoryTests covers concrete creation, configured instance and rejected outer/class; add mutable invocation isolation, CallingContext, replacement preservation and nested scoped-feedback routing. SlowTaskTests covers the Core feedback substrate |
| Stage 1 duplicate/naming/save | ContentBrowserDataTests, ContentBrowserWorkflowTests, AssetPackageTests, EditorAssetWorkflowTests | Replace Original_Copy2/forced-clean-copy assumptions; add numeric/zero-padded names, explicit destination, Dirty-copy/GC retention, filtered empty save and projection-pending callbacks |
| Stage 1 reload/unload/document roots | AssetPackageReloadTests, EditorOperationTests, AssetEditorFamilyTests | Existing native reload and editor lifecycle basis; add PackageTools interaction modes, pending-retirement drain, dirty-history and protected-owner behavior. Native Vulkan qualification is not automatically required |
| Stage 1 project/program consumers | RoadSceneIntegrationTests, RoadEditorIntegrationTests; DurinAssetTool and RoadWeaverEditor consumer builds | RoadScene fixture migration is direct; add/focus RoadNet creation/cleanup behavior where current integration lacks it. Compile the headless program branch; no MainFrame link dependency |
| Stage 2 identity/multi-asset/unsaved | ContentBrowserWorkflowTests, AssetPackageTests, EditorOperationTests, AssetEditorFamilyTests | Replace PackageRenameReturnsRelocatedAssetIdentity; add object-name change, selected sibling extraction, checked reparent, same/cross-mount, case-only, Dirty/new object and open/recent/pending document identity |
| Stage 2 references/redirectors/failure | AssetPackageTests, AssetReferenceStoreTests, PrimaryAssetTests, MaterialFunctionTests, MaterialEditorInteractionTests | Existing relocation/fix-up failures and external stores; add renamed hard/soft paths, dirty loaded references, save failure and valid-alias retry boundaries. Keep native path-only relocation tests at native owner |
| Stage 3 standalone/task import | EditorOperationTests, TextureImportWorkflowTests, TextureTests, StaticMeshTests, AssetImportTests | Replace positional null/old batch preflight assertions; add task defaults, class/settings replacement matrix, options, successful-output lists, resubmission and managed retention |
| Stage 3 scene/async/lifetime | SceneImportTests, AssetImportTests, AssetEditorFamilyTests | Existing session source/preview/cancel basis; add multiple outputs, polling versus GetObjects wait, synchronous fallback, optional save failure, view close and adapter/module teardown. SceneImportVulkanTests only if changed resource behavior requires it |
| Stage 4 model/API/independent dispatch | ContentBrowserDataTests, ContentBrowserWorkflowTests, EditorOperationTests | Replace context identity/lease tests with UE default single/bulk dispatch, model states, count versus bool, UI/headless confirmation and independent peer rejection |
| Stage 4 safety/partial effects | ContentBrowserWorkflowTests, AssetPackageTests, MaterialFunctionTests | Existing fresh-hash/open-level/contributor/partial-removal tests remain; add model-own-reference exclusion, unused redirector-only selection, Dirty-selected/multi-asset sibling survival, cancellation and repeated-attempt protections |
| Stage 5 notifications/integration | ContentBrowserDataTests, ContentBrowserWorkflowTests, EditorAssetWorkflowTests, AssetEditorFamilyTests | Existing reentry/save notification basis; add direct tool invocation, nested prompt callbacks, provider retirement, exact object identity and exactly-once host invalidation |
| Stage 5 workspace/lifecycle | Explicit all build; LaunchStartupCharacterizationTests/FLaunchStartupCharacterizationTests.BoundedTickExitUsesNormalApplicationShutdown | One bounded shutdown case, not the whole characterization suite. SandboxGameplayTests exists; execute only if changed native inputs reach gameplay. No GPU qualification implied by shared fixture includes |

Stage 0 acceptance is source/inventory/decision verification and document checks.
Stage 1 may start without another architecture decision; it must refresh changed
inputs and fill the identified runtime regressions before its own acceptance.

### UE authorities

| Responsibility | Local UE source authority | Required comparison |
| --- | --- | --- |
| Creation, duplication, naming, rename data, import and fix-up | `Source/Developer/AssetTools/Public/IAssetTools.h`, `Private/AssetTools.cpp` | Exact argument roles, overload behavior, result types and registry/dirty publication |
| Rename and move execution | `Source/Developer/AssetTools/Private/AssetRenameManager.h`, `AssetRenameManager.cpp` | Live object identity, dirty/new objects, referencers, redirector decisions, package saves and partial failure |
| Import tasks | `Source/Editor/UnrealEd/Public/AssetImportTask.h` | Task inputs, result ownership, multi-output results and async completion |
| Object duplication and deletion entrypoints | `Source/Editor/UnrealEd/Public/ObjectTools.h`, `Private/ObjectTools.cpp` | Explicit target duplication versus naming orchestration, deletion counts and confirmation |
| Deletion analysis | `Source/Editor/UnrealEd/Public/AssetDeleteModel.h`, `Private/AssetDeleteModel.cpp` | Pending objects, disk/live references, scanning state, actions and progress |
| Package unload/reload | `Source/Editor/UnrealEd/Public/PackageTools.h`, `Private/PackageTools.cpp` | Parameter structure, dirty/history policy, interaction and lifetime |
| Editor saving | `Source/Editor/UnrealEd/Public/FileHelpers.h`, `Private/FileHelpers.cpp` | `UEditorLoadingAndSavingUtils`, `FEditorFileUtils`, dirty predicates and prompt return codes |
| Browser dispatch | `Source/Editor/ContentBrowserData/Public/ContentBrowserDataSource.h`, `Private/ContentBrowserDataSource.cpp` | Single/bulk signatures, defaults and aggregate semantics |
| Asset Browser adapters | `Plugins/Editor/ContentBrowser/ContentBrowserAssetDataSource/Source/ContentBrowserAssetDataSource/Private/ContentBrowserAssetDataCore.cpp`, `ContentBrowserAssetDataSource.cpp` | Delegation to AssetTools/ObjectTools and item update production |

Source inspection, not API-name similarity, is the authority. Stage 0 recorded
supported overloads and semantic differences with file/symbol references; later
stages update that record if decisions change. Do not paste UE implementation
bodies into the plan or copy unsupported subsystems into Durin.

### Target ownership

| Surface | Owner and selected responsibility |
| --- | --- |
| `IAssetTools` | Create, explicit-target duplicate, unique-name generation, import/task dispatch, rename and redirector fix-up |
| `DPackageTools` | Editor package unload and reload policy over Engine mechanisms |
| `DEditorLoadingAndSavingUtils` | UI-neutral package/object saving entrypoints |
| `FEditorFileUtils` | Interactive saving, dirty collection and prompt outcomes |
| `ObjectTools` | Batch object duplication and asset/object deletion orchestration |
| `FAssetDeleteModel`, `FPendingDelete` | One deletion analysis/action model; observable reference facts, progress and actual deleted-object count |
| Browser Asset/File providers | Resolve source-owned selections, validate capabilities and adapt tools to item outputs/updates; ordinary files remain File-owned |
| CoreDObject / Engine / AssetRegistry | Object/package primitives, residency, physical persistence and immutable discovery/live-query publication |

These surfaces do not require new DLLs. AssetTools currently depends on DurinEd
for factories; therefore put `ObjectTools` and `FAssetDeleteModel` in AssetTools,
alongside `DPackageTools` and the two editor save facades. DurinEd remains the
lower editor substrate and supplies factory, host and presentation bindings.
Do not create a reverse DurinEd-to-AssetTools link cycle. The Stage 0 decisions
above resolve lower-layer document, task and identity consumers explicitly.
Keep AssetForgeBuiltins behind registered factory/import adapters rather than
making AssetTools depend on concrete built-in import implementations.

### Target interfaces and data

The abbreviated declarations below summarize argument roles; the Stage 0 freeze
above owns supported types, defaults, overloads and ownership. Add the owning
module export macros to concrete/standalone declarations and reflected classes;
IAssetTools virtual members dispatch through its exported service. `PackagePath`
in Create/Duplicate is a destination directory,
not a full package identity. Public destination paths remain logical paths.

```cpp
DObject* CreateAsset(AssetName, PackagePath, AssetClass, Factory, CallingContext);
DObject* DuplicateAsset(AssetName, PackagePath, OriginalObject);
void CreateUniqueAssetName(BasePackageName, Suffix, OutPackageName, OutAssetName);
bool RenameAssets(std::span<const FAssetRenameData> AssetsAndNames);
std::vector<DObject*> ImportAssets(Files, DestinationPath, ChosenFactory, ...);
void ImportAssetTasks(std::span<DAssetImportTask* const> Tasks);
void FixupReferencers(RedirectorObjects, ..., ERedirectFixupMode Mode);
```

- `FAssetRenameData`: weak `Asset`, directory `NewPackagePath`, `NewName`,
  `OldObjectPath`, `NewObjectPath`, and `bOnlyFixSoftReferences`. Preserve typed
  `FSoftObjectPath` and UE constructor roles. Normal asset rename produces
  `Directory/NewName.NewName`; explicit path forms cover supported nonstandard
  identities. Do not expose `bAlsoRenameLocalizedVariants` without localization
  support. Explicitly document that omission.
- `EAssetRenameResult` and dialog/deferred rename are omitted in this increment.
  UE's Pending means outstanding discovery/work, never cancellation; a future
  overload must implement that behavior before exposing the enum.
- `ERedirectFixupMode`: use UE mode names. Implement delete/leave behavior;
  expose `PromptForDeletingRedirectors` only with an actual host prompt and
  headless policy. The public fix-up return shape follows the selected UE
  overload; native typed failure/effect facts remain available internally.
- `DAssetImportTask`: `Filename`, `DestinationPath`, `DestinationName`, mutable
  `Factory`, `Options`, `bReplaceExisting`, `bReplaceExistingSettings`,
  `bAutomated`, `bSave`, `bAsync`, `ImportedObjectPaths`, `GetObjects()`, and
  `IsAsyncImportComplete()`. Use reflected/managed strong references for objects,
  options and factories. Keep result storage private; do not copy deprecated
  public `Result` or UE-specific Interchange result handles.
- `FPendingDelete`: pending object identity and retained lifetime, disk/live
  reference facts, remaining external-reference counts and visibility state.
  Use Durin reference inspection; do not duplicate the collector's graph.
- `FAssetDeleteModel`: supported UE scanning states (`Waiting`,
  `StartScanning`, `Scanning`, `UpdateActions`, `Finished`), pending collection,
  `Tick`, `GetState`, `GetProgress`, `CanDelete`, `DoDelete`,
  `GetDeletedObjectCount`, and `OnStateChanged`. Async analysis means bounded
  owner-thread advancement, not unsafe worker access to managed objects.
- `DPackageTools::FUnloadPackageParams` retains UE-shaped options and supported
  overloads. Reload moves behind `ReloadPackages`; Engine's atomic reload
  request/result remains a lower-level mechanism rather than an AssetTools job.
- Save uses `SavePackages(Packages, bOnlyDirty)` and supported UE-shaped
  `FEditorFileUtils` methods. Interactive prompt results distinguish success,
  failure, cancellation and decline according to the local enum. Do not collapse
  these into Browser OR-success or duplicate package-writer state in diagnostics.

### Behavior contracts to replace

1. Create/duplicate construct live Dirty assets and publish live discovery once.
   Duplicate takes an explicit target; ObjectTools/Browser choose unique names
   separately. Duplicate does not inherently save. Browser batch-copy save
   policy must follow the inspected UE workflow, including supported host/source
   control conditions, rather than unconditionally saving every new copy.
2. Rename/move acts on objects and their packages, including unsaved and Dirty
   objects. Move preserves the asset name; rename changes the asset and package
   identity. Preserve object ownership and live references without discarding
   unsaved edits. Handle case-only names, collisions, subobjects and multi-asset
   packages explicitly; never silently relocate sibling assets.
3. Rename considers loaded soft references, persistent referencers, package
   writeability and save policy, and decides when a redirector is required.
   Replace the current universal rule that only Fix Up may canonicalize paths.
   Keep unaffected packages untouched and preserve resolvable aliases after
   partial failure. Fix Up still verifies references before alias removal.
4. No global all-provider preflight, rollback or atomicity is promised. Within a
   native operation, retain staging necessary for package correctness, but define
   its actual failure/continuation boundary from the corresponding UE workflow.
   A multi-item bool is not automatically OR-success: preserve each API's UE
   meaning. Browser aggregation, rename result, save success and deletion count
   have separate contracts. Creating an incidental directory alone does not
   establish successful transfer of an asset request.
5. Import can produce multiple objects per file/task. `GetObjects()` may wait for
   async completion as UE documents; interactive callers poll completion before
   reading. A request for async import may complete synchronously when the format
   lacks async support. Save failure preserves imported live assets. Publish
   imported paths for actual outputs, including supported replacement outputs.
6. Deletion exposes UE's ordinary `DeleteItem/BulkDeleteItems` and ObjectTools
   entrypoints. UI confirmation lives inside the orchestration/model workflow;
   a public owner-bound Browser deletion capability is no longer required.
   `bShowConfirmation=false` suppresses UI, not reference, scope or file checks.
   File/asset partitions execute independently and retain earlier effects.
7. Retain mount/reparse/no-overwrite checks, companion ownership, external stores,
   open-document/live-reference protection and partial-I/O evidence. Freeze the
   user's selection internally for each confirmed destructive attempt, revalidate
   relevant changes before I/O, and reconfirm changed scope in interactive flows.
   Headless flows reject changed scope. Internal capabilities are single-use.
   Do not introduce force deletion, force package release or deletion Undo.
8. Registry/object lifecycle events are authoritative for asset rows; File owns
   ordinary-file/directory discovery and changes. Tools publish observed native
   effects independently of success booleans. Define exactly one owner for host
   mutation/history invalidation and prevent duplicated deltas or revision bumps.
   Failed/no-effect actions emit no optimistic changes; committed content with
   delayed Registry projection remains observable and recoverable.

Durin-specific physical writer reports, DAST/bulk companions, transactional file
staging, exact inspection and external reference-store extensions remain native
mechanisms. They must not become another generic public operation envelope.
The package format and current typed identity substrate are unchanged.

## Migration And Removal Ledger

Temporary adapters are allowed only within a stage to keep intermediate builds
usable. Migrate all callers and delete the replaced public surface before closing
that stage. Do not retain aliases, deprecated production overloads, exported
test shortcuts or adapters solely to keep old tests unchanged. Private native
mechanisms may survive under explicit ownership and names.

| Current surface | Destination | Removal gate |
| --- | --- | --- |
| Full-path `CreateAsset` overload and `CreatePackageLeafAssetForTesting` | UE-shaped creation; test fixture helpers | Stage 1: all callers migrated, no production test convenience method |
| `FAssetDuplicateRequest` and request-based `DuplicateAsset` | Explicit-target Duplicate plus `CreateUniqueAssetName`/ObjectTools | Stage 1: automatic/exact naming callers separated; remove obsolete header |
| `IAssetTools::SaveAssets`, `FAssetSaveRequest`, `EAssetSaveMode` | Editor save facades; canonical resave remains AssetMaintenance | Stage 1: save consumers and persistence callbacks migrated; remove old save header |
| `IAssetTools::RecoverPackages` and discard policy methods | PackageTools reload/editor discard policy or private construction cleanup | Stage 1: preserve user discard versus failed-construction semantics; remove old service methods |
| `FAssetRelocationRequest`, `FAssetRelocation`, `IAssetTools::RelocateAssets` | `FAssetRenameData`, `RenameAssets`, private rename orchestration | Stage 2: move coordinators and Browser migrated; remove redundant request structs |
| Public native `RelocateAssets`, `FAssetRelocationMapping`, IAssetMoveObserver and registration/broadcast APIs | Private relocation helper plus Registry OnAssetRenamed and AssetTools OnAssetPostRename | Stage 2: migrate documents, recent assets, LevelEditor and native tests, then remove package-only observer/mapping protocol; no editor command bypass |
| `FAssetRedirectorFixupRequest`, `IAssetTools::FixUpRedirectors`, duplicate fix-up modes | `FixupReferencers`, `ERedirectFixupMode` | Stage 2: migrate callers and internal reports; remove obsolete public workflow headers |
| `FAssetImportRequest`, `FAssetImportBatchRequest`, positional null-result batch, single-output `ImportAsset` service | `DAssetImportTask`, UE-shaped `ImportAssets`/`ImportAssetTasks` | Closed in Stage 3: task/file-list callers migrated; old request/header and positional-null protocols removed |
| `ImportPackageLeafAssetForTesting` | Test fixtures over supported import APIs | Closed in Stage 3: fixtures use tasks; no exported test convenience method |
| Public `FAssetImportSession` / `ImportAssetOutputs` workflow exposure | Task-backed pipeline adapters; format-preview controller where needed | Closed in Stage 3: dialogs/headless callers submit tasks; feature preview has no public execution entrypoint |
| `IAssetTools::PrepareDeletion`, public `FAssetDeletionOperation`/commit protocol | ObjectTools plus delete model and private native executor | Stage 4: all callers migrated; keep companion extension capabilities separately |
| Public content deletion Request/Plan/Operation/Result protocol | Model-owned internal selection/execution and File-specific facts | Stage 4: move needed native records private; delete exported workflow headers |
| Public ContentBrowserDeleteRequestLease helper | Private model/executor reentry protection | Stage 4: delete header and all Data/Asset/File lease uses with the context protocol |
| `PrepareDeleteItems`, context-taking `BulkDeleteItems`, `FContentBrowserDeleteContext`, `IContentBrowserDeleteRequest`, preview and validation protocol | UE-shaped `DeleteItem/BulkDeleteItems`; internal confirmation presentation | Stage 4: service, Item, providers, panels and extensions migrated together; increment source feature version |
| Browser asset transfer/save/recovery orchestration | AssetTools/ObjectTools/save services | Stages 1-4: provider adapters retain item resolution, capability and projection only |
| `FAssetMutationResultDetails` public workflow envelope | Private native evidence; distinct package-mutation characterization snapshot | Closed in Stage 5: public MutationTypes header removed; no Browser/public command dependency; writer evidence retained at I/O owner |
| Const factory execution, FactoryCreateFromFile, discovery-CDO execution assumptions | Mutable FactoryCreateNew/FactoryCreateFile, transient invocation factories and scoped feedback | Stage 1: base and all built-in/test overrides and callers migrated; keep read-only discovery metadata |
| Trailing public creation Flags/Context/Warn and DefaultAssetCreationFlags service exposure | Fixed UE creation flags, FName CallingContext and lexical feedback routing | Stage 1: project/program/feature consumers migrated; private primitive flags remain available where owned |

The Stage 0 inventory above expands this ledger for overloads, bindings, provider
exports, fixtures and external-store callbacks. Deletion contribution registration,
companion queries, native reload and write reports are not removed merely because
their headers currently share a workflow name. Split those capabilities first.

## Implementation Stages

### Stage 0: Freeze the UE contract and workspace migration inventory

Dependency: none. Outcome: an executable, symbol-level mapping with no unresolved
ownership, path or result-semantic decision blocking Stage 1.

- [x] Recheck baseline, reference Build.version and applicable module guidance.
- [x] Inspect listed UE declarations and implementation branches; freeze exact
      supported signatures, fields, constructors, defaults, overloads and enums.
      Record deviations for localization, source control, dialogs and Interchange.
- [x] Resolve factory mutability, creation flags versus CallingContext, supported
      replacement policy, multi-asset package rename, deferred/dialog rename,
      save prompt enum, and headless behavior. Do not add unsupported switches.
- [x] Search each changed symbol across source and test roots of every project
      declared in `Durin.dworkspace`, plus bindings, module declarations and
      generated-code inputs. Inventory caller -> replacement -> owning stage.
- [x] Include AssetTools, DurinEd, AssetForgeBuiltins, AssetMaintenance, both native
      Browser providers, ContentBrowserData/ContentBrowser, MainFrame, LevelEditor,
      asset editors, Engine/AssetRegistry/CoreDObject and found project consumers.
- [x] Record the module dependency graph and UI-neutral host bindings; resolve
      cycles before selecting header/class ownership.
- [x] Query the test registry for actual targets; map each semantic gate below
      to existing coverage or a missing regression. Store raw inventories under
      ignored `Build/UE58AssetOperationsAlignment/` and concise decisions here.

Acceptance: complete replacement/removal ledger and consumer inventory; all
unsupported UE features have explicit dispositions; no implementation checkbox
is closed from source inspection alone.

Accepted 2026-11-16: all seven audit tasks are complete. Read-only registry queries
and source/inventory verification are recorded above. Stage 0 modifies this plan
only; native builds/tests remain unrun and their gates remain in Stages 1-5.

### Stage 1: Separate package policy and align creation and duplication

Dependency: Stage 0. Outcome: a smaller IAssetTools and working shared save/reload
services; no old creation, duplicate or save entrypoint remains.

- [x] Add editor save facades and complete PackageTools reload policy; preserve
      unload options, history behavior, atomic reload and document root migration.
- [x] Separate ordinary saving from AssetMaintenance canonical-resave workflows;
      preserve physical persistence callbacks and delayed Registry diagnostics.
- [x] Implement UE-shaped CreateAsset, DuplicateAsset and CreateUniqueAssetName;
      move naming/batch duplicate orchestration to ObjectTools. Migrate mutable
      factory entrypoints/all overrides, transient invocation lifetime, safe
      creation replacement and Core's nested scoped-feedback routing together.
- [x] Migrate factory users, Browser duplicate/save, Texture/StaticMesh import
      saves, editor documents, tools and all inventoried project/test callers.
- [x] Remove Stage 1 ledger surfaces, test-only service helpers and unused headers.
      Document public creation/duplicate lifetime and save result semantics.

Acceptance: explicit-name collision and unique-name tests; clone internal reference
remapping and external reference preservation; live-query visibility once; Dirty
copy retained after save failure; mixed clean/dirty and partial save behavior;
committed-file/projection-pending handling; reload/discard preserve intended live
roots; unload rejects protected objects. Owning and direct consumer tests/builds
pass, including any project caller discovered in Stage 0.

Accepted 2026-11-16. Creation, explicit duplication, numeric unique naming,
mutable FactoryCreateNew/FactoryCreateFile, transient discovery invocations and
nested thread-local feedback are implemented. FileHelpers owns ordinary saves,
PackageTools owns reload/unload, and ObjectTools owns batch duplicate naming.
Browser, all four built-in factories, Texture/StaticMesh imports, Level/Material
editors, MainFrame, DurinAssetTool, RoadWeaverEditor and native fixtures use the
new contracts. Document Save/Discard stays on native Engine mechanisms to avoid
a DurinEd -> AssetTools dependency cycle. Lasting contracts are in
[Editor Asset Tools](../Editor/Architecture/Assets/AssetTools.md).

The Stage 1 removal ledger is closed: full-path creation and its service test
helper, request-based duplicate/AssetDuplicate.h, SaveAssets/AssetSave.h and its
request/mode, service RecoverPackages/DiscardPackage, old factory execution and
public creation flags/context/warn tails are removed. ImportPackageLeafAssetForTesting
and standalone import requests remain for Stage 3; the Engine-native free test
fixture helper is not an IAssetTools service. Rename, deletion and native typed
write/reload mechanisms remain at their assigned stages/owners.

Implementation decisions: Browser ordinary Save uses IncludeClean policy;
canonical resave remains explicit AssetMaintenance Plan/Apply, with no second
canonical-save facade. No source-control provider exists, so duplicates return
Dirty and unsaved. MainFrame binds native batch save/reload confirmation and a
duplicate destination folder chooser; missing interactive hosts cancel. The
current host does not provide a per-package retry dialog: failed packages remain
available for ordinary Save retry. UE checkout/explicit-save intent fields are
retained, but no checkout or external-world save preparation is claimed.

Validation used the Windows default `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset, 18 build jobs, serial target execution through
DevTool, default native-test settings. The tested baseline above plus all Stage 1
source/fixture changes is the commit input; later documentation edits do not
change runtime evidence. Browser and dialog results below were rerun after their
last relevant source changes. The final default-factory cleanup removes unused
diagnostic locals from its null-returning implementations; EditorOperationTests
was rerun, while the other results remain applicable because no signatures,
outputs, fixtures or runtime settings changed. Affected-selection expansion from Core/shared
fixtures was inspected in `Build/UE58AssetOperationsAlignment/stage1-affected-explain.txt`;
named owning/direct-consumer targets cover this stage without implicit GPU or
application-host execution. Raw receipts are under `Build/.agent-state/logs/`:

| Target | Result | Receipt basename (20261116-) |
| --- | --- | --- |
| SlowTaskTests | 16 passed | 014544-166050-17232-SlowTaskTests.log |
| FileDialogContractTests | 2 passed | 015833-052664-37328-FileDialogContractTests.log |
| EditorOperationTests | 372 passed | 020148-513938-41768-EditorOperationTests.log |
| ContentBrowserDataTests | 59 passed | 015804-499607-40816-ContentBrowserDataTests.log |
| ContentBrowserWorkflowTests | 188 passed, 2 skipped | 015821-401038-39760-ContentBrowserWorkflowTests.log |
| TextureImportWorkflowTests | 27 passed | 014631-794158-25672-TextureImportWorkflowTests.log |
| AssetPackageReloadTests | 22 passed | 014813-295035-41928-AssetPackageReloadTests.log |
| AssetEditorFamilyTests | 10 passed | 014900-836630-32536-AssetEditorFamilyTests.log |
| AssetPackageTests | 251 passed | 014922-182513-25056-AssetPackageTests.log |
| StaticMeshTests | 201 passed | 014948-147644-30468-StaticMeshTests.log |
| AssetImportTests | 22 passed | 015000-048224-40004-AssetImportTests.log |
| SceneImportTests | 34 passed | 015008-299583-9940-SceneImportTests.log |
| EditorAssetWorkflowTests / MainFrame | 44 passed, compiled | 015335-147395-16732-EditorAssetWorkflowTests.log |
| RoadSceneIntegrationTests | 5 passed | 015350-660040-48272-RoadSceneIntegrationTests.log |
| RoadEditorIntegrationTests | 8 passed | 015405-137030-28316-RoadEditorIntegrationTests.log |
| TextureTests | 151 passed | 015842-625832-2668-TextureTests.log |
| TextureThumbnailTests | 11 passed | 015901-221485-45096-TextureThumbnailTests.log |
| MaterialFunctionTests | 30 passed | 015912-621973-33904-MaterialFunctionTests.log |
| DurinAssetTool build | passed | 015942-847952-44292-cmake.log |
| RoadWeaverEditor build | passed | 015948-953734-34912-cmake.log |
| Sandbox build | passed | 015952-682706-47340-cmake.log |

The two Browser skips are existing directory/file symlink cases requiring a
Windows privilege unavailable on this host. No Stage 1 test failed. New regressions
cover configured/default factory isolation and CallingContext, failed/protected
and saved overwrite with sibling preservation, explicit collision/numeric naming,
Dirty duplicate save retry, save filtering and distinct prompt outcomes, nested
save observers including projection-pending persistence, and reload interaction
modes. Existing duplication, Browser publication, native reload and editor-family
coverage checks graph references, live ownership and unload rejection.

A workspace scan of Engine source/tests, Sandbox, RoadWeaver, Tools and Templates
finds zero removed Stage 1 symbols/headers; the exact query/result is retained in
`Build/UE58AssetOperationsAlignment/stage1-old-interface-audit.txt`. Stage 1 does
not close later-stage migration, final all-build/native-suite/startup gates, or
cross-platform validation; the macOS native prompt branch is unexecuted here.
Changed-document validation passed for 10 files, all-plan validation passed for
11 active, 8 completed and 457 archived plans, and `git diff --check` passed.

### Stage 2: Implement object-based rename, move and redirector workflows

Dependency: Stage 1. Outcome: RenameAssets is the public execution path for asset
rename/move and does not require saving Dirty assets as a universal precondition.

- [x] Implement FAssetRenameData constructors, normalization and supported
      overloads; add a private rename manager with explicit per-item failure state.
- [x] Implement live-object/package identity updates, package reassignment where
      needed, reference handling, redirector creation and package save decisions
      from the local UE implementation. Retain unsaved authored values. Add
      Registry OnAssetRenamed and migrate lower-layer package-only observers.
- [x] Support normal rename, directory move, explicit object paths and case-only
      changes. Handle multi-asset packages without silently moving siblings;
      migrate identity consumers and observers to actual old/new object paths.
- [x] Implement FixupReferencers and supported ERedirectFixupMode values;
      preserve external-store validation, payload closure and recovery evidence.
- [x] Migrate LevelEditor's move coordinator, Browser transfer/rename, redirector
      commands, primary-asset observers, native tooling and all test callers.
- [x] Remove Stage 2 public relocation/fix-up requests and bypasses. Keep required
      native byte transformations private and migrate native tests to their owner.
- [x] Replace contract claims that relocation preserves all asset names or that
      only Fix Up can rewrite references, where those claims describe editor rename.

Acceptance: saved/unloaded, resident clean, resident Dirty and unsaved assets;
ordinary and explicit-path rename; multi-asset package and subobject identity;
case-only rename; same/cross-writable-mount moves; occupied/read-only destinations;
live strong references and loaded/unloaded hard/soft referencers; external stores;
owned payloads; redirector necessity, fix-up keep/delete and reopenability.
Inject failures at destination, reference, payload and Registry publication:
earlier successful items remain usable, unresolved aliases remain valid, and no
retry duplicates committed work or loses unsaved edits. Replace old package-only
Browser rename expectations with the selected asset-identity contract.

Stage 2 acceptance evidence (2026-11-16):

The local UE 5.8.2 source baseline (CL 56702186, compatible CL 55116800) was
rechecked for FAssetRenameData, rename-manager reference/save ordering and
FixupReferencers. The public object-based APIs now replace relocation/fix-up
requests and the package-only observer protocol. Private native byte transforms
remain available only through their owner and explicit MutationTesting helpers.
Registry identity events drive documents, recent assets, LevelEditor, MaterialEditor
preferences and primary-asset records. Browser uses exact selected object paths,
including selected redirectors; moving one export retains siblings.

The manager retains live values through rename and package reassignment. It
prepares payload copies, revalidates source edits/bytes/mounts after callbacks,
saves destination before referencers and retains aliases on persistence failure.
Retries of committed identities save pending destinations/aliases without repeating
identity events. Soft reference repair preserves subobject suffixes. Exact paths
are also used by native alias rewrites. Individual alias removal checks persistent
incoming references and native/live owners; whole selected unambiguous alias
packages retain staged closure, external-store validation and recovery backups.
Package-valued external stores cannot identify one export in a mixed package:
retain the alias rather than rewrite an ambiguous occurrence. Checkout flags have
no source-control provider work in this branch. These supported adaptations are
recorded in the owning AssetTools contract.

Validation used windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC, 18 build jobs,
against the baseline above plus these implementation changes. Named owner and
consumer targets were selected after `test affected --explain`: public module
headers/shared fixtures expand the automatic selection beyond the changed
behavior. The build graph is unchanged; the explicit CoreDObject,
AssetRegistry/Engine, editor-family, package/cook and workspace-project coverage
below covers the affected consumers. Final all-build/bounded-startup/selected-regression gates
remain assigned to Stage 5; GPU, application-hosted and cross-platform execution
are not claimed here.

Receipts are preserved under ignored
`Build/UE58AssetOperationsAlignment/stage2-receipts/`; names below share the
`20261116-` prefix. Source inventory and obsolete-interface scan are under
`Build/UE58AssetOperationsAlignment/`. Earlier receipts are reused for unchanged data-query and family fixtures and
project integrations; final fix-up selection changes leave their exercised rename
paths unchanged. Manager/Browser/MainFrame, package/reference-store/cook, primary
asset, material and object-runtime suites cover the final sources. Tool/project
builds were also repeated. Seven suites were rerun after older raw logs expired;
all listed receipts are preserved in the stage directory.

| Selection | Result | Receipt suffix |
| --- | --- | --- |
| EditorOperationTests | 388 passed | 034241-177921-24496-EditorOperationTests.log |
| ContentBrowserWorkflowTests | 190 passed, 2 skipped | 034259-021146-30116-ContentBrowserWorkflowTests.log |
| EditorAssetWorkflowTests / MainFrame | 44 passed, compiled | 034325-248942-10532-EditorAssetWorkflowTests.log |
| ContentBrowserDataTests | 59 passed | 030725-739379-34432-ContentBrowserDataTests.log |
| AssetEditorFamilyTests | 10 passed | 030733-420381-21120-AssetEditorFamilyTests.log |
| AssetPackageTests | 251 passed | 034849-038888-3272-AssetPackageTests.log |
| AssetReferenceStoreTests | 15 passed | 034907-521850-14244-AssetReferenceStoreTests.log |
| PrimaryAssetTests | 29 passed | 034909-700540-27000-PrimaryAssetTests.log |
| MaterialFunctionTests | 30 passed | 034912-705281-26940-MaterialFunctionTests.log |
| MaterialEditorInteractionTests | 67 passed | 034917-766727-23180-MaterialEditorInteractionTests.log |
| CoreObjectTests | 181 passed | 034930-119081-47216-CoreObjectTests.log |
| AssetCookTests | 38 passed | 034944-166308-9688-AssetCookTests.log |
| TextureTests | 151 passed | 031118-964789-32612-TextureTests.log |
| TextureThumbnailTests | 11 passed | 031134-561785-38272-TextureThumbnailTests.log |
| StaticMeshTests | 201 passed | 031146-627193-12464-StaticMeshTests.log |
| RoadSceneIntegrationTests | 5 passed | 031204-399588-8280-RoadSceneIntegrationTests.log |
| RoadEditorIntegrationTests | 8 passed | 031211-237478-23328-RoadEditorIntegrationTests.log |
| DurinAssetTool build | passed | 034629-761729-17240-cmake.log |
| RoadWeaverEditor build | passed | 034631-078032-26892-cmake.log |
| Sandbox build | passed | 034633-242129-10452-cmake.log |

Total: 1,678 passed, two existing Windows symlink-privilege skips, no failures.
Sixteen focused rename/fix-up regressions and two Browser end-to-end cases cover
dirty/new/saved-unloaded identities, explicit and case-only names (including the
physical filename), selected sibling preservation, hard/soft/subobject references,
read-only persistent references, writable mounts, occupied targets, mixed and
multi-destination aliases, keep/delete, dirty referencer saves, payload preparation,
external source edits, recursive diagnostics and projection/save failure retries.
Existing native staging/reference-store/payload fault tests remain covered.
The Engine, Sandbox and RoadWeaver source/test roots plus Tools/Templates have no
removed Stage 2 symbols or obsolete public headers. Changed-document/all-plan
validation and `git diff --check` complete the documentation acceptance checks.

### Stage 3: Align import tasks and converge all import execution

Dependency: Stage 1; Stage 2 for replacement/identity interactions.
Outcome: file-list and task imports share one pipeline with UE-shaped task data.

- [x] Add reflected DAssetImportTask and standalone ImportAssetTasks with managed
      factory/options/results, synchronous completion, replacement settings and
      optional save. Migrate Texture/StaticMesh UI admission to tasks.
- [x] Add the file-list import entrypoint and extend completion ownership to
      registered multi-output scene adapters, including actual async waiting.
- [x] Adapt standalone factories and feature-owned multi-output sessions through
      registered adapters. Preserve detached build preparation and owner-thread
      publication; no AssetTools-to-AssetForgeBuiltins dependency.
- [x] Migrate glTF/FBX preview/progress controllers, Texture/StaticMesh import,
      automated/script/headless callers, reimport integration where affected,
      tools and native tests. Keep preview reuse without a parallel import API.
- [x] Preserve underlying cancellation/drain semantics behind task/session owners;
      do not expose a new generic cancellation ledger or fake pending results.
- [x] Remove Stage 3 ledger APIs, positional result assumptions and unused headers.

Historical standalone increment accepted on 2026-11-16: `DAssetImportTask` is reflected in DurinEd and
`ImportAssetTasks` runs standalone factories through the existing prepared-package
publication boundary. Tasks retain inputs/results, reject recursive submission,
clear completed-run results on reuse, and preserve live results independently
from requested save success. Built-in factory configuration retains existing
settings unless replacement settings are selected. At that increment, scene task
dispatch, asynchronous waiting, the file-list overload and API removal were still
open; the final acceptance below supersedes those remaining gates.

Standalone increment validation (2026-11-16): tested `440e84730787bbe8c301b8eb69bbe8008d767a05`
plus this implementation working tree, windows-msvc-x64 /
Win64-Debug-DurinEditor, MSVC 14.44, 14 build jobs. Source/test/module SHA-256
inputs, six raw passing receipts and the refreshed lexical consumer inventory
are retained under ignored `Build/UE58AssetOperationsAlignment/`;
`stage3-ue-reference-manifest.json` records the current UE reference identity.
Receipt names below share the `20261116-` prefix and are copied into
`stage3-receipts/`.

| Selection | Result | Receipt suffix |
| --- | --- | --- |
| EditorOperationTests | 393 passed | 132141-987956-8348-EditorOperationTests.log |
| TextureTests | 152 passed | 131559-918686-30036-TextureTests.log |
| TextureImportWorkflowTests | 27 passed | 131706-112701-12000-TextureImportWorkflowTests.log |
| StaticMeshTests | 201 passed | 131851-539421-32792-StaticMeshTests.log |
| AssetImportTests | 22 passed | 131937-207381-27032-AssetImportTests.log |
| SceneImportTests | 34 passed | 131959-511958-24304-SceneImportTests.log |

Total: 829 passed, no failures or skips. Six new regressions cover managed
factory/options/results across GC, null and failed peers, completed-task reuse,
recursive task/shared-factory rejection, save failure/retry, automatic CDO-based
factory selection without CDO mutation, default names and successful saving,
unsupported options, and the real StaticMesh replacement/settings matrix.
Invocation configuration restores the supplied factory's settings on every
exit. The final operation suite supersedes its earlier 392-test receipt; its
last change adds only an operation test and does not invalidate the other five
suites' unchanged production, fixture or environment inputs.

`test affected --explain` expands through public editor module labels into
unrelated rendering/material/tool targets. These named owner/direct-consumer
suites cover the changed factory dispatch, UI admission and existing scene
callers; no GPU behavior changed. The initial sandbox build failed with the
known MSVC C1902/PDB environment issue; all passing native validation ran outside
the sandbox through DevTool, as prescribed by the build workflow. Stage 5 still
owns the workspace all-build and startup gates. Changed-document/all-plan
validation and `git diff --check` passed for this increment. No legacy import
API or positional-result removal is claimed by this increment.

Final Stage 3 acceptance (2026-11-16):

`ImportAssets(Files, DestinationPath, ...)` and `ImportAssetTasks` use one task
pipeline. File lists snapshot files/mappings, honor configured factories even with
scene selection, preserve successful peers and return successful objects only.
An optional MainFrame binding synchronizes real outputs to Browser presentation;
headless execution has no presentation dependency. Registered scene adapters own
multi-output work; AssetTools owns admission/ticking/provider drain, and DurinEd
owns managed inputs/results and completion storage. Polling does not advance work;
`GetObjects` waits on the actual operation. Source capture, preview reuse,
compilation, cancellation and native saving retain their owning implementations.
Temporary file/UI tasks release result references when handing objects to their
next owner, so completed imports do not block later deletion while awaiting GC.
User-held tasks continue to retain their outputs.

The three Stage 3 ledger rows are closed. All Engine, Sandbox and RoadWeaver
source/test roots plus Tools/Templates have zero old command, request, session or
header matches. Native fixtures call supported task APIs. Preview/progress data
remain feature-owned and read-only; the preview cannot execute an import without
a submitted task. The old synchronous scene branch and external cancellation
callback storage were also removed. Reimport's existing settings policy is unchanged.

Supported-policy clarification: same-class replacement/settings combinations apply
to standalone factory families. Scene models retain their existing one-time import
contract; even `bReplaceExisting=true` rejects occupied scene outputs before mutation.
This is a deliberate limitation relative to UE's general replacement capability,
not a new model update/reimport feature. The UE authority is the current local
`AssetImportTask.h`/`AssetTools.cpp`; the Durin limit is the existing scene importer
and `DSceneImportData` policy. Scene task/file-list callers and glTF/FBX forms share
this policy. Repeated-import/edit-preservation, mapped-material and collision
regressions cover it. An explicit scene `DestinationName` names the sole mesh;
ambiguous multi-mesh naming or contradictory prepared settings reject explicitly.
Submission also freezes preview configuration before queued work can advance.

Validation: base revision above plus the implementation inputs recorded in
`Build/UE58AssetOperationsAlignment/stage3-final-receipts/tested-inputs.json`.
Host/preset: windows-msvc-x64 / Win64-Debug-DurinEditor, MSVC 14.44, 14 build jobs,
DevTool outside the sandbox as required by the established C1902/PDB constraint.
Eight native selections passed **871 tests**, with no failures or skips. Raw logs,
five passing build receipts and SHA-256 identities for 30 changed/deleted native
inputs are preserved in `stage3-final-receipts/`; its UE manifest rechecks the
user-supplied 5.8.0 / CL 53629095 reference. Receipt names share `20261116-`.

| Selection | Result | Receipt suffix |
| --- | --- | --- |
| SceneImportTests | 38 passed | 175743-213977-7408-SceneImportTests.log |
| EditorOperationTests | 391 passed | 174009-818397-33672-EditorOperationTests.log |
| TextureTests | 152 passed | 172821-919048-43956-TextureTests.log |
| TextureImportWorkflowTests | 27 passed | 173200-795612-46968-TextureImportWorkflowTests.log |
| StaticMeshTests | 201 passed | 173208-661276-12328-StaticMeshTests.log |
| AssetImportTests | 22 passed | 172845-952821-33400-AssetImportTests.log |
| AssetEditorFamilyTests | 10 passed | 172924-514760-35736-AssetEditorFamilyTests.log |
| MaterialFunctionTests | 30 passed | 174038-783406-39728-MaterialFunctionTests.log |
| MainFrame build | passed | 173218-864450-38064-cmake.log |
| SceneImportVulkanTests build | passed, execution not selected | 173258-715857-5756-cmake.log |
| RoadWeaverEditor build | passed | 173337-271930-33344-cmake.log |
| Sandbox build | passed | 173351-678840-46636-cmake.log |
| DurinAssetTool build | passed | 173400-868322-44024-cmake.log |

Final scene/operation suites cover the last prepared-settings consistency,
queued-preview freeze, admission advance and actual-path reporting changes. Earlier passing selections and consumer builds
remain applicable: their factory/fixture behavior, exported signatures, module
edges, generated layouts and environment are unchanged by those last private
checks. The final scene suite also covers multi-output polling versus waiting,
managed retention through GC, typed selections, explicit names, source changes,
pre/post-publication cancellation, save failure retaining live task outputs,
view teardown and active-task provider shutdown. Shared fixture ownership prompted
the additional MaterialFunction selection and the unload/delete regression fix.

`stage3-final-affected.txt` records why new shared test headers and broad editor
module labels expand automatic selection to unrelated Engine tests. Named owner,
factory, UI and direct-consumer selections above cover this stage; no GPU behavior
changed, so the migrated Vulkan fixture was compiled only. The whole-workspace
all-build/startup gates remain Stage 5. Changed-document/all-plan validation and
`git diff --check` passed. No Stage 3 implementation or mandatory validation gate
remains open.

Acceptance: one and multiple outputs, chosen/automatic factory, typed options,
destination name/path, automated mode, each replacement/settings combination,
optional save, sync/async-capable and synchronous-fallback formats. Verify
completion polling, documented GetObjects waiting, GC retention, source changes,
failure/cancel before and after graph publication, save failure retaining live
outputs, progress view close, and module shutdown with active work. Reimport must
preserve its existing authored settings policy unless explicitly selected otherwise.

### Stage 4: Align deletion models and remove the Browser confirmation protocol

Dependencies: Stages 1-2. May precede Stage 3 on the single-writer lane.
Outcome: UE-shaped ObjectTools/delete-model and Browser deletion surfaces with
private confirmation/execution state and independent provider dispatch.

- [x] Implement FPendingDelete, FAssetDeleteModel and ObjectTools DeleteAssets/
      DeleteObjects with supported confirmation/cancellation behavior and actual
      object deletion counts; add UE's FDeleteItemsParameters/Result and DeleteItems.
      Do not count directory cleanup as an asset deletion. Add read-only native
      referencer information and preserve multi-asset package siblings.
- [x] Consolidate policy/reference facts and destructive attempt state; keep
      native fingerprint, companion and partial-I/O records internal. Document
      analysis, confirmation and final-validation ownership to avoid repeated work.
- [x] Preserve open-document/external live references, loading/dirty policy,
      redirector reference safety and external-store checks under the frozen
      selected-Dirty/unused-alias policies. Separate model-owned retention
      from external references so the model does not block its own deletion.
- [x] Migrate Item/service/provider/Panel APIs together to DeleteItem/BulkDeleteItems;
      remove public prepare/context/preview/validation types and update feature
      version, extensions, scripts and all tests. Confirmation UI observes the
      native model through its host; generic providers need no shared delete plan.
- [x] Migrate ordinary-file and mixed-folder deletion to File-owned execution;
      preserve unique companion ownership, confirmed selection, empty-directory
      cleanup and partial effects without all-provider preflight or rollback.
- [x] Remove Stage 4 ledger surfaces; retain separately scoped companion extension
      registration and Engine removal primitives with correct module lifetimes.

Acceptance: model state/progress and references; asset/object counts; confirmed
and headless entrypoints; changed selection/files/providers/mounts; read-only and
reparse paths; bytes changed without timestamp changes; companion conflicts;
external references acquired after analysis; protected/open documents; mixed
folders; independent source rejection; user cancellation and partial removal.
Verify no repeated destructive callback, no unconfirmed scope expansion, no
force collection, correct stale Registry fencing, and retained evidence of every
committed/failed/unattempted participant. Replace tests of removed public context
identity with tests of the public behavior and internal executor invariants.

Stage 4 acceptance (2026-11-16, Windows/MSVC, Win64-Debug-DurinEditor):

- The current UE 5.8.0 reference at `E:/Programs/Epic Games/UE_5.8` was checked
  through UnrealEd's `AssetDeleteModel.h`, `ObjectTools.h` and `ObjectTools.cpp`.
  Durin retains its stricter native protection, File-owned ordinary-file execution
  and deferred collection policy rather than adopting UE force-delete APIs.
- `FPendingDelete`/`FAssetDeleteModel` expose incremental states, reference facts,
  visibility, progress, action acceptance and actual object counts. Read-only
  CoreDObject traversal distinguishes object/property references from native
  strong/function owners and excludes only exact model-owned registrations.
- Whole-package attempts retain private asset policy, companion and external-store
  snapshots. Confirmed live export identities are rechecked after callbacks.
  Partial-package deletion preserves sibling identity, persists the surviving
  exports and retains the prior Dirty state. Garbage package registration is
  retired without forcing collection; loading, duplication and test consumers
  use the renamed checked retirement primitive.
- Source feature version 18 removes the public Browser prepare/context/preview/
  validation/lease protocol. Base bulk falls back to single actions; Item, service
  and panel use independent OR-success dispatch. MainFrame owns the native prompt.
  A missing required host rejects; explicit headless calls retain all admission.
- File owns private scope/fingerprint/effect records and mixed-folder execution.
  ObjectTools folder requests freeze file scope and expand exact asset identities
  into one model before confirmation, then clean up confirmed ordinary files and
  empty directories. The private peer-scope validation remains for that ordered
  native handoff; it is not a generic Browser preflight protocol.
- Production and test roots in Engine, Sandbox and RoadWeaver, plus Tools and
  Templates, contain no removed public deletion protocol/header references.
  MaterialFunction consumers now exercise the public model/ObjectTools boundary.
  Companion extension registration remains separately in
  `AssetTools/AssetDeleteExtensions.h`.

Raw receipts are retained under ignored `Build/UE58AssetOperationsAlignment/`
and `Build/.agent-state/logs/`. The final selections total **1,156 passed** with
**2 existing Windows symlink-privilege skips**:

| Selection | Result | Retained receipt |
| --- | --- | --- |
| ContentBrowserWorkflowTests | 207 passed, 2 skipped | `stage4-ContentBrowserWorkflowTests.log` |
| ContentBrowserDataTests | 62 passed | `stage4-ContentBrowserDataTests.log` |
| EditorOperationTests | 391 passed | `stage4-EditorOperationTests.log` |
| AssetPackageTests | 251 passed | `stage4-AssetPackageTests.log` |
| MaterialFunctionTests | 30 passed | `stage4-MaterialFunctionTests.log` |
| CoreObjectTests | 181 passed | `stage4-CoreObjectTests.log` |
| EngineGarbageCollectionTests | 10 passed | `stage4-EngineGarbageCollectionTests.log` |
| PackageRegistryContractTests | 9 passed | `stage4-PackageRegistryContractTests.log` |
| AssetReferenceStoreTests | 15 passed | `stage4-AssetReferenceStoreTests.log` |
| MainFrame build | passed | `stage4-build-MainFrame.log` |
| RoadWeaverEditor build | passed | `stage4-build-RoadWeaverEditor.log` |
| Sandbox build | passed | `stage4-build-Sandbox.log` |
| DurinAssetTool build | passed | `stage4-build-DurinAssetTool.log` |

Coverage includes model state/progress, native retention exclusion and late
external owners, exact same-package and sibling-only disk references, Dirty
sibling survival, late export expansion, unsaved folder assets, exact redirector
selection, public/headless confirmation, cancellation/reentry, independent peer
rejection, object versus directory counts, fresh hashes, unchanged-timestamp byte
edits, contributor/store changes, read-only/reparse paths, protected documents,
open levels, partial I/O, repeat-attempt rejection and Registry fencing. Host
verified hashes are shared without reopening guarded package files or repeating
companion inspection; callback-sensitive admission is checked at handoff.

`test affected --explain` expands broadly through CoreDObject/Engine labels and
shared workflow fixtures. These named owner, lifecycle, persistence, UI and direct
consumer selections cover the changed behavior without GPU or startup execution.
Passing receipts remain applicable across the final documentation and unused
private test-include cleanup; effective production/test headers and runtime
settings are unchanged. Full-workspace all-build/bounded-startup/selected-regression acceptance
remains Stage 5. Changed-document/all-plan validation and `git diff --check`
passed for this stage's final documentation and source set.

### Stage 5: Finish notifications, contract cleanup and integrated acceptance

Dependencies: Stages 1-4. Outcome: one supported API per responsibility, all
workspace consumers migrated, no superseded public protocol or unresolved gate.

- [x] Audit Browser adapters: no package staging, direct relocation workflow,
      ad hoc save/recovery state or public native effect ledger remains there.
- [x] Verify registry/live-object event ownership, source item updates, directory
      events and host history invalidation. Handle nested callbacks, reentry,
      provider retirement and committed projection-pending writes exactly once
      at each owning channel; do not infer effects from bool or diagnostic text.
- [x] Complete the removal ledger and whole-workspace symbol/include/link audit.
      Identify every surviving native type by an actual owner/consumer; remove
      dead adapters, duplicate enums, request types, test exports and module deps.
- [x] Update implemented contracts listed below, marking intentional deviations;
      remove contradictory active contract text and link the authority once.
- [x] Run final owning/direct consumer regression selections on final relevant
      inputs, an explicit `all` build across the declared workspace, and one
      bounded startup/shutdown case covering operation-service registration and
      teardown. Public vtable/generated-type changes and module dependency moves
      justify this integration build; it is not repeated after every stage.
- [x] Run changed-document and all-plan validation, inspect the final diff, record
      evidence and complete this plan only after every mandatory gate passes.

Acceptance: every removal gate closed, supported API/behavior matrix complete,
no layering cycle, Engine/Sandbox/RoadWeaver accepted, lifecycle smoke passed,
and no pending implementation or required validation. No GPU qualification,
whole native-test suite or additional preset is implied absent changed behavior
that requires it; record a specific uncovered risk before broadening coverage.

Stage 5 acceptance (2026-11-16, windows-msvc-x64 / Win64-Debug-DurinEditor,
MSVC 14.44, 18 build jobs):

- Browser adapters delegate asset commands to their policy owners and retain
  item resolution, admission and projection. Incidental target-parent creation
  no longer reports successful asset transfer. Save observation starts after
  loading; prompted saves reserve their observer before host callbacks, excluding
  nested prompt writes while observing every selected package. The duplicate
  deletion-prompt line and duplicate private rejection check were removed.
- The public MutationTypes header is removed. FAssetMutationResultDetails is
  private to byte-transform executors; native characterization adapters copy its
  evidence into FPackageMutationTestResult. Existing native fault seams remain
  because package, reference-store, material, mesh and texture tests consume them.
  Duplicate test includes and an unused production testing include were removed.
- The workspace symbol/include audit found no removed public command/protocol
  consumer. Its 40 PrepareDeleteItems lexical matches are exclusively File's
  private selection executor/module handoff and native test fixtures. Private
  File scope/effect records and AssetTools deletion/staging records retain actual
  owners. All 48 registered module descriptors form an acyclic dependency graph;
  DurinEd retains no reverse dependency on AssetTools.
- Registry/live-object events own asset identity and row projection, File owns
  ordinary-file/directory effects, and the explicit host mutation callback owns
  Transactor invalidation. Source projection does not replay asset command
  results; external committed notifications survive provider retirement. Existing
  nested feedback, projection-pending save, provider withdrawal, directory/history
  and partial-deletion regressions passed. New direct AssetTools coverage proves
  one Added/Added/Moved row sequence and no duplicate reconciliation event; the
  new prompt regression proves nested-save isolation across a multi-package save.
- Reviewed all contracts named in Validation And Evidence, plus Editor Asset
  Tools and Asset Data Lifecycle. Corrected obsolete deletion ownership,
  Valid/Stale/Rejected protocol, public native ledger, source feature version,
  package-relocation and standalone-import return claims. Existing package
  lifetime, native writer, task/import and document ownership contracts remain
  applicable; their authority links are retained without duplicating them here.

Final coverage is **1,541 passed across 21 distinct targets/case selections**,
with **2 existing Windows symlink-privilege skips** and no failures. The final
ContentBrowserDataTests selection supersedes its earlier 62-test batch result:

| Selection | Result | Receipt in stage5-receipts/ |
| --- | --- | --- |
| `@asset-workflow` (11 targets) | 524 passed, 2 skipped | `asset-workflow.log`, `asset-workflow-ctest-raw.log` |
| ContentBrowserDataTests, final | 63 passed | `ContentBrowserDataTests-final.log` |
| EditorOperationTests | 392 passed | `EditorOperationTests.log` |
| AssetPackageTests | 251 passed | `AssetPackageTests.log` |
| AssetReferenceStoreTests | 15 passed | `AssetReferenceStoreTests-final.log` |
| MaterialFunctionTests | 30 passed | `MaterialFunctionTests-final.log` |
| StaticMeshTests / StaticMeshMaterialTests | 201 / 10 passed | corresponding `*-final.log` |
| MaterialPackageTests / MaterialRuntimeTests | 6 / 99 passed | corresponding `*-final.log` |
| TextureThumbnailTests | 11 passed | `TextureThumbnailTests-final.log` |
| LaunchStartupCharacterizationTests bounded case | 1 passed | `startup-shutdown.log`, `startup-ctest-raw.log` |
| Explicit workspace `all` build, final | passed | `build-all-final.log` |

The bounded case is exactly
`FLaunchStartupCharacterizationTests.BoundedTickExitUsesNormalApplicationShutdown`
with `--mode characterization`; normal application teardown returned success.
The workspace build includes Engine, Sandbox and RoadWeaver. The affected
explanation expands through broad module labels; the asset-workflow domain and
named direct native-test consumers cover the changed notification, lifetime,
persistence and characterization boundaries. No GPU behavior changed; no whole
native suite, GPU qualification or additional preset was required or executed.
Earlier passing selections are reused only where later changes leave their
relevant inputs unchanged; the final direct-source test has its own fresh receipt.

Raw inventories, owner decisions, module graph, final result summary and SHA-256
identities for all 16 changed/deleted native inputs are retained under ignored
`Build/UE58AssetOperationsAlignment/stage5-receipts/`; DevTool logs retain their
reported paths under `Build/.agent-state/logs/`. This stage changes no selected
UE-shaped declaration or UE behavior branch. The user-supplied E:/ UE 5.8.0
installation is unavailable on this host; the recorded D:/ UE 5.8.2 / CL 56702186
Build.version was rechecked and retained, without claiming revalidation against
the unavailable installation. Prior-stage reference receipts remain historical.
Changed-document validation, all-plan validation and final diff review passed.
No implementation task or mandatory acceptance gate remains open.

## Validation And Evidence

Follow [build](../Agents/BuildAndRun.md), [testing](../Agents/Testing.md) and
[documentation](../Agents/Documentation.md) workflows. Each stage uses its owning
and affected direct-consumer targets; refresh the registry/affected explanation
when inputs or module boundaries change. Candidate existing coverage includes
EditorOperationTests, ContentBrowserDataTests, ContentBrowserWorkflowTests,
TextureImportWorkflowTests, relevant import workflow targets, and package/primary
asset tests; resolve exact available target names rather than infer them.

For each stage record tested revision plus uncommitted-input identity if needed,
selection, host profile/preset, result, log/report location, retained skips and
remaining gates. Raw reports and inventories live in ignored
`Build/UE58AssetOperationsAlignment/` or CI artifacts. Reuse passing evidence
only when relevant source, fixtures, generated/package inputs and environment
are unchanged. Documentation-only commits do not invalidate applicable native
receipts. Stage commits use exact Plan/Stage provenance from the repository
handoff rules; do not claim implementation completion from planning validation.

Contract updates are mandatory in the stage that changes behavior, with a final
consistency pass in Stage 5:

- [Asset catalog and mutation](../Runtime/Assets/Packages/AssetCatalogAndMutation.md):
  operation ownership, result boundaries, duplication, rename/reference strategy,
  deletion, fix-up and persistence evidence.
- [Asset packages](../Runtime/Assets/Packages/AssetPackages.md),
  [residency](../Runtime/Assets/Packages/AssetResidency.md), and
  [package persistence](../Runtime/ObjectSystem/PackagePersistence.md): supported
  capability entrypoints, lifetime, reload/save policy and native writer boundary.
- [Async asset operations](../Editor/Architecture/Assets/AsyncAssetOperations.md)
  and [import framework](../Editor/Architecture/Assets/AssetImportFramework.md):
  task/session ownership, multi-output publication, cancellation and optional save.
- [Browser data](../Editor/Architecture/Assets/ContentBrowserData.md): UE deletion
  signatures, independent dispatch, item outputs, notifications and removal of
  public confirmation contexts. The existing Valid/Stale/Rejected type is real
  baseline API and becomes obsolete through Stage 4, not through a documentation fix.
- [Code modules](../Workspace/CodeModules.md) and
  [asset editors](../Editor/Architecture/Framework/AssetEditors.md): tool/facade
  placement, dependency direction, document protection and migrated save/reload use.

## Risks And Completion Rules

The largest behavior risks are identity changes inside multi-asset packages,
retention of Dirty/unsaved edits during rename, reference/redirector consistency,
delete-model ownership versus external GC blockers, and duplicate host mutation
events. Cover these through observable cross-layer regressions, not tests that
only mirror new field assignments. Package reopen and fault injection are
required where persistence or partial effects change.

The old public deletion protocol is deliberately removed; its guarantees about
cross-provider preflight are not preserved by a hidden global transaction.
Confirmation and per-native-attempt validation remain internal protections.
Any unavoidable deviation from supported UE behavior must record the UE source,
Durin limitation, selected behavior, affected callers and regression before the
owning stage closes. Do not silently defer core rename, task, save or delete
semantics while declaring interface alignment complete.
