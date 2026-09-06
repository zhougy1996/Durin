# Content Browser Operation Service Plan

Summary: Unify browser mutation orchestration behind a UI-independent service and separate immutable deletion confirmation from recoverable execution state.

Last reviewed: 2026-09-07

Status: Completed
Completed: 2026-09-07

## Current Status

All stages are complete. Automated validation passed in the implementation
handoff. On 2026-09-07 the user confirmed validation was successful and requested
closeout ("验证没问题了，收尾吧"). That user acceptance closes the remaining
editor smoke and visible workflow gates; no additional agent-run application
smoke is claimed. Lasting behavior is documented in the Content Browser
architecture contract.

Final automated evidence on 2026-09-07, macOS Debug DurinEditor profile:

- Full `all` build passed in 1.49 seconds. Receipt:
  `Build/.agent-state/logs/20260907-042348-572066-41457-cmake.log`.
- `test affected --base 38a47a504` passed all 14 selected targets in one CTest
  invocation; ContentBrowserWorkflowTests passed all 94 cases. Receipt:
  `Build/.agent-state/logs/20260907-042430-875699-41595-ctest.log`.
- Changed-document validation, all-plan validation, and Git whitespace checks pass.
- The agent did not run macOS application smoke under the sandbox policy. The
  remaining manual acceptance is based on the user's 2026-09-07 validation signoff,
  not inferred from compilation or direct native tests.

Stage 0 proved the old within-directory partial deletion retry failed and that
projection-pending AssetTools results are truthy. The old cross-root test bypassed
Panel freshness checks. New coverage exercises production service submission,
partial ordinary and asset deletion, explicit reconciliation between retries,
recreated-root/changed-byte rejection, stale confirmation retirement, modal
cancellation retaining destructive progress, and duplicate submission. It also
covers live policy, stop admission, no-op dispatch, notifications and Fix Up scope.

Publication ownership is now uniform: the browser service alone advances the host
revision for its scoped commands. AssetTools' `bPublished` describes catalog
publication and must not suppress that host notification. Save/duplicate callbacks,
Panel's immediate-command publication guesses, and MainFrame's move/Fix Up dispatch
adapters are removed. Shared mount/path algorithms serve both Model and service
without either retaining the other. The service injects only required asset
capabilities and cannot be moved because its operation callbacks retain its owner.

Panel owns the service for the tool lifetime; hiding/docking keeps it alive.
MainFrame stops admission before retiring host dependencies. Jobs and publication
sinks do not capture Panel. Closing a prepared modal discards that session; closing
one after destructive progress retains it for `Retry Pending Deletion`.

Recovery refinement: forward-pending deletion remains fenced and deliberately does
not publish an automatic reconciliation request. AssetTools retains original
outside catalog/reference/companion facts and confirmed asset bytes; unexpected
outside changes block the same session. Explicit reconciliation of recorded
removals is tolerated. Physical completion publishes once. Projection-pending
completion retires the destructive session and transfers remaining work to
RefreshCoordinator/manual Refresh rather than duplicating its retry ownership.


## Goal

Menu, keyboard, and drag/drop callers use one browser operation boundary for
availability, preparation, execution-time revalidation, execution, and results.
Panel captures intent, obtains confirmation, and presents outcomes. The service
owns browser policy and mixed filesystem/asset coordination without retaining a
Model, Panel, ImGui context, or view-owned selection.

## Scope

- Save Package and canonical resave, asset duplicate/paste, asset and ordinary-file
  rename, folder rename/relocation, folder creation, asset move, scoped/project
  redirector Fix Up, and recursive mixed deletion.
- Narrow construction dependencies, structured outcomes, publication ownership,
  deletion-session lifetime, and all existing UI entry points for these commands.
- Preserve package versus top-level asset identity, operation-specific naming and
  destination policies, and existing user-visible confirmation requirements.

