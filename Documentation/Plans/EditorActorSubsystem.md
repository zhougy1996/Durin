# Editor Actor Subsystem Plan

Summary: Move general Actor and Component editing and structural history into DurinEd through DEditorActorSubsystem, and unify LevelEditor callers while retaining document-owned selection.

Last reviewed: 2026-10-17

Status: Completed
Completed: 2026-10-17

## Current Status

All stages completed on 2026-10-17. General Actor/Component edits and structural
history are DurinEd-owned, production callers use the host subsystem, and
specialized SkyBox/StaticMesh commands compose opaque presence changes. Selection
and gizmo gestures retain their document/LevelEditor ownership. No additional
history owner or DurinEd dependency on LevelEditor/UI was introduced.

The selected UE reference remains a responsibility boundary; selection stays
with the document as explicitly chosen. Stage 0 contracts were resolved before
production migration. Stages 1-4 were implemented and validated together as
authorized by the user.

The follow-up review is complete: structural replay and failure recovery now
cover detached owners, callback interruption and World transition/closing gates.
Single oversized edits remain undoable under a soft history budget. The final
review matrix passed 699 whole-target cases plus the isolated selections below;
obsolete capacity assertions and historical attachment-test naming were cleaned up.
The subsequent UE 5.3.2 source audit confirmed immediate Undo for oversized
transactions while documenting the different trimming time, and removed repeated
admission checks and dead branches without adding states or changing budgets.
The byte-only capacity cleanup is also complete: no count cap or dynamic-limit
API remains. Old-history trimming follows the inspected UE byte comparison,
deferred until successful finalization to preserve failed/canceled history without
new recovery storage. The final matrix passed 702 cases and isolated transaction
validation; see the capacity-cleanup evidence below.

### Resolved Stage 0 contracts

- `DEditorActorSubsystem` owns `SpawnActor`, `DestroyActors`, `DuplicateActors`,
  `RenameActor`, `AttachActors`, `DetachActors`, and Component add/delete/duplicate/
  rename/reparent. Every call supplies its Level or owning Actor and an explicit
  `FActorEditContext(Host, bReadOnly)`; non-document native callers must explicitly
  supply their editability policy too. No active selection is consulted.
- Results distinguish Changed, NoOp and Failed, carry an admission/domain error,
  resulting participants, presentation message, and the original transactor result
  on submission. Same-name rename and same-parent reparent are successful NoOps;
  empty batches are rejected. Batches validate all inputs before one submission.
- A private friend native-test seam injects a transactor into an independently
  constructed service. Production services require their registered host, open
  work gate, editor World ownership, stopped PIE and an idle host transactor.
- Export only `MakeActorPresenceChange` for specialized SkyBox/StaticMesh compound
  changes. It returns opaque change ownership; specialized owners keep prepared
  actors private and clean up abandoned/rejected preparations. General commands
  dispose unpublished objects on failure. History retains complete Outer trees.
- Workspace audit: Engine owns all general consumers listed above. Sandbox and
  RoadWeaver have no consumers of these helpers or editor structural mechanisms.
  StaticMesh's temporary/final renames remain specialized compound replay.
  Runtime World/Level construction, loading and gameplay remain mechanisms.
  Remaining direct Editor calls are asset preview/thumbnail scenes, new-Level
  studio-light initialization and the PIE editor camera; these are construction
  or gameplay setup rather than document structural commands.
- DurinEd registers the service beside Tools/Notifications; general replay tags
  become DurinEd. No stored preparation callbacks enter history. Relevant discovered
  targets are LevelMutationTests, ViewportTests, SkyBoxTests, SplineTests,
  EditorShellTests and WorldTests, plus the DurinEd and LevelEditor module builds.
  Test CMake registrations stop compiling migrated private LevelEditor sources.
- Validation exposed a pre-existing Execute finalization hole: rejected history
  could leave published changes without history. Candidate accounting now runs
  before replay, rejected finalization compensates before replacing redo, and
  incomplete compensation enters RecoveryRequired. This is required by the
  selected rejected-submission contract. Already-applied property recordings are
  not automatically compensated. Acceptance includes EditorOperationTests for
  transaction records and EditorPropertyTests for property-editing regression.
  The review follow-up replaced hard single-entry byte rejection with a soft
  history budget: evict oldest undoable entries and retain the newest complete
  entry even when oversized. Accounting overflow remains a real failure.

