# UE-Aligned Asset Loading and Management Plan

Summary: Align asset identity, loading interfaces, streamable ownership, primary assets, bundles, and Cook policy with UE while retaining Durin's package implementation.

Last reviewed: 2026-11-10

Status: Completed
Completed: 2026-11-10

## Current Status

Stages 0-6 are complete. The Stage 0 compatibility
matrix below freezes the supported subset and consumer inventory. Stage 1
ships canonical `FSoftObjectPath`, nullable ordinary synchronous loads, explicit
`LoadObjectDetailed` / `LoadPackageDetailed` diagnostics, structured dependency
failure identities and the runtime bootstrap rename across all workspace projects.
No migration aliases remain. Stage 2 adds scoped/default streamable managers,
ordered batches, combined handles, managed ownership and per-target reports over
one backend; thumbnails use local owners and scene-to-World transfers keep their
existing lifetime. Stage 3 separates native/reflected/handle ownership from
editor, explicit and unsaved package retention. Stage 4 adds persisted primary
identity/bundle discovery, configurable native managers, independent primary
ownership and independent map preload handoff. Review corrections publish primary
state at load completion without waiting for queued public child delegates. Logical ID
validation precedes loaded-state creation; shutdown clears requests without
rebuilding their owner, and metadata wire code lives outside the value headers.
Stage 5 integrates the same configured manager's Cook rules and declared bundle
references with existing dependency discovery and transactional publication.
Cooked bootstrap admits manifest-listed package headers before reflected defaults
without authored scans; no new catalog format is introduced. Stage 6 completes
the workspace/obsolete-symbol audit, final cross-project build and bounded
authored Editor/cooked Game lifecycle handoff. Runtime behavior needed no further
layer or fallback. The supported UE subset and explicit differences live in the
owning streaming, residency, primary-management and Cooking contracts.