Feature-owned create/import/reimport implementations, extension registration,
asset opening, thumbnails, new bulk UX, asynchronous job scheduling, new modules,
Undo/Redo, quarantine, and disk-persistent recovery sessions are outside scope.
Existing multi-package resave and mixed deletion remain supported. Do not absorb
feature dialogs into this service or redesign Engine mutation jobs.

## Selected Design

### Ownership and dependencies

Evolve `FContentBrowserOperations` into a private
`FContentBrowserOperationService` under ContentBrowser's operation implementation
area. Keep a single implementation path during migration; temporary forwarding
adapters must have explicit removal tasks.

The service receives narrow path-resolution and owned mount-snapshot access,
live mutation/admission policy, filesystem operations, the required AssetTools
capabilities, and a host-lifetime content-publication sink. Move mount value types
out of Model so the service never imports its header. Tests can substitute these
dependencies without constructing a browser view. Do not create an interface for
every helper or relocate Engine safety policy into the browser.

MainFrame remains the composition owner. It supplies policy and publication
dependencies; per-command move/Fix Up dispatch callbacks disappear once their
required behavior is represented by the shared asset adapter. AssetTools retains
its opaque mutation jobs and package persistence authority. Clipboard and OS
Explorer behavior move to a presentation/platform adapter.

Run preparation and execution on the existing editor thread. UI actions retain
owned request values and execute after the relevant tree/content traversal ends.
No new concurrency is introduced. Availability queries are lightweight and must
not recursively enumerate folders, hash bytes, load assets, or mutate Model.

### Request and result protocol

Use typed requests containing explicit selection identities and destination/scope.
Do not pass Model or both a view item collection and a separate selection set.
Distinguish folder and project Fix Up explicitly; an empty folder must never mean
project scope. Preserve canonical top-level identity for duplication and package
identity/deduplication for save, resave, relocation, and deletion.

The conceptual sequence is `Query -> Prepare -> Revalidate -> Execute -> Result`.
Immediate commands may expose one convenience call that performs the latter
steps; do not require a session or confirmation modal for every command.
Query answers are advisory. Execution checks current admission, mutation policy,
mount writability, and operation-specific authoritative state again. Preserve
AssetTools' own final validation instead of trusting a browser snapshot.

Results retain errors, warnings, affected identities, persistence/recovery state,
and content-change effects from AssetTools. Browser-only filesystem operations
produce equivalent effects. Optional reveal/focus suggestions are values applied
by Panel after publication. Preserve `ForwardPending` and
`ContentCommittedProjectionPending`; neither may be flattened into a generic
failure or inferred by parsing diagnostic text.

### Publication and refresh

Assign one publication owner for each actual mutation outcome across AssetTools,
the browser adapter, and the host. Keep an existing lower-layer notification when
it is authoritative and avoid emitting a second one. Browser filesystem changes
publish through the same sink. Record the exact ownership map in Stage 0 before
changing callbacks; a success boolean alone is insufficient to infer an effect.

The service reports/publishes effects without refreshing Model. The existing
RefreshCoordinator consumes revisions, reconciles when needed, and refreshes
derived snapshots. Preserve shared acknowledgement and failed-revision suppression
across panels. No-op and preflight rejection do not publish. Retry and projection
reconciliation must not count the same committed mutation twice. Partial physical
change must produce an explicit recoverable outcome and fencing even when the
command reports failure. Deletion delays automatic reconciliation until physical
completion so partial progress does not discard its confirmed execution scope.

### Immutable deletion confirmation and independent session

Preparation returns an immutable confirmation value plus an opaque session handle.
The confirmation contains the exact scope, deterministic counts, blockers, and
warnings needed for display. A version binds it to the prepared session. Private
session state owns the physical fingerprints, maximal roots, the move-only
`FAssetDeletionOperation`, and execution/recovery progress. The UI cannot mutate
or execute an asset operation through a const confirmation object.

