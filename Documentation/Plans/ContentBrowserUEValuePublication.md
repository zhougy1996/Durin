# Content Browser UE Value Publication Plan

Summary: Freeze inputs across provider calls and commit results through live ownership, using one asynchronous model path.

Last reviewed: 2026-11-15

Status: Completed
Completed: 2026-11-15

## Current Status

Baseline: 3cae34d3c. Both reviewed lifetime/reentry defects are fixed through
owned query inputs, local binding policy and controlled publication. Projection
owns paths/settings and prefetches sort keys before comparison; obsolete or
ownerless results are discarded. Source admission reuses the data service's
pointer-ownership check. The production model has one asynchronous path, no
ItemsRequestCount and no synchronous refresh/test-wait API. Test fixtures drive
the normal worker/frame pump and inspect existing request ownership. Four modules
are unchanged; no Revision, generation or activation Session was added.

Final validation on Win64-Debug-DurinEditor over the final source changes:

- Five new formal regressions cover destruction during compile/enumeration,
  binding-policy lifetime after query destruction, sort reentry/latest-result
  commit, receiver destruction during projection and provider-free comparison.
  Focused receipt:
  `Build/.agent-state/logs/20261115-071905-072273-44132-ContentBrowserWorkflowTests.log`.
- Full browser workflow passed 185 tests with two existing platform skips after
  migrating former synchronous tests; the final module selection also passed
  the whole workflow plus providers and direct consumers (seven targets):
  `Build/.agent-state/logs/20261115-072654-664072-44928-ctest.log`.
  Shared test-fixture headers make `affected` select Engine-wide tests; actual
  users are browser/thumbnail targets covered by this explicit selection.
- Final `build all` passed for Engine, Sandbox and RoadWeaver:
  `Build/.agent-state/logs/20261115-072952-220773-36132-cmake.log`.
- One bounded EmptyDefaultLevelShowsWorkspaceAndExitsNormally startup/shutdown
  case passed (characterization mode, timeout 120 seconds):
  `Build/.agent-state/logs/20261115-073007-700918-11716-ctest.log`.

Consumer audit found no removed APIs or mode/counter fields in source/test roots
of the declared workspace projects. No relevant source/build inputs changed after
final receipts; documentation, all-plan validation and diff checks close acceptance.

## Implementation Stages

### Stage 1: Own inputs and control publication

- [x] Prepare Query inputs locally and stop after owner destruction/cancellation;
      retain binding policy by value across provider callbacks.
- [x] Freeze projection inputs, prefetch sort attributes, and commit only while
      the receiver and input state remain current.
- [x] Add formal preparation/binding destruction and projection reentry regressions.

### Stage 2: Unify the asynchronous model and migrate tests

- [x] Remove ItemsRequestCount, synchronous constructor mode and synchronous tree
      refresh; migrate every consumer to the asynchronous model.
- [x] Drive tests through real workers/frame pumping; compare existing request
      ownership, snapshot contents and retained ranges rather than counters.

### Stage 3: Validate and accept

- [x] Run owning/direct-consumer regression coverage and audit all workspace projects.
- [x] Build all for shared model/query API changes and run one bounded startup/shutdown case.
- [x] Update concise contracts, validate documents/plans and commit.

## Validation

Follow [build](../Agents/BuildAndRun.md), [testing](../Agents/Testing.md) and
[documentation](../Agents/Documentation.md). Test utilities may pump/wait the
normal asynchronous model but introduce no synchronous production branch.