## Goal

Provide one editor-host service for general Actor editing, reusable by UI and
future Toolset adapters without private LevelEditor dependencies. Existing
Outliner, viewport, placement and Component Details paths must share admission,
preparation, transaction and replay behavior. Preserve the single DTransactor
history and existing structural Undo/Redo guarantees.

## Baseline and Migration Inventory

| Existing location | Selected destination and responsibility |
| --- | --- |
| `LevelEditor/Private/Operations/ActorMutations.*` | DurinEd Actor editing feature: public subsystem entry points and private preparation/presence/rename changes |
| `LevelEditor/Private/Panels/ActorAttachmentTransaction.*` | DurinEd Actor editing feature: attachment validation, capture and replay; panels stop constructing entries/changes |
| `LevelEditor/Private/Operations/ComponentMutations.*` | DurinEd Component editing implementation used by the subsystem; preserve add/remove/duplicate/rename/reparent behavior |
| WorldOutlinerPanel | Call service operations for create/delete/duplicate/rename/attach/detach; retain menus, confirmation and selection |
| SceneViewportPanel and LevelEditorViewportEditing | Route placement and structural shortcuts through the service; retain viewport interaction |
| DetailsComponentTree | Route structural Component edits through the service; retain tree presentation and selected Component |
| SkyBoxPlacement and StaticMeshLevelMutations | Keep specialized orchestration in LevelEditor; compose exported DurinEd structural changes instead of private ActorMutations |
| FLevelEditorContext | Keep document World/Level, Actor/Component/sub-element selection, selection anchors and focus policy |

Source roots are `Engine/Source/Editor/DurinEd`,
`Engine/Source/Editor/LevelEditor`, and `Engine/Tests/Native/EngineTests`.
Implementation must also search all source/test roots declared by
`Durin.dworkspace` (Engine, Sandbox and RoadWeaver), including direct runtime
mutation calls that may bypass the named helpers. Runtime construction, loading
and gameplay mutation are not automatically editor-service consumers.

## Selected Design

### UE reference and Durin ownership

Use Epic's [UEditorActorSubsystem API](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEditorActorSubsystem)
as the reference for an editor-wide Actor utility service: spawn, destroy,
duplicate and related Actor operations are callable independently of panels.
This reference establishes API responsibilities, not an assertion that UE's
private transaction implementation has been inspected or should be copied.
UE also exposes selection helpers; Durin deliberately keeps selection with the
document for this refactor.

- Add reflected `DEditorActorSubsystem : DEditorSubsystem` to DurinEd and
  register it through the existing Editor subsystem provider mechanism. Resolve
  it through the admitting host's `GetEditorSubsystem<DEditorActorSubsystem>()`.
- Put public interfaces under `DurinEd/Public/Actors/` and implementation under
  `DurinEd/Private/Actors/`; Component helpers may have a sibling `Components/`
  feature directory. Keep `Editor/` focused on host/subsystem lifecycle.
- Expose Actor spawn/delete/duplicate/rename/attach/detach and owned Component
  add/delete/duplicate/rename/reparent through this service. Component mechanics
  remain separate implementation units; this scope does not introduce a second
  Component subsystem or a parallel public static command facade.
- DurinEd depends on Runtime mechanisms only. It must not depend on LevelEditor,
  MainFrame, widgets, ImGui, or a document's private context. Runtime Actor/Level
  APIs remain the mechanism boundary and acquire no editor-service dependency.
- Keep SkyBox/material/static-mesh policy in its feature owner. Generic Actor
  editing must not absorb asset-specific preparation or conversion policy.

### Targets, admission and results

- Every mutation identifies its Level and participants explicitly. The service
  does not cache the active World/Level or infer a mutation target from selection.
- Use a narrow edit context carrying required host/document editability facts,
  rather than passing `FLevelEditorContext` or a widget. Read-only state must be
  supplied from the document owner, with an explicit policy for non-document
  callers; a default `false` boolean must not silently bypass admission.