Post-plan corpus maintenance (2026-11-10): all 26 tracked packages now use DAST
v11, and the v10 reader/writer and temporary conversion code were removed.
The obsolete `AuthoredComponents` field is absent. Existing object paths,
classes and Outer relationships were preserved; Box gained its existing
geometry-derived `BodySetup`. All eight external bulk files remain byte-identical.
Cook now applies ordinary loading's generic removed-field rule, retaining
surviving dependencies and rejecting incompatible current fields. See
[package version policy](../Runtime/ObjectSystem/Versioning.md#authored-package-policy)
and [Cooking](../Runtime/Assets/Build/Cooking.md).

Validation used `f77c110d1` plus this maintenance working tree, Windows MSVC,
14 jobs, outside the sandbox. Incremental `all` passed for both
`Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame`. The bounded package,
registry, primary-asset, Cook, workflow, RoadWeaver, texture-fixture and complete
StaticMesh selections passed 656 cases; the asset-picker fixture and bounded
Editor startup case each passed once. StaticMesh cooked fixtures now mount
`/Game/` before bootstrap admission. Fresh repository Sandbox Cook published
eight packages with explicit root `/Game/Models/GrayboxPawn`; RoadWeaver Cook
published four. The new Sandbox generation completed three running Game ticks
and exited 0, with no missing-pawn-visual warning. Final checks report Ready,
Compatible and no identity/deprecated-route repair evidence for both projects.
This closes the raw-project Cook limitation recorded in the Stage 6 receipt.
Raw inventories, backups and native receipts are in
`Build/corpus-v11-migration/`; build logs are
`Build/.agent-state/logs/20261110-174907-415464-3884-cmake.log` and
`20261110-175834-823536-17256-cmake.log`. Cook receipts are
`20261110-180053-722136-39628-DurinAssetTool.log` and
`20261110-180056-230289-4716-DurinAssetTool.log`; Game/startup receipts are
`20261110-180108-305161-29132-DurinGame.log` and
`20261110-180124-231915-28204-ctest.log` in the same log directory. No broad GPU
qualification or complete native-test sweep was selected for this format/content
maintenance.

Stage 1 evidence (2026-11-09): tested `6f70ca02d` plus the Stage 1 working tree,
Windows MSVC, default `Win64-Debug-DurinEditor`, 14 build jobs, outside sandbox
after a sandbox-only C1902 PDB-service failure. Incremental `all` passed and
compiled Sandbox, RoadWeaver/RoadWeaverEditor, Launch, tools and editor consumers.
The final relevant source passed AssetPackageTests (218), CoreObjectTests (178),
PackageLinkerContractTests (20), PackageWriterContractTests (22),
PackageRegistryContractTests (8), AssetMetadataQueryTests (6),
AssetRuntimeLifecycleTests (1) and RoadSceneIntegrationTests (5): 458 cases.
The lifecycle case covers cold Cooked configuration, reflected defaults,
idempotence, closed admission and teardown; ordinary bootstrap behavior is
unchanged apart from names, so no full application startup suite was run.
Path/soft serialization, redirects/subobjects, mutations, failed-load rollback
and existing shared retention tests pass; new cases cover nullable observations,
report reset on resident/error paths and structured missing-dependency identity.
Raw receipts: `Build/.agent-state/logs/20261109-181648-880878-41096-cmake.log`,
`20261109-181721-104975-41096-AssetPackageTests.log` and the per-target logs
through `20261109-181921-908135-40596-AssetMetadataQueryTests.log` in that directory;
workspace build `20261109-181935-719178-44132-cmake.log`, RoadWeaver
`20261109-182148-720962-38432-RoadSceneIntegrationTests.log`.
Removed-symbol audit is empty in `Build/asset-stage1-obsolete-symbol-audit.txt`.
`affected --explain` expands widely because shared support headers and many
consumer modules changed (`Build/asset-stage1-affected.txt`); the named tests
cover changed identity/loading behavior and `all` covers consumer compilation.
No GPU execution was selected because no GPU behavior changed. The Stage 6
final-state build gate remains open for future stages; this receipt applies to
Stage 1's source and is reusable only while its relevant inputs are unchanged.

Stage 2 evidence (2026-11-09): tested `d4fd1b530` plus the Stage 2 working tree,
same Windows MSVC/default Editor preset and 14-job configuration, outside sandbox.
AssetPackageTests (227), AssetRuntimeLifecycleTests (1), TextureThumbnailTests
(14), MaterialThumbnailTests (8), StaticMeshThumbnailTests (10),
ViewportTests `FLevelDocumentResidencyTests.*` (2), and RoadSceneIntegrationTests
(5) pass: 267 cases. New coverage exercises partial/duplicate/invalid/empty
batches, per-target reports after release, managed owner inspection, shared and
nested combined children, independent cancel/release, manager destruction,
reentrant completion and cancel/drain registration. Existing package ownership,
rollback, redirect/subobject and replacement cases remain passing. Incremental
`all` passes for final production inputs; ThumbnailVulkanTests is compiled only.
Its compile check exposed two preexisting package-level AssetClassName accesses;
they now query exact asset metadata while retaining package fingerprints. No GPU
execution or full application startup suite was required for this request-layer
change. The bounded cold lifecycle, document residency and road scene cases
cover affected startup/scene ownership; frame progression is unchanged.

Stage 2 raw receipts under `Build/.agent-state/logs/`:
`20261109-184524-794172-37472-AssetPackageTests.log`,
`20261109-184546-942212-24424-AssetRuntimeLifecycleTests.log`,
`20261109-184623-597768-4236-TextureThumbnailTests.log`,
`20261109-184653-253419-23680-MaterialThumbnailTests.log`,
`20261109-184731-742602-6852-StaticMeshThumbnailTests.log`,
`20261109-184753-617435-40464-ViewportTests.log`,
`20261109-184803-325914-18700-RoadSceneIntegrationTests.log`,
`20261109-184806-619484-39808-cmake.log` (all), and
`20261109-184925-837587-44028-cmake.log` (Vulkan consumer compilation).
The final two exact-asset lookup corrections affect only that opt-in test source;
all other recorded inputs remain unchanged. Removed free requests, the result
wrapper and GetLoadedObject audit is empty in
`Build/asset-stage2-obsolete-symbol-audit.txt`; no additional source matches exist
in Sandbox or RoadWeaver beyond already migrated consumers. Selection rationale
is recorded by `Build/asset-stage2-affected.txt`: named request/thumbnail/scene
coverage avoids unrelated broad module tests. The lasting contract is
[Asset Streaming](../Runtime/Assets/Packages/AssetStreaming.md). Stages 3-6 remain
unvalidated; do not infer primary/Cook/residency-policy completion from these gates.

Stage 3 evidence (2026-11-10): tested `96848b606` plus the Stage 3 working tree,
Windows MSVC, default `Win64-Debug-DurinEditor`, 14 jobs, outside sandbox because
the established sandbox-only C1902 failure prevents reliable MSVC compilation.
Incremental `all` passed: DPackage layout and configuration DLL API changes
require compilation of consumers across every workspace project. AssetPackageTests
(235), CoreObjectTests (178), CoreObjectReplacementTests (41),
AssetPackageReloadTests (21), EditorAssetWorkflowTests (44), CookFunctionalTests
(40), AssetRuntimeLifecycleTests (1), TextureThumbnailTests (14),
MaterialThumbnailTests (8), StaticMeshThumbnailTests (10), ViewportTests
`FLevelDocumentResidencyTests.*` (2), and RoadSceneIntegrationTests (5) pass:
599 cases. New cases cover bare/soft observations, independent export owners,
unsaved publication, explicit editor host policy and failed-unload root restoration.
Existing rollback, replacement, Cook and editor workflows remain passing.
Named selection is recorded in `Build/asset-stage3-affected.txt`; no GPU behavior
changed and no GPU execution or full application startup suite was selected.
Passing receipts are reused while their relevant inputs remain unchanged.

Stage 3 raw receipts under `Build/.agent-state/logs/` include
`20261110-111503-919674-36768-cmake.log` (all),
`20261110-111517-339719-9352-AssetPackageTests.log`,
`20261110-105509-771005-42348-CoreObjectTests.log`,
`20261110-105529-984222-48280-CoreObjectReplacementTests.log`,
`20261110-110133-323678-44520-AssetPackageReloadTests.log`,
`20261110-110328-258886-30724-EditorAssetWorkflowTests.log`,
`20261110-110538-745773-25628-CookFunctionalTests.log`,
`20261110-110602-108386-8984-AssetRuntimeLifecycleTests.log`,
`20261110-111011-944247-28380-TextureThumbnailTests.log`,
`20261110-111030-320653-17996-MaterialThumbnailTests.log`,
`20261110-111048-704687-45096-StaticMeshThumbnailTests.log`,
`20261110-111114-167441-47444-ViewportTests.log`, and
`20261110-111500-474966-32644-RoadSceneIntegrationTests.log`.
The obsolete cache API audit is empty in
`Build/asset-stage3-removed-cache-api-audit.txt`. The lasting ownership contract
and UE compatibility boundary are in
[Asset Residency](../Runtime/Assets/Packages/AssetResidency.md).
Stages 4-6 and the final-state acceptance gate remain open.

Stage 4 evidence (2026-11-10): tested `c5bab8af1` plus the Stage 4 working tree,
Windows MSVC, default `Win64-Debug-DurinEditor`, 14 jobs, outside sandbox because
the established sandbox-only C1902 failure prevents reliable MSVC compilation.
Incremental `all` passes across Engine, editor/tools, Sandbox and RoadWeaver:
the new DObject virtual hooks, shared registry records and reflected manager
cross DLL boundaries that bounded consumer compilation cannot cover reliably.
PrimaryAssetTests (15), AssetPackageTests (235), PackageLinkerContractTests (20),
PackageWriterContractTests (22), PackageRegistryContractTests (8),
AssetMetadataQueryTests (9), AssetRuntimeLifecycleTests (1), CoreObjectTests
(178), CookFunctionalTests (40), AssetPackageReloadTests (21),
EditorAssetWorkflowTests (44), WorldTests selected map-change/preload cases (6),
TextureTests selected cooked/volume persistence cases (3), TextureThumbnailTests
(14), MaterialThumbnailTests (8), StaticMeshThumbnailTests (10), and
RoadSceneIntegrationTests (5) pass: 639 unique cases. The map case verifies
preload completion followed by independent World ownership after primary unload.
New coverage also exercises metadata-only discovery, duplicate IDs, redirects,
save/deletion/relocation mapping changes, no-work requests, independent primary
roots, transition races/failure/cancellation and manager shutdown.

DAST v10 frozen-byte/hash fixtures remain unchanged and explicitly select their
legacy writer; ordinary-save assertions select v11. Reader diagnostics now name
DAST without incorrectly reporting v10 for v11 failures. Passing feature receipts
are reused after diagnostic wording/whitespace-only edits; final `all` rebuilds
the affected DLL. No GPU behavior changed and no GPU execution or full executable
startup suite was selected. CPU map lifecycle, cold cooked bootstrap, Cook and
consumer cases cover the changed contracts; standalone cooked primary policy
and the final authored-editor/cooked-game lifecycle gates remain Stages 5-6.

Raw receipts are preserved outside tool log rotation in
`Build/asset-stage4-evidence/`; `validation-summary.txt` records exact per-target
filenames and counts. The final workspace build is
`20261110-115902-044724-29472-cmake.log`; primary behavior is
`20261110-115059-283516-8708-PrimaryAssetTests.log`.
Selection reasons and the workspace symbol audit are in
`Build/asset-stage4-final-affected.txt` and `Build/asset-stage4-consumers.txt`.
The lasting contract and supported UE boundary are in
[Primary Asset Management](../Runtime/Assets/Packages/PrimaryAssetManagement.md).

Stage 5 evidence (2026-11-10): tested `e6e9a0be7` plus the Stage 5 working tree,
Windows MSVC, 14 build jobs, outside sandbox under the established C1902 recovery
policy. Default Win64-Debug-DurinEditor passes PrimaryAssetTests (29),
AssetCookTests (38), CookFunctionalTests (42), AssetPackageTests (236),
AssetRuntimeLifecycleTests (1), AssetMetadataQueryTests (9),
PackageRegistryContractTests (8), WorldTests
`*MapChange*:FWorldTests.PrimaryMapPreloadTransfersRetentionToTheWorld` (6),
and RoadSceneIntegrationTests (5): 374 cases. Win64-Debug-DurinGame additionally
passes PrimaryAssetTests (29), AssetCookTests (38) and
AssetRuntimeLifecycleTests (1): 68 case executions in the actual no-Editor
configuration, for 442 final case executions total. DurinAssetTool builds in
the Editor configuration (`20261110-164223-474468-2416-cmake.log`). No production
or test input changed after these final receipts; later edits are documentation
and ignored evidence only.

New coverage includes recursive management cycles/hard closure, precedence,
equal-priority conflict determinism, EditorOnly rules, optional NeverCook bundle
filtering, required explicit/default/hard conflicts, unsupported chunk/install
requests, v10 defaults and all-or-nothing cooked catalog admission. The map and
native primary-data fixture exercise authored load, cold/warm Cook, removal of
authored content access, cooked ID/path discovery, bundle load and final release.
The independent cold process verifies manifest catalog admission before DObject
and CDO construction in both variants. No GPU behavior changed; no GPU or full
executable startup sweep was selected. Stage 6 retains the final `all` and
integrated lifecycle gates. Named selections bound the broad Engine module
expansion in `Build/asset-stage5-final-affected.txt`; the workspace symbol audit
is `Build/asset-stage5-consumers.txt`. New methods have no direct Sandbox or
RoadWeaver consumers; the map and Road scene cases cover existing indirect users.
Raw test/build receipts are preserved outside log rotation in
`Build/asset-stage5-evidence/`; `validation-summary.txt` lists exact final files,
variants, filters and counts. The lasting policy and UE subset are documented in
[Cooking](../Runtime/Assets/Build/Cooking.md#primary-asset-policy-and-cooked-discovery)
and [Primary Asset Management](../Runtime/Assets/Packages/PrimaryAssetManagement.md).

Stage 6 evidence (2026-11-10): tested `a4816aedd` plus the Stage 6 working tree,
Windows MSVC Debug, 14 build jobs, outside sandbox under the established C1902
recovery policy. There are no production-source changes in this stage. The
PackageTests restart helper now names AssetRuntime rather than AssetManager;
three old soft-path names and synchronous diagnostic guidance were corrected,
and streaming usage now states which API supplies each ownership lifetime.

Fresh Editor coverage passes AssetPackageTests (236), AssetPackageReloadTests
(21), EditorAssetWorkflowTests (44), TextureThumbnailTests (14),
MaterialThumbnailTests (8), StaticMeshThumbnailTests (10), and the single
`FLaunchStartupCharacterizationTests.AutomatedLaunchPreservesInteractiveHistory`
case in characterization mode (1): 334 cases. Reuse the Stage 5 final receipts
for the other 206 executions, including 68 actual Game-variant executions:
their relevant production sources, fixtures, generated/package inputs,
toolchain and settings are unchanged. The helper-only PackageTests edit has its
new receipt above. Combined final native coverage is 540 executions (472 distinct
case names), not a full native-test sweep. Final incremental `all` passes for
the final inputs (`20261110-170523-063137-8072-cmake.log`), covering Engine,
editor/tools, Sandbox and RoadWeaver public-header/DLL consumers.

The real authored Editor project startup exits normally and preserves interactive
history (`20261110-165021-790283-20984-ctest.log`). The actual cooked Sandbox Game
passes the native BeginPlay/presentation gate, completes three running ticks
and exits 0 using `DevTool run --preset Win64-Debug-DurinGame --project
Sandbox/Sandbox.dproject -- --exit-after-ticks=3`
(`20261110-165857-274525-39096-DurinGame.log`). Launcher, Sandbox and Renderer Game
targets were rebuilt against the final public ABI. An initial missing Sandbox
export and Renderer light virtual-slot recursion came from stale dynamically
loaded DLLs; rebuilding those targets resolved both without source workarounds.
These are normal visible-window lifecycle smokes on NVIDIA GeForce GTX 1060 6GB,
Vulkan 1.4.312, not a GPU qualification sweep.

The Game smoke uses the existing 2026-11-09 Cook generation. A sampled new raw
Sandbox Cook still rejects the obsolete `AActor::AuthoredComponents` field
before publication; this is the already recorded content-migration limitation
in [startup integration](RuntimeStartupAndSceneActivation.md#current-status).
Tracked authored content and strict schema checks were not changed. That older
generation also lacks the optional GrayboxPawn visual; the existing native pawn
fallback warns and remains playable. This handoff qualifies lifecycle, not a new
whole-Sandbox Cook or complete gameplay visuals. The final-input primary/map/
bundle authored-to-Cook-to-cooked round trip and failed-publication behavior
remain covered by Stage 5's passing independent fixtures.

The fresh symbol audit is empty across Source/Tests for all three workspace
projects and current Runtime/Editor/Development contracts, including old soft
paths, load-result wrappers, bootstrap names, free batch requests and cache-origin
bookkeeping (`Build/asset-stage6-final-obsolete-symbols.txt`). Consumer inventory
is `Build/asset-stage6-consumers.txt`; project-specific detailed loads are in
`Build/asset-stage6-project-consumers.txt`. Review confirmed one shared load
backend/live object identity authority: registry metadata and the revision-keyed
primary catalog are projections, handle roots own request retention, and primary
Current/Pending handles own policy state. They introduce neither a second
resident package cache nor implicit caller leases. Native selection is recorded
in `Build/asset-stage6-affected.txt`. UE 5.8 public release/cancel/preload and rule
union contracts were rechecked; existing documented subset boundaries, including
fatal equal-priority Cook conflicts, remain explicit without a licensed-source
parity claim. Raw passing/failure diagnostics, the debugger trace, Cook generation
hashes and exact final receipt/count summary are preserved outside log rotation
under `Build/asset-stage6-evidence/`. All required plan gates are closed; the
separate Sandbox content migration remains outside this plan.

Inspected baseline: `fade01575`. Recent work already separates package/asset
metadata, aligns soft-reference operations, unifies streamable handles, and
selects the runtime domain before reflected defaults. Preserve these gains.
The baseline `InitializeAssetManager` initialized `FAssetRuntimeState`; Stage 1
renamed that bootstrap to `InitializeAssetRuntime`. It remains separate from the
primary-asset policy manager proposed here.

## Goal

Make UE knowledge transfer to Durin's asset APIs: the same concepts should have
recognizable names, data shapes, ownership, completion, and failure behavior.
An extra facade over unchanged conflicting contracts does not satisfy this goal.
Keep package parsing, dependency restoration, publication, and GC implementation
behind those contracts. Replace public result wrappers where pointer returns,
handle observation, or explicit diagnostic operations better match UE.

## Scope and Ownership

| Owner | Responsibility and starting points |
| --- | --- |
| CoreDObject | Object/path identities, soft and strong references, GC, package graph; `DObject/AssetPath.h`, `SoftObjectPtr.h`, `Package.h` |
| AssetRegistry | Discoverable metadata and dependency snapshots; `AssetRegistry/Catalog.h`, `PackageTypes.h`; no live ownership |
| Engine loading | `Asset/Load.h`, `StreamableHandle.h`, private `AssetRuntimeStateInternal.h`, `AssetAsyncLoad.cpp`, linker and load transactions |
| Engine management | New `Engine/StreamableManager.h`, `Engine/AssetManager.h`, asset-management value types and primary data assets |
| AssetTools and editor sessions | Import/save/delete policy, dirty assets, editing and preview ownership |
| Cook | Consume management policy through existing Cook discovery and publication; do not duplicate the cooker |
| Launch and consumers | Runtime initialization, manager setup, shutdown; scene loading, previews, thumbnails, tools and project code |

Search all source and test roots declared in [Durin.dworkspace](../../Durin.dworkspace)
before changing shared symbols, including Engine, Sandbox, and RoadWeaver.
Record consumer targets in each stage's evidence.

Coordinate lifecycle and scene integration with
[Runtime Startup and Scene Activation](RuntimeStartupAndSceneActivation.md).
That plan owns frame-loop progression and scene presentation; this plan owns
asset requests and retention. Neither introduces a second readiness authority.

Out of scope: UE binary/package compatibility, Blueprint implementation,
IoStore/AsyncLoading2 replication, download/install services, physical chunk
containers, texture mip streaming, global task-system replacement, and a generic
all-subsystems readiness manager. GPU or compilation readiness remains separate
from object-load completion.

## Reference Baseline

Use Epic's public UE 5.8 documentation inspected on 2026-11-09 as the initial
API reference, not a claim of source-level parity. Stage 0 must record exact
supported signatures and version differences before public-header changes.
When licensed UE source is available, record its version/revision for behavior
that public documentation leaves ambiguous; otherwise document the chosen Durin
behavior explicitly. Do not silently mix legacy and current overload semantics.

- [Asset management](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine): primary identity, discovery, bundles, and manager layering.
- [Object pointers](https://dev.epicgames.com/documentation/en-us/unreal-engine/object-pointers-in-unreal-engine): soft identity versus GC ownership.
- [Asset Registry](https://dev.epicgames.com/documentation/unreal-engine/asset-registry-in-unreal-engine): metadata queries without loading assets.
- [FStreamableHandle](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FStreamableHandle): separate completion, cancellation, release, loaded objects, and errors.
- [CreateCombinedHandle](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStreamableManager/CreateCombinedHandle): completion composition and child retention.
- [LoadPrimaryAssets](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UAssetManager/LoadPrimaryAssets): manager-owned retention independent of the returned handle.
- [FPrimaryAssetRules](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FPrimaryAssetRules): management priority, recursion, Cook rule, and chunk assignment.

The following designs are selected Durin targets. They are not copied UE
declarations or assertions that every edge case already matches UE.

## Selected Interface and Data Design

### Identity and Metadata

| Type | Target representation and contract |
| --- | --- |
| `FTopLevelAssetPath` | Package name plus asset name; preserve exact top-level selection |
| `FSoftObjectPath` | Top-level identity plus optional subobject path; migrate the public soft-path role currently served by `FObjectPath` |
| `TSoftObjectPtr<T>` | Typed soft identity and weak cache; `Get` never performs I/O and never retains the object |
| `FPrimaryAssetType` | Named primary-asset category with validity, equality, and hashing |
| `FPrimaryAssetId` | `PrimaryAssetType` plus `PrimaryAssetName`; `Type:Name` textual form, no implicit file lookup |
| `FAssetBundleEntry` | `BundleName` and `BundleAssets`, using top-level asset paths for the initial supported bundle format |
| `FAssetBundleData` | Collection of named bundle entries with deterministic deduplication and serialization |
| `FPrimaryAssetTypeInfo` | Type, base-class identity, scan directories, explicit assets, editor-only flag, and rules |
| `FPrimaryAssetRules` | `Priority`, `ChunkId`, `bApplyRecursively`, and `CookRule`; define override/default semantics before use |

Keep `FPackagePath` as Durin's validated package boundary type. Do not rename
unrelated live-object paths merely to match soft-reference spelling. Stage 0
must inventory `FObjectPath` roles and choose a single canonical representation
with a bounded migration adapter, not two independently serialized identities.
UE-like names do not waive Durin's mount, path-validation, or case rules; record
those deliberate differences in the compatibility matrix.

Primary IDs are logical identities, not permanent GUIDs. Define duplicate-ID
failure, type/name redirects, path relocation, missing targets, and cache
invalidation. Renaming an asset may require an ID redirect. Registry metadata
must expose primary identity and bundle facts without constructing live objects.
Keep management edges distinct from serialized hard/soft dependency edges.
Unsupported Blueprint-class discovery must be rejected explicitly if represented
in settings, never accepted as a no-op.

### Loading API and Diagnostics

Target ordinary synchronous entry points return pointers:

```cpp
DPackage* LoadPackage(const FPackagePath& Path);
DObject* LoadObject(const FSoftObjectPath& Path, const DClass* ExpectedClass);
template<class T> T* LoadObject(const FSoftObjectPath& Path);
```

Failure returns null; returned pointers are observations, not new GC owners.
Keep `TSoftObjectPtr<T>::LoadSynchronous()` consistent with this surface.
These sketches omit exports, includes, and convenience overloads.

Structured failure remains mandatory for tools, tests, and actionable UI.
Provide a separately named detailed load operation or an explicit per-call
diagnostic output, chosen in Stage 0. It must retain category, requested/resolved
identity, dependency cause, and mutation/report evidence. Avoid global last-error
state, string parsing, and mandatory duplicate logging. Internal transactions
may continue using `std::expected`; this is an implementation choice, not an
interface compatibility requirement. Save, mutation, and Cook result APIs are
outside this load-interface migration unless a direct consumer requires change.

### StreamableManager and Handle

Introduce a non-object `FStreamableManager` as a request/retention owner over the
existing shared loading service. Each manager may own requests, but all managers
reuse one package loader, live object registry, and coalesced package work.
Support a default manager plus explicitly owned managers where lifetime warrants
them. Manager destruction must retire its requests safely and invalidate any
manager back-pointer visible from surviving handles.

Proposed surface, using existing C++ containers/smart pointers where appropriate:

```cpp
class FStreamableManager {
public:
    std::shared_ptr<FStreamableHandle> RequestAsyncLoad(FStreamableAsyncLoadParams Params);
    std::shared_ptr<FStreamableHandle> RequestSyncLoad(
        std::span<const FSoftObjectPath> Targets, bool bManageActiveHandle = false);
    std::shared_ptr<FStreamableHandle> CreateCombinedHandle(
        std::span<const std::shared_ptr<FStreamableHandle>> Children,
        /* supported combination options and callbacks */);
};
```

`FStreamableAsyncLoadParams` carries target paths, completion/cancel callbacks,
priority, managed-handle choice, and a debug name. Confirm the exact UE field
names and delegate forms in Stage 0. Implement stalled-start support only after
its state transitions are specified; do not publish an ignored option. Container
spelling need not reproduce Unreal containers or introduce a new smart-pointer
library. Object streaming targets are object paths; package-only tools keep a
distinct package-loading API rather than inventing an asset from a package leaf.

Move public observation toward `HasLoadCompleted`, `WasCanceled`, `IsActive`,
`GetLoadedAsset`, `GetLoadedAssets`, `GetRequestedAssets`, `HasError`, `GetError`,
`ReleaseHandle`, and `CancelHandle`. Remove the mandatory
`expected<FStreamableLoadResult, ...>` observation pattern after migration.
Keep detailed errors per target for batch diagnostics, with a defined aggregate
error policy. Public completion does not mean every target succeeded.

Specify and test these behaviors before implementing batching:

- Completion and retention are independent dimensions; completed work can remain
  active or be released. Callback delivery is a further observable boundary.
- Batch target ordering, duplicate paths, null/invalid paths, empty batches,
  redirects, partial success, and typed-result cast failures have explicit rules.
- Define null-handle versus failed-handle returns and callback behavior for
  invalid requests, shutdown rejection, no-op requests, and resident hits.
- Cancellation suppresses that consumer's completion and releases its ownership;
  shared package work and unrelated consumers are not implicitly canceled.
- Release before completion must have defined deferred-release behavior. Handle
  destruction, callback reentrancy, and manager destruction must not create leaks
  or dangling callback owners.
- Combined handles retain child requests while active. Define failure aggregation
  and release/cancel propagation for shared children; parent cancellation must
  not accidentally invalidate another parent's request.
- `bManageActiveHandle` is explicit ownership by the manager, not a hidden cache.
  Query/release operations must make these owners inspectable.

Existing callbacks execute on GameThread; preserve that until an explicit,
validated contract change. Backend I/O coalescing and scheduling stay private.

### Residency and Bootstrap

Keep one live object/package registry. Strong graph edges, streamable owners,
world/editor owners, and explicit editor retention determine lifetime. A soft
reference never becomes an owner simply because it resolved successfully.

Audit the current unconditional clean `Standalone` cache retention and the
request transaction's cache cleanup. Target clean game-runtime assets becoming
collectable when their actual owners disappear, without load-call-history
dependent pins. Keep necessary editor/dirty retention explicit and separately
diagnosable. Do not blindly remove `Standalone` or change package-to-export
retention before characterizing multi-export packages and all native owners.
Record any intentional departure from UE package/GC behavior.

`ReleaseHandle` and primary-asset unload release ownership; they do not promise
immediate destruction or force global GC. Checked destructive package unloading
remains a separate tools operation with unsaved-state and reference checks.

Rename bootstrap APIs to `InitializeAssetRuntime` / `ShutdownAssetRuntime`.
Configure the final data domain before CDO construction. Construct/start the
policy AssetManager later, after reflection and the required catalog are ready.
CDO default loading must not recursively require primary-asset discovery.
Shutdown closes admission, detaches consumers, retires manager-owned requests,
drains accepted loading, and then retires runtime resources. Reuse the existing
startup state authority and preserve partial-initialization teardown.

### Primary Asset Policy

Introduce `DAssetManager`, following Durin's object prefix, with the role of
UE's `UAssetManager`. Engine owns its lifetime; support project-specific policy
through the existing configuration/reflection facilities after Stage 0 verifies
their extension points. Provide `GetStreamableManager`, primary ID/path queries,
`LoadPrimaryAsset(s)`, `UnloadPrimaryAsset(s)`, and
`ChangeBundleStateForPrimaryAssets` with a supported load-parameter structure.

Add `DPrimaryDataAsset` and an appropriate `GetPrimaryAssetId` extension point.
Do not give CoreDObject a dependency on Engine: identity value types needed by a
base-object hook must live in a lower neutral layer. Initial built-in primary
coverage is levels/maps plus a data-asset fixture; ordinary textures/materials
remain directly streamable secondary assets.

The manager owns loaded primary state independently of the returned observation
handle. A caller releasing that handle does not unload the primary asset.
Global primary state is not automatically reference-counted per caller: multiple
features that independently acquire/release one ID need an explicit higher-level
owner or separate streamable requests. Do not silently add lease semantics to
UE-named primary load/unload operations.

Track committed and pending bundle states separately. A stale callback must not
overwrite a newer request. Define replacement/cancellation, partial failure,
and preservation of the previous usable state before implementing transitions.
Loading a bundle resolves its declared paths and ordinary hard dependencies;
it does not recursively load every soft reference. Explicit recursive bundle
expansion, if supported, needs cycle detection and a documented policy.

Persist bundle/primary metadata through existing save and registry publication.
Version any changed persistent projection, rebuild derived registry caches, and
support existing packages with absent metadata. Runtime-only registration may
be added through a UE-shaped `AddDynamicAsset` API with a clear lifetime.

### Cook Integration

Project/type rules and per-ID overrides produce management policy consumed by
the existing Cook roots and dependency discovery. Specify precedence, equal
priority conflicts, recursive management, hard-dependency inclusion, and
NeverCook conflicts. Freeze the supported `EPrimaryAssetCookRule` values against
the reference version; reject unsupported values explicitly.

Emit primary ID/path/bundle metadata needed by cooked games without editor scans.
Validate authored and cooked lookup against the same logical catalog contents.
Preserve `ChunkId` in the design, but reject non-default physical chunk requests
until a packaging backend supports them. This plan does not claim delivery of
chunk files merely because the rules contain a chunk field.

## Implementation Stages

### Stage 0: Freeze the Compatibility Matrix and Consumer Inventory

Dependencies: none. Outcome: implementable, versioned API decisions.

- [x] Inventory shared load/path/handle APIs and consumers across all workspace projects.
- [x] Record UE reference signatures, supported subset, intentional differences,
  and exact replacement types/headers; settle diagnostics and callback edge cases.
- [x] Characterize current cache, dirty/edit-session, package-export, and native
  ownership; identify every implicit pin before changing retention.
- [x] Resolve primary metadata storage/versioning, settings extension points,
  soft-path migration, combined-handle propagation, and Cook rule precedence.
- [x] Select focused test targets and coordinate startup-plan integration.

Acceptance: no unresolved public state/ownership/error behavior needed by Stage 1;
record baseline behavior and known deviations without claiming UE parity.

### Stage 1: Align Identity and Synchronous Loading Interfaces

Dependencies: Stage 0. Outcome: one public identity model and pointer-oriented loads.

- [x] Implement canonical soft-path naming and migrate all serialized/API consumers
  without changing existing asset identity or losing subobject/redirect behavior.
- [x] Introduce pointer-returning load APIs with separate structured diagnostics;
  migrate tools/tests away from mandatory public `std::expected` load results.
- [x] Rename runtime bootstrap APIs and update every host and test fixture.
- [x] Remove temporary adapters after all consumers compile; retain only documented
  externally required compatibility paths with an explicit removal condition.

Acceptance: path round trips, null/missing/type failures, diagnostic evidence,
CDO domain selection, and relevant startup/teardown checks pass. Existing assets
remain readable; no dependency on the future policy manager is introduced.

### Stage 2: Establish StreamableManager and Batch Handle Semantics

Dependencies: Stage 1. Outcome: UE-shaped request APIs over one backend.

- [x] Refactor the existing request service into manager ownership plus shared loading.
- [x] Implement params, batch targets, combined handles, managed handles, and
  separate completion/retention/error observation.
- [x] Migrate scene loading, previews, thumbnails, maintenance, and project consumers;
  retire the old free-function streaming surface and result wrapper.
- [x] Add owner/debug diagnostics without a second resident-package table.

Acceptance: mixed-success batches, duplicate/resident targets, shared roots,
cancel/release timing, reentrant callbacks, shared combined children, and manager
destruction satisfy the Stage 0 matrix. No per-manager duplicate package graphs.

### Stage 3: Converge Residency and Editor Retention

Dependencies: Stage 2. Outcome: explicit owners determine collectability.

Execution decision (2026-11-10): distinguish editor Standalone retention from
runtime ownership explicitly in bootstrap configuration, independently of the
Authored/Cooked data domain. Launch selects editor retention only for the editor
host. Generic tools/tests default to runtime retention. UE 5.8 documents
[RF_Standalone](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/EObjectFlags)
as editing retention even without references, and its
[package tools](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UPackageTools)
restore Standalone on reachable objects during checked unloading. Therefore do
not apply runtime no-cache behavior indiscriminately to editor assets. Track
editor, explicit and unsaved reasons separately; restore the exact prior roots
on failed tools unloading. Unsaved package roots and retaining all exports of
one package remain explicit Durin policies, not verified UE source equivalence.
Rollback withdraws failed graph identities immediately and schedules physical GC;
unrelated clean graphs require their actual native/reflected owners across GC.

- [x] Replace incidental clean-runtime pins with the selected ownership policy.
- [x] Preserve explicit dirty/editor-session retention and checked tool unloading.
- [x] Remove obsolete cache-origin bookkeeping only once equivalent cleanup is proven.
- [x] Verify failed-load rollback and independently completed dependencies.

Acceptance: two owners can release independently; the last owner permits GC;
soft references do not retain; dirty/new assets survive intended editor flows;
multi-export packages, reload/replacement, canceled requests, and failure paths
do not leak, lose edits, or expose prematurely retired objects.

### Stage 4: Add Primary Assets and Bundle Management

Dependencies: Stages 1-3. Outcome: primary discovery and manager-owned policy state.

Execution decision (2026-11-10): the current registry cache schema is 6 rather
than the Stage 0 baseline's 3; advance it to 7. DAST v11 adds a version-1
management projection per top-level registry record; an empty ID and bundle
list mean absent metadata. Retain v10 registry-v3 readers and explicit writers;
ordinary saves use v11. Existing v10 content stays readable without a corpus
rewrite and is upgraded on ordinary save. Source-preserving mutations keep
their source version. Native DPrimaryDataAsset subclasses publish bundles via
UpdateAssetBundleData; Blueprint discovery and recursive bundle expansion remain
unsupported. Registry discovery consumes persisted metadata only. With no Types
filter, include all valid published primary IDs; configured Types restrict scans.
This differs from UE's project settings representation, while retaining its
logical identities and ownership semantics. No-work primary requests return
null with immediate completion. Per-primary generations preserve committed
state on failure/cancellation; explicit Unload and newer transitions supersede
pending work. No implicit caller leases are introduced. Cook rule enforcement
and standalone cooked metadata handoff are covered by Stage 5.

Review correction (2026-11-10): use UE-shaped nonrecursive PreloadPrimaryAssets
for the default map, with retention owned only by its returned handle. The map
must not replace or unload global primary state owned by another service.
Restore generic combined completion to child load-terminal state, independent
of queued public delegates. Primary state commits through a private load-terminal
notification before parent notification; public completion remains deferred.
This supersedes the initial global callback-fence and map Load/Unload pairing.
The other review findings remain outside this correction's requested scope.

Review correction evidence: tested `139430f3d` plus the review working tree,
Windows MSVC, default `Win64-Debug-DurinEditor`, 14 jobs, outside sandbox for
the established C1902 limitation. PrimaryAssetTests (18), AssetPackageTests
(236), WorldTests selected map-change/preload cases (6),
AssetRuntimeLifecycleTests (1), TextureThumbnailTests (14),
MaterialThumbnailTests (8), StaticMeshThumbnailTests (10), and
RoadSceneIntegrationTests (5) pass: 298 unique cases. New cases prove preloads
survive global bundle changes/unload, preload cancellation preserves a pending
primary load, generic combined completion can precede child public delegates,
and primary state publishes before its public completion delegate. The map
integration case retains an already-loaded primary through map completion,
then verifies World retention after the test explicitly unloads its own primary.

Raw receipts and exact target filenames are preserved in
`Build/asset-preload-review-evidence/validation-summary.txt`. Selection and
workspace consumer audits are in `Build/asset-preload-review-affected.txt` and
`Build/asset-preload-review-consumers.txt`. No public object layout, vtable,
package format or build graph changed; bounded owner/consumer targets cover
these changes without another all build. Bootstrap, project admission and
level resolution are unchanged; CPU map lifecycle coverage exercises the
ownership handoff, with no full executable startup or GPU execution selected.
Stages 5-6 remain open.

Review simplification decision (2026-11-10): resolve logical IDs before inserting
loaded state. Reuse a UE-shaped CreateErrorHandle for identity failures instead
of passing empty paths into the loader; failed/canceled first loads leave no empty
entry. Keep one stable policy state and its streamable manager. Reuse the existing
request cleanup for policy shutdown and streamable destruction, without a second
per-entry cancellation loop or replacement owner allocation. Retain the policy
active flag: primary initialization and ordinary streamable availability are
distinct responsibilities. This also preserves GetStreamableManager availability
before primary initialization and across shutdown/restart. Move the unchanged binary
metadata codec from AssetBundleData.h into a declaration header and CoreDObject
implementation, with explicit includes at its package/cache callers. No new
serialization service, wire version or migration layer is introduced.

Review 3/4/5 evidence (2026-11-10): tested `d9814c69d` plus this working tree,
Windows MSVC, default `Win64-Debug-DurinEditor`, 14 jobs, outside sandbox for
the established C1902 limitation. Incremental all passed for final inputs
(`20261110-160158-589216-31932-cmake.log`); removing a transitive include from
the foundational object header requires consumer compilation across Engine,
Sandbox and RoadWeaver. PrimaryAssetTests (23), AssetPackageTests (236),
PackageWriterContractTests (22), PackageLinkerContractTests (20),
PackageRegistryContractTests (8), AssetMetadataQueryTests (9),
AssetRuntimeLifecycleTests (1), WorldTests selected map/preload cases (6), and
RoadSceneIntegrationTests (5) pass: 330 unique cases.

New cases exercise direct error handles without work, invalid/missing IDs,
mixed identity failures, failed/canceled first loads, repeated shutdown, ordinary
streaming before primary initialization, stable owner identity and restart with
old callbacks detached. The codec function bodies match the baseline after
removing inline; frozen v10 wire/hash fixtures and v11 metadata discovery pass.
Package/cache receipts are reused after the Engine-only cleanup refinement;
their relevant CoreDObject/AssetRegistry inputs are unchanged. Final owner,
loader and map cases cover the refined shutdown implementation. No GPU behavior,
package format or process bootstrap configuration changed; no GPU execution or
full process startup suite was selected. Stages 5-6 remain open.

Exact target filenames, counts and raw receipts are preserved in
`Build/asset-review-345-evidence/validation-summary.txt`. That directory also
contains codec-body and shutdown audits. Selection and workspace symbol audits
are in `Build/asset-review-345-affected.txt` and
`Build/asset-review-345-consumers.txt`.

- [x] Add identity/rule/bundle types, primary data assets, metadata publication,
  type settings, ID validation and redirects.
- [x] Implement manager initialization, queries, primary load/unload, and bundle changes.
- [x] Integrate one map/scenario preload flow while preserving document/world ownership.
- [x] Keep arbitrary editor previews on local requests rather than global primary state.

Acceptance: metadata discovery constructs no assets; duplicate IDs are diagnosed;
redirects and save/rename/delete invalidate mappings correctly; releasing the
returned handle leaves manager-owned assets active; unloading drops only manager
ownership. Bundle transition races/failure and manager shutdown pass focused tests.

### Stage 5: Integrate Cook Policy and Cooked Metadata

Dependencies: Stage 4. Outcome: one policy feeds existing Cook mechanisms.

Implementation decision (2026-11-10): reuse the configured DAssetManager for
UE-shaped ModifyCook/ModifyCookReferences hooks and the existing dependency
discovery and transactional publisher. Management edges remain distinct from
serialized references. Unknown leaves ordinary discovery unchanged; AlwaysCook
adds managed packages, while NeverCook filters optional soft/bundle references
and fails for explicit/default/external roots or retained hard dependencies.
Equal-priority incompatible rules fail with both manager IDs (a stricter
publication boundary than UE's NeverCook winner plus conflict report).
Editor-only primary types ignore AlwaysCook, as in UE. Unsupported enum values,
non-default ChunkId and install settings are rejected rather than simulated.
Cooked discovery uses the existing manifest inventory and DAST headers, including
their primary/bundle metadata, without authored-directory or live-object scans.
No second catalog format or physical chunk backend is introduced.

- [x] Apply supported type/asset Cook rules and management edges to root selection.
- [x] Publish cooked primary/bundle metadata and validate it without editor services.
- [x] Diagnose rule conflicts and unsupported chunk/install settings.
- [x] Exercise a map and primary-data-asset fixture through authored load, Cook,
  cooked discovery, bundle load, and release.

Acceptance: deterministic inclusion/exclusion and conflict reports; required hard
dependencies remain valid; old package defaults are defined; failed Cook publishes
no partial output. No claim of physical chunk delivery.

### Stage 6: Complete Integration and Contract Handoff

Dependencies: Stages 1-5. Outcome: migrated consumers and durable documentation.

- [x] Audit removed symbols and all workspace consumers; remove dual state authorities.
- [x] Run one final `all` build because public object/path/loading headers cross
  project DLL boundaries that local asset tests cannot fully validate.
- [x] Complete affected owner/consumer tests and bounded authored-editor/cooked-game
  lifecycle coverage; expand only for an identified uncovered risk.
- [x] Update owning contracts, API usage guidance, compatibility differences, and
  evidence; close this plan only when every required gate is satisfied.

Acceptance: no obsolete public wrappers or hidden migration fallbacks; final
relevant inputs have passing evidence and the documented supported UE subset is
accurate. No full native-test or GPU qualification sweep is implied by this plan.

## Validation and Handoff

Follow [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows.
Record tested revision, target/case selection, configuration, environment, raw
receipt location under ignored `Build/`, and remaining gates per stage. Reuse
unchanged relevant evidence; documentation-only work needs no native build.

Update [Asset Packages](../Runtime/Assets/Packages/AssetPackages.md),
[Asset Catalog and Mutation](../Runtime/Assets/Packages/AssetCatalogAndMutation.md),
and [Cooking](../Runtime/Assets/Build/Cooking.md) as their behavior ships. Add
separate lasting streaming/primary-management contracts if those responsibilities
outgrow the package document. The plan describes future decisions, not current APIs.

Implementation commits include `Plan: Documentation/Plans/UEAlignedAssetManagement.md`
and the exact applicable `Stage: Stage N: <stage title>` trailer. Do not mark
implementation stages complete when only this planning document has been validated.

## Stage 0 Compatibility Matrix and Evidence

Baseline: `34eb2b1c11264dad931dd39ba0cabbeb17d1c0fc`. No licensed UE source
was available; unspecified behavior below is a Durin decision. This section
records selected future behavior, not shipped API claims.

### Reference and selected declarations

Epic's public pages identify themselves as UE 5.8, inspected 2026-11-09.
Container and delegate spelling is deliberately adapted to Durin's C++ library.

| Reference | Observed declaration/shape | Durin selection |
| --- | --- | --- |
| [Async params](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStreamableAsyncLoadParams) | `TargetsToStream`, `OnComplete`, `OnCancel`, `Priority`, `bManageActiveHandle`; also stalled, update, JIT and download fields | `Engine/StreamableManager.h`: vector of soft paths, handle-reference callbacks, int32 priority, managed flag; debug name and uniform ExpectedClass validation are Durin params extensions. Omit stalled/update/JIT/download options until supported. |
| [Async request](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStreamableManager/RequestAsyncLoad) | Params rvalue, separate debug name and source location; legacy container/delegate overload | By-value params only initially; no source-location or legacy overload compatibility promise. |
| [Handle](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStreamableHandle) | Active retention, separate completed/canceled/released flags, child handles, optional unified error | Keep `Asset/StreamableHandle.h`; add UE-shaped observations, per-target structured errors, independent completion/cancel/release; no Unreal unified-error dependency. |
| [Primary loading](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UAssetManager/LoadPrimaryAssets) | IDs, bundles, `FAssetManagerLoadParams`, source location; legacy callback/priority overload | `Engine/AssetManager.h`: `DAssetManager`, spans of IDs/names, by-value params, no source location; manager retains independently. |
| [Rules](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FPrimaryAssetRules) | Priority, ChunkId, bApplyRecursively, CookRule; non-default override and inherited Cook rules | `Engine/AssetManagerTypes.h`; explicit optional per-ID overrides allow resetting defaults. |
| [Cook enum](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/EPrimaryAssetCookRule) | Unknown, NeverCook, ProductionNeverCook, DevelopmentAlwaysProductionNeverCook, DevelopmentAlwaysProductionUnknownCook, AlwaysCook; two legacy aliases | Initially support Unknown/NeverCook/AlwaysCook. Reject the production/development variants until Cook has an explicit matching distribution setting; never silently map them. |

The combined-handle reference fetch timed out. Do not claim source parity for
combination flags or propagation: the explicit selected behavior below is the
initial Durin subset, with no published ignored UE options.

### Identity and synchronous diagnostics

`DObject/AssetPath.h` remains the identity header. Rename the existing class
`FObjectPath` to `FSoftObjectPath`, including hashing, archive virtuals, package
capture/read validation, redirect destinations, catalog queries and editor paths.
Inventory found one package-backed identity, used for both hard reference targets
and soft references; it has no independently serialized live-object identity.
`DObject::GetObjectPath()` continues returning its existing textual live path.
Do not rename `FObjectPathError`, resolution result structures or unrelated method
names. No alias or second serializer is needed after all workspace users migrate.
Existing strings, separators, ordering, ASCII case folding, UTF-8 and length
validation, mount rules and subobject selection remain unchanged. This intentionally
retains Durin validation rather than accepting every Unreal path spelling.

Ordinary `LoadPackage(Path, OutReport)` and `LoadObject(Path, Class, OutReport)`
return nullable pointers; typed and top-level overloads match. They add no GC
owner and do not log failures automatically. `TSoftObjectPtr::LoadSynchronous`
uses this path; `Get` remains a weak, no-I/O observation.

Detailed operations are named `LoadPackageDetailed` and `LoadObjectDetailed`
in `Asset/Load.h`, returning the existing `std::expected<Pointer,FAssetReadError>`
with optional `FAssetLoadReport`. Detailed callers opt into error processing;
ordinary callers no longer unwrap an expected. Private load services/transactions
retain their current expected contracts. Both surfaces use exactly one loader.
Reports must reset per call, cover invalid/admission/resident failures and retain
requested/resolved object paths in addition to package paths. Preserve dependency
category/context, mutation and canonicalization evidence. Do not use global errors.

Rename bootstrap to `InitializeAssetRuntime`/`ShutdownAssetRuntime` everywhere,
including Launch and native fixtures. Preserve same-configuration idempotence,
pre-CDO domain selection and partial teardown. No future policy discovery occurs
in bootstrap or CDO loads.

### Request and callback matrix

All APIs and callbacks remain GameThread-only. Every admitted async completion,
including resident hits and ordinary input errors, is delivered by the existing
pump. Completion means object readiness only. Sync APIs have no completion callback.

| Situation | Selected result and ownership |
| --- | --- |
| Empty streamable batch | Non-null completed success; async callback deferred; no retained objects. |
| Invalid/null target | Non-null handle, failure in that target slot; other slots proceed. |
| Admission/guard/shutdown rejection | Non-null terminal failed handle; no queued completion or cancel callback. |
| Batch ordering/duplicates | Preserve one result slot per input in input order; duplicates observe the same object and share backend package work. |
| Missing/type/subobject failure | Null object and detailed target error; keep successes active. First failed input slot is aggregate error. |
| Completion | `HasLoadCompleted` records finished success/failed targets; cancellation before finishing does not claim completion, and cancellation after finishing preserves that history. Callback delivery is separate. |
| Cancel | Set canceled independently even in queued-completion window; suppress completion, immediately release this consumer, invoke OnCancel once. Never cancel shared package work needed by peers. |
| Release before delivery | Record deferred release; successful objects survive through callback, then drop ownership. Release/cancel/destruction are idempotent. |
| Released success | Weak result observations may become null after GC; historical completion/error remain available. |
| Combined handles | Own child references and separate retention of successful results; flatten slots by child order, then target order. Wait for every child terminal state; a canceled child is a target error. |
| Shared combined children | Parent cancellation/release drops only its own retention/references; never call CancelHandle/ReleaseHandle on shared children. Explicit child cancellation remains visible to all parents. |
| Manager ownership | Managed requests retained until explicit release/cancel. Inspection exposes active handles/debug names. Track unmanaged requests weakly for destruction/shutdown cancellation. |
| Manager destruction | Close admission, clear manager links, cancel unfinished callbacks, release managed/combined retention; surviving handles are safe terminal observations. |
| Callback reentrancy | Move callbacks out before invoking; keep handle alive while invoking; do not iterate mutable owner containers across callback calls. |

Package-only tools retain a separately named package-request operation; never infer
an export from the package leaf. Remove the old free object-streaming APIs and
mandatory result wrapper after migration, rather than leaving a facade indefinitely.

### Baseline ownership inventory and target

| Owner/pin | Baseline implementation and migration constraint |
| --- | --- |
| Live registry | `FindPackage`/GDObjectArray provide identity, not another manager cache. Loading candidates stay private until SCC validation/PostLoad/publication. |
| Package/export graph | `DPackage::AddReferencedObjects` retains every top-level export; subobjects/hard references and outer traversal keep their normal object edges. Preserve multi-export packages. |
| Unconditional Standalone | `CreatePackage` and linker package allocation set Standalone. Clean direct sync loads therefore survive GC without a caller owner. This is a known deviation. |
| Transaction cleanup | `FPackageLoadTransaction` records packages completed by that operation. `ReleaseCache` calls nonblocking `ReleasePackageCache` to clear clean Standalone and schedule GC. The separate tools `ReleasePackages` operation collects and restores Standalone on survivors; outside disk dependencies can prevent that cleanup. Direct synchronous loads still keep incidental pins. |
| Request retention | `AssetAsyncLoad.cpp` installs strong package/object pointers before nonblocking transaction cache release. Existing two-owner tests assert last-handle collectability for newly loaded packages. A resident hit from a previous direct synchronous load can retain its preexisting Standalone pin. |
| Dirty/new assets | Cleanup rejects dirty/new packages; checked unload requires explicit discard. Preserve edits with explicit editor retention before removing incidental pins. |
| Editor/native owners | `FAssetRetentionService` uses a weak shared-state map plus strong object roots; sessions own editors, preview/thumbnail resources use strong pointers or handles; scene requests/documents/worlds own handles and reflected objects. CDO material defaults and Sandbox mesh defaults are native/reflected owners. |
| Failure/shutdown | Incomplete SCCs roll back; independently completed dependencies survive according to their owners. Shutdown closes requests, cancels/drains reads and saves, flushes catalog, retires resources, marks remaining graphs garbage. |

Stage 3 must replace clean-runtime Standalone with actual graph/handle owners,
and make dirty/new editor roots inspectable. Do not change package-to-export
edges. Remove transaction origin bookkeeping only after failure, two-owner and
multi-export tests prove replacement behavior. Unload remains a checked tools
operation; release only permits future GC.

### Primary metadata, configuration and policy

Identity and bundle value types belong below Engine in CoreDObject headers
`DObject/PrimaryAssetId.h` and `DObject/AssetBundleData.h`; Engine rules/settings
belong in `Engine/AssetManagerTypes.h`. A base-object primary-ID hook can then
return an invalid ID by default without introducing an Engine dependency.
`DPrimaryDataAsset` belongs in `Engine/PrimaryDataAsset.h`; maps use type `Map`.
Named identities use FName; textual ID parsing requires exactly one colon and
nonempty type/name. Bundle assets are sorted, deduplicated exact top-level paths.

Publish primary ID and bundle data per top-level asset, beside redirect/class
metadata, through package capture/header inspection, registry projection and Cook.
Introduce a versioned optional management metadata section; retain the v10 reader
with absent metadata interpreted as no primary ID/bundles. Bump registry cache
version from 3 when the projection changes; derived caches rebuild. Never encode
management edges as hard/soft serialized reference edges or scan live objects to
answer catalog queries. Reject corrupt/duplicate bundle entries and duplicate IDs
with both source paths. Invalidate derived ID maps on every catalog revision.

Use `Configs/Project.yaml` alongside `FProjectGameSettingsStore` as the settings
extension point, with a dedicated AssetManager section/store. Existing game
settings preserve unrelated sections. Resolve an optional exact reflected manager
subclass after required project modules/reflection/catalog are ready; validate
inheritance before construction. Reject Blueprint discovery settings explicitly.
Do not extend pre-CDO runtime initialization to construct the policy object.

ID redirects apply type then full ID with cycle/depth detection (32 steps).
Missing redirected IDs fail explicitly. Relocations with stable logical IDs update
catalog paths; changing logical names requires an explicit ID redirect. Delete
invalidates mappings. Runtime dynamic registration is optional and must reject
collisions with persistent IDs, scoped to the manager lifetime.

Manager primary state has a generation number and separate committed/pending
bundle sets. Keep previous usable state until the complete replacement succeeds;
failure/cancellation preserves it. Newer requests invalidate older callbacks.
Unload increments the generation and drops only manager ownership. Releasing
an observation handle does not release primary state. Multiple callers do not
create implicit leases. Expand only declared bundle targets plus normal hard
dependencies; recursive soft-bundle expansion is unsupported initially.

Rules default to Priority=0, ChunkId=-1, bApplyRecursively=true, CookRule=Unknown.
Apply project defaults, then type rules, then explicit optional per-ID fields.
Higher-priority managers win recursive secondary policy; equal-priority
incompatible non-default Cook rules fail deterministically with both IDs.
Unknown defers to existing Cook roots/discovery. AlwaysCook adds roots;
NeverCook excludes optional roots but conflicts with required roots/hard closure
and fails Cook. Never drop a required hard dependency silently. Recursive policy
uses a visited set over declared management/hard-dependency edges. ChunkId other
than -1 fails until physical chunk packaging exists. Failed Cook publishes nothing.

### Consumer and validation inventory

Raw symbol inventory: ignored `Build/asset-api-inventory.txt` (1,106 matches,
181 files across all three workspace projects; excludes third-party sources).
Reproduce using `rg` for load/path/bootstrap/request symbols over each project's
Source and Tests roots. Do a fresh removed-symbol search at each migration gate.

| Workspace project | Affected owners/consumers and bounded validation |
| --- | --- |
| Engine | CoreDObject archive/capture/read/linker and soft pointers; AssetRegistry catalog/cache/redirect resolution; Engine loader/transactions/material defaults/scene requests; Launch; AssetTools/AssetMaintenance; DurinEd sessions/retention/previews/thumbnails; ContentBrowser and editor families; DurinAssetTool/StudioCubeGenerate. Validate CoreDObjectTests, PackageRegistryContractTests, AssetMetadataQueryTests, AssetPackageTests, AssetRuntimeLifecycleTests, then changed scene/editor owner targets. |
| Sandbox | PlayerPawn default mesh loading; migrate the explicit expected branch, compile Sandbox, preserve CDO domain selection. No direct load API matches in Sandbox tests. |
| RoadWeaver | RoadSceneIntegrationTests load/default scene helpers; project source has no direct load API matches. Compile RoadWeaver and the registered road scene target; run its bounded CPU cases where feasible. |

Later gates add AssetPackageReloadTests, editor session/retention/thumbnail
contracts, AssetCookTests and CookFunctionalTests for changed behavior. Query
DevTool's registry before selecting exact target names. Stage 6 retains one final
incremental `all` build for cross-project public-header/DLL coverage.

Startup integration stays with `RuntimeStartupAndSceneActivation.md`: preserve
the single frame pump and scene operation ownership; its AssetsReady observes
streamable completion/error, while GPU/compilation readiness remains separate.
Stage 1 uses lifecycle tests plus one bounded startup/domain case if normal
startup behavior changes. Stage 0 is design/inventory only: no runtime acceptance
or native execution is claimed by this document.
