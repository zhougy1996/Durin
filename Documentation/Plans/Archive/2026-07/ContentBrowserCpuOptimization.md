# Content Browser CPU Optimization Plan

Last reviewed: 2026-07-24

## Current Status

Archived on 2026-07-24 by explicit owner acceptance. Stage 1's lazy tree-drop
destination resolution and Stage 2's lexical path relations were implemented
on 2026-07-24. All 115 `CoreTests` and 182 `EngineTests` pass, the full
`Win64-Debug-DurinEditor-Tests` build succeeds, and a hidden-window editor smoke
run remained healthy for eight seconds.

A post-change Visual Studio CPU Usage sample supplied during validation reduced
`FContentBrowserPanel::Draw` from 66.24% to 16.64% of inclusive samples and
`DrawDirectoryTree` from 50.53% to 6.58%, relative reductions of approximately
74.9% and 87.0%. `DrawToolbar` at 7.81% replaced the directory tree as the
larger content-browser child hotspot. The owner accepted this result and chose
to defer further optimization until a future measured need, so Stage 3's cache
redesign was not pursued.

The fixed three-sample process-CPU median, non-Debug comparison, and recorded
visible interaction matrix were not completed. They remain unchecked below and
must not be inferred from this archive. This is an explicitly accepted early
closure based on the substantial sampled-hotspot reduction, not a claim that
every originally planned validation gate passed.

A 22.62-second Visual Studio CPU sample of an otherwise idle
`Win64-Debug-DurinEditor-Tests` editor session established the initial baseline.
Within the editor process, the content browser accounted for 66.24% of inclusive
samples, directory-tree drawing for 50.53%, physical-to-virtual directory
conversion for 38.61%, descendant checks for 23.00%, and
`std::filesystem::relative` for 12.49%. These inclusive percentages overlap and
must not be added together.

The captured session is machine-local at
`Build/Profiling/idle-cpu.diagsession`. It is supporting diagnostic evidence,
not a source-controlled build artifact.

## Goal

Reduce steady-state editor CPU consumption caused by an open, idle content
browser while preserving directory navigation, mount boundaries, tree state,
context menus, and asset drag-and-drop behavior.

## Scope

- Remove physical-to-virtual path conversion from the idle per-directory-node
  draw path.
- Make descendant and relative-path calculations purely lexical where inputs
  are already absolute and normalized.
- Cache stable directory-node path metadata when profiling shows meaningful
  residual per-frame path or allocation cost.
- Establish a repeatable before/after CPU sampling scenario for the content
  browser.
- Add focused validation for path-boundary behavior and interactive content
  browser operations.

## Non-Goals

- Replacing Dear ImGui or changing the editor's immediate-mode UI architecture.
- Adding asynchronous filesystem watching or background directory scanning.
- Redesigning the asset registry, content mounts, thumbnail pipeline, or content
  browser visual layout.
- Optimizing initial directory discovery unless it remains a measured
  steady-state hotspot after the idle path is corrected.
- Treating Debug-build Vulkan validation overhead as representative of shipping
  runtime performance.

## Design Decisions and Invariants

- An idle frame with no accepted asset drag must not compute a destination
  virtual path for directory-tree drop targets.
- Destination conversion happens only after a compatible asset payload is
  present and delivery requires the destination.
- All lexical containment helpers accept only non-empty absolute, normalized
  paths. Callers normalize at state-ingress or cache-construction boundaries,
  not repeatedly during drawing.
- Path containment is component-aware. A path such as `ContentExtra` is not a
  descendant of `Content`, and a parent-directory component (`..`) never passes
  containment checks.
- Mount-root equality remains valid for physical-to-virtual conversion, while a
  path is not considered its own descendant.
- Directory cache invalidation remains tied to the existing refresh and
  mutation paths. Cached metadata must not make renamed, moved, created, or
  deleted folders stale.
- Optimization must not change navigation history, selection, context-menu, or
  drag-and-drop semantics.
- Debug profiles are used for symbol-level attribution. A non-Debug editor
  profile is used for the final steady-state performance comparison.

