# Asset Package Reload Plan

Summary: Add transactional package reload and live reference replacement, then route editor discard through saved-content restoration.

Last reviewed: 2026-09-09

Status: Archived
Completed: 2026-09-09

## Current Status

Stages 0 through 5 are complete. Saved authored Texture2D, VolumeTexture,
Material and MaterialInstance packages use transactional graph replacement;
editor Discard restores the current disk baseline, rebinds package consumers,
and retires affected history without clearing unrelated package content.
The existing package format, linker, residency registry, and render-resource
lifecycle remain authoritative.

Final qualification passed on MacOS arm64 Debug with an Apple M4 GPU. The
sandbox hides Metal from MoltenVK; the user explicitly authorized GPU execution
outside it. `AssetPackageReloadVulkanTests` now exercises shared editor Discard
against a real renderer scene, separately changes its weather and volume inputs,
and checks that restored readbacks exactly match the saved image. Runtime-product
and render-publication fault injection preserve the edited frame through GC.
A subsequent save and fresh load preserve both source identities. The five
retained PNGs were visually inspected; the edited images show the clear background
and both restored images reproduce the saved cloud image. This is offscreen scene
and shared document-service evidence; no native-window manual smoke is claimed.

The earlier `AssetCookTests` and `TextureTests` crashes shared an Archive diagnostic
bug: the error-name table omitted `UnsupportedOperation` and `UnsupportedTarget`,
shifting names and reading beyond the array for `TrailingData`. The corrected table,
a compile-time size check, and a focused regression pass. The reload coordinator's
previously disconnected bulk, PostLoad, participant, history, and render-publication
fault seams are now exercised. Sixteen coordinator failures preserve the edited
source, resident registration, cloud reference and Dirty state through GC, and a
later retry succeeds. History failure also preserves the undoable cross-package
transaction. A rejected Discard keeps close confirmation and the document open.

Final receipts (repository-relative paths):

- `./DevTool test affected --base 8d4388ac1^`: 74/74 targets passed, including
  `AssetPackageReloadTests` (10 cases), `AssetPackageTests`,
  `CoreObjectReplacementTests`, `CoreUtilityTests`, `AssetCookTests`,
  `TextureTests`, `EditorOperationTests` and `MaterialTests`;
  `Build/.agent-state/logs/20260909-234818-996921-26790-ctest.log`.
- `DURIN_TEST_KEEP_WORK=1 ./DevTool test AssetPackageReloadVulkanTests --mode qualification`
  outside the sandbox: passed;
  `Build/.agent-state/logs/20260909-234730-557462-25924-ctest.log`.
- `./DevTool test RendererResourceReloadVulkanTests --mode qualification` outside
  the sandbox: passed;
  `Build/.agent-state/logs/20260909-233829-552853-22687-ctest.log`.
- `./DevTool build`: full `all` target passed;
  `Build/.agent-state/logs/20260909-234859-057602-27755-cmake.log`.

Rendered evidence is retained under
`Engine/Binaries/MacOS/Debug/Tests/DurinEditor/AssetPackageReloadVulkanTests/Work/Runs/run-p26149-ae5c792a335f44a4b2990a2034a3f6a0/`:
`saved.png`, `weather-edited.png`, `weather-discarded.png`, `volume-edited.png`,
and `volume-discarded.png`. Each 96x64 RGBA readback contains 24,576 bytes;
both restored buffers equal the saved buffer, while both edited buffers differ.
Use `DURIN_TEST_KEEP_WORK=1` to regenerate inspectable evidence in the next run's
isolated directory.

The earlier failing receipts remain historical diagnostics, superseded by the
passing runs above: `20260909-201815-476091-13856-ctest.log` (texture crashes)
and `20260909-202038-091755-16210-ctest.log` (sandbox Metal admission).

## Goal

Make “discard changes” restore the package content currently saved on disk so
that scenes, editors, and subsequent saves all use the restored assets. If
recovery fails, preserve the original package content, references, dirty state,
and usable history; keep the document open and do not report success. The new
capability must support explicit Reload, not only the two texture windows.

## Scope

- Support saved authored DAST v9 non-World asset packages, including multiple
  top-level assets, embedded subobjects, internal reference cycles, and package
  sets that must be replaced together. The initial production integration covers
  Texture2D, VolumeTexture, and Material/MaterialInstance, with other document
  callers audited.
- Reuse the existing linker, unpublished object graphs, BulkData, registry,
  compilation, and render-resource contracts. Do not add a second package parser,
  residency table, or file format; do not write to disk or trigger source reimport.
- World/Level hot replacement, type-definition hot reload, automatic file
  monitoring, version-control rollback, and automatic repair of arbitrary plugin
  native pointers are out of scope. Unintegrated consumers require an explicit
  rejection path.
- A never-saved package has no disk baseline: Reload in this plan must explicitly
  reject it and preserve its dirty state. Removing such a package belongs to an
  explicit asset-deletion/DiscardPackage flow; Reload must not silently delete a
  still-referenced asset.

## Selected Architecture

### Ownership and reference semantics

| Owner | Responsibility |
| --- | --- |
| CoreDObject | Controlled object-registration replacement, writable reference traversal, type validation, old-to-new object mapping, GC retention, and old-graph retirement primitives; no file I/O or editor dependency |
| Engine | Read and validate disk closures, prepare unpublished graphs, resolve dependencies, coordinate asset-family preparation and commit, and provide structured Reload results and lifecycle events |
| AssetRegistry | Continue to own disk-metadata projection; Reload does not fabricate a disk save or alter referencers' disk dependencies |
| AssetTools | Editor package selection, scope display, conflict policy, and recovery requests; no object copying or reference scanning |
| DurinEd | Document state machine, edit-session quiescence, transaction preflight and post-commit invalidation, error presentation, and rebinding edited objects from old to new |
| Asset families and rendering consumers | Candidate runtime preparation, compilation-generation isolation, cache updates, and switching/retiring render resources under existing threading contracts |

