# Content Browser Directory Tree Read-Only Traversal Plan

Summary: Keep published directory-cache entries stable during tree drawing so cached child spans can be traversed without per-node vector and path copies.

Last reviewed: 2026-08-19

Status: Archived
Completed: 2026-08-19

## Current Status

The model already separates directory discovery into a request phase and a refresh
phase. `PrepareForDraw()` commits requested directory snapshots before
`DrawContents()`, while recursive tree drawing only requests missing snapshots for a
later frame. This keeps filesystem enumeration outside recursive drawing, although
insertion of distinct cache entries would not by itself invalidate existing entry
references.

The remaining violation is that tree interactions execute side effects immediately.
Node navigation, directory context actions, and delivered drag-and-drop operations can
refresh content and clear `DirectoryChildrenCache` while `DrawDirectoryNode()` or one
of its ancestors still holds a span into the cache. The view therefore copies every
node's cached child vector before drawing it.

Stage 0 is complete. The invalidating paths are node navigation; directory context
open, rename, delete, and refresh; and delivered drag-and-drop moves followed by mounted
content publication. Create, import, and redirector fix-up already use the post-pane
content action. Directory snapshot refresh and mount synchronization run in
`PrepareForDraw()` before either pane. The selected deterministic baseline fixture is a
16-level expanded chain whose root also has 64 leaf siblings and whose remaining chain
nodes each have 8 leaf siblings. Once warm, the current defensive copy performs 16
child-vector storage allocations and 200 cached path copies per frame.

Stage 1 is complete. Tree click navigation and directory context open, rename, delete,
and refresh now commit after the tree child window ends and before the content pane.
Delivered drag-and-drop moves commit through the post-pane content phase. Named queue
helpers preserve the first action and diagnose a second action without overwriting it.
All content action sites now use the same helper. The focused `LevelEditor` build passes.

Stage 2 is complete. `DrawDirectoryNode()` now uses one cached span for leaf detection
and recursive iteration. A focused regression publishes 256 distinct directory entries
while retaining an existing span and verifies that its address and values remain stable.
The configured registry maps these model tests to `EditorAssetWorkflowTests`; the new
case and focused `LevelEditor` build pass.

Stage 3 implementation and automated validation are complete. The exact 16-level,
200-edge fixture proves that queuing a missing node performs no enumeration and that
published ancestor storage remains stable as descendant entries are added. Existing
mutation coverage now explicitly observes cache invalidation before repopulation.
`EditorAssetWorkflowTests` passes with 83 tests run, 82 passed, and one existing
conditional skip; the full `all` build passes. The editor remained live through an
8-second startup smoke. Source-level allocation accounting on the same fixture is now
zero child-vector allocations and zero cached-path copies because the defensive vector
no longer exists. Interactive ImGui coverage for navigation, context actions, rename,
delete, and drag-drop was confirmed during final user acceptance. All stages and
acceptance gates are complete.

## Goal

Establish and enforce one frame-ordering invariant: from entry to
`DrawDirectoryTree()` until its recursive traversal returns, every published child
vector that can be observed through a span remains alive and unchanged. The map may
gain distinct entries because `std::unordered_map` rehash preserves references and
pointers to existing elements. Execute invalidating tree-originated side effects only
after that boundary, then traverse cached child spans directly without allocating or
copying `std::filesystem::path` values per node.

## Scope

- Define a tree-specific deferred-action phase in `FContentBrowserPanel`.
- Route every tree interaction that can navigate, refresh, mutate mounted content, or
  clear directory snapshots through that phase.
- Keep directory snapshot discovery staged through
  `RequestDirectoryChildrenSnapshot()` and
  `RefreshRequestedDirectoryChildrenSnapshots()`.
- Remove the per-node `std::vector<std::filesystem::path>` copy from
  `DrawDirectoryNode()`.
- Add focused regression coverage for deep cached traversal and request/refresh
  ordering.
- Record the lasting stable-entry traversal invariant in the Content Browser
  architecture contract after implementation.

## Non-Goals

- Replacing the directory cache with shared ownership, an arena, or a persistent tree.
- Moving filesystem enumeration to a worker thread.
- Changing mount discovery, hidden-folder filtering, tree expansion state, sorting, or
  navigation history semantics.
- Eliminating unrelated per-frame UI formatting allocations such as node labels and
  filenames.
- Adding application-hosted UI automation solely for this optimization.

## Design Decisions and Invariants

1. Insertion of a distinct `DirectoryChildrenCache` entry is permitted during
   traversal. Rehash may invalidate map iterators but does not invalidate references or
   pointers to existing elements, and it does not move an existing value vector's
   internal storage.
