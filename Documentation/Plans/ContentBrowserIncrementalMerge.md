# Content Browser Incremental Merge Plan

Summary: Consume complete provider deltas directly, preserve item and thumbnail identity, and reconcile asynchronous baselines without ordinary-update enumeration.

Last reviewed: 2026-11-16

Status: Completed
Completed: 2026-11-16

## Current Status

Stages 0 through 4 are complete. Implementation started from `7650830e2`, whose
preservation fix prevents thumbnail churn but still recaptures the current range.
Ordinary provider updates now merge shared immutable records directly, including
request-local replay across in-flight baselines. Thumbnail replacement and real
cold/warm rendering integration passed; temporary probes have been removed.

Stage 1 evidence (base `4391b5ce5`, Win64-Debug-DurinEditor): all 60
ContentBrowserDataTests passed; MainFrame and its browser consumers built.
Raw logs: `Build/.agent-state/logs/20261116-013341-654049-34044-ContentBrowserDataTests.log`
and `Build/.agent-state/logs/20261116-013427-403923-10936-cmake.log`.
Consumer audit covered source/test roots of all three Durin.dworkspace projects;
Sandbox and RoadWeaver have no direct consumers of the changed provider API.

Stage 2 evidence (base `ffc79d0d8`, same preset): ContentBrowserWorkflowTests
passed 191 cases with 2 pre-existing skips; all 60 ContentBrowserDataTests passed.
The new merge test covers all four delta types, duplicate addition, unchanged
capture/request counts, immutable old ranges, unaffected record addresses and
frontend search. A second test retains another source's folder contribution.
Raw logs: `Build/.agent-state/logs/20261116-014511-056288-26292-ContentBrowserWorkflowTests.log`
and `Build/.agent-state/logs/20261116-014633-543285-21712-ContentBrowserDataTests.log`.
Shared test-header affected expansion selects the entire Engine test project;
bounded execution uses the audited data/workflow owners here, with thumbnail
and remaining browser-consumer targets retained for final integration.

Stage 3 evidence (base `f94b0ce9d`, same preset): ContentBrowserWorkflowTests
passed 193 cases with 2 pre-existing skips; all 60 ContentBrowserDataTests passed.
Gated stale-baseline replay covers modification, removal, move, transient
add/remove and repeated updates with one capture and unchanged request identity.
Reentrant baseline binding removal is replayed before first publication. Existing
explicit refresh, mount withdrawal, navigation replacement and receiver-destruction
coverage also passed. Raw logs:
`Build/.agent-state/logs/20261116-015505-631654-12344-ContentBrowserWorkflowTests.log`
and `Build/.agent-state/logs/20261116-015532-034414-46416-ContentBrowserDataTests.log`.

