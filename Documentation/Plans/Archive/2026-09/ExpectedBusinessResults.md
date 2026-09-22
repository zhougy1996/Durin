# Expected Business Results Plan

Summary: Replace redundant asset operation result wrappers with C++23 expected while preserving diagnostics, asynchronous state, and publication effects.

Last reviewed: 2026-09-21

Status: Archived
Completed: 2026-09-21

## Current Status

Stages 0–5 are complete. The migration preserves asynchronous admission,
load-report observations, resource ownership and partial publication outcomes.
Stage 4 passed the shared build and 222 semantic cases; Stage 5 changed only
contract documentation and reused that runtime evidence. All selected interfaces
and retained exceptions are documented in the owning domains below.
Stage 1 validation: Win64-Debug-DurinEditor `all` build passed; TextureTests
(125), TextureImportWorkflowTests (21), StaticMeshTests (126), AssetImportTests
(14), and SceneImportTests (10) passed, 296 cases total. The inspection failure
fixture now checks only its error branch; a new ambiguous valid atlas case
checks successful dimensions and suggestions. Existing tests cover malformed
sources, limits, nested decode/save causes and asynchronous completion.
The initial targeted compile caught an rvalue passed to a borrowed volume build
input; it now borrows the expected value for the synchronous call. An initial
TextureTests run caught obsolete failure-value access; the final run passes.
No Stage 1 runtime inputs changed after its successful all build, only test assertions
and contract documentation. Domain-error sentinel values remain available for
existing default construction; expected success never inspects them.
Commit `1c001f246` established value-returning file fingerprints, encoded source
capture, and LDR image decoding, plus distinct JSON/YAML load and parse errors.
This plan continues at the business interface boundary. It does not repeat that
migration or authorize changes to persistence semantics.

Stage 2 validation: the final Win64-Debug-DurinEditor `all` build passed.
AssetBulkContainerTests (11), AssetImportDataTests (5), AssetPackageTests (189),
TextureTests (125), TextureImportWorkflowTests (21), StaticMeshTests (126),
AssetImportTests (14), and SceneImportTests (10) passed, 501 cases total.
Batch migration removed the selected wrapper structs and source-hint result
shell across all workspace projects. Resource construction returns owned values;
read admission/retirement, lease lifetimes, generation counters and publication
behavior remain separate. Initial compile/test iterations found obsolete member
access, non-void fixture assertions and cleared-output assertions; these were
corrected before the final passing runs. Source-hint tests additionally check
that failure owns its diagnostic paths without exposing a value.

## Goal

Return either a useful success value or a typed business error, remove redundant
success flags and output parameters, and make error propagation consistent.
Keep file causes structured until a business presentation boundary decides
whether to log or display them. Optional inputs and cache misses remain normal
outcomes. Expected does not add retries, rollback, ownership, or durability.

## Implemented Contracts

This plan records migration decisions and validation history. Current API and
ownership contracts are maintained by their owning domains:

- [Asset import](../../../Editor/Architecture/AssetImportFramework.md): detached values,
  validation, hints, submission, rebuilds and returned scene reports.
- [Asset packages](../../../Runtime/Assets/AssetPackages.md): synchronous/async loading,
  soft resolution, caller-owned reports, dependency residency and save effects.
- [Package bulk data](../../../Runtime/Assets/BulkData.md): construction, validation,
  registration, storage inspection, pending reads and admission leases.
