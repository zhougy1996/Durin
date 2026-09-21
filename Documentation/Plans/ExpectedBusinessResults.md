# Expected Business Results Plan

Summary: Replace redundant asset operation result wrappers with C++23 expected while preserving diagnostics, asynchronous state, and publication effects.

Last reviewed: 2026-09-21

Status: Active
Completed:

## Current Status

Stages 0–3 are complete. Execution is paused at the user's request before Stage 4.
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

The strongest immediate candidates are detached import preparation, translation,
and validation. Asset loading needs a separate report/lifetime review. Save,
scene import, cache, and pending request outcomes require explicit state models;
their existing result types are retained unless a lossless replacement is justified.

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

## Selected Design

- Use `std::expected<T, FDomainError>` for a detached value and
  `std::expected<void, FDomainError>` for validation or an operation without a
  returned value. Preserve domain errors; do not reduce every failure to `FFileError`.
- Return multiple inseparable success values in a named struct, not additional
  output arguments or an opaque tuple. Physical path values use `FFilePath`;
  package, object, mounted, and source-hint identities retain their own types.
- Remove wrappers whose only purpose is `Error.Code == None`. Remove enum `None`
  only after auditing embedded errors, default states, serialized values, and all
  consumers. Expected success must not depend on a sentinel error.
- Keep parsing/decoding/validation silent. Formatting functions remain presentation
  adapters. Business boundaries choose severity and avoid duplicate logging.
- Use direct early returns and `std::unexpected` by default. Use `transform` or
  `transform_error` where they clarify a small mapping; do not introduce propagation
  macros or a general custom Result framework.
- Mark fallible entry points `[[nodiscard]]`. Access `error()` only on failure;
  move buffers, leases, and prepared values without introducing copies or new
  dangling views. Success is not a promise of persistence or async completion.
- Migrate each selected interface and all consumers together. Do not retain a
  permanent parallel bool/out-error API. Audit forward declarations before
  replacing a struct with an alias; aliases cannot satisfy `struct F...Result;`.

## Candidate Inventory

Paths below are relative to the repository root. Proposed new error/value names
are design targets, not claims that those types already exist.

| Current interface or result | Proposed treatment | Reason / boundary |
| --- | --- | --- |
| `PrepareTexture2DImport` / `FTexture2DPreparationResult` | `expected<FPreparedTexture2DImport, FTexture2DPreparationError>` | Detached output currently travels through `OutPrepared`. |
| `TranslateTexture2DSource` / `FTexture2DTranslationResult` | `expected<FTextureSource, FTexture2DTranslationError>` | Return authored source directly; retain decode cause and dimensions. |
| `TranslateVolumeTextureAtlasSource` / `FVolumeTextureTranslationResult` | `expected<FVolumeTextureSourceData, FVolumeTextureTranslationError>` | Same value/error split; retain layout, limit, and source identity facts. |
| `InspectVolumeTextureAtlasSource` / `FVolumeTextureAtlasInspection` | `expected<FVolumeTextureAtlasInspection, Image::FImageDecodeError>` after removing Error/bool from the value | A valid inspection with no confident layout is still success. |
| `FVolumeTextureImportSettingsResult` | `expected<void, FVolumeTextureImportSettingsError>` | Pure validation wrapper. |
| `FTexture2DSubmissionResult` | `expected<void, FTexture2DSubmissionError>` | Means submission/admission succeeded; completion stays in the existing callback. |
| `FStaticMeshRebuildResult`, `FVolumeTextureRebuildResult` | `expected<void, corresponding domain error>` | Preserve build/import/save causes, including effects carried by nested save results. |
| `FAssetImportDataResult` | `expected<void, FAssetImportDataError>` for validators | Error-only wrapper; audit other producers before choosing their success types. |
| `MakeSourceHint`, `ResolveSourceHint` / `FSourceHintResult` | Named `{Base, Hint}` success value for Make; `FFilePath` for Resolve; keep `FSourceHintError` | Replace coupled outputs without confusing hints with physical paths. |
| `FBulkDataResult`, `FEditorBulkDataResult`, `FPackageBulkDataResult`, `FEditorBulkDataStorageResult` | Expected with existing errors; choose void or owned value per producer | Validation/mutation wrappers are candidates; read admission and lease state are separate. |
| `FPackageResourceRangeResult`, `FPreparedPackageResourceResult`, `FPackageGenerationResult` | Expected with existing domain errors; inspect each producer's outputs | Range validation is immediately suitable; prepared resource ownership must stay explicit. |
| `FPackageResourceRegistrationResult` | `expected<FPackageResourceHandle, FPackageResourceRegistrationError>` | Already contains mutually exclusive handle and error. Preserve retirement/publication rules. |
| `FAssetReadResult` and `LoadPackage` / `LoadObject` | Extract `FAssetReadError`; return expected of the appropriate object/package pointer or void | Broad shared API; reports and package residency can remain meaningful after failure. |
| `FSoftObjectResolveResult`, `TSoftObjectResolveResult` | Expected of a resolve value retaining State/Object/ResolvedPath/bRedirected | Null allowed, not loaded, and loaded are distinct normal states; do not equate nullptr with error. |
| `FPackageResourceReadResult` | Retain until pending request state is separated from terminal completion | `Pending` is not an error or a completed value. |
| `FBulkDataReadResult` | Retain admission model in this plan | Acquired/Empty/Busy/Retired/ReadFailed and move-only lock ownership require more than bool success. |
| `FCacheGetResult`, `FCachePutResult` | Retain in this plan | Keep backend-neutral cache policy and normal misses explicit; no mandatory expected conversion. |
| `FAssetWriteResult`, `FPackageWriteResult`, `FAssetBatchSaveResult`, `FSceneImportResult` | Review and retain outcome/report types by default | Failure can coexist with committed packages, partial writes, recovery files, and useful diagnostics. |