Sessions belong to the service owner, not to a modal. Closing a confirmation
before execution may discard its prepared session. After destructive progress,
retain the session independently of modal visibility and expose retry/status
through the browser. Stop admission during shutdown; do not initiate additional
destructive work after dependencies retire. In-memory sessions do not promise
recovery across process restart. Never capture Panel in retained jobs or sinks.

State transitions are explicit:

| State/outcome | Required behavior |
| --- | --- |
| Prepared, blocked | Display blockers; execute nothing |
| Prepared, confirmation stale | Publish a replacement confirmation for the captured request and require confirmation again |
| Prepared, confirmed/current | Revalidate physical scope and asset safety, then execute once |
| ForwardPending | Retain the same session and retry only its confirmed remaining work; do not rebuild from current UI selection |
| ContentCommittedProjectionPending | Retire destructive session and hand off to RefreshCoordinator/manual Refresh; never repeat physical deletion |
| Completed | Reject duplicate destructive submission and release execution resources |

Distinguish initial freshness checks from recovery checks. Removed confirmed roots
and partially removed descendants must be accounted for by recovery state, while
replacement bytes or newly created descendants remain unconfirmed and cannot be
silently deleted. Stage 0 must establish how existing AssetTools retry validation
handles these cases; any necessary lower-layer change stays limited to preserving
this contract. Rebuilding a plan from surviving files is not a recovery strategy.

Preserve filesystem traversal independent of filters, maximal-root deduplication,
streamed byte fingerprints, directory descendant checks, writable mount and
source-control restrictions, reparse rejection, unknown-package blockers,
loading/dirty protection, companion ownership, complete alias closure, external
reference blockers, warning/reference-store revalidation, and Registry fencing.
Preserve irreversible forward-only deletion and relocation without editor history.

## Implementation Stages

### Stage 0: Characterize dispatch, publication, and recovery

- [x] Inventory each scoped operation's menu, keyboard, and drag/drop entry,
  validation owner, underlying asset call, result conversion, and publication sink.
- [x] Record service/session composition and shutdown ownership, including hidden
  browser behavior, and the exact callbacks/adapters to remove.
- [x] Add focused characterization coverage for deletion failure after one maximal
  root, failure within a root, and projection publication failure; distinguish
  observed behavior from intended recovery and record necessary fixes.
- [x] Confirm available focused test targets using the repository testing workflow.

Acceptance: the publication map, lifetime map, reproducible recovery observations,
and bounded lower-layer changes are recorded here before migration begins.

### Stage 1: Establish the service boundary and structured outcomes

Depends on Stage 0.

- [x] Extract owned path/mount types and inject narrow dependencies; remove Model
  reads/refreshes and ImGui/platform actions from the operation implementation.
- [x] Introduce typed requests, lightweight availability, live policy revalidation,
  and structured results preserving AssetTools recovery/persistence information.
- [x] Establish publication ownership and route effects to existing refresh
  coordination without retaining UI objects or duplicate notifications.
- [x] Verify the service can be exercised without Model, Panel, or ImGui context.

Acceptance: the service has no Model/UI dependency; policy changes between query
and execution are enforced; structured failure effects survive every adapter.

### Stage 2: Route immediate commands through the service

Depends on Stage 1.

- [x] Migrate save/resave, duplicate/paste, rename, folder creation/relocation,
  move, and all Fix Up scopes, including availability and selection normalization.
- [x] Route all matching menu, shortcut, and drag/drop actions through those calls;
  preserve deferred execution after snapshot traversal.
- [x] Remove direct Panel mutation calls, move/Fix Up host dispatch callbacks,
  obsolete forwarding adapters, and per-command Panel publication guesses.
- [x] Preserve name collision/redirector policy, ordinary-file companion restrictions,
  scope semantics, focus/reveal behavior, and package deduplication.

Acceptance: each scoped immediate command has one validation/execution path,
equivalent inputs behave consistently across entry points, and content publication
and multi-panel refresh counts match the Stage 0 ownership map.

### Stage 3: Separate deletion confirmation and recoverable execution