An object's memory address is not an identity guarantee across Reload. Map
objects in the old package by package identity and relative Outer path, and
validate that each target type is assignable; persisted object paths retain
their existing contract. Do not infer corresponding objects from array positions
or raw addresses, and do not introduce a new permanent object-ID format.

CoreDObject writable reference traversal must cover reflected properties,
structures, fixed and dynamic arrays, and maps. Changed map keys require index
rebuilding, collision validation, and reserved rollback capacity. Existing GC
retention enumeration must not be assumed to support reference rewriting. Native
strong references and caches receive prepare, commit, and exit hooks from
registered participants; native weak references are rebound by their owners.
Ordinary weak handles may expire but must never resolve incorrectly to a new
object. Soft paths remain unchanged, while runtime soft-reference caches are
invalidated and resolved again through the existing epoch mechanism.

Objects added only in memory and absent from disk may retire with the old graph,
but if an external strong reference must be preserved and has no replacement
target, reject the entire request before commit. Reject incompatible types as
well; never silently write null. Exports present on disk but absent in memory are
created through normal loading. For cross-package cycles, prepare every skeleton
in the replacement set together: references within the set target candidate
graphs, while external dependencies retain their currently resident objects.
Do not incidentally overwrite other dirty packages.

### Saved baseline and admission

“Restore saved version” explicitly means restoring the current disk closure read
by the recovery request, not an in-memory snapshot from when a window was opened
or an undo cursor. If the disk changes externally, show the version change and
allow an explicit retry against the latest version. If the fingerprint changes
between reading and commit, return Stale rather than mixing two versions.

Read and retain a consistent set of main/bulk data and required source payloads.
Handle missing, corrupt, or incompatible versions with the existing complete-
closure validation rules. Lazy BulkData must not silently read a different file
generation after commit. If save projection remains in
ContentCommittedProjectionPending or the path is fenced, complete the existing
coordination flow before Reload.

Deduplicate requests by package, and explicitly show that multiple documents for
the same package are jointly affected. An explicit batch has all-or-nothing
in-memory commit semantics. Enforce memory and package-count budgets and reject
over-budget requests during preparation; do not claim project-wide atomicity for
unbounded batches. File reads and pure data validation may run on workers, while
DObject operations, reference scanning, and commit run on the GameThread. During
preparation, block editing, saving, reentrant reloads, and related history
operations for target packages. Before commit, revalidate package generations and
the referencer set so references added during preparation cannot escape the scan.

### State machine and failure boundary

```text
Requested -> Preflight -> Quiesce -> ReadAndPrepare -> ReadyToCommit
          -> CommitRegistrationAndReferences -> Publish -> RetireOldGraph -> Succeeded
Any pre-commit failure/cancellation -> AbortPreparedGraph -> Restore admission -> Failed/Cancelled
```

- First flush actual edit-session changes into the current in-memory state and
  complete the transaction; recovery must not discard input that has not yet
  entered a transaction. Return Busy or defer while Recording, Undoing, Redoing,
  or an asynchronous transaction remains active.
- Quiesce blocks new compilation requests for the target old graph, cancels the
  selected object tasks, and waits for those tasks to reach terminal states.
  Cancellation is advisory and does not replace selected finish. Never block the
  GameThread waiting for a continuation that itself needs the GameThread, and do
  not wait on unrelated assets through finish-all. On failure, reopen work for the
  old graph and, where necessary, re-request builds for the cancelled old state so
  the previously usable resources remain available.
- Prepare graphs, dependencies, a reference-write plan, a transaction-invalidation
  plan, native participants, and required runtime products. Isolate candidate-graph
  PostLoad side effects so they do not enter global rendering or editor-visible
  lists early.
- Move every expected failure—I/O, validation, build, budget, participant rejection,
  and resource preparation—before commit. Commit executes behind a boundary where
  no half-state can be observed. Preallocate storage required for writes, and do
  not allow commit callbacks to fail, reenter, or broadcast arbitrarily. If a
  primitive cannot provide this guarantee, first implement verifiable compensation
  and pass fault injection; do not package a partial commit as an ordinary failure.
- Notify observers, invalidate related history, and establish the new package save
  checkpoint only after publication. Retain the old graph until every threaded
  consumer detaches. Retire GPU/C++ resources through existing deferred-cleanup
  rules rather than deleting them immediately.
- Succeeded means the authored content, references, and resource switches required
  for recovery have been accepted. If render commit requires a completion receipt,
  remain Pending until that receipt completes. Subsequent device loss follows the
  existing resource-recovery contract and must not republish unsaved content.
  Resource-preparation failure preserves the old graph and returns failure.

### Editor and history contract

Shared Discard calls only the package-recovery service and no longer accepts an
optional void “restore” callback. Asynchronous recovery uses explicit
Pending/Succeeded/Failed/Cancelled results. Preserve close confirmation while
Pending and on failure; only after success rebind every document for the package
and complete the original close request. Window destruction or module shutdown
must detach UI observers and safely terminate or settle accepted operations;
completion notification must not be managed by capturing raw Widget pointers.

On success, enumerate references in old-graph targets, package participants, and
history payloads to invalidate each affected transaction in full. Do not merely
replace transaction object pointers and allow old values to overwrite restored
content later. Retire a cross-package transaction in full, but do not undo changes
already applied to another package or clear that package's dirty flag; invalidate
its save checkpoint if necessary. Preserve unrelated transactions. Preflight the
invalidation plan and reserve its resources before commit, then execute it after
reference commit and before externally visible successful publication. On failure,
do not call ForgetPackage or ClearDirty. Other referencers must not become dirty
solely because their pointers were replaced.

## Implementation Stages