- Revalidate service/host lifetime, game-thread access, target World/Level
  ownership, supported play state, membership, class/creation constraints,
  duplicate participants and hierarchy constraints before effects. Reject
  mismatched hosts, expired targets and unsupported cross-Level operations.
- The normal service path uses its host's DTransactor. Preserve an explicit
  transactor injection seam for independent tests/internal operations without
  introducing another production history owner or falling back to unrecorded
  mutations when history is unavailable.
- Return owned domain results with changed status, resulting participants and
  actionable failure information. Do not infer success solely from an empty
  message or a non-null Component. Preserve replay failure causes and format
  presentation text once at the appropriate boundary. Stage 0 fixes exact types
  and no-op/batch semantics before migrating callers.
- Selection changes, confirmation, focus, notifications and error presentation
  belong to adapters. The service must work without an open LevelEditor widget.

### Preparation, composition and history

- Keep DTransBuffer as the only ordered history. Public synchronous operations
  perform validation/preparation and submit one logical transaction; no service
  history, ID bridge or remote begin/end transaction session is introduced.
- Separate internal preparation from submission. Export only the narrow native
  structural-change composition API required by SkyBox and StaticMesh workflows,
  using opaque ITransactionCustomChange ownership rather than exposing concrete
  replay classes. Consumers can prepare several changes and submit their existing
  compound operation once; they must not call auto-committing commands in a loop.
- Define ownership/cleanup for prepared but unpublished objects, failed
  preparation, abandoned plans and rejected history submission. Complete copying,
  cross-graph reference remapping, reconstruction and derived-state rebuilding
  before publication; preparation must not dirty packages or expose boundary
  attachments. Do not retain arbitrary UI callbacks in history.
- Preserve identity across absent membership, complete Outer-subtree GC
  traversal, authored ordering, root/primary-camera state, attachment transforms,
  registration/lifecycle and package revision/checkpoint behavior. Deletion in
  editor history remains reversible membership removal, not irreversible runtime
  destruction or resurrection by name/path.
- Preserve validate-before-write and reverse compensation, including compound
  operation failures. A rejected edit leaves no visible partial mutation or new
  history; incomplete compensation must retain the existing recovery-required
  disposition rather than being reported as success.
- Move custom-change module tags with their actual implementation owner. General
  changes become DurinEd-owned; specialized LevelEditor changes keep their own
  tags. Audit stored callbacks and module unload/retirement dependencies.
- Reuse current transaction/package events for refresh and history consumers.
  Add a domain event only for a demonstrated missing consumer need; if needed,
  define execution and Undo/Redo timing after successful completion and prevent
  duplicate or premature publication.

### Scope boundaries

Selection remains in FLevelEditorContext, including reconciliation after deletion
and Undo/Redo. The subsystem neither retains that context nor selects returned
objects. This work does not change property editing, introduce general Actor
conversion/clipboard features, or implement Python/MCP tools. Future tools must
call this service; coordinate that boundary with the
[Editor Agent Toolset Expansion plan](EditorAgentToolsetExpansion.md).

Transform gizmo gestures and their heterogeneous target protocol remain owned by
LevelEditor. Preserve their continuous-edit/one-gesture history semantics; do not
route every drag tick through a new auto-committing command. Attachment/reparent
transform handling is included because it belongs to the migrated structural
changes. Standalone general transform commands are a follow-up unless Stage 0
identifies an existing structural caller that requires one.

## Implementation Stages

### Stage 0: Freeze service and composition contracts

Outcome: concrete signatures and a complete consumer/validation map.
Dependencies: none.

- [x] Audit Actor/Component edits and structural changes across every workspace
  project; distinguish runtime mechanisms, general editing and specialized policy.
- [x] Finalize public operations, target/editability context, typed results,
  no-op behavior, supported batch atomicity and internal test injection.
- [x] Specify prepared-object ownership and the minimal cross-module composition
  API needed by SkyBox and StaticMesh; prove one logical history entry per command.
- [x] Map subsystem registration, module tags, retirement and test target/build
  dependencies; record exact affected targets from DevTool discovery.
- [x] Update this plan with resolved API choices before implementation. Completion
  requires no unresolved ownership, read-only admission or composition contract.

### Stage 1: Establish DurinEd Actor editing ownership