Depends on Stages 1 and 2.

- [x] Replace mutable plan execution data with immutable confirmation plus an
  opaque version-bound handle; keep one move-only asset operation per session.
- [x] Move initial revalidation, execute, and result transitions out of Panel;
  reject stale confirmation handles and duplicate submission.
- [x] Retain forward-pending sessions and expose explicit retry/status; distinguish
  ordinary plan replacement from recovery and projection-only reconciliation.
- [x] Implement the bounded retry fixes identified in Stage 0, preserving every
  physical, asset, reference, warning, and fencing check listed above.
- [x] Cover cancel-before-execute, modal closure after partial progress, changed UI
  selection, hidden browser, and stop-admission behavior.

Acceptance: confirmation values contain no mutable execution capability; destructive
retry uses the same session, never expands confirmed scope, never restores deleted
content, and never repeats physical deletion after a projection-only failure.

### Stage 4: Qualify integration and document the implemented contract

Depends on Stage 3.

- [x] Run focused service, deletion, asset workflow, and refresh coordination tests
  selected through the repository testing workflow.
- [x] Complete the full `all` build on the selected profile.
- [x] Accept macOS editor smoke validation from the user; signoff recorded in
  Current Status, with no additional sandbox application run.
- [x] Exercise save/resave, duplicate/paste, ordinary-file and folder rename,
  drag-move, all Fix Up scopes, confirmation replacement, recovery retry, Play-mode
  rejection, and browser/editor shutdown through production UI entry points.
- [x] Search for retired Model dependencies, ImGui calls, mutable plan operations,
  direct Panel mutation calls, lossy result adapters, and temporary forwarding seams.
- [x] Update the lasting Content Browser contract and any changed AssetTools
  recovery contract; validate changed docs and all plans.
- [x] Record exact automated test/build evidence and user-confirmed manual acceptance.
- [x] Record visible workflow and smoke evidence, then complete the plan.

Acceptance: all stages pass, required checks have recorded evidence, and the
architecture document describes implemented behavior rather than this proposal.

## Validation Matrix

| Concern | Required regression evidence |
| --- | --- |
| Shared command path | Menu/shortcut/drop requests yield equivalent eligibility and execution for the same inputs |
| Live policy | Mutation/admission/mount changes after query or confirmation prevent unauthorized execution |
| Identity and scope | Multi-asset packages deduplicate correctly; empty folder Fix Up never expands to project |
| Stale confirmation | Byte replacement, new descendants, reference/companion/warning changes require rejection or renewed confirmation |
| Forward recovery | Failure after a root and within a root retains confirmed scope and session; new/replaced files are protected |
| Projection recovery | Committed deletion with failed publication retries reconciliation without another physical delete |
| Publication | No-op/rejection, success, partial mutation, and retry have correct effects and no duplicate revision publication |
| Multiple panels | One shared reconciliation, independent snapshot refresh, and failed-revision suppression remain intact |
| Lifetime | Closing/hiding a panel cannot destroy pending recovery or leave captured Panel callbacks; shutdown stops admission safely |
| Existing safety | Unknown packages, reparse paths, read-only roots, dirty/loading packages, references and alias closure remain covered |

## Related Documentation

- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Documentation Workflow](../Agents/Documentation.md)

## Related Code

- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Operations/ContentBrowserOperationService.h`
- `Engine/Source/Editor/ContentBrowser/Private/Operations/ContentBrowserOperationService.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Operations/ContentDeletionOperation.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserModel.h`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserRefreshCoordinator.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/ContentBrowserTool.cpp`
- `Engine/Source/Editor/ContentBrowser/Public/ContentBrowser/ContentBrowserContracts.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/AssetTools/Public/AssetTools/AssetOperation.h`
- `Engine/Source/Editor/AssetTools/Public/AssetTools/AssetDeletion.h`
- `Engine/Source/Editor/AssetTools/Private/AssetTools/AssetDeletion.cpp`