### Stage 0: Freeze reload boundaries and regression fixtures

- [x] Reproduce both texture editors' defects where the scene still changes after
  discard and a later save pollutes the disk, and establish failure assertions.
- [x] Audit live registration, unpublished linker graphs, reference traversal,
  native retention/weak caches, transaction payloads, TextureReference, and scene
  consumers; list the initial participants and deterministic rejection rules for
  unsupported asset families.
- [x] Freeze the writable-reference API, registration-switch primitive, Reload
  result, resource-preparation receipt, and budgets, including concrete header
  ownership; confirm gaps in PostLoad isolation and stable disk-closure reads.
- [x] Freeze fault-injection points and GameThread/RenderThread commit boundaries.
  If the selected atomic contract cannot be implemented, update the decision and
  rationale before Stage 1 rather than silently degrading to partial success.

Completion condition: the interface and participant inventory directly guide
implementation; regression tests capture content errors in the old implementation
rather than checking only the dirty flag. Only this stage may freeze as-yet
unnamed internal interfaces.

#### Stage 0 handoff: audited gaps and frozen interfaces

The following names are frozen designs for subsequent implementation, not already
exported production APIs. Paths are relative to `Engine/Source/`. The tests prove
only the existing defects and cannot be used to accept Stages 1–5.

| Source boundary | Observed behavior and required change |
| --- | --- |
| `Runtime/CoreDObject/Private/DObject/Package.cpp` | `InitializeAssetPackage` registers immediately and asserts on duplicate paths; the old package cannot be unloaded before attempting the candidate read. A new candidate initializer sets only package identity, while registration switching replaces the existing entry's value in place and forbids commit-time allocation through erase/emplace. |
| `Runtime/CoreDObject/Public/DObject/DObjectArray.h` | All objects share handle and Outer indexes. Add candidate-graph visibility to the same object system; LiveOnly queries exclude candidates while GC can still retain them. Old handles preserve the old generation until retirement; do not swap pointers in slots to simulate rebinding. |
| `Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp` | Object operations pass only a local `DObject*` to the collector, and Map uses const traversal. `ForEachObjectReference` does not invoke the object's native `AddReferencedObjects`. The collector cannot directly serve as a writable visitor. |
| `Runtime/CoreDObject/Public/DObject/ContainerOps.h` | Fixed/dynamic containers and structures require recursive preparation. Existing STL adapters provide TransactionalCommit only when swap is noexcept. Rebuild Map keys and values in detached storage; an InsertCopy DuplicateKey fails during preparation. Accept only adapters with provably infallible commit. |
| `Runtime/CoreDObject/Private/DObject/StrongObjectPtr.cpp` | Native strong pointers register handle counts, not writable owner slots. Owner participants must rebind each pointer and reconcile external counts. Reject additional strong holders without a participant; do not redirect ordinary weak pointers by changing handle resolution. |
| `Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp` | Skeletons use NewObject/ForceRegistration, then immediately load dependencies, restore the ledger, and invoke PostLoad. Failure calls CollectGarbage and releases dependencies from a snapshot. Extract an explicit prepare context and in-batch resolver; candidate cancellation cleans up only objects and dependencies it owns. |
| `Runtime/Engine/Private/Asset/PackageResource.cpp` | Loose range reads reopen SegmentPath. RegisterLoosePackage replaces the manager slot and retires the previous resource. A candidate must own a validated immutable closure resource and must not call this publication entry point during preparation; replace the existing resource slot only at commit. |
| `Runtime/Engine/Private/Texture/Texture2D.cpp`, `VolumeTexture.cpp` | PostLoad synchronously builds and installs products. Each DTexture construction owns a separate TextureReference, and UpdateResource may queue render initialization. Separate candidate deserialization/migration from runtime-product preparation; ordinary PostLoad cannot run while claiming to be side-effect free. |
| `Editor/DurinEd/Private/Editor/Transactor.cpp` | ReferencesPackage scans only PackageTransitions; ForgetPackage silently returns outside Idle and cannot find references that exist only in history payloads. Use a collector to enumerate complete transaction payloads read-only, then prepare a whole-transaction retirement plan. |

CoreDObject's `Public/DObject/ObjectGraphReplacement.h` owns
`FObjectReplacementMap`、`FObjectReferenceReplacementPlan`、
`IObjectReplacementParticipant`、`FObjectGraphReplacement`。
Map keys are package identity plus relative Outer path, and the package itself is
also mapped. Use actual FName/package-path comparison semantics. Reject duplicate
paths, incompatible targets, and required out-of-graph references without targets.
`Prepare` returns diagnostics without modifying live slots. At the freeze boundary,
`Validate` rechecks source/target handles, property values, container contents, and
native-participant revisions. `Commit() noexcept` consumes only pre-reserved writes
and registration switches; `Abort() noexcept` releases candidates and reopens the
old graph; `Retire` waits for consumer receipts. Custom collectors inside structures
require a participant or explicit rejection. Never retain a native collector's
temporary slot as a commit address.

`Public/DObject/Package.h` adds `InitializePreparedAssetPackage` and
`CommitPreparedPackageRegistration`, called only by the replacement coordinator.
`Public/DObject/DObjectArray.h` owns candidate visibility and object-set revision.
`Public/DObject/SoftObjectPtr.h` continues to use `InvalidateSoftObjectCaches` for
unified invalidation; do not add a new soft-reference epoch. The entire commit runs
on the GameThread, with no arbitrary callback pumping or GC during the freeze.
When the editor waits across frames, retain a package-level lease and do not yield
the GameThread between the final scan and commit. Automatic discovery of arbitrary
plugin raw pointers is out of scope, but participant registration and module unload
must be lease-protected. Any enumerable native owner that has not declared itself
replaceable is uniformly Unsupported.