Outcome: working host subsystem and DurinEd-owned Actor structural history.
Dependencies: Stage 0.

- [x] Register DEditorActorSubsystem using the existing lifecycle contract.
- [x] Move general Actor preparation, presence, rename and attachment changes to
  DurinEd; implement service admission and submission without LevelEditor includes.
- [x] Export the narrow structural composition seam and migrate native test seams.
- [x] Verify host separation, unavailable/retiring service rejection, exact identity,
  GC retention, attach/detach replay, rejected submission and abandoned preparation.
- [x] Completion: owning module and relevant test targets compile; focused tests
  pass, with migrated changes tagged to the correct module.

### Stage 2: Consolidate Component editing infrastructure

Outcome: owned Component editing uses the same admission/history foundation.
Dependencies: Stage 1.

- [x] Move Component add/delete/duplicate/rename/reparent implementation to DurinEd
  and expose the agreed service methods; share only mechanisms with identical
  semantics, keeping Actor and Component replay constraints explicit.
- [x] Preserve creation-method restrictions, root protection, authored ownership,
  subtree retention, registration and child attachment restoration.
- [x] Verify Component errors and no-ops through the new result contract and
  reject detached, foreign-owner, generated or otherwise unsupported targets.
- [x] Completion: Component structural regression tests pass through the new
  boundary, including isolated execution of new lifecycle/failure cases.

### Stage 3: Unify existing editor callers

Outcome: all existing general editing paths use the new service or composition API.
Dependencies: Stages 1 and 2.

- [x] Migrate Outliner, viewport shortcuts and placement, and DetailsComponentTree.
  Remove panel-built attachment changes and per-caller general transaction logic.
- [x] Migrate SkyBox and StaticMesh composite workflows while preserving their
  specialized preparation, atomic replay and single history entry.
- [x] Keep document selection/focus/error handling in adapters; verify stale
  selection reconciliation and consistency between Outliner and viewport actions.
- [x] Remove obsolete ActorMutations/ComponentMutations interfaces, private-header
  test dependencies and build registrations; search every workspace project for
  legacy includes/symbols and accidental bypasses.
- [x] Completion: consumer targets compile and relevant placement, structural,
  Details and viewport contract tests pass without a private LevelEditor API bridge.

### Stage 4: Validate integration and document final contracts

Outcome: final service ownership and preserved behavior are evidenced.
Dependencies: Stage 3.

- [x] Run the bounded final validation matrix below and record tested revision,
  selections, preset/environment, outcomes and any remaining gates here.
- [x] Update EditorSubsystems, TransactionRecords, LevelEditorDetails,
  ViewportEditing and StaticMeshLevelMutations contracts, plus CodeModules/routing
  only where ownership or task triggers changed. Reconcile the Toolset plan's
  scene-operation ownership so it does not introduce a competing mutation path.
- [x] Confirm no DurinEd dependency on LevelEditor/UI, no duplicate public command
  implementation, no selection ownership transfer and no second history.
- [x] Complete only after all required gates pass; follow plan lifecycle rules
  for completion and later archive.

## Validation and Acceptance

Follow [Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md), using DevTool's host-selected profile and registry
discovery rather than guessed target names. Inspect affected coverage before
execution. Implementation requires the bounded runtime and consumer validation below.

Implementation coverage must include:

- Service admission/lifecycle without a UI, multiple hosts, read-only and PIE
  rejection, absent/busy/recovering transactor, foreign Level/owner and stale inputs.
- Actor create/delete/duplicate/rename/attach/detach execute/Undo/Redo; same live
  identity, graph references, attachment transforms, primary camera, ordering,
  package revisions and GC retention while absent and after history release.
- Component create/delete/duplicate/rename/reparent, root and creation-method
  restrictions, complete subtree retention, reconstruction and registration.
- Preparation failure/cleanup, replay conflicts, compensation and compound
  SkyBox/StaticMesh history; no partial publication, dirtying or success on rejection.
- UI adapter parity, document selection reconciliation and unchanged gizmo
  gesture semantics. New lifecycle/conflict tests must also pass individually.