## Current Foundations and Gaps

- `FContentBrowserPanel` already caches each directory's child paths in
  `DirectoryChildrenCache`.
- Tree recursion is limited to open ImGui nodes, so collapsed subtrees are not
  traversed.
- `DrawDirectoryNode` nevertheless normalizes every visible node and copies its
  cached child vector on every frame.
- `DrawDirectoryNode` eagerly calls `PhysicalToVirtualDirectory` before
  `AcceptAssetDrop` can determine that no drag target is active.
- `PhysicalToVirtualDirectory` normalizes mount roots repeatedly and performs a
  descendant calculation followed by another relative-path calculation.
- `IsDescendantPath` uses `std::filesystem::relative`, whose canonicalization and
  filesystem interaction are unnecessary for already normalized paths and show
  up prominently in the captured trace.
- There is no checked-in focused test coverage for the path-containment edge
  cases used by the content browser.

## Implementation Stages

### Stage 0: Lock the Baseline and Path Contracts

Caller audit (2026-07-24): physical-to-virtual conversion serves navigation,
folder snapshots, toolbar enablement, directory context actions, folder moves,
and folder creation. Containment serves current-directory filtering,
breadcrumbs, mount matching, and folder-move validation. The only remaining
`std::filesystem::relative` use in the panel enumerates directories during an
executed folder rename and therefore remains a filesystem mutation boundary,
not steady-state drawing work.

- [ ] Record the editor profile, visible panels, selected directory, expanded
  directory nodes, viewport state, warm-up duration, sample duration, and
  machine power state used for repeatable measurements.
- [ ] Capture three 20–30 second idle samples after the editor has reached a
  stable frame rate and record the median process CPU consumption.
- [x] Identify every caller that relies on physical-path normalization,
  containment, and physical-to-virtual conversion.
- [x] Define focused cases for mount-root equality, direct children, recursive
  descendants, siblings with shared text prefixes, `..` components, different
  roots or volumes, trailing separators, and case behavior on Windows.

#### Acceptance Gate

- The baseline scenario can be repeated without changing content, directory
  expansion state, editor layout, or sampling duration.
- Path helper preconditions and expected Windows behavior are explicit enough
  to implement without relying on filesystem canonicalization.

### Stage 1: Remove Idle Drag-and-Drop Path Work

- [x] Change the directory-tree drop-target path so it enters the ImGui drag
  target first and delays destination virtual-path construction until a
  compatible payload is delivered.
- [x] Preserve the existing item-grid and details-view drop behavior, which
  already use stored virtual paths.
- [x] Avoid temporary destination strings until a delivered move operation
  actually needs one.
- [ ] Exercise dragging over a directory, cancelling a drag, dropping onto a
  valid directory, and attempting an invalid or same-path move.

#### Acceptance Gate

- `PhysicalToVirtualDirectory` is absent from the idle
  `DrawDirectoryNode -> AcceptAssetDrop` call path.
- Asset moves initiated from tree, grid, and details views retain their current
  delivery and error behavior.
- A same-scenario Debug CPU trace no longer attributes a material share of idle
  samples to physical-to-virtual conversion from directory-tree drop targets.

### Stage 2: Make Normalized Path Relations Lexical

- [x] Introduce or extract a component-aware helper for containment and relative
  paths over absolute, normalized inputs.
- [x] Replace steady-state uses of `std::filesystem::relative` where the lexical
  helper's preconditions hold.
- [x] Normalize registered mount roots once per stable mount snapshot rather
  than once per visible node or toolbar frame.
- [x] Add focused native tests for the Stage 0 path matrix, including Windows
  drive and case behavior where applicable.
- [x] Retain error-code-based filesystem operations only at boundaries that
  genuinely query filesystem state.

#### Acceptance Gate

- Focused path tests pass and demonstrate component-boundary correctness.
- Idle content browser drawing performs no filesystem-relative operation for
  already normalized node and mount paths.