Engine's `Public/Asset/PackageReload.h` owns `FPackageReloadRequest`,
`FPackageReloadBudget`, `FPackageReloadResult`, `FPackageReloadOperation`,
and `ReloadPackages`. Result states are fixed as Pending/Succeeded/Failed/Cancelled;
failure codes distinguish
Unsupported、Unsaved、Busy、Stale、BudgetExceeded、IoError、InvalidClosure、
IncompatibleGraph、UnmappedReference、ParticipantRejected、ResourcePreparationFailed。
Diagnostics carry the package path, object path, stage, and message; rejection must
not fabricate success or partial success. Defaults allow at most 16 packages,
65,536 objects, 1,048,576 reference slots, 512 MiB of retained CPU data, and
256 MiB of candidate GPU storage per batch. Closures, decompressed source data,
write plans, runtime products, and candidate resources all count toward the budget
with overflow checks. Reject unknown GPU sizes before runtime preparation. The old
live graph does not count toward the candidate budget, but its retention duration
must be recorded. These are conservative admission limits, not performance promises.

`Private/Asset/AssetPackageLinker.h` owns `FPreparedPackageGraph` and
`PreparePackageGraphs`, reusing the same parser/codec/schema. Prepare every skeleton
in the batch before resolving in-set references; outside the set, reuse only
resident packages or record dependencies newly loaded by this request.
`Public/Asset/PackageResource.h` adds `FPreparedPackageResource`, which owns a
consistent main/bulk snapshot. Re-read the entire closure digest for the pre-commit
stale check and retain snapshots for lazy payloads; timestamps and file sizes alone
are insufficient. Asset-path fences or pending projection must complete through the
existing coordination flow and cannot be cleared by Reload. Reads and pure
validation may use existing accepted task entry points; Stages 1 and 2 do not need
to wait for the Async Task Framework Stage 5 migration.

`Public/Asset/PackageReload.h` also defines the asset-family
`IPackageReloadParticipant` and `FPackageReloadResourceReceipt`. Prepare may fail
and owns candidate products. Receipt states are Pending/Ready/Failed/Retired;
Ready means candidate resources are prepared and publication commands reserved,
not merely that BeginInit was issued. Commit must not broadcast on the GameThread.
The RenderThread switches references/proxies in one ordered publication batch;
only confirmation permits Succeeded and old-graph retirement. Queue-admission
failure must occur before CPU-reference commit. An ordinary enqueue failure after
irreversible commit cannot be treated as whole-batch failure. Existing resource-
initialization APIs do not provide this guarantee by themselves; Stage 3 must add
the receipt.

Initial participants and deterministic rejection rules:

| Participant / consumer | Admission and work |
| --- | --- |
| Texture2D compilation manager | Use selected cancel/finish for the exact DClass; block new requests, give candidates independent handles, validate generations, and reopen the old graph on failure. FinishAllCompilation is forbidden. |
| VolumeTexture | A synchronous provider with no current Texture2D asynchronous class route. Prepare voxel and GPU products separately; do not invoke Texture2D selected finish based on inheritance. |
| Material / MaterialInstance | Isolate PostLoad cache changes, compilation, parent, and parameter graph; prepare render proxies and uniform texture bindings, and rebuild MaterialRenderTypes FRHITextureReferenceRef caches. |
| DVolumetricCloudComponent / scene proxy | After rewriting reflected Weather/Base/Detail references, regenerate FRHITextureReferenceRef values in scene data; changing only the DObject pointer is insufficient. StaticMesh scene material bindings also register a participant; reject the request if preparation is impossible. |
| Texture/material windows and previews | Individually Prepare/Rebind OpenTextures' native unordered_map, OpenMaterials, parameter panels, PropertyView edited objects, and preview caches. Manage observers through operation tokens and weak owners. |
| DTransBuffer | Admit only while Idle; scan context, records, custom changes, PackageTransitions, and payload collectors. Reserve whole-transaction deletion/event storage, and preserve other package content, dirty flags, and unrelated history at commit. |
| StaticMesh inspector / LevelEditor | StaticMesh uses FReadOnlyAssetDocumentModel and is not a caller of the two textures' shared Discard path; add no Reload support. The Level World recovery path retains its existing ownership boundary. |
| Unregistered asset families, unknown derived types, unknown native consumers | Use an exact-class allowlist and participant-capability preflight. TextureCube, StaticMesh, World/Level, type definitions, cooked packages, and unsaved packages must not pass through broad Texture/Asset base-class admission. |

Fault-injection points are fixed at PreflightBudget, QuiesceSelected, ReadMain, ReadBulk,
CreateSkeleton、ResolveDependency、ApplyValues、RestoreLedger、PreparePostLoad、
PrepareReferences、PrepareNativeParticipant、PrepareHistory、PrepareRuntimeProduct、
ReserveRenderPublish、RevalidateDisk、RevalidateReferencers、BeforeCommit。
Select injection by package/object ordinal and include failure in the second package
of a batch. For every failure, verify the old registry/handles, source identity,
strong references/maps, dirty state, history, and candidate-resource count. Inside
an otherwise infallible Commit, tests may pause only to observe the boundary; do
not add hooks that return ordinary failures. If compensating rollback is required
instead of a noexcept swap, record the primitive's decision and fault-injection
evidence before accepting Stage 1. Never silently weaken the atomicity requirement.

Validation: `Win64-Debug-DurinEditor`,
`DevTool.bat test AssetDiscardCharacterizationTests --mode characterization`
passed both cases; receipt:
`Build/.agent-state/logs/20260907-153330-060182-24752-ctest.log`。
Each case first uses Save/Unload/Load to verify the disk baseline, then has a real
cloud component retain the texture and invokes the editor's shared Discard path.
Two `EXPECT_EQ(source identity, saved identity)` checks are captured by
`EXPECT_NONFATAL_FAILURE`, proving respectively that memory was not restored and
that a later save polluted disk. Stage 4 must remove the capture wrappers and move
these assertions into normal feature validation. This stage created no GPU scene,
render screenshot, or real window-close acceptance evidence; the relevant matrix
remains for Stages 3–5.