Build DurinEd and affected consumer/test targets, including discovered Sandbox or
RoadWeaver consumers if present. Broad all-build/all-test coverage is not a
default gate: expand only if generated/reflected API or build-graph impact cannot
be bounded, and record the uncovered risk before expansion. Native tests do not
substitute for a consumer build where that consumer is outside test dependencies.
Reuse applicable passing results when relevant inputs remain unchanged.

### Initial implementation validation (25c35507a)

Validated on Windows/MSVC using the host-selected
`Win64-Debug-DurinEditor` preset, build jobs 14. Tested source/test inputs are the
implementation in `25c35507a`, based on `71a855e090395bf1065524f4cdedc61cdbcf341a`.
Subsequent edits only record documentation completion. Native execution is direct
and requires no open editor widget. A sandbox-only C1902 was resolved by using
the documented unsandboxed build path.

| Selection | Outcome |
| --- | --- |
| `build LevelEditor` (including DurinEd/DHT dependencies) | Passed |
| `LevelMutationTests` | 42 passed |
| `EditorOperationTests` | 85 passed |
| `EditorPropertyTests` | 67 passed |
| `WorldTests` | 171 passed |
| `ViewportTests` | 100 passed |
| `SkyBoxTests` | 13 passed |
| `SplineTests` | 48 passed |
| `EditorShellTests` | 59 passed |
| `MaterialGraphEditingTests` (shared record API consumer) | 104 passed |
| `LevelMutationTests FActorMutationTests.* --isolate --test-jobs 1` | 15 passed independently |
| `LevelMutationTests WorldOutlinerActorAttachmentTests.* --isolate --test-jobs 1` | 3 passed independently |
| `WorldTests FWorldSubsystemTests.Actor* --isolate --test-jobs 1` | 2 passed independently |
| `EditorOperationTests FTransBufferTests.* --isolate --test-jobs 1` | 27 passed independently |
| Changed-document validation, all-plan validation, Git diff review | Passed |

The nine whole-target selections total 689 passing cases. Property and material
results are reused: the transactor/record implementation, their sources, fixtures,
shader environment and toolchain did not change during the final Actor-only
preparation corrections. Other affected targets were rerun against those changes.
Reports are under `Build/NativeTestResults/Win64-Debug-DurinEditor/`; full build,
test and isolation receipts are under `Build/.agent-state/logs/` on 2026-10-17
(14:37-14:46 local time). The final Actor isolation receipt is
`20261017-144624-727906-28208-ctest.log`; transaction isolation is
`20261017-144453-727264-39132-ctest.log`.

Coverage includes retained identity/GC, attachment transforms and conflicts,
component lifecycle and generated/root restrictions, multi-host/read-only/PIE
admission, unavailable/recovering history, callback retirement and reentrancy,
rejected submission and compensation recovery, module drain ownership, private
preparation boundary links, compound workflows and adapter selection reconciliation.
Workspace searches found no Sandbox/RoadWeaver consumers or legacy private
mutation interfaces. The shared `FTransactionRecord::Apply` consumers are DurinEd,
its native tests and MaterialGraphEditingTests, all covered above. No broad
foundational-header or build-graph change requires an all-build. No manual UI or
GPU qualification was required or run; no required gates remain open.

### Review follow-up (2026-10-17)

The user requested a second review of UE alignment, failed-transaction rollback,
lifecycle boundaries and obsolete tests, then explicitly removed hard rejection
of a single oversized edit. The public UE subsystem reference still defines
responsibilities; document selection and the one-oversized-entry history policy
remain explicit Durin decisions.

The review fixed these issues:

- Component replay now rejects a detached owner and all structural replay rejects
  subsystem BeginPlay and a requested World shutdown before deferred teardown.
  Wrong-thread component calls reject before dereferencing the owner.
- Actor/component lifecycle callbacks that retire participants or close the World
  cannot report success. Incomplete effects enter recovery instead. Structural
  replay suppresses incidental package dirty marks; successful history transitions
  remain the authority for package revisions.
- Component history refreshes the complete Outer subtree before removal and after
  callbacks, retaining subobjects created after initial publication. Actor history
  also refreshes retention after replay.
- Custom replay exceptions, incomplete initial compensation and asynchronous
  finalization compensation retain the pending participant graph in recovery.
  A module cannot drain executable changes still owned by that pending entry.