- [Package persistence](../../../Runtime/Core/PackagePersistence.md#writer-outcome-reports):
  physical writer phases, committed/recovery/partial outcomes and file diagnostics.
- [Asset data lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md): cache outcomes
  and diagnostic ownership independent of build success.

## Implementation Stages

### Historical Migration Boundaries

The inventory covered all six source/test roots declared by `Durin.dworkspace`:
Engine, Sandbox and RoadWeaver. The initial selected-result/loading search reached
151 files. Each selected API and its consumers migrated together; ownership,
publication, scheduling, persistence format and retry policy stayed unchanged.

Stage 3 retained `FAssetReadResult` for codec/inspection callbacks, pending
skeleton bindings, registry operations and explicit residency release. These are
not parallel overloads of the completed-load APIs. Error sentinel enumerators
were retained for existing embedded/default states; expected success does not
inspect them. Stage 4 retained publication, cache and admission reports, with
only the scene bool/out-report pair replaced by a returned report.

Registry-selected semantic targets and each stage's shared `all` gate are
recorded below. Qualification targets received compilation coverage where their
call sites changed; this migration did not qualify GPU behavior.

### Stage 0: Confirm contracts and migration boundaries

Dependency: the existing file/result migration is the baseline.

- [x] Inventory producers, consumers, forward declarations, callbacks, and test
  helpers for the selected types across Engine, Sandbox, and RoadWeaver source
  and test roots declared by `Durin.dworkspace`.
- [x] Record which outputs remain meaningful on failure and which methods mutate
  caller state before returning. In particular, trace `FAssetLoadReport` and
  dependency residency on root-load failure, and nested save effects in rebuilds.
- [x] Settle ownership of loading reports on both branches, and classify soft
  reference states and async admission versus completion. Record decisions here
  before implementing those interfaces; do not infer rollback from expected.
- [x] Confirm affected native targets through the registry and identify module
  or reflected/serialized dependencies that constrain enum/alias replacement.

Completion: an explicit success/error/state mapping exists for every interface
selected for the following stages; retained report types are listed with reasons.

### Stage 1: Return detached import values

Dependency: Stage 0 mappings for import preparation and translation.

- [x] Migrate Texture2D preparation/translation and VolumeTexture translation,
  inspection, and settings validation to the value shapes in the inventory.
- [x] Migrate submission and StaticMesh/VolumeTexture rebuild wrappers to expected
  without changing completion timing, mutation, or persistence behavior.
- [x] Update importers, editor inspection/preview code, formatters, and tests;
  remove superseded wrappers and success-only error access.
- [x] Verify malformed input, limits, ambiguous but valid inspection, typed nested
  causes, successful values, and asynchronous rejection/completion distinctions.

Completion: no old wrappers/out-value forms remain for these interfaces. Relevant
TextureTests, TextureImportWorkflowTests, StaticMeshTests, AssetImportTests, and
SceneImportTests pass, along with the shared API build gate below.

### Stage 2: Simplify validation and resource construction

Dependency: Stage 0 producer inventory; integrate after Stage 1 to avoid repeated
edits to import callers.

- [x] Migrate import-data validation and source-hint creation/resolution.
- [x] Migrate the listed bulk validation/storage, range, preparation, generation,
  and registration wrappers with a success type chosen per operation.
- [x] Preserve existing empty/busy/retired admission states and lease lifetimes;
  do not include `FBulkDataReadResult` or pending read requests in this migration.
- [x] Verify invalid ranges/hints, normal empty data, resource ownership and
  retirement, storage failure propagation, and failed construction without a value.

Completion: selected wrappers and output parameters are removed with all consumers
migrated. Relevant AssetImportDataTests, AssetBulkContainerTests, AssetPackageTests,
and import targets selected from the registry pass, plus the shared API build gate.

### Stage 3: Separate loaded values from asset read failures

Dependency: Stages 0 and 2; load-report and lifetime decisions must be recorded.

- [x] Introduce a typed asset read error retaining classification and diagnostic
  context; preserve structured underlying causes where currently available.
- [x] Migrate synchronous package/object loading and typed overloads to expected
  values. Preserve object ownership and the optional report contract or its
  explicitly selected replacement on both success and failure.
- [x] Migrate soft-reference results while retaining null policy, not-loaded
  results, redirection identity, and successful-null behavior where allowed.
- [x] Update result conversion helpers, async boundary adapters, forward
  declarations, and every project consumer. Keep pending state outside terminal
  expected values; no scheduler or cancellation-policy redesign.
- [x] Verify missing assets versus I/O errors, type mismatch, redirects, allowed
  null references, failed root loads with admitted dependencies, and report parity.

Completion: migrated loading APIs no longer pair an error-only Result with an
OutObject/OutPackage. Relevant package, reference, cooked-mesh, and editor workflow
tests selected from the registry pass, plus the shared API build gate.

Validation: the shared `all` build passed on 2026-09-21
(`20260921-185510-137755-2732-cmake.log`). AssetPackageTests (189),
EditorAssetWorkflowTests (40), AssetPackageReloadTests (14),
AssetReferenceStoreTests (11), CookedMeshLoadingTests (4), and
EditorPropertyTests (40) passed: 298 cases. Reports are under
`Build/NativeTestResults/Win64-Debug-DurinEditor/`. This covers read categories,
redirects, allowed null, async cancellation/readiness, report evidence, and failed
roots retaining completed dependencies. The changed-document and all-plan
validators passed. `affected --explain` expands to all tests because the shared
native fixture header changed; bounded semantic tests were selected instead.
An additional 28 test targets containing migrated consumers compiled successfully,
including RoadSceneIntegrationTests and the five qualification targets
PackageBulkQualificationTests, MaterialQualificationTests,
AssetPackageReloadVulkanTests, SceneImportVulkanTests, and
StaticMeshRenderPreparationVulkanTests. These targets are not part of the default
`all` build. Qualification/GPU execution was not selected; this was compilation
coverage for changed call sites. The final compile log is
`20260921-190418-122941-6736-cmake.log`.

### Stage 4: Review publication outcomes without losing partial results

Decision: retain asset, physical writer, batch and scene outcome reports. Only
`ImportSceneAssets` changes shape: return its complete report by value, preserving
all report fields on early exits and partial publication. All workspace callers
can consume the returned report. One failure-injection fixture observed the old
output argument during execution; it now inspects live destination packages
independently, keeping the pre-publication visibility assertion meaningful.
Physical and business state tables belong to
[package persistence](../../../Runtime/Core/PackagePersistence.md#writer-outcome-reports)
and [asset packages](../../../Runtime/Assets/AssetPackages.md#production-save-and-load).
No expected conversion of publication reports is selected. Cache hit/miss and
put outcomes, pending package-resource requests, and bulk acquired/empty/busy/
retired/read-failed admission retain their existing state models and ownership.


Dependency: Stages 0 and 3 provide the new error boundary.

- [x] Trace success and failure construction for asset writes, package writer
  phases, batch saving, and scene import. Document a state table covering committed,
  projection-pending, partially written, recovery-required, and uncertain outcomes.
- [x] Preserve `Effect`, `State`, `RecoveryFiles`, `AffectedFiles`, `SavedPackages`,
  `FailedPackage`, outputs, and diagnostics wherever meaningful on either branch.
- [x] Review `ImportSceneAssets`' redundant bool plus `OutResult`; prefer a returned
  report value if all consumers support it, without removing partial-success facts.
- [x] Record explicit retention decisions for cache outcomes, pending package reads,
  bulk admission, and publication reports. Any selected expected conversion must
  first define complete success and failure payloads and update this plan.

Completion: the outcome review is documented and no partial-result data is lost.
If publication-facing code changes, validate relevant PackageWriterContractTests,
AssetSaveReadinessTests, AssetPackageTests, and SceneImportTests, including existing
failure-injection/rollback cases, plus the shared API build gate. Retention alone
does not require rerunning unchanged runtime tests.

Validation: Win64-Debug-DurinEditor `all` passed
(`20260921-231432-204690-50268-cmake.log`). PackageWriterContractTests (20),
AssetSaveReadinessTests (3), AssetPackageTests (189), and SceneImportTests (10)
passed, 222 cases total; XML reports are under the profile's NativeTestResults.
SceneImportVulkanTests compiled (`20260921-231757-022270-49236-cmake.log`);
GPU execution was not selected because rendering behavior is unchanged.
The final SceneImportTests run additionally verifies retained cancellation and
rejection diagnostics; an initial new assertion used the wrong enum namespace,
corrected before the passing run. Runtime code did not change after the all build.
Changed-document validation passed.

### Stage 5: Close the migration and publish contracts

Dependency: Stages 1–4 complete with evidence-backed decisions.

- [x] Search all project source/test roots for old signatures, dead result shells,
  success-only error inspection, and duplicated logging introduced by adapters.
- [x] Move implemented contracts to their owning Runtime/Editor documentation;
  keep this plan as execution history rather than a second API specification.
- [x] Record validation and retained interfaces; complete the plan only when all
  selected migrations and required gates pass.

Stage 5 audit: searched the six workspace source/test roots for obsolete selected
wrapper declarations and forward declarations, source-hint result shells, and
old output-value loading/import signatures; none remain. Retained aliases are
expected types with live consumers. Reviewed expected error access and adapter
logging at import, load, async, source/import-data and resource boundaries; no
new success-branch error access or duplicate logging was identified. Retained
codec and registry `FAssetReadResult` consumers remain intentional.

Corrected stale contract prose about cleared failure outputs, error-sentinel
success and separate transient-mesh outputs. Storage inspection now documents
owned vectors with no partial failed value; cache/pending/bulk reports have
explicit retention contracts. Replaced the plan's duplicate API inventory and
design specification with domain links and historical boundary decisions.
Changed-document and all-plan validation passed. Stage 5 has no runtime changes;
all earlier relevant passing build/test evidence is reused.

## Validation and Handoff

Follow [Build and Run](../../../Agents/BuildAndRun.md),
[Testing](../../../Agents/Testing.md), and [Documentation](../../../Agents/Documentation.md).
Each shared API migration requires an `all` build and affected project coverage.
Use semantic risk and the test registry to choose bounded tests; investigate
`affected --explain` rather than implicitly running unrelated GPU/application tests.
Test error categories, ownership, and partial outcomes, not just expected syntax.
Reuse passing evidence when relevant inputs are unchanged.

Commit each stage with updated checklists and exact Plan/Stage trailers required
by the repository. Validate plan lifecycle changes with the all-plan validator.
Do not combine this work with changes to retry policy, package format, cache
behavior, logging infrastructure, or a repository-wide conversion of every Result.
Cook/compiler-wide result families and Radiance/grayscale decode APIs remain
follow-up candidates outside this bounded plan unless scope is explicitly revised.
