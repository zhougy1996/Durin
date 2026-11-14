# Content Browser UE Mount Lifecycle Plan

Summary: Replace browser mount-version polling with committed mount events and UE-style provider ownership.

Last reviewed: 2026-11-15

Status: Completed
Completed: 2026-11-15

## Current Status

Baseline: 116290a16. Final implementation uses UE-style weak provider ownership,
mount/update events and owner-controlled asynchronous tasks. Activation counters,
mount-version fields, copied project admission and the compiled-filter wrapper
are removed. There is no custom activation Session. Reusing a live provider after
reactivation may reuse old descriptions; active-source dispatch and native final
path/permission checks remain. Four module/build boundaries are unchanged.

Final validation on Win64-Debug-DurinEditor, covering the final source changes:

- The explicit `core+asset-registry+content-browser+content-browser-data+
  content-browser-asset-data-source+content-browser-file-data-source` module
  selection passed all 22 targets with `--test-jobs 1`. Receipt:
  `Build/.agent-state/logs/20261115-035218-769192-20868-ctest.log`.
  Provider tests passed 59/59; Workflow passed 175 with two existing Windows
  symlink-permission skips. An earlier parallel run hit a Core pipe timing bound;
  the final serial run passed without relaxing that assertion.
- `affected --explain` expanded to Engine-wide tests because of the shared browser
  fixture header. Its actual users are browser/thumbnail tests covered by the
  explicit module selection; unrelated engine/GPU execution was not required.
- `build all` passed for Engine, Sandbox and RoadWeaver. Receipt:
  `Build/.agent-state/logs/20261115-035152-957607-40192-cmake.log`.
- One bounded `EmptyDefaultLevelShowsWorkspaceAndExitsNormally` startup/shutdown
  case passed in characterization mode, timeout 120 seconds. Receipt:
  `Build/.agent-state/logs/20261115-035357-468949-26844-ctest.log`.

No relevant source/build inputs changed after these receipts. Documentation and
plan validation plus diff checks complete the handoff. Native registry consistency
counters remain available outside browser cache synchronization. File discovery
retains its existing metadata-polling cadence and incomplete-scan behavior.

## Goal

Publish owner-thread mount changes after successful registry mutations, including
test replacement/restoration. Invalidate caches and native discovery through events.
Remove activation generations, mount-version fields and custom session identities
from ItemData, compiled filters, captures and operation contexts. Source-owned
ItemData retains weak ownership; bindings may be reused after the same provider
reactivates. Compiled raw scopes are recompiled against current mounts. Views own
and cancel their tasks; native discovery drains on invalidation or shutdown.
No generic task manager, Session, result or recovery abstraction is introduced.

## Implementation Stages

### Stage 1: Publish mount changes and invalidate caches

- [x] Add committed mount-change events; rejected publication emits no event.
- [x] Migrate query, provider, service and model caches away from mount polling.

### Stage 2: Align provider ownership and task lifetime

- [x] Remove activation/mount fields and Session abstractions from bindings, filters
      and captures; migrate all workspace consumers.
- [x] Keep owner/active-source admission and permission checks at callback boundaries.
- [x] Invalidate native discovery through mount events, cancel/drain owned tasks on
      invalidation/shutdown and preserve immutable worker inputs.

### Stage 3: Validate and accept

- [x] Regress mount publication/replacement/restoration, cache invalidation,
      provider withdrawal, cancellation and callback reentry. Remove tests that
      only enforce abandoned activation anti-revival semantics.
- [x] Validate owning/direct-consumer tests and inspect affected selection.
- [x] Build all workspace projects for Core and ContentBrowserData public-header
      and layout changes; run one bounded startup/shutdown lifecycle case.
- [x] Simplify implemented documentation, validate documents/plans and commit.

## Validation

Follow [build](../Agents/BuildAndRun.md), [testing](../Agents/Testing.md) and
[documentation](../Agents/Documentation.md). Native catalog and mount-registry
consistency counters used outside browser synchronization remain outside scope.