- The byte limit now trims oldest undoable entries while preserving the latest
  complete edit, even when that entry alone exceeds the budget. Invalid size
  accounting still rejects safely, compensates synchronously and preserves redo.

Cleanup removed the obsolete byte-limit failure code and exact hard-limit test,
renamed attachment tests to `FEditorActorAttachmentTests`, and migrated property
and function tests to successful oversized edits. Material graph rollback/retry
tests now use a busy transactor to exercise actual submission rejection instead
of relying on the removed capacity policy. Historical initial-run counts above
were corrected from 789 to 689.

Validated the follow-up source/test changes based on `25c35507a` with Windows/MSVC,
`Win64-Debug-DurinEditor`, build jobs 14. `build LevelEditor` passed. Every whole
target below was rerun with `--report` against the final relevant inputs.

| Target | Passed |
| --- | --- |
| LevelMutationTests | 48 |
| EditorOperationTests | 87 |
| EditorPropertyTests | 67 |
| WorldTests | 173 |
| ViewportTests | 100 |
| SkyBoxTests | 13 |
| SplineTests | 48 |
| EditorShellTests | 59 |
| MaterialGraphEditingTests | 104 |

The nine reports total **699 passing cases**, computed from their XML counts.
Reports are under `Build/NativeTestResults/Win64-Debug-DurinEditor/`. Final whole
target receipts are under `Build/.agent-state/logs/` on 2026-10-17, 15:08-15:10
local time; the LevelEditor build receipt is
`20261017-150808-578320-43284-cmake.log` and final material consumer receipt is
`20261017-150949-524994-31656-MaterialGraphEditingTests.log`.

Serial isolated selections also passed, each case in its own process:

| Selection | Passed |
| --- | --- |
| LevelMutationTests `FActorMutationTests.*` | 18 |
| LevelMutationTests `FComponentMutationTests.*` | 19 |
| LevelMutationTests `FEditorActorAttachmentTests.*` | 3 |
| EditorOperationTests `FTransBufferTests.*` | 29 |
| WorldTests `FWorldSubsystemTests.Actor*` | 4 |
| EditorPropertyTests `FEditorFunctionTests.Oversized*` | 2 |
| MaterialGraphEditingTests `FMaterialGraphOperationsTests.*HistoryFailure*` | 3 |

The isolated counts total 78; these overlap the whole-target cases. Reports use
`review-*-isolated.xml` beside the whole-target XML. Receipts are under the same
log root at 15:10-15:11, ending with
`20261017-151100-770348-29184-ctest.log`. Changed-document and all-plan validation
and Git diff review passed. No required gates remain open.

The new World queries are inline reads without layout changes. Searches across
Engine, Sandbox and RoadWeaver found no additional consumers of the removed
transactor failure code. The bounded Engine/DurinEd/LevelEditor and material
consumer matrix covers the changed executable contracts; no all-build or manual
UI/GPU qualification was required.

### UE source audit and redundancy cleanup (2026-10-17)