Primary declarations:

- [Texture2D import](../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/Texture2DImport.h),
  [VolumeTexture import](../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/VolumeTextureImport.h),
  [StaticMesh import](../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/StaticMeshImport.h).
- [Asset import data](../../Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h),
  [source hints](../../Engine/Source/Runtime/Engine/Public/Asset/SourceHint.h),
  [bulk data](../../Engine/Source/Runtime/Engine/Public/Asset/BulkData.h),
  [package resources](../../Engine/Source/Runtime/Engine/Public/Asset/PackageResource.h).
- [Asset read result](../../Engine/Source/Runtime/Engine/Public/Asset/AssetReadResult.h),
  [loading](../../Engine/Source/Runtime/Engine/Public/Asset/Load.h),
  [asset write result](../../Engine/Source/Runtime/Engine/Public/Asset/AssetWriteResult.h),
  [package writer](../../Engine/Source/Runtime/Core/Public/Misc/PackageWriter.h),
  [batch save](../../Engine/Source/Runtime/Engine/Public/Asset/PackageSerialization.h),
  [scene import](../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/SceneImport.h).

## Implementation Stages

### Migration contract decisions

- Workspace search covers `Engine/Source`, `Engine/Tests`, `Sandbox/Source`,
  `Sandbox/Tests`, `RoadWeaver/Source`, and `RoadWeaver/Tests`. The initial
  selected-result/loading search reaches 151 files. Producers are concentrated
  in Engine asset runtime and AssetForgeBuiltins; consumers include editor
  workflows, cook/load adapters, test fixtures, and project source.
- Detached texture preparation and translation return owned values. Failed
  translations currently clear output buffers; the new interface exposes no
  failed value. Inspection without confident layout remains a successful value.
  Settings validation returns void. Submission returns admission only, retaining
  the existing game-thread completion callback and its publication diagnostics.
- Rebuild returns void while preserving mutations and save effects. Volume
  rebuild retains `FAssetWriteResult` through `SaveCause`; StaticMesh retains it
  through `CompletionCause.Error.SaveCause`. Neither failure implies rollback.
  `CreateTransientStaticMeshFromFile` also produces an object and therefore needs
  an expected pointer value rather than the void rebuild alias.
- Import validators return void; `InspectAssetImportInfo` returns owned import
  info. MakeSourceHint returns a named Base/Hint pair, while ResolveSourceHint
  returns `FFilePath`. Hint identities and physical paths remain distinct.
- Bulk creation/attachment returns owned bulk values; UpdatePayload and metadata,
  segment, and range validation return void. Storage inspection returns owned
  descriptor/path vectors; partially collected descriptors on validation failure
  are not a usable construction result. Prepared Read/Prepare return an owned
  unpublished closure; Revalidate returns void. Owned-resource construction and
  registration return handles. Loose-generation validation returns void and keeps
  its instrumentation counters separate (they can advance on failure).