Delivery validation: `DevTool.bat test affected` selected 78 targets because of
the newly registered target. The build succeeded and 77 passed. In
`CoreConcurrencyTests`,
`FAsyncOperationGroupTests.SharedResultAliasesAndExternalSourcesRetainModuleStorage`
failed an immediate Join readiness assertion in the unmodified
`AsyncOperationGroupTests.cpp:260`. The initial failure receipt is retained:
`Build/.agent-state/logs/20260907-153705-870975-33144-ctest.log`。
The case passed in isolation, and all 176 cases in a subsequent full
`DevTool.bat test CoreConcurrencyTests` run passed; receipt:
`Build/.agent-state/logs/20260907-153940-356314-17236-CoreConcurrencyTests.log`。
This is not evidence that the first affected run was fully green. This stage did
not modify the task system or its tests. `doc validate --scope changed`,
`doc plan validate --scope all`, and `git diff --check` passed.

### Stage 1: Add controlled object graph replacement primitives

Depends on Stage 0.

Implementation detail: expose `TryCommit`, placing the fallible final Validate and
internal `CommitPrepared() noexcept` in the same GameThread call so the caller
cannot yield between Validate and Commit. Ordinary failure or partial success
remains forbidden during commit. `IncludeUnpublished` is an internal GC/replacement
query scope over the same GDObjectArray; ordinary LiveOnly/IncludeTemplates queries
expose neither candidates nor retiring graphs, and no residency table is added.

To address the layering gap identified by the user, first add CopyConstruct and
CopyAssign support plus generated-property lifecycle wiring to shared `ContainerOps`,
then have the replacement plan use `FReflectedValueStorage`. Do not retain an
independent container-cloning implementation in Reload. Raise the descriptor-table
version to 2 without changing the disk format. Explicitly reject nested non-copyable
elements. A failed copy construction cleans up partially constructed values; copy
assignment copies first and then swaps without throwing, supports self-assignment,
and preserves the target's pre-failure content.

- [x] Implement old/new graph mapping, type checks, reflected/container reference-
  write plans, and native-participant contracts in CoreDObject, including Map keys
  and nested fields, while keeping GC enumeration and writing responsibilities clear.
- [x] Implement same-path candidate-graph isolation, unique registration switching,
  soft-reference cache invalidation, and delayed retirement of old graphs without a
  parallel residency table or weak-handle misresolution caused by slot reuse.
- [x] Validate strong references, weak handles, cycles/subobjects, missing replacement
  targets, incompatible types, Map collisions, references added during preparation,
  and rejection paths for unsupported participants.

Completion condition: in-memory graph replacement without disk or editor dependencies
commits atomically. After failure, every original reference remains usable; after
success, no accessible old-graph residue or dangling reference remains.

Implementation and boundary: `Public/DObject/ObjectGraphReplacement.h` and its
Private implementation provide in-memory replacement. Index old and new graphs by
FName Outer paths, separating container snapshots from writes. Take ownership of
candidate graphs only after successful preparation. After commit, retain the
operation until every participant permits retirement, then mark the old graph and
defer destruction to GC. Strong-holder counts must be declared exactly. Native raw
pointers not enumerated by a collector must still be covered by asset-family
admission audits; do not claim automatic discovery of arbitrary C++ pointers.
Editing/task blocking and main/bulk/resource byte budgets remain responsibilities of
the later Engine coordinator. The current primitive budgets only packages, objects,
and reference slots; it does not claim a complete resource-memory budget.

Validation: all 20 `CorePropertyValueSnapshotTests` passed in
`Win64-Debug-DurinEditor`, including independent nested-container storage,
array/Map copy fault injection, destructor cleanup, and capability rejection;
receipt: `Build/.agent-state/logs/20260907-162415-175732-32936-CorePropertyValueSnapshotTests.log`.
All 79 `DevTool.bat test affected` targets passed; receipt:
`Build/.agent-state/logs/20260907-163356-433891-30868-ctest.log`。
Manual root-reference retention and handle-slot reuse tests were then added. After
fixing the new test's NewObject parameters, all 18 `CoreObjectReplacementTests`
passed; final receipt:
`Build/.agent-state/logs/20260907-163637-703882-39692-CoreObjectReplacementTests.log`。
Coverage includes scalar/fixed/nested/struct/Map keys and values, candidate-internal
and cross-package cycles, subobjects/deleted objects, incompatible types, collisions,
stale state, unsupported native holders, exact strong-pointer transfer, delayed
receipts, and late-arriving references. This is CoreDObject CPU acceptance, not
disk Reload or rendering/window acceptance.

### Stage 2: Prepare saved packages without publishing live state

Depends on Stage 1.

- [x] Extract candidate-graph preparation from the Engine's existing load transaction,
  reusing the canonical linker, schema, authored provenance, and dependency closure
  without returning the currently resident object through ordinary LoadPackage.
- [x] Implement consistent closure reads, fingerprint revalidation, BulkData lifetime,
  and budgets; define reuse of resident external dependencies and cycle binding
  within the replacement set.
- [x] Implement structured results for new packages, missing/corrupt files, fenced
  paths, and Unsupported/Busy/Stale/Cancelled, releasing only this request's
  dependencies and objects when preparation fails.

Completion condition: a complete saved-version candidate graph can be produced.
Any preparation failure leaves the current package, registration, disk, and other
packages' dirty states unchanged. A multi-package preparation failure aborts the
entire batch.

#### Stage 2 progress: allocation-free hierarchy marking and batch storage limits

