# RDG Access Migration Baseline Diagnostic Receipt - 2026-10-08

Summary: Preserve the Stage 0 Vulkan reference failure, prerequisite lifetime fix, and three Release observations before RDG access and scene decomposition changes.

Last reviewed: 2026-10-08

## Environment and Authority

- Reference revision: `4bc5d8c033920eaa5c2d77696403dea29ae9d29b`. Passing observations
  include the pending compute owner lifetime fix and its regression from this
  handoff; no RDG access API or Base Scene decomposition is applied.
- Host: Windows x64; MSVC 14.44.35207; Ninja;
  `Win64-Release-DurinEditor`; `DURIN_ENABLE_TRACY=OFF`, verified
  `DURIN_WITH_TRACY=0` in generated compile definitions.
- Device: NVIDIA GeForce GTX 1060 6GB, Vulkan 1.4.312, driver `0x91908000`
  (`2442166272`). Threaded RHI; graphics/compute share family/index `(0,0)`;
  graphics-only production policy. Validation is off under Release auto policy.
- The user confirmed an exclusive quiet GPU lane. Diagnostic debugger runs
  exposed OBS and NVIDIA capture hooks. Passing Release observations set
  `DISABLE_VULKAN_OBS_CAPTURE=1` and `DISABLE_LAYER_NV_GR2608_1=1` in the test
  process environment, using the loader's advertised disable controls. No
  system-wide configuration or application lifecycle was changed.
- Authority: **diagnostic observation**. This adapter is not the named RTX 3090
  / Vulkan 1.4.325 fixture environment. These values neither freeze a portable
  budget nor satisfy the named timing gate. Plan-specific acceptance on this
  adapter requires an explicit user decision; Metal remains unqualified.

## Original Failure and Fix Validation

The unmodified reference's Release qualification aborted after 12.52 seconds:
`FRHIResource::AddRef` rejected a `BufferView` already marked for deferred
deletion. The same failure reproduced in Debug. A Debug debugger stack locates
the call in `FVulkanPendingComputeState::SetShaderParameters` during RHI replay.
Its partial-update owner rebuild released all old references before acquiring
the replacement snapshot, allowing an unchanged explicit view to reach zero
references. The fix acquires replacement owners first, then replaces the old
owner snapshot. It does not alter access, culling, transitions or rendering policy.

Evidence:

- Original Release build passed; failed test report:
  `Build/NativeTestResults/Win64-Release-DurinEditor/RdgAccessBaselineRun1.xml`;
  raw failure log:
  `Build/.agent-state/logs/20261008-180644-453880-13880-ctest.log`.
- Debug reproduction report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/RdgAccessBaselineDiagnostic.xml`;
  debugger stack: `Build/RdgAccessBaselineDebugStack.txt`.
- Independent regression
  `FVulkanTextureSamplingTests.ComputePartialRebindRetainsUnchangedExplicitViews`
  failed before the fix with the same assertion, and passed after it. It uses an
  explicit view whose only remaining owner is the pending compute binding,
  updates another binding before any dispatch, then rebinds the preserved view.
  No descriptor cache or command-storage reference masks its lifetime.
- Full `VulkanRHIIntegrationTests`: **114/114 passed** on Debug, including
  validation-enabled inline/threaded replay and deferred binding coverage.
  Report: `Build/NativeTestResults/Win64-Debug-DurinEditor/RdgAccessVulkanIntegration.xml`;
  raw log:
  `Build/.agent-state/logs/20261008-181838-335004-43452-VulkanRHIIntegrationTests.log`.
- Default `test affected --test-jobs 1 --report`: **8/8 targets passed** on Debug
  in 71.58 seconds: `EditorGridVulkanTests`, `SceneImportVulkanTests`,
  `StaticMeshRenderPreparationVulkanTests`, `TextureCookIntegrationTests`,
  `ThumbnailVulkanTests`, `VolumetricCloudSceneVulkanTests`,
  `VolumetricCloudVulkanTests`, and `VulkanRHIIntegrationTests`. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/RdgAccessStage0Affected.xml`;
  raw scheduler log:
  `Build/.agent-state/logs/20261008-182213-619749-43624-ctest.log`.

## Reproducible Observation Selection

Run from the repository root after setting the two process-local capture-disable
environment variables above:

```powershell
.\DevTool.bat configure --preset Win64-Release-DurinEditor -DDURIN_ENABLE_TRACY=OFF
.\DevTool.bat test GBufferQualificationTests --mode qualification --preset Win64-Release-DurinEditor --test-jobs 1 --report Build/Baselines/RdgAccess/2026-10-08/Run1.xml
```

Repeat with `Run2.xml` and `Run3.xml`. After each run preserve
`Build/Win64-Release-DurinEditor/Testing/Temporary/LastTest.log` as the matching
`Build/Baselines/RdgAccess/2026-10-08/Run<N>.log`; the successful JUnit output is
truncated by CTest and cannot replace these full raw logs. All three preserved
runs passed the registered
`FGBufferQualificationTests.StaticAndSplinePassMeetsFrozenRTX3090TimingAndMemoryGates`
case with its existing readback, output-parity and memory assertions. Shader/PSO
cache preparation and workload data are unchanged. Resolution is 1920x1080;
each timing population uses 30 warm-up and 120 measured frames.

## Normalized Observations

Durations are nanoseconds, copied from `HYBRID_PRODUCTION_QUALIFICATION` in the
three preserved logs. The fixture reports `status=observation` on this adapter.

| Metric | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Total median | 2171152 | 2171328 | 2195440 |
| Total p95 | 2176768 | 2176384 | 2201472 |
| Base Scene median | 1368176 | 1368080 | 1382752 |
| Production deferred median | 1326864 | 1327040 | 1341280 |
| Retained opaque median | 23776 | 23840 | 24080 |
| Sorted translucency median | 20128 | 20096 | 20256 |
| Active bytes | 69984000 | 69984000 | 69984000 |

The full logs additionally retain GBuffer, isolated deferred, full/half GTAO,
contact fragment/compute, constrained contact, FXAA, shadow and retained-memory
observations. This selected production timing route has clouds disabled; it
does not establish cloud-composite or editor-depth acceptance.

## Outstanding Evidence

- The [RDG access plan](../../Plans/RdgAccessDeclarationsAndBaseSceneDecomposition.md)
  still requires the complete feature matrix's graph captures, rendered output
  comparison, failure/result and finalization continuity evidence. The passing
  fixture assertions do not substitute for every planned route.
- CPU author/compile/record costs, graph transition/submission counts and exact
  route shapes are not emitted by this fixture and remain outstanding.
- No named RTX 3090 timing gate, independent async-queue topology or Metal
  compilation/runtime qualification is claimed. Preserve the authority rules in
  [Rendering Performance Baseline](RenderingPerformanceBaseline.md).
