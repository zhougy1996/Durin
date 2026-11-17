# Content Browser UE Operation Simplification Plan

Summary: Adopt UE provider operation results and independent dispatch, remove generic transfer planning and cancellation, and simplify policy binding and admission.

Last reviewed: 2026-11-15

Status: Archived
Completed: 2026-11-15

## Current Status

Implemented and accepted on baseline bf8ffdf4c plus this change. Public operation
results now use UE bool aggregation and independent source dispatch. Generic
prepared transfers, invocation claims and transfer cancellation are removed.
Feedback binding cannot widen existing mutation restrictions; native invocation
views own the lifecycle fence instead of storing nested fences in item payloads.
Four module boundaries and dependency directions are unchanged. Consumers and
regressions have migrated, removed paths have no remaining source/test consumers,
and all acceptance gates below passed. There are no remaining implementation gates.

The user's latest decision supersedes retaining Durin's public per-effect results,
cross-provider preflight and generic transfer cancellation. The earlier completed
architecture convergence plan remains historical evidence; this plan owns these
semantic changes. Confirmed deletion deliberately retains its exact-scope preview
and execution validation; background query cancellation is unaffected.

## Reference And Decisions

Local reference: D:/Programs/UE_5.8, UE 5.8.2 CL 56702186. ContentBrowserDataSource
exposes bool CopyItem/MoveItem/RenameItem and bool bulk operations. Default bulk
copy calls CopyItem for every item and ORs success. ContentBrowserItem dispatches
to sources; rename aggregates successful output contributions. These interfaces
carry no generic prepared transfer context or cancellation state.

Retain the four modules, source-owned opaque ItemData, async publication fences,
confirmed deletion, no-overwrite/companion/reparse/mount safety, and owner-thread
lifetime safety. Do not add a new request state machine or deferred-lifecycle
framework to preserve guarantees the user has now explicitly dropped. Source
retirement still revokes admission immediately; command inputs and source lifetimes
remain retained across actual extension callbacks. Native diagnostics/recovery
facts may remain internal implementation details, without a public browser effect
ledger. Native operations may validate their own inputs; there is no global
all-sources-before-write promise. Success means at least one contribution succeeded,
not that every selected contribution succeeded. Prior successes are retained.

## Implementation Stages

### Stage 1: Adopt UE public operation semantics

- [x] Source and Item mutation APIs return bool; migrate service/UI/test consumers.
- [x] Default bulk copy/move delegate to single-item operations and OR success.
- [x] Dispatch independent provider partitions without cross-provider preflight.
- [x] Remove public transfer contexts, invocation claims, cancellation and the
      parallel prepared execution extension points; native transfers execute
      directly through the public action overrides.
- [x] Regress independent failures, partial native effects, rename outputs and
      source withdrawal through ordinary public operations.

### Stage 2: Separate feedback updates from mutation restrictions

- [x] Binding feedback cannot remove an existing permission restriction.
- [x] Provider-owned restrictions and caller restrictions are conjunctive; feedback
      remains optional and independent from mutation permission.
- [x] Migrate consumers/tests of the previous implicit full-policy replacement.
- [x] Preserve callbacks/notifications after committed native effects and reject
      stale or foreign bound contributions.

### Stage 3: Simplify binding and admission and accept the result

- [x] Remove transfer-only lifetime/state wrappers made unnecessary by Stage 1;
      retain source strong ownership and actual callback admission checkpoints.
- [x] Consolidate repeated binding/publication admission checks without weakening
      activation/mount/project fences or headless operation.
- [x] Regress permission overlay, payload reuse, lifecycle/reentry, UI refresh,
      successful rename selection and confirmation behavior.
- [x] Inspect affected selection, run owning/direct-consumer tests, and complete
      an all build across Engine/Sandbox/RoadWeaver for changed public vtables.
- [x] Run one bounded startup/shutdown characterization case for final integration.
- [x] Update implemented contracts, validate changed documents and all plans,
      audit removed paths and commit the completed change.

## Validation

Follow [build](../../../Agents/BuildAndRun.md), [testing](../../../Agents/Testing.md), and
[documentation](../../../Agents/Documentation.md) workflows. Iteration uses owning
ContentBrowserDataTests/ContentBrowserWorkflowTests; the browser module selector
covers direct asset workflow, import, Python and thumbnail consumers. All build
covers the public ABI and cross-project consumers; it does not imply GPU
qualification. Preserve existing platform skips. Replace obsolete ledger/global
preflight/cancellation assertions with observable filesystem, source call and
update/confirmation assertions, rather than recreating the removed contract.

## Acceptance Evidence

Validated 2026-11-15 on windows-msvc-x64 / Win64-Debug-DurinEditor, Debug,
18 configured build/test slots. Evidence covers the final source and test inputs;
only documentation status/receipts changed after these runs.

- Seven owning/direct-consumer targets passed through the four-module selector:
  ContentBrowserDataTests, ContentBrowserWorkflowTests, EditorAssetWorkflowTests,
  PythonScriptPanelTests, SceneImportTests, StaticMeshThumbnailTests and ThumbnailTests.
  Receipt: `Build/.agent-state/logs/20261115-015057-057065-43232-ctest.log`.
  Data coverage includes the new multi-contribution Edit/Duplicate and aliased-output
  regression. Workflow retains its two existing Windows symlink-permission skips.
- All workspace projects (Engine, Sandbox and RoadWeaver) built and linked.
  Receipt: `Build/.agent-state/logs/20261115-015114-961666-18028-cmake.log`.
- The bounded startup/shutdown case
  `FLaunchStartupCharacterizationTests.EmptyDefaultLevelShowsWorkspaceAndExitsNormally`
  passed in characterization mode with a 120-second timeout. Receipt:
  `Build/.agent-state/logs/20261115-015126-420857-42632-ctest.log`.
- Obsolete cached-transfer-claims and prepared-transfer-cancellation tests were
  removed because their public contract was intentionally removed. Remaining tests
  assert actual files, diagnostics, updates, source dispatch, ownership and
  confirmation behavior; test providers return bool directly instead of wrapping
  synthetic effect ledgers. Native diagnostic/recovery bookkeeping remains an
  implementation detail, including separate native confirmed-deletion hooks.
- Removed API symbols were audited across source/test roots of all projects in
  Durin.dworkspace. Four module manifests and workspace/project dependencies are
  unchanged. Changed-document and all-plan validation passed before commit.

Implemented contracts live in
[Content Browser Data](../../../Editor/Architecture/Assets/ContentBrowserData.md) and
[Content Browser](../../../Editor/Architecture/Assets/ContentBrowser.md).