Candidate graph destruction uses hierarchy marking that follows existing Outer
indexes and sibling back-pointers without temporary allocations or recursion.
The traversal retains the existing template/permanent-object rules, visits private
and already-marked descendants, and does not run callbacks or physical collection.
This removes temporary traversal storage from the graph owner's failure cleanup;
it does not claim that every loader or dependency-release path is allocation-free.

Graph preparation checks aggregate retained main/bulk bytes before parsing any
package, with a 512 MiB default and overflow-safe subtraction. The boundary is
separate from parser/decoded/native/runtime allocations and external load budgets.
Tests cover the exact byte limit, a one-byte batch excess, unchanged prior output,
and a deep private hierarchy after sibling reparenting and repeated garbage marking.

Focused `CoreObjectTests` and `AssetPackageTests` regressions passed. All 82 Windows
Debug affected targets passed; receipt:
`Build/.agent-state/logs/20260909-184138-816851-20988-ctest.log`.
Documentation/plan validation and `git diff --check` passed. Full request admission,
automatic dependency cleanup, complete budgeting and resource preparation remain
required before Stage 2 can close.

#### Stage 2 progress: guarded callbacks and replacement composition

Candidate constructors, field restoration, ledger restoration and final validation
now run behind the existing live-operation guard, after explicit external loading.
Ignored load/save/unload rejections invalidate the batch. Throwing callbacks return
InvalidClosure and preserve previous outputs; bad allocation remains BudgetExceeded.
Tests cover second-package failures at every skeleton/value/ledger seam, including
a real throwing constructor with an already constructed default inner.

A saved-file CPU composition regression passes prepared two-package cycles to
CoreDObject replacement, checks outside-reference rebinding without dirtying the
referencer, releases graph owners, retires old handles, and saves the restored
content. It does not run PostLoad or publish runtime resources.

Both focused regressions passed. All 82 Windows Debug affected targets passed;
receipt: `Build/.agent-state/logs/20260909-183019-752057-22632-ctest.log`.
The first affected build failed with TextureEditor's LNK1103 corrupt object debug
information; deleting only the named generated object and rerunning the same
command resolved it. Documentation and plan validation and `git diff --check`
passed. Stage 2's two remaining tasks are still open.

#### Stage 2 progress: owned external dependencies

Dependency ownership uses a caller-owned `FAssetPackageLoadScope`. Candidate
preparation may explicitly borrow the scope to load non-resident external
dependencies. On success or failure, the coordinator retains exact weak records,
releases them explicitly after candidate cleanup, and retains the scope for retry
when InUse. Release failure must not be hidden in a destructor. Source audit found
that ordinary Release protects disk dependencies of every resident package. Failed
Reload cleanup needs an exception that may ignore disk dependencies from explicitly
identified current target packages while still protecting objects through actual
live references and dirty/new state. Exceptions must match exact weak identity,
not paths that could apply to a replacement loaded later. Ordinary callers omit
this set and retain the existing protection rules.

That channel is implemented. Scoped loading requires every replacement target to
be resident, preventing ordinary recursive loading of out-of-set dependencies from
publishing a replacement target; implicit loading remains rejected without a scope.
Both scope package records and saved-dependency exceptions match exact instances.
A newly published same-path package is not selected for release while an old private
graph remains alive. Candidate preparation rejects active loads and projection
fences on targets or external dependencies, and checks fences again after disk
revalidation; rejection does not clear a fence. Ordinary failed load transactions
also retire successfully loaded nested bulk slots, fixing resources left behind
after object rollback.

Five new tests cover scoped transitive dependency closures; failure and cancellation
in the second package; candidate/live-reference/dirty protection and retry; preserving
unrelated dirty packages; fences before, during, and after disk revalidation;
rejection of non-resident replacement targets; same-path publication while the old
graph remains alive; saved-dependency exceptions not following new instances; and
nested bulk cleanup on failure. The targeted selection, including prior cases,
passed 10/10. All 80 Windows Debug `test affected` targets passed; receipt:
`Build/.agent-state/logs/20260907-183435-132689-28812-ctest.log`. Documentation/plan
validation and `git diff --check` passed. No GPU qualification was added.

The caller owns the scope, so preparation failure neither implicitly destroys it
nor discards its records. The complete request coordinator is not implemented, so
the two open tasks in this stage remain unchecked. Unsaved/lease/asset-family
admission, complete byte accounting, and public Reload result mapping remain to be
implemented. This internal loading channel is not a Reload-success entry point.

#### Stage 2 progress: private batch value graphs

`FPreparedPackageGraph` / `PreparePackageGraphs` now live at the selected private
linker boundary. They reuse canonical/schema validation, skeleton construction,
field restoration, and ledger restoration extracted from the ordinary loader.
Inputs are validated immutable closures. Validate the entire batch and explicit
exact-class admission first, create every private skeleton second, and apply values
last. Default inner objects are reused through private enumeration. Internal-cycle
and subobject references bind to candidates; external dependencies are retained by
resident exact identity, with missing dependencies explicitly rejecting implicit
loads. Outputs retain load-version/migration metadata, ordinary PostLoad does not
run, and the state means only ValuesPrepared.

Candidates and external objects are strongly retained across GC, and failure
preserves existing outputs. The owner marks only this request's private hierarchy
as garbage; it does not invoke global GC, global load-difference release, or disk
writes. Package/object count limits, latched cancellation, reentrant Busy, and a
whole-batch disk-digest recheck at the end are connected. See
[Asset Packages](../../../Runtime/Assets/AssetPackages.md) for the lasting boundary.

Four new real-saved-file tests cover two-package cycles and subobjects, default-inner
uniqueness, saved scalar/Array/Map values, Forced provenance, unchanged live dirty/
revision state, GC during preparation, Busy reentry, second-package skeleton/value/
ledger failures, cancellation/budget/exact-class rejection, disk Stale, preservation
of existing outputs, resident external dependencies and missing-dependency rejection,
and lazy saved-bulk reads with unchanged live slots. Targeted tests passed 4/4.
Implementation fixed a DLL friend declaration, ledger path comparison, and removal
of Registry dependencies before fixture cleanup saves; production load admission
was not loosened.

