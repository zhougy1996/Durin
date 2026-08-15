# Asset Architecture Final Integration Plan

Summary: Reconcile the completed asset simplification milestones, remove remaining legacy references, and transfer the final runtime/editor contracts.

Last reviewed: 2026-08-16

Status: Archived
Completed: 2026-08-16

## Current Status

M0-M4 of the
[Asset Architecture Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md)
are complete. M5 has reconciled the final cross-module surface, qualification,
lasting-contract handoff, and roadmap completion.

## Outcome

- Production source and tests contain no retired asset manager, draft,
  compatibility, import-registry, build-executor, or mutable runtime-domain API.
- Each public asset operation has one focused entry and one owning module.
- The completed child-plan behavior remains qualified as one integrated system.
- Lasting Runtime and Editor documents own the final contracts and the roadmap
  is completed without leaving implementation checklists behind.

## Non-Goals

- New asset, import, build, streaming, packaging, or source-control capability.
- Reopening completed child-plan decisions or changing DAST/DBLK wire formats.
- Repeating performance experiments whose implementation boundary did not
  change after their recorded child-plan baseline.

## Stage 0: Reconcile Milestone Evidence

- [x] Confirm every M0-M4 exit gate is complete and linked from the roadmap.
- [x] Reconcile deferred and conditional proposals against the final codebase.
- [x] Freeze the final production/test legacy-symbol inventory.

### Acceptance Gate

- Every roadmap requirement has implemented evidence or an explicit non-goal
  disposition.

All five owning child plans are completed and linked. Remote build execution,
streaming, packaging orchestration, source-control automation, and alternate
identity/format work remain explicit non-goals because no production consumer
was introduced. The final inventory uses the retired-symbol sets recorded by
M0-M4 and distinguishes private service implementation types and the focused
`AssetTestSupport.h` seam from production APIs.

## Stage 1: Remove Final Legacy Surface

- [x] Search source and tests for retired manager, umbrella, draft, migration,
  import-registry, build-executor, mutable-domain, and failure-injection APIs.
- [x] Verify focused public headers and module dependencies expose one owner per
  operation.
- [x] Remove or repair stale active-document links and descriptions.

### Acceptance Gate

- Retired production symbols and invalid active-document references are empty.

Production/test searches are empty for `FAssetManager`, `AssetSystem.h`, draft
stores/discard APIs, migration/upgrader and partial-compatibility state, public
import registry getters, build definition/function/request-owner APIs, deleted
Build headers, and mutable package-load mode/context APIs. AssetCore,
AssetImportCore, and AssetBuildCore expose focused operation/service/cache/host
headers. The only single/record handler registry types are private storage owned
by `FImportService`; failure controls are isolated in `AssetTestSupport.h`.
Stale AssetBuildCore module routing and the roadmap's deleted `BuildRegistry.h`
link were repaired.

## Stage 2: Integrated Qualification

- [x] Re-run focused boundaries affected by final reconciliation.
- [x] Pass the default full build, complete native tests, and hidden-window
  editor smoke under the repository workflows.
- [x] Reconcile the recorded load/refresh I/O baselines with the unchanged
  catalog/load implementation boundary.

### Acceptance Gate

- Program behavior, performance contracts, and lifecycle qualification pass as
  one integrated result.

Focused M4 evidence covers cache/host and every authored/cooked payload family.
The final default `Win64-Debug-DurinEditor` `all` build, complete native-test
aggregate, and eight-tick hidden-window Sandbox editor smoke passed on
2026-08-16. The M5 diff does not change catalog refresh, resolution, package
reader, or residency-hit algorithms. The qualified M0 bounds therefore remain
the controlling baseline: bounded header reads, one physical read for a cold
direct or redirected final package, zero for resolution and warm residency,
zero on a stable warm refresh, and changed-entry-only incremental refresh.

## Stage 3: Contract And Roadmap Handoff

- [x] Update Runtime and Editor ownership documents where final behavior was
  still described only by plans.
- [x] Complete this plan and the roadmap, then run changed, all-plan,
  all-roadmap, repository, and archive-inclusive documentation validation.
- [x] Archive the completed roadmap and disposition completed child plans to the
  normal monthly plan-archive batch.

### Acceptance Gate

- Lasting contracts own final behavior; no active plan or roadmap remains for
  the completed program.

Asset Packages and Asset Data Lifecycle own the catalog, residency,
compatibility, cache/host, immutable runtime-domain, payload, and Cook
contracts. Asset Import Framework and Content Browser already own the final
import and editor mutation workflows. Code Modules now routes AssetBuildCore to
its cache-and-host-only responsibility. Lifecycle validation passed before the
roadmap archive transaction. The scoped roadmap archive moved only this program
and repaired all six child-plan links. The plan-archive preview would also move
five unrelated completed programs, so the six completed asset child plans remain
in the completed queue for the normal repository-wide monthly batch rather than
expanding this task's mutation scope.

## Completion Criteria

- All stages and acceptance gates pass with evidence recorded here.
- The parent roadmap completion criteria are satisfied and validated.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
