# Native Test Execution Granularity Stage 3 Evidence

Profile: `windows-msvc-x64`

Preset: `Win64-Debug-DurinEditor-Tests`

Baseline commit: `d587e708`

## Migrated Targets

Stage 3 moved 21 ordinary targets to target-default execution:

- Stage 0 pilot: `AssetDecodeTests`, `AssetDerivedDataTests`,
  `CoreFileSystemTests`, `EditorShellTests`, `RenderShaderCacheTests`,
  `RenderShaderContractTests`, `RenderShaderServiceTests`,
  `RHIInitializationTests`, and `RHIThreadTests`.
- Filesystem and service group: `AssetCookTests`, `AssetImportCoreTests`,
  `AssetImportTests`, `AssetReferenceStoreTests`, `EditorHierarchyTests`,
  `ExternalToolTests`, `NativeTestIsolationProbeTests`,
  `RendererSceneContractTests`, and `SkeletalSceneLifecycleTests`.
- Previously shuffled object-backed expansion: `EditorPropertyTests`,
  `ViewportTests`, and `WorldTests`.

Their ownership boundaries are unchanged: writable data stays in the unique
process sandbox; filesystem, asset, import, and shader fixtures create or
replace named subdirectories and scoped service overrides; editor and world
fixtures use the shared process-once object-system initializer and reset their
per-test models; RHI fixtures drain their owned thread/initialization state;
and scoped services, stores, schedulers, and application-shell hooks are
destroyed before the next test. No production lifecycle was changed for
batching because repeated pilot runs exposed no leakage.

## Aggregate Evidence

JUnit artifacts are under
`Build/NativeTestExecutionGranularityEvidence/` in the qualifying checkout.

| Mode | Runs | Processes | JUnit wall seconds | Result |
| --- | ---: | ---: | ---: | --- |
| hybrid randomized | 3 | 910 each | 26, 27, 26 | pass |
| case randomized | 2 | 1,243 each | 27, 26 | pass |
| target randomized | 2 | 46 each | 18, 18 | expected Stage 4 repair failures |

Hybrid contains 21 target processes and 889 isolated case processes. Relative
to the 1,242-process Stage 0 baseline, it removes 332 launches (26.7 percent).
Its 26-second median is below the approximately 30-second Stage 0 median and
therefore passes the `baseline + 10 percent` gate.

The new controlled probe increases the ordinary case count from 1,242 to
1,243. Both case runs retained the two expected platform skips and two disabled
benchmarks. CTest records an internal skip in a successful target process as a
passing process rather than a skipped CTest registration; a focused
`CoreFileSystemTests` run confirmed its one GoogleTest skip, while the remaining
hybrid case skip stayed visible as an isolated CTest registration. No migrated
target changed its failure or skip semantics.

The randomized all-target diagnostic reproduced only the existing Stage 4
repair queue: both seeds failed `CoreObjectTests` and `AssetPackageTests`; the
second also reproduced `CoreConcurrencyTests` attribution-order dependence.
All 21 Stage 3 target-default processes passed in both runs.

## Failure Diagnosis

With `DURIN_TEST_BATCH_FAILURE_PROBE=1`, the hybrid aggregate failed
`NativeTestIsolationProbeTests` at
`FNativeTestProcessSandboxTests.ReportsControlledBatchedFailureWithRetainedWork`.
The output included the exact case-mode command, the process sandbox, and the
`controlled-batch-failure.txt` marker. The case-mode regex rerun selected one
registration, reproduced the assertion, and preserved its own sandbox. With
the environment variable absent, the same focused case passed through one
direct executable launch.

## Handoff

Stage 4 starts from `d587e708` plus the Stage 3 implementation commit. Its
remaining lifecycle repairs are the shared object initialization/reset used by
`CoreObjectTests` and `AssetPackageTests`, attribution registration reset in
`CoreConcurrencyTests`, and expected-death-child sandbox cleanup. The remaining
ordinary targets keep explicit `Stage 4` CASE migration metadata until their
randomized target qualification passes.