2. An entry already exposed through `GetDirectoryChildren()` must not be erased,
   replaced, or mutated until tree traversal returns. Whole-cache `clear()` is likewise
   forbidden during that lifetime.
3. Directory snapshot publication remains a pre-draw operation in the current design
   to keep synchronous filesystem enumeration outside recursive UI drawing, not because
   insertion would invalidate existing spans. Recursive drawing continues to queue
   missing paths for a later refresh.
4. A tree-originated action that can invalidate model snapshots must not execute from
   inside `DrawDirectoryNode()`. It is captured by value and committed after the tree
   child window has ended.
5. Tree navigation and directory context actions execute after the tree and before the
   content pane. This preserves same-frame content-pane navigation while ending every
   tree span lifetime first.
6. Asset moves delivered from drag and drop execute only after both browser panes have
   finished traversal, matching other mounted-content mutations. Payload parsing may
   occur during drawing, but the operation and publication may not.
7. At most one committed ImGui action is accepted per phase per frame. Scheduling uses
   an explicit helper that retains the first action and diagnoses an unexpected second
   action in non-shipping configurations; call sites do not assign action slots
   directly.
8. `GetDirectoryChildren()` continues to return a non-owning span. Its lifetime
   contract is the stable-entry tree phase rather than shared ownership.
9. Cache invalidation remains immediate outside tree drawing. No general-purpose
   delayed model invalidation or stale-cache grace period is introduced.

## Current Foundations and Gaps

- `FContentBrowserPanel::PrepareForDraw()` refreshes requested directory snapshots
  before either pane draws.
- `FContentBrowserModel::RequestDirectoryChildrenSnapshot()` mutates only the request
  set; its request-set rehash is unrelated to spans into the directory cache.
- `FContentBrowserModel::RefreshRequestedDirectoryChildrenSnapshots()` is the sole
  normal publisher of requested child vectors.
- Existing `DeferredContentAction` already protects item-pane snapshots from many
  context-menu mutations, but it has no tree-specific commit point or scheduling
  helper.
- Node click navigation and `Open Folder` execute immediately.
- Tree rename and delete call `FocusFolderInParent()`, which navigates and refreshes
  immediately.
- `AcceptAssetDrop()` executes `Operations.Move()` and publishes mounted-content
  mutation immediately for both tree and item targets.
- The current local `Children` vector protects against all of those invalidation paths
  at the cost of repeated allocation and path copying.

## Implementation Stages

### Stage 0: Freeze the mutation inventory and baseline

- [x] Audit every call reachable from `DrawDirectoryTree()` and classify it as
  read-only, distinct-entry insertion, existing-entry mutation, or
  snapshot-invalidating.
- [x] Confirm that mount refresh and requested snapshot publication occur only before
  `DrawContents()` in the normal frame path.
- [x] Create a deterministic deep-and-wide directory fixture suitable for model tests
  and manual allocation profiling.
- [x] Capture a baseline profile after the fixture's snapshots are warm, recording the
  allocations and copied path values attributable to the local `Children` vectors.

#### Acceptance Gate

- The invalidating call-site inventory includes node navigation, directory context
  open/rename/delete, and delivered asset drops, with no unexplained tree-reachable
  cache clear, erase, replacement, or existing-value mutation.
- Baseline evidence is recorded before the copy is removed.

### Stage 1: Introduce explicit tree and pane action phases

- [x] Add a private tree-action slot and named scheduling/execution helpers to
  `FContentBrowserPanel`; route existing content-action assignment through equivalent
  helpers so phase ownership is visible at call sites.
- [x] Execute the tree action immediately after the tree child window ends and before
  `DrawContentArea()` begins.
- [x] Defer node click navigation and directory context open/rename/delete through the
  tree phase, capturing normalized paths rather than references or spans.
- [x] Split drag-drop acceptance from mutation execution. Defer delivered asset moves
  through the post-pane content phase for both tree and item targets.
- [x] Preserve the current behavior of selection repair, rename focus, deletion-plan
  creation, errors, and mounted-content publication when each deferred action commits.
- [x] Add non-shipping diagnostics for an unexpected second action in the same phase
  without allowing it to overwrite the first action silently.

#### Acceptance Gate

- No tree call site directly invokes navigation, refresh, mounted-content mutation, or
  a filesystem operation while recursive traversal is active.
- Tree navigation updates the content pane in the same frame because it commits between
  pane traversals.
- Drag-drop mutation executes after both panes and cannot invalidate either pane's
  current snapshots.

### Stage 2: Traverse cached spans without copies