- Navigation, breadcrumbs, mount roots, refresh, rename, create, delete, and
  reveal-in-browser operations resolve the same virtual and physical locations
  as before.

### Stage 3: Remove Residual Per-Frame Node Churn

- [x] Re-profile after Stages 1 and 2 before changing the directory cache shape.
- [ ] If node normalization, child-vector copies, filename conversion, sorting,
  or label construction remains material, replace the cache value with stable
  node metadata containing normalized physical path, display name, virtual
  directory, filtered-child state, and child references as required by the
  measured hotspot.
- [ ] Ensure recursive traversal cannot retain iterators or references across a
  cache rehash.
- [ ] Invalidate affected nodes on refresh, create, rename, move, delete, source
  visibility changes, and mount changes.
- [ ] Avoid caching per-frame ImGui interaction state.

#### Acceptance Gate

- The implementation introduces node metadata only for costs confirmed by the
  post-Stage-2 profile.
- Directory mutations and refreshes cannot display stale nodes or stale virtual
  destinations.
- The directory-tree draw path performs no full child-vector copy per visible
  node.

### Stage 4: Performance and Regression Validation

- [x] Build through the root BuildTool using the relevant editor profile, as
  documented in `Documentation/Setup/BuildAndRun.md`.
- [x] Run focused native tests and the applicable editor test suite.
- [ ] Repeat three same-scenario Debug samples and compare median process CPU,
  content-browser inclusive samples, directory-tree inclusive samples, and the
  top exclusive functions against Stage 0.
- [ ] Repeat the steady-state comparison with a non-Debug editor build without
  Vulkan validation overhead.
- [ ] Manually verify collapsed, partially expanded, and deeply expanded trees,
  including drag-and-drop and all directory mutation actions.
- [x] Record final evidence in this plan and move lasting path/cache invariants
  into the relevant Architecture documentation before archiving.

#### Acceptance Gate

- Median idle process CPU improves by at least 25% against the Stage 0 scenario,
  or the remaining cost is explained by a newly captured dominant hotspot and
  explicitly accepted before completion.
- `PhysicalToVirtualDirectory`, `IsDescendantPath`, and
  `std::filesystem::relative` are no longer dominant steady-state
  content-browser samples.
- No functional regression is observed in navigation, mount handling, content
  mutations, context menus, or drag-and-drop.
- The same-profile editor build and relevant automated tests pass.

## Validation Matrix

| Area | Scenario | Evidence |
| --- | --- | --- |
| Unit | Normalized lexical containment and relative-path edge cases | Focused native tests |
| Integration | Mount-root conversion, navigation, breadcrumbs, refresh, reveal, and directory mutations | Editor tests plus manual checks |
| Interaction | Drag hover, cancellation, valid delivery, invalid delivery, and same-path delivery across tree, grid, and details views | Manual visible-editor validation |
| Rendering | Collapsed, partially expanded, and deeply expanded directory trees | Visible-editor inspection |
| Performance | Three fixed 20–30 second idle samples after warm-up | Median Debug and non-Debug CPU comparison |
| Regression | Existing editor and content-browser coverage | BuildTool test result |

## Definition of Done

- Every required stage acceptance gate is satisfied.
- The fixed idle scenario demonstrates the required CPU reduction or contains an
  explicitly accepted explanation backed by a replacement profile.
- Content browser behavior remains unchanged for navigation, mounts, directory
  mutations, and asset drag-and-drop.
- Automated path coverage protects the lexical containment contract.
- Lasting implementation rules are documented outside the active plan.
- The plan is archived according to `Documentation/Plans/AGENTS.md`.

## Deferred Follow-ups

- Filesystem watching and incremental cache invalidation.
- Background directory enumeration for unusually large mount trees.
- Tree virtualization if profiling later shows ImGui node submission dominating
  after path work and allocation churn are removed.
- A reusable editor performance benchmark harness beyond this fixed manual
  sampling scenario.

## Related Documentation

- [Build and Run](../../../Setup/BuildAndRun.md)
- [Runtime Architecture](../../../Architecture/RuntimeArchitecture.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
