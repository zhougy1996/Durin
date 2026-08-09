# Native Test Execution Granularity Stage 0 Evidence

This document is the Stage 0 handoff for
`Documentation/Plans/NativeTestExecutionGranularity.md`. It records topology,
classification, and bounded repair work. Raw counts and timings are owned by
the machine-readable
[`NativeTestExecutionGranularityStage0Baseline.json`](../../Plans/NativeTestExecutionGranularityStage0Baseline.json)
and its referenced JUnit artifacts.

Baseline source commit: `dd7c3472`

## Reproduction

The case and direct qualification baseline uses the selected Agent Build
Profile and the repository wrapper:

```powershell
.\DevTool.bat test --target all --schedule-random `
  --output-junit Build\NativeTestExecutionGranularityEvidence\case.xml --agent
.\DevTool.bat test --target all --include-direct --schedule-random `
  --output-junit Build\NativeTestExecutionGranularityEvidence\qualification.xml --agent
```

Target-internal order qualification uses a fixed GoogleTest seed while still
entering through DurinDevTool:

```powershell
$env:GTEST_SHUFFLE = "1"
$env:GTEST_RANDOM_SEED = "240809"
.\DevTool.bat test --target CoreFileSystemTests --agent
```

GoogleTest normalizes the recorded seed to `40811`. The two qualification
seeds were `240809` and `240810`.

## Classification

All 46 direct lifecycle registrations are ordinary migration candidates. The
only exceptional execution domain is
`NativeTestIsolationProbeCharacterization`: its two discovered cases and
custom runner intentionally coordinate multiple processes to prove shared-root
collision and isolated-root behavior. It stays outside every routine
aggregate.

GoogleTest death tests in `CoreUtilityTests`, `CoreObjectTests`,
`RHICommandListTests`, and `RenderContractTests` remain ordinary tests. Their
controlled child processes do not justify case-default execution. The harness
must distinguish an expected death child from an unexpected process crash so
the child does not leave a diagnostic sandbox after a successful parent case.

The ordinary migration order is:

1. Pilot: `AssetDecodeTests`, `AssetDerivedDataTests`,
   `CoreFileSystemTests`, `EditorShellTests`, `RenderShaderCacheTests`,
   `RenderShaderContractTests`, `RenderShaderServiceTests`,
   `RHIInitializationTests`, and `RHIThreadTests`.
2. Filesystem and service targets: `AssetCookTests`, `AssetImportCoreTests`,
   `AssetImportTests`, `AssetReferenceStoreTests`, `EditorHierarchyTests`,
   `ExternalToolTests`, `NativeTestIsolationProbeTests`,
   `RendererSceneContractTests`, and `SkeletalSceneLifecycleTests`.
3. Object, asset, editor, and CPU-render targets after shared lifecycle repair:
   `AssetPackageTests`, `CoreObjectTests`, `EditorAssetWorkflowTests`,
   `EditorPropertyTests`, `EditorRenderingTests`, `EnvironmentLightingTests`,
   `MaterialThumbnailTests`, `SkeletalAssetTests`, `SkyBoxTests`,
   `SplineTests`, `StaticMeshTests`, `StaticMeshThumbnailTests`,
   `TextureTests`, `TextureThumbnailTests`, `ThumbnailTests`, `ViewportTests`,
   and `WorldTests`.
4. Concurrency and death-test targets after their tracked repairs:
   `CoreConcurrencyTests`, `CoreUtilityTests`, `RenderContractTests`, and
   `RHICommandListTests`.
5. Lock-bearing targets: `MaterialTests`,
   `RendererResourceReloadVulkanTests`, `SceneImportVulkanTests`,
   `SkyBoxVulkanIntegrationTests`,
   `StaticMeshRenderPreparationVulkanTests`, `TextureCookIntegrationTests`,
   and `VulkanRHIIntegrationTests`.

The first six lock-bearing targets retain both `durin-gpu` and legacy
`renderer-runtime` serialization. `VulkanRHIIntegrationTests` retains only
`durin-gpu`. Target batching does not alter either lock or the configured
timeouts.

## Pilot Evidence

The nine pilot targets passed normal execution in three complete direct
aggregates and target-internal shuffled execution with both fixed seeds. They
left no successful run directories. They cover pure filesystem state,
derived-data overrides, shader compiler/cache/service lifecycle, application
shell globals, and RHI thread/initialization contracts without object-system,
GPU, renderer, external-process, or expected-crash ownership.

## Bounded Repair Queue

- `CoreConcurrencyTests`: three terminal-publication assertions were repaired
  in Stage 0 by waiting for documented asynchronous ownership release and by
  making the discard callable observe cancellation before returning. A
  separate shuffled-order failure still requires attribution-registration
  reset or explicit per-test registration.
- Object-backed targets: `DObjectInit` is process-scoped, but independent test
  helpers guard it with unrelated function-local statics. A shared test
  lifecycle must own one initialization boundary before these targets migrate.
- Death-test targets: expected child termination currently follows the same
  sandbox-retention path as an unexpected crash. The child must use a bounded
  ephemeral sandbox while the successful parent retains ordinary cleanup and
  failure diagnostics.
- Writable test state remains below each process `Work/Runs` directory. Checked
  in `DURIN_TEST_DATA_DIR` and the repository `Sandbox` project are read-only
  inputs; any newly found writer must move to a process sandbox before its
  target migrates.

## Validation Outcome

Three consecutive randomized case aggregates and their direct lifecycle
phases passed after the Stage 0 synchronization repair. Expected skip and
disabled sets were identical. The pilot passed both shuffled seeds with no
retained successful work. The raw baseline also retains every pre-repair
failure instead of presenting the final pass as the original result.
