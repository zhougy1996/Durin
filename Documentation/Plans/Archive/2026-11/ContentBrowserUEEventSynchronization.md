# Content Browser UE Event Synchronization Plan

Summary: Replace browser/query revision polling with incremental events and explicit cache invalidation while preserving asynchronous admission generations.

Last reviewed: 2026-11-15

Status: Archived
Completed: 2026-11-15

## Current Status

Baseline: cf535d2fa. Traced query, provider and view revision histories and their
consumers. Query/provider/service revision APIs and journals have been removed;
picker, view and panel consumers now use events or explicit cache invalidation.
Async captures use their existing cancellation tokens and activation/mount/project
admission. No standalone query Tick API remains. Four module boundaries are unchanged.
All acceptance gates passed on Win64-Debug-DurinEditor with the final source
changes over cf535d2fa. No source or build inputs changed after these receipts:

- Four-module selector: seven owning/direct-consumer targets passed; Data tests
  60/60, Workflow 174 passed with two existing Windows symlink-permission skips.
  Receipt: `Build/.agent-state/logs/20261115-031814-657758-32420-ctest.log`.
- AssetMetadataQueryTests 10/10 and EditorShellTests `FAssetPickerTests.*` 7/7.
  Receipts: `20261115-031852-826703-36416-AssetMetadataQueryTests.log` and
  `20261115-031909-417554-27460-EditorShellTests.log` under the same log directory.
- `build all` passed for Engine, Sandbox and RoadWeaver; receipt
  `Build/.agent-state/logs/20261115-031445-860247-28240-cmake.log`.
- One bounded `EmptyDefaultLevelShowsWorkspaceAndExitsNormally` startup/shutdown
  case passed (characterization mode, timeout 120 seconds); receipt
  `Build/.agent-state/logs/20261115-031914-526884-32572-ctest.log`.

Consumer audit found no removed API usages in the three projects. Obsolete cursor
coverage was replaced with event/coalescing, scoped invalidation and reentrant
refresh assertions. The stray browser registration type assertions were moved
from UI style tests into browser coverage, avoiding a new shell dependency.
Native catalog consistency revisions and the file source's existing metadata
polling remain intentionally outside this query-synchronization cleanup.

## Goal

Use the UE ContentBrowserData pattern: item-update events, explicit refresh events
and owned filter caches. Remove global asset-query and browser discovery cursors,
query revision histories and operation/view change replay. Retain generations for
async requests, activation, mount/project admission and native storage consistency;
asset catalog/deletion revision checks are outside cache synchronization scope.
No generic retry, recovery or replacement result framework is introduced.

## Implementation Stages

### Stage 1: Replace query and provider revision APIs

- [x] Publish asset-query changes without a public revision journal; migrate picker
      and metadata-query consumers to explicit invalidation/events.
- [x] Remove source discovery revisions and invalidate owned compiled-filter caches
      through content updates/refresh instead.
- [x] Keep immutable worker captures and activation/mount/project safety checks.

### Stage 2: Migrate data service and browser consumers

- [x] Expose item-updated/refreshed events and remove service/query revision histories.
- [x] Invalidate only affected view/tree caches and replace obsolete async requests
      through request generation/ownership rather than content cursors.
- [x] Migrate panel reconciliation and host consumers away from global query polling.
- [x] Preserve no-browser updates, multiple readers, reentrant event delivery,
      selection migration, explicit reconciliation and source retirement safety.

### Stage 3: Simplify and accept

- [x] Remove obsolete revision tests/wrappers; regress actual updates, cache reuse,
      explicit invalidation, async cancellation and reentry.
- [x] Audit consumers in every Durin.dworkspace project; run owning/direct-consumer
      tests and all workspace build for changed public interfaces and headers.
- [x] Run one bounded editor startup/shutdown case for changed event subscriptions.
- [x] Update concise implemented contracts, validate documents/plans and commit.

## Validation

Follow [build](../../../Agents/BuildAndRun.md), [testing](../../../Agents/Testing.md) and
[documentation](../../../Agents/Documentation.md) workflows. Use bounded browser and
AssetMetadataQuery coverage during iteration. Final all build covers foundational
AssetRegistry and browser public-interface migration across workspace projects.
Preserve existing platform skips and required native safety checks.