- Loading retains the optional caller-owned `FAssetLoadReport*` on both branches.
  Reports are observations, not success values: redirects, reads and mutation
  evidence can precede root failure. Preserve existing early-return report
  behavior. `FAssetPackageLoadScope` still records completed dependencies and
  explicitly releases residency; expected pointers do not acquire ownership or
  roll back admitted dependencies. Internal linker callbacks that expose pending
  skeletons need separate review from public synchronous loaded values.
- Soft resolution returns a value containing State/Object/ResolvedPath/bRedirected.
  Allowed null and NotLoaded are successes. Rejected null, lookup failures and
  type mismatch are errors; preserve resolved identity diagnostics when present
  on a failing branch. Cache mutation and async admission/completion stay separate.
- Stage 3 retains `FAssetReadResult` for codec/inspection callbacks, pending
  skeleton bindings, registry operations, and explicit residency release. These
  are not parallel overloads of the migrated completed-load APIs. The codec
  boundary explicitly converts failures to `FAssetReadError`; resource, storage,
  and soft-object validation causes remain typed where available. Async handles
  store an optional terminal expected value and require completion before result
  access; their pending/loading state and strong retention remain independent.
- Retain pending package reads, bulk read admission and move-only leases, cache
  misses, and write/batch/scene publication reports. Their state and partial
  effects are not equivalent to a success flag.
- Error enums selected here are not reflected declarations; `ESourceHintBase`
  is reflected and unchanged. Retain sentinel enum values until each family has
  completed its embedded/default-state audit; expected branch state must never
  depend on them. Forward declarations requiring migration include preparation
  and volume-settings results, plus asset-read declarations in AssetWriteResult,
  PackageResourceError, AssetSubsystemFwd and PropertyView.
- Registry discovery confirms AssetImportDataTests, AssetBulkContainerTests,
  AssetPackageTests, AssetPackageReloadTests, AssetReferenceStoreTests,
  CookedMeshLoadingTests, TextureTests, TextureImportWorkflowTests, StaticMeshTests,
  AssetImportTests, SceneImportTests and EditorAssetWorkflowTests as relevant
  direct-hosted targets. GPU qualification is not implied by this API migration.
  Batch consumer edits and static audits before compilation; reuse unchanged
  passing evidence and reserve `all` for shared API stage acceptance.

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

Dependency: Stages 0 and 3 provide the new error boundary.

- [ ] Trace success and failure construction for asset writes, package writer
  phases, batch saving, and scene import. Document a state table covering committed,
  projection-pending, partially written, recovery-required, and uncertain outcomes.
- [ ] Preserve `Effect`, `State`, `RecoveryFiles`, `AffectedFiles`, `SavedPackages`,
  `FailedPackage`, outputs, and diagnostics wherever meaningful on either branch.
- [ ] Review `ImportSceneAssets`' redundant bool plus `OutResult`; prefer a returned
  report value if all consumers support it, without removing partial-success facts.
- [ ] Record explicit retention decisions for cache outcomes, pending package reads,
  bulk admission, and publication reports. Any selected expected conversion must
  first define complete success and failure payloads and update this plan.

Completion: the outcome review is documented and no partial-result data is lost.
If publication-facing code changes, validate relevant PackageWriterContractTests,
AssetSaveReadinessTests, AssetPackageTests, and SceneImportTests, including existing
failure-injection/rollback cases, plus the shared API build gate. Retention alone
does not require rerunning unchanged runtime tests.

### Stage 5: Close the migration and publish contracts

Dependency: Stages 1–4 complete with evidence-backed decisions.

- [ ] Search all project source/test roots for old signatures, dead result shells,
  success-only error inspection, and duplicated logging introduced by adapters.
- [ ] Move implemented contracts to their owning Runtime/Editor documentation;
  keep this plan as execution history rather than a second API specification.
- [ ] Record validation and retained interfaces; complete the plan only when all
  selected migrations and required gates pass.

## Validation and Handoff

Follow [Build and Run](../Agents/BuildAndRun.md),
[Testing](../Agents/Testing.md), and [Documentation](../Agents/Documentation.md).
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