Checked Epic's UE 5.3.2 source at mirror revision
`c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf`. `BeginInternal` trims existing history
before appending a new transaction; `End` does not reject or remove that transaction
for exceeding MaxMemory. Thus immediate Undo for oversized edits agrees with
Durin, while trimming time differs. The owning
[transactor contract](../Editor/Architecture/Transactors.md#bounds-and-accounting)
records pinned source references and the version boundary. No later UE version
is claimed verified.

Removed nine repeated read-only/transactor/owner admission sequences already
covered by `Admit`, two forwarding validation helpers, repeated owner predicates
already covered by the replay helper, and two unreachable transaction branches.
Preparation callback revalidation, replay validation and recovery remain necessary.
The one-entry trimming condition introduces no new state. Durin's count cap and
dynamic limit-change behavior are additional policies; searches found no
production `SetLimits` callers, only native tests.

Validated source changes based on `f498fd7a1` with Windows/MSVC,
`Win64-Debug-DurinEditor`, build jobs 14: `build LevelEditor`,
`LevelMutationTests --report` (48) and `EditorOperationTests --report` (87) passed.
The 135 cases cover the owning Actor/Component service and transaction buffer;
no shared API, fixture or budget behavior changed. Other consumers retain the
earlier full-matrix evidence. Receipts end in
`20261017-151501-900308-33784-cmake.log`,
`20261017-151510-436379-40304-LevelMutationTests.log` and
`20261017-151512-774235-22696-EditorOperationTests.log` under the existing log root.
Changed-document validation and diff review passed.

### Byte-only capacity cleanup (2026-10-17)

The user selected removal of the entry-count cap and unused dynamic-limit API.
Capacity will use only the fixed 64 MiB native-byte budget. Before a successful
new transaction is appended, trim old history from the front while its size is
at least the budget; do not include the incoming transaction in that decision.
As in the inspected UE source, replacing a redo branch skips byte trimming.
Unlike UE's Begin-time removal and Cancel-time restoration, defer destructive
history changes until successful finalization. This preserves cancellation,
no-op and failed-submission history without adding rollback storage or states.
Remove SetLimits, its diagnostics and the explicit one-entry retention rule.
Tests may select an initial byte budget through private test access only.

Removed the limits structure, SetLimits/GetLimits, three dynamic-limit rejection
reasons, limit snapshots in rejection results and the now-unused inconsistent-
accounting failure. The remaining production configuration is a single byte
budget with a read-only getter. Native tests select their initial budget through
existing private friend access, guarded against setup after transactions begin.
Eviction now calculates the removable prefix and erases it once, without
repeated whole-history accounting or an explicit minimum record count.

Tests cover 300 retained entries under budget, exact-budget eviction, oversized
recording and custom edits, canceled/no-op/failed submissions, redo replacement,
and collection of evicted participants. Obsolete dynamic-limit assertions were
removed; property/function/Actor budget tests use initial test configuration.

The affected-test resolver conservatively expands the common native-test helper
change to all Engine targets. Explicit source searches bound executable consumers
of the changed transaction header to DurinEd, LevelEditor and MaterialEditor;
Sandbox and RoadWeaver have no consumers. Use the nine existing owning/consumer
targets plus the LevelEditor build and isolated transaction cases, rather than
unrelated runtime, GPU or infrastructure suites. No foundational headers or
build-graph contracts changed.

Validated the changes based on `2da784f61` with Windows/MSVC,
`Win64-Debug-DurinEditor`, build jobs 14. `build LevelEditor` passed. The nine
whole-target selections were rerun with `--report` against final source inputs:

| Target | Passed |
| --- | --- |
| EditorOperationTests | 90 |
| EditorPropertyTests | 67 |
| LevelMutationTests | 48 |
| WorldTests | 173 |
| ViewportTests | 100 |
| SkyBoxTests | 13 |
| SplineTests | 48 |
| EditorShellTests | 59 |
| MaterialGraphEditingTests | 104 |

XML reports total **702 passing cases**. Serial isolation of
`EditorOperationTests FTransBufferTests.* --isolate --test-jobs 1` also passed
32 cases, including every new budget case. Reports are under the existing
`Build/NativeTestResults/Win64-Debug-DurinEditor/` root; isolation uses
`byte-budget-transactor-isolated.xml`. Receipts under `Build/.agent-state/logs/`
on 2026-10-17, 15:20-15:24 local time include
`20261017-152040-984353-2108-cmake.log`,
`20261017-152335-149375-13848-MaterialGraphEditingTests.log` and
`20261017-152357-759097-44668-ctest.log`. Changed-document and all-plan validation,
source/consumer searches and Git diff review passed. No required gates remain open.

## Required References

- [Editor subsystems](../Editor/Architecture/EditorSubsystems.md)
- [Editor transactor core](../Editor/Architecture/Transactors.md)
- [Transaction record foundation](../Editor/Architecture/TransactionRecords.md)
- [Level Editor Details](../Editor/Architecture/LevelEditorDetails.md)
- [Viewport editing](../Editor/Architecture/ViewportEditing.md)
- [Static mesh level mutations](../Editor/Architecture/StaticMeshLevelMutations.md)
- [Level system](../Runtime/World/LevelSystem.md)
- [Code modules](../Workspace/CodeModules.md)
- [Documentation workflow](../Agents/Documentation.md)