All 80 Windows Debug delivery-validation `test affected` targets passed; receipt:
`Build/.agent-state/logs/20260907-175141-741489-41340-ctest.log`。
Documentation/plan validation and `git diff --check` passed. No GPU qualification
was added.

Value-graph extraction does not provide complete request coordination. Scope and
fence checks were connected later, but Unsaved handling, leases, automatic failure
cleanup, complete CPU/GPU byte accounting, asset-family-isolated PostLoad, and
public structured Reload result mapping remain unimplemented. The candidate-graph
extraction task for this stage is complete, while the other two tasks remain open;
value-graph preparation is not complete Reload success.

#### Stage 2 progress: explicit package load bindings

`AssetPackageArchive.h` adds `FPackageLoadBindings`, through which callers supply
a bulk handle and external-object resolver. `LoadAuthoredObject` no longer accesses
the live load service or resource registry itself. The ordinary linker explicitly
supplies the existing resource and ordinary resolver; candidate callers may supply
private skeletons and immutable snapshots. Missing bindings, resolver rejection,
or a successful resolver that returns null all fail without falling back to the
currently resident graph. See [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
for the lasting boundary.

The interface was narrowed to `FPackageLoadBindings` so its name does not imply
authored-only use. It still contains only a bulk handle and external-object resolver.
The ordinary linker creates the binding once outside its object loop and shares it
across all objects. Preserve this centralized selection point without factories,
inheritance, or load-mode branches. The candidate-load coordinator will later own
in-batch/out-of-batch dependency selection; Archive only consumes bindings. After
this naming and boundary cleanup, all 80 Windows Debug `test affected` targets
passed; receipt:
`Build/.agent-state/logs/20260907-173118-930282-35248-ctest.log`。

Two private-graph field-level tests were added. External paths can bind private
objects while a same-path live package remains present, and internal/null references
do not depend on the resolver. Registered live bulk cannot satisfy a missing binding,
and candidate lazy payloads continue to read saved bytes after explicit binding/
storage owners exit and disk contents change. Tests check live registration, fields,
dirty/revision state, and object counts; field-level reads are not treated as complete
candidate-graph preparation. Batch skeletons, schema/ledger stage extraction,
dependency ownership, cancellation/admission, and PostLoad isolation remained
incomplete at this point; serializer/struct-migration callbacks still need separate
admission.

Both targeted Windows Debug tests passed. All 80 delivery `test affected` targets
passed, including 117/117 `AssetPackageTests`; receipt:
`Build/.agent-state/logs/20260907-171121-888796-11252-ctest.log`。
Implementation corrected test-construction arguments, private-state queries, Archive
diagnostic matching, and deletion cleanup after saving fixtures. No additional GPU
qualification was performed.

#### Stage 2 progress: immutable storage preparation

`FPreparedPackageResource` now lives in `Public/Asset/PackageResource.h`. `Read`
uses the existing codec to parse main data and the directory; `Prepare` validates
and retains external bulk snapshots; `Revalidate` recomputes complete main/bulk
digests. Preparation does not register resources, construct objects, restore backups,
or write to disk, and failure preserves existing output. Lazy ranges share snapshots,
so payload owners can still read the original bytes after disk replacement/deletion
and after the preparation owner exits. See [BulkData](../../../Runtime/Assets/BulkData.md)
for lasting rules.

This budget covers only retained main/bulk bytes, not parser scratch space, decoded
values, reference plans, or runtime products. `Ready` means only that storage
preparation succeeded; it is not Reload success and cannot bypass fences, pending
projection, or edit/save leases. The next step still needs to extract
`FPreparedPackageGraph`/batch skeletons and a private parse context, isolate PostLoad,
and integrate request-owned dependencies, complete budgets, and structured Reload
admission/cancellation results. The stage tasks above therefore remain unchecked.
Dependency cleanup can reuse exact weak-identity records from the existing
`FAssetPackageLoadScope` rather than applying the linker's global
`CapturePackageLoadSnapshot` difference release to candidate rollback.

Validation (Windows Debug): six storage tests and one real saved-package test were
added, covering same-size/same-timestamp content changes, main replacement during
reads, missing/corrupt bulk, no backup restoration, budget boundaries and cancellation,
lazy reads after owner exit, and unchanged original object sets, registration, dirty
state, and revisions. All 80 final `test affected` targets passed, including 115/115
`AssetPackageTests`; receipt:
`Build/.agent-state/logs/20260907-165530-484015-40120-ctest.log`. During this work,
test object-enumeration API names and cleanup admission before deleting dirty fixtures
were corrected; neither change affects production recovery behavior. Documentation
and plan lifecycle validation passed. No additional GPU qualification was performed.

### Stage 3: Coordinate compilation and runtime resource publication

Depends on Stage 2. Use the currently implemented task contracts without bypassing
production-transition gates from other plans.

- [x] Implement target-graph task-admission blocking, selected cancel/finish, and
  request-generation validation. Candidate-graph tasks use independent identities;
  old completion notifications must not reapply after cancellation, commit, or
  object retirement.
- [x] Integrate candidate preparation and resource publication for Texture2D,
  VolumeTexture, and Material/MaterialInstance. Handle VolumeTexture through its
  actual compilation route rather than assuming registration in the Texture2D
  asynchronous domain.
- [x] Integrate texture-reference and cache refreshes for scenes, materials, and
  previews; complete ordered RenderThread switching and deferred cleanup of old
  resources; verify that old resources remain usable when recovery fails.
- [x] Build the Engine Reload coordinator and staged notifications. Validate
  preparation failure, cancellation, close, shutdown, stale callbacks, render-
  admission failure, and that unrelated tasks are not globally drained.

Completion condition: assets can reload while scenes continuously reference them,
restoring both CPU content and rendered output. Errors or cancellation leave no
half-switched state, and old tasks completing after commit cannot overwrite the
restored result.

### Stage 4: Route editor discard through package reload

Depends on Stage 3.

- [x] Add a package-level recovery policy to AssetTools. Integrate Pending close
  flow, conflict blocking, same-package document impact scope, and error messages
  in DurinEd, with explicit recovery rejection for never-saved packages.
- [x] Implement transaction-invalidation preflight/commit, cross-package history
  checkpoint handling, and old-to-new edited-object rebinding for documents.
- [x] Migrate the Texture2D, VolumeTexture, and Material editors to the shared
  recovery entry point. Remove superseded discard-only snapshots and dirty-clearing
  paths that do not recover content. Audit callers such as StaticMesh; explicitly
  reject unintegrated types while preserving their original dirty state.
- [x] Verify that requested documents close only after successful recovery, while
  other same-package documents continue on new objects. Failures are retryable,
  and closing one window does not lose the resource state of remaining documents.

Completion condition: no Discard entry point reports success without recovery. Both
texture P1 regressions pass, and Material no longer uses a window-open baseline as
the representation of disk-saved state.

### Stage 5: Qualify recovery and publish lasting contracts

Depends on Stage 4.

- [x] Complete the acceptance matrix below and affected-module validation. Record
  actual test targets, configurations, command receipts, and manual scene evidence;
  documentation validation or compilation success alone cannot complete the plan.
- [x] Document implemented ownership, failure, reference, and close semantics in
  AssetPackages, AssetCompilation, Transactors, and the relevant rendering/editor
  contracts, linking from this plan to those authoritative documents.
- [x] Remove compatibility bridges and duplicate entry points, confirm no new format
  or second residency table was added, complete the final cross-module build, and
  update this plan's status and evidence before closing it.

Completion condition: every required scenario passes, fault paths reproducibly
preserve the original state, lasting rules live in the appropriate contract
documents, and rejection behavior for unintegrated asset types has tests.

Stage 5 acceptance evidence combines the saved-baseline GPU scene test with
`AssetPackageReloadTests` for latest-save/dirty-activation/rejected-save behavior,
all exports, unsupported families, admission, failure retry, batches and history.
`AssetPackageTests` and `AssetBulkContainerTests` retain the Stage 2 closure,
fence, dependency, cycle, stale-disk and cancellation coverage;
`CoreObjectReplacementTests` retains the Stage 1 strong/weak/soft, container,
subobject, late-reference, participant and deferred-retirement coverage.
`EditorOperationTests` covers document close refusal and retry, workspace lifetime,
and transaction shutdown. The lasting contracts are linked below.

## Acceptance Matrix

| Scenario | Required observation |
| --- | --- |
| Modify and discard a scene-referenced Texture2D/VolumeTexture | Authored values, source data, and rendered output return to the disk version; another save followed by a fresh load contains none of the discarded content |
| Save B successfully, edit C, then discard | Restore B; a failed save does not establish a new disk baseline |
| Package was already dirty before opening the document, or edit history was evicted | Still restore from disk without depending on window snapshots or complete undo history |
| Multiple top-level exports, multiple documents, added/deleted subobjects, and cycles | Switch consistently at package scope; an unmappable out-of-graph strong reference fails the request |
| Strong/weak/soft references, native caches, and Map keys | Strong references switch correctly, weak references safely expire or rebind, soft paths remain unchanged, and containers are not corrupted |
| Cross-package transactions and history payloads reference the old graph | Retire related transactions in full, preserve unrelated history, and do not clear other packages' content or dirty state |
| Cancellation during compilation, late results, or new edits/references after preparation | No stale application occurs; conflicts return Busy/Stale and the original state remains usable |
| I/O, bulk, schema, build, resource-admission, or participant failure | Preserve the original graph, references, dirty state, history, and open documents, with no candidate-graph leaks |
| New unsaved package, missing file, external modification, or fenced path | Return explicit diagnostics without guessing a baseline, overwriting disk, or fabricating success |
| Second package in a batch fails, GC pressure, window destruction, or module shutdown | No partial commit, dangling callback, or leak; old render storage retires across the correct thread boundary |

## Validation and Dependencies

Before implementation, follow the [Build and run workflow](../../../Agents/BuildAndRun.md)
and [Testing workflow](../../../Agents/Testing.md). Follow repository test-registration
rules when adding native tests. Committing this plan runs only documentation and
plan lifecycle validation; it does not complete any implementation stage above.

The archived [Async Task Framework Refactor](AsyncTaskFrameworkRefactor.md)
qualified task integration for package reads and texture compilation. Reuse the
landed production APIs, and explicitly record any additional capability this plan
needs before depending on it. Do not create a second scheduler or migrate its pilot
early.

UE's [ReloadPackages](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/ReloadPackages)
and
[FPackageReloadedEvent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPackageReloadedEvent)
provide references for package replacement, reference mapping, and notifications.
Atomic failure, participant admission, and resource-preparation gates in this plan
are selected Durin requirements; public UE API documentation is not implementation
evidence for those guarantees.

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectArray.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectPtr.h`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageReload.h`
- `Engine/Source/Runtime/Engine/Private/Asset/PackageReload.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/Load.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageSerialization.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceRootWindow.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceManager.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetPackageReloadTests.cpp`

## Related Contracts

- [Asset packages](../../../Runtime/Assets/AssetPackages.md)
- [BulkData](../../../Runtime/Assets/BulkData.md)
- [Asset compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Async asset operations](../../../Editor/Architecture/AsyncAssetOperations.md)
- [Transactors](../../../Editor/Architecture/Transactors.md)
- [Render resource lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