- [x] Replace the local child-vector construction in `DrawDirectoryNode()` with one
  `std::span<const std::filesystem::path>` used for hidden-child detection and recursive
  iteration.
- [x] Replace the temporary-copy comment with a concise stable-entry lifetime contract
  at the traversal boundary.
- [x] Keep the missing-snapshot placeholder behavior: a requested but unpublished node
  remains expandable for the current frame and receives its cached children on a later
  frame.
- [x] Verify that recursive requests mutate only
  `RequestedDirectoryChildrenSnapshots`, including when that set rehashes.
- [x] Verify separately that inserting enough distinct directory-cache entries to
  rehash the map does not invalidate a span into an existing entry; do not treat that
  safe growth as a prohibited mutation.

#### Acceptance Gate

- Warm directory-tree traversal constructs no per-node child-vector snapshot and copies
  no cached `std::filesystem::path` solely to preserve traversal lifetime.
- Deep expansion, hidden-folder filtering, leaf flags, sorting, and mount-root default
  expansion remain behaviorally unchanged.

### Stage 3: Regression coverage, profiling, and contract handoff

- [x] Extend `ContentBrowserModelTests.cpp` with deep traversal coverage that retains an
  ancestor span while publishing enough distinct descendant cache entries to force map
  growth, then proves the ancestor child values and storage address remain valid.
- [x] Preserve request/refresh ordering coverage to show that filesystem enumeration
  still occurs at the explicit refresh boundary for responsiveness rather than span
  lifetime safety.
- [x] Cover cache invalidation and repopulation after navigation or mounted-content
  mutation without retaining a span across the phase boundary.
- [x] Run the focused `EditorAssetWorkflowTests` target discovered from the configured
  native test registry.
- [x] Perform a manual editor smoke test for node navigation, context open, rename,
  delete confirmation, and tree/item drag-drop on a deeply expanded tree.
- [x] Repeat the warm-tree allocation profile and compare it with the Stage 0 baseline.
- [x] Add the directory-tree snapshot publication and stable-entry traversal invariant
  to `Documentation/Editor/Architecture/ContentBrowser.md`.

#### Acceptance Gate

- Focused native tests pass.
- Manual interaction checks preserve action ordering and show no stale tree or content
  pane after a committed action.
- Profiling shows the per-node child-vector allocation and path-copy cost is absent.
- The lasting invariant is documented outside this plan.

## Validation Matrix

| Concern | Validation | Required Evidence |
| --- | --- | --- |
| Snapshot request ordering | Focused model tests | Requests queued during traversal do not enumerate until the explicit pre-draw refresh |
| Safe cache growth | Forced-rehash model regression | Inserting distinct entries preserves an existing entry's span and child storage |
| Span lifetime | Deep ancestor/descendant model regression | Ancestor spans remain readable until the stable-entry traversal phase ends |
| Action phase ordering | Focused helper/panel test where practical, plus manual smoke | Tree actions run after tree traversal; content actions run after both panes |
| Navigation behavior | Manual editor smoke | Tree click and context open update the content pane in the same frame |
| Mutation behavior | Manual rename/delete/drag-drop smoke | No traversal invalidation, lost action, or stale snapshot after publication |
| Functional regression | `EditorAssetWorkflowTests` | Target passes under the configured preset |
| Performance | Before/after allocation profile on the same warm deep tree | Per-node child-vector allocation and cached-path copying are removed |
| Documentation | Plan and changed-document validators | Active plan and Content Browser contract have valid structure and links |

Build and test execution must follow
[Agent Build and Run Workflow](../../../Agents/BuildAndRun.md) and
[Agent Testing Workflow](../../../Agents/Testing.md).

## Definition of Done

- No known tree interaction can clear, erase, replace, or mutate an observed directory
  cache entry during recursive drawing; insertion of distinct entries remains allowed.
- `DrawDirectoryNode()` directly traverses cached spans and owns no defensive child
  vector.
- Tree navigation retains same-frame content-pane updates; filesystem mutations occur
  after both panes finish drawing.
- Focused tests, manual interaction coverage, and before/after allocation evidence pass
  their acceptance gates.
- The Content Browser architecture contract owns the lasting phase-ordering invariant.
- Every implementation stage lands as an isolated commit with plan status and stage
  provenance updated in the same commit.

## Deferred Follow-ups

- General UI-frame snapshot infrastructure shared by other panels.
- Background filesystem enumeration and cancellation.
- Optimization of node-label, normalization, and filename formatting allocations.
- Shared ownership for directory snapshots if a future consumer must retain children
  beyond the stable-entry phase.

## Related Documentation

- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
