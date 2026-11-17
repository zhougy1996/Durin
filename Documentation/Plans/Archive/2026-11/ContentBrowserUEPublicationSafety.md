# Content Browser UE Publication Safety Plan

Summary: Fix reentrant publication and scoped cancellation while removing test-only version counters.

Last reviewed: 2026-11-15

Status: Archived
Completed: 2026-11-15

## Current Status

Baseline: 2f9bf52e6. All three reviewed defects are fixed using task ownership,
cancellation and weak receiver lifetime. No activation generation/Session was
introduced, and the four module boundaries are unchanged. MountRegistryRevision
and snapshot Version fields/getters are removed across workspace consumers and
plugin fixtures; tests observe events, retained snapshot identity, contents and
existing request instrumentation instead.

Final validation on Win64-Debug-DurinEditor over the final source changes:

- Five formal regressions passed: reentrant list and tree task ownership,
  cancellation during binding, ancestor/root folder notifications, and receiver
  destruction inside a physical-path callback. Positive receipt:
  `Build/.agent-state/logs/20261115-042347-648612-31248-ContentBrowserWorkflowTests.log`.
- Removing the receiver guard for a negative control produced SEH `0xc0000005`;
  the failed test process was terminated and the guard restored. Receipt:
  `Build/.agent-state/logs/20261115-042500-190055-25988-ContentBrowserWorkflowTests.log`.
  The final suite rebuilt and passed with the restored guard.
- The explicit Core, AssetRegistry and four-browser-module selection passed all
  22 targets with `--test-jobs 1`. Receipt:
  `Build/.agent-state/logs/20261115-043227-036320-1388-ctest.log`.
  Shared browser-fixture edits make `affected` expand to Engine-wide tests; actual
  fixture users are browser/thumbnail targets covered by this bounded selection.
- The independent PluginFixtureStartupProbe and PluginFixtureManagementProbe
  compiled in an ignored fixture copy. Workspace declaration was restored by
  original bytes. Receipts `20261115-042902-236005-36456-cmake.log` and
  `20261115-042941-325544-12812-cmake.log` under the same log directory.
  These were compile checks, not full plugin qualification runs.
- Final `build all` passed for Engine, Sandbox and RoadWeaver. Receipt:
  `Build/.agent-state/logs/20261115-043354-652006-32072-cmake.log`.
- One bounded EmptyDefaultLevelShowsWorkspaceAndExitsNormally startup/shutdown
  case passed (characterization mode, timeout 120 seconds). Receipt:
  `Build/.agent-state/logs/20261115-043410-276457-45612-ctest.log`.

No relevant source/build inputs changed after the final receipts. Documentation,
all-plan validation and diff checks complete the handoff.

## Implementation Stages

### Stage 1: Fix publication and notification boundaries

- [x] Detach completed task ownership before callbacks and reject results canceled
      during binding; preserve tasks started by reentrant requests.
- [x] Normalize folder notifications consistently, including trailing slash/root.
- [x] Stop notification processing if provider callbacks destroy the model.
- [x] Add formal regressions for all three defects.

### Stage 2: Remove test-only counters and migrate consumers

- [x] Remove MountRegistryRevision/getter and snapshot Version fields/getter.
- [x] Migrate native tests and plugin fixtures to events, retained snapshot identity,
      contents and existing request instrumentation; audit all workspace projects.

### Stage 3: Validate and accept

- [x] Run owning/direct-consumer regressions and plugin fixture coverage.
- [x] Build all workspace projects for the Core public API and shared snapshot
      layout changes; run one bounded editor startup/shutdown lifecycle case.
- [x] Update implemented documentation, validate documents/plans and commit.

## Validation

Follow [build](../../../Agents/BuildAndRun.md), [testing](../../../Agents/Testing.md) and
[documentation](../../../Agents/Documentation.md). Native asset catalog consistency
checks and navigation action identity remain outside the counter removal.