Stage 4 evidence (base `28be4bce1` plus this stage's changes, same preset):

- Five owning targets passed: ContentBrowserDataTests (61 cases),
  ContentBrowserWorkflowTests, EditorAssetWorkflowTests, StaticMeshThumbnailTests
  and ThumbnailTests. Receipt:
  `Build/.agent-state/logs/20261116-023216-738841-19568-ctest.log`.
  After the final navigation-history adjustment, the workflow target passed
  194 cases with 2 pre-existing skips:
  `Build/.agent-state/logs/20261116-023357-010067-32760-ContentBrowserWorkflowTests.log`.
- Material/Texture thumbnail consumer targets passed:
  `Build/.agent-state/logs/20261116-021118-634306-44320-ctest.log`.
  The Vulkan cold-generation/warm-cache case passed, including metadata reuse,
  refresh retention, cancellation/revisit and replacement/removal:
  `Build/.agent-state/logs/20261116-020945-712931-27748-ThumbnailVulkanTests.log`.
  These results cover the final pool implementation; later native folder and
  navigation changes are covered by the data/workflow targets above.
- Final-source PythonScriptPanelTests (8) and SceneImportTests (34) passed:
  `Build/.agent-state/logs/20261116-024135-341958-14092-PythonScriptPanelTests.log`
  and `Build/.agent-state/logs/20261116-024147-126038-42752-SceneImportTests.log`.
- Real Sandbox editor runs on RTX 3090/Vulkan each completed 2,400 ticks,
  automatically opening Engine/Renderer. Each run injected 36 ordinary Modified
  events in 18 batches and observed all 18 incremental merges. Both previews
  remained Ready with stable textures and zero per-frame drawing/state
  regressions. From the first injection through exit there were zero new range
  captures and zero requested GCs. Cold: two loads/renders, zero disk hits;
  warm: zero loads/renders, two disk hits. Both completed two jobs/uploads;
  initial cancellation count stayed at two throughout, with no update churn.
  Logs: `Build/.agent-state/logs/20261116-023940-217829-23164-DurinEditor.log`
  and `Build/.agent-state/logs/20261116-024024-372582-24060-DurinEditor.log`.
  Checked receipt: `Build/.agent-state/incremental-thumbnail-validation.json`.
  This is instrumented real rendering evidence, rather than a manual visual
  inspection. The isolated cache is under `Build/.agent-state/`.
- All probes, forced navigation and diagnostic cache settings were removed.
  The normal MainFrame modules rebuilt successfully:
  `Build/.agent-state/logs/20261116-024119-952235-47636-cmake.log`.
  Final changed-document and all-plan validation passed. Bounded coverage follows
  the audited consumers across all workspace projects; no broad build or full
  Vulkan qualification is required by this plan.

## Goal

Align ordinary Content Browser updates with UE 5.8's source-qualified item
merge and per-item filtering. Added, Modified, Moved and Removed events must
update the current collection without preparing another range capture. Keep
value-owned external items, owner-thread provider dispatch, immutable worker
inputs, independent views and current mutation-policy checks.

The reference is the local UE 5.8 `SAssetView::HandleItemDataUpdated`,
`FAssetViewItemCollection::UpdateData`, source filter conversion/checking and
thumbnail-pool refresh/relevancy code. OS directory watching is a separate
platform project: the existing File provider may continue discovering changes
through metadata scans, but consumers must merge its resulting deltas directly.

## Selected Contract Changes

1. Provider Added/Modified/Moved updates contain complete current descriptions
   and usable payloads. Removed and previous move paths identify exactly one
   source contribution. Failed operations publish no optimistic delta.
2. Provider filter conversion/checking is available for a single item and has
   the same scope and backend-filter semantics as baseline enumeration. Bind
   updates with the receiving view's operation policy; do not inherit authority
   from another view's item.
3. The view owns a keyed contribution collection. Modified replaces a
   contribution, folders merge sources, removal preserves other contributions,
   and moves migrate existing selection/reveal state. External item copies stay
   immutable. Rebuilding the frontend projection is permitted; re-enumerating
   the backend and rebuilding unaffected item records is not an ordinary delta.
4. Each baseline request retains relevant deltas until publication and replays
   them after its captured values. Ordinary deltas do not cancel the baseline.
   Source/mount withdrawal, unknown refresh and replaced navigation revoke its
   publication rights. No public service revision or persistent journal is added.
5. Item metadata/residency updates preserve thumbnail demand. Actual content
   changes and explicit refresh remain the authority for preview invalidation.
   Accepted texture replacement retains usable pixels until new output arrives
   where ownership remains valid; deleted or withdrawn resources are revoked.
6. Full refresh remains an explicit recovery mechanism. Directory subtree
   invalidations with incomplete child information may request reconciliation;
   ordinary complete item deltas must never silently fall back to range capture.

Final integration refinement: Asset folder membership is indexed from asset
deltas and coalesced per batch, so empty File folders cannot become phantom Asset
contributors. An initial uniform object-name experiment conflicted with the
existing single-asset-package relocation contract, so that presentation rule is
retained. Package membership is indexed alongside folder counts; crossing the
single/multiple-asset boundary emits complete Modified events for affected
siblings, without recapturing unrelated rows or packages.

Lasting contracts belong in [data providers](../Editor/Architecture/Assets/ContentBrowserData.md),
[browser presentation](../Editor/Architecture/Assets/ContentBrowser.md) and
[thumbnails](../Editor/Architecture/Assets/AssetThumbnails.md).

## Implementation Stages

### Stage 0: Select contracts and acceptance gates

- [x] Record scope, UE reference, contract changes and staged acceptance.
- [x] Validate this plan and commit the selected approach.

Evidence: all-plan validation passed (11 active, 8 completed, 457 archived);
changed-document validation passed for this plan on 2026-11-16.

Acceptance: all-plan and changed-document validation pass. No OS watcher,
public journal or broad build is implicitly required.

### Stage 1: Publish complete deltas and single-item filters

Depends on Stage 0.

- [x] Add provider single-item conversion/filter checks with safe defaults.
- [x] Hydrate native Asset/File event descriptions without range enumeration.
- [x] Audit shared API consumers in Engine, Sandbox and RoadWeaver.
- [x] Document implemented contracts and cover metadata, scope and owner policy.

Acceptance: native event items have correct names, class/extension/size/time and
opaque payload ownership; per-item checks match baseline scope/filter behavior.
Run owning provider/data tests and bounded consumer builds selected through the
[build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows.

### Stage 2: Merge ordinary item updates into the view

Depends on Stage 1.

- [x] Consume source-owned updates directly without converting them to path invalidations.
- [x] Add keyed contribution replacement, insertion, removal and move behavior.
- [x] Preserve unrelated records, thumbnail references and source-merged folders.
- [x] Update frontend projection, selection migration and affected tree caches.
- [x] Add explicit zero-capture gates alongside the earlier preservation regression.

Acceptance: after an initial baseline, all four ordinary event types cause zero
additional PrepareItemsCapture calls. Test duplicate events, filters, sorting,
recursive search, multi-source folders, multiple views and removed contributions.
Content Browser workflow and affected consumer tests pass.

### Stage 3: Replay deltas across asynchronous baseline publication

Depends on Stage 2.

- [x] Retain request-local updates and overlay them on a completed baseline.
- [x] Keep ordinary updates from cancelling relevant in-flight captures.
- [x] Fence navigation replacement, source retirement and explicit full refresh.
- [x] Document and test reentrant and hidden-view delivery behavior.

Acceptance: gated worker tests cover update-before-publish, add/remove, move and
repeated modification; late results cannot revive removed or old-path items.
Source retirement cannot publish actionable old descriptions. Ordinary updates
do not start replacement captures. Owning data/workflow tests pass.

### Stage 4: Validate thumbnail lifetime and final integration

Depends on Stage 3.

- [x] Align thumbnail refresh/replacement with retained valid textures and demand.
- [x] Cover metadata/residency updates separately from content invalidation.
- [x] Run final affected tests and build the runnable editor modules.
- [x] Verify both Renderer StudioCube previews with real Vulkan rendering and
      stable request/texture/GC observations; retain raw logs under Build/.
- [x] Remove temporary probes, record receipts, complete lasting contracts and plan.

Acceptance: ordinary updates neither cancel/requeue thumbnail generation nor
drop unrelated references. Both StudioCube previews reach Ready, continue drawing
with stable textures, and requested GC does not repeat after settling. Record
cold/warm conditions and any unavailable gates truthfully. All affected tests,
changed-document validation and all-plan validation pass on final relevant inputs.

Each stage updates this plan and is committed separately with exact Plan and
Stage trailers. Evidence records the base/tested revision, preset, selections,
raw-log paths and applicability of reused passing results.
