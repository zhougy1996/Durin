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

The original GTX 1060 records above are historical observations. During the
resumed Stage 3-5 execution, the runtime reports RTX 3090 / Vulkan 1.4.351 / driver
`0x9a100000` (616.64). Their timings must not be compared across these environments.
The same-date candidate/reference observations below preserve this distinction.

- The [RDG access plan](../../Plans/RdgAccessDeclarationsAndBaseSceneDecomposition.md)
  still requires the complete feature matrix's graph captures, rendered output
  comparison, failure/result and finalization continuity evidence. The passing
  fixture assertions do not substitute for every planned route.
- The resumed-execution receipt below adds CPU phase and graph/native-submission
  observations; stable timing acceptance remains outstanding.
- No named RTX 3090 timing gate, independent async-queue topology or Metal
  compilation/runtime qualification is claimed. Preserve the authority rules in
  [Rendering Performance Baseline](RenderingPerformanceBaseline.md).


## Resumed Execution: Matched RTX 3090 Observations

The candidate is `92366a6cc9e14cb85e3ac98e2d8882e1874dafb4`. The matched
reference is `32bbb237222e3fc3c1d60904d59ae779d0611e1a`, immediately after the
independently validated compute-view lifetime fix required to run the original
`4bc5d8c033920eaa5c2d77696403dea29ae9d29b` baseline. It contains no migration or
batching implementation. The reference additionally carries identical CPU timing
and native-submit observation code, preserved in
`Build/Baselines/RdgAccess/2026-10-08/ReferenceQualificationProbe.patch`, SHA256
`ecf011e194ce024f78ba0e22c965920beb48bd9f62f54799f129ca91ea0f00f2`.
This patch is diagnostic instrumentation, not an alternative renderer baseline.

Both builds use Windows x64 / MSVC 14.44.35207,
`Win64-Release-DurinEditor`, Tracy explicitly off (`DURIN_WITH_TRACY=0`),
threaded RHI, validation off, graphics-only production policy, warm shader/PSO
preparation, and the same unchanged qualification workload and assertions.
The runtime device is NVIDIA GeForce RTX 3090, driver 616.64
(`0x9a100000`), Vulkan 1.4.351; the compile SDK is 1.4.357.0.
The two capture-disable process variables in the original selection remain set.

After the user paused other GPU work, two complete cohorts ran in alternating
order R1/C1/R2/C2/R3/C3 and R4/C4/R5/C5/R6/C6. No build or other agent test
ran concurrently. Idle observations fell from 47–57% / 87–88 degrees Celsius to
10–17% / 46–49 degrees / P8. This removes the previously identified heavy
external workload; it does not prove that every intermittent GPU stall is gone.
All twelve runs pass their readback/parity and memory assertions. The named
fixture still reports `observation`, since its frozen Vulkan patch is 325.

The first cohort preserves raw logs and reports as
`Build/Baselines/RdgAccess/2026-10-08/QuietReferenceRun1.log` through
`QuietReferenceRun3.log`, and `QuietCandidateRun1.log` through
`QuietCandidateRun3.log`, with matching `.xml` reports. The second cohort uses
suffixes 4 through 6 in the same directory. `QuietNormalized.json` preserves all
prefixed fields. Earlier `CandidateRun*`, `ReferenceRun*`, and
`PairedCandidateRun*` logs remain busy-lane diagnostics and are not mixed with
these results. No fastest run was selected for acceptance.

CPU probes run a separate 30-warm-up / 120-measured-frame production population
with half-resolution GTAO and contact shadows disabled. Authoring measures
`FSceneRenderer::Render` (graph-resource preparation and composition), after
prepared-view/resource resolution. RDG statistics measure compilation and
recording. Capture construction occurs outside these reported phase durations;
CPU capture does not alter the existing GPU timing populations. The original
GPU populations retain their own 30/120 counts. CPU values also have GoogleTest
properties with explicit unit suffixes.

### Cohort 1

| Metric | R1 | C1 | R2 | C2 | R3 | C3 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Author median (ns) | 30200 | 32400 | 30100 | 32050 | 32050 | 33100 |
| Author p95 (ns) | 40200 | 42100 | 41600 | 45900 | 46100 | 48700 |
| Compile median (us) | 22 | 23 | 22 | 23 | 24 | 23 |
| Compile p95 (us) | 29 | 30 | 29 | 33 | 34 | 30 |
| Record median (us) | 38 | 37 | 38 | 37 | 41 | 37 |
| Record p95 (us) | 51 | 51 | 50 | 53 | 56 | 51 |
| Total GPU median (ns) | 292272 | 293520 | 291952 | 290496 | 291488 | 286192 |
| Total GPU p95 (ns) | 313568 | 321760 | 417856 | 313440 | 305312 | 583584 |

### Cohort 2

| Metric | R4 | C4 | R5 | C5 | R6 | C6 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Author median (ns) | 34100 | 33300 | 30100 | 32350 | 30600 | 32750 |
| Author p95 (ns) | 47500 | 50100 | 39700 | 48200 | 42000 | 48300 |
| Compile median (us) | 25 | 24 | 22 | 23 | 22 | 23 |
| Compile p95 (us) | 32 | 30 | 29 | 35 | 28 | 31 |
| Record median (us) | 42 | 38 | 38 | 37 | 39 | 38 |
| Record p95 (us) | 55 | 52 | 49 | 54 | 51 | 50 |
| Total GPU median (ns) | 290192 | 292320 | 290400 | 312816 | 297296 | 299376 |
| Total GPU p95 (ns) | 521888 | 319040 | 313792 | 320992 | 302464 | 409440 |

### Comparison and Remaining Authority

The six-run median of run medians is 291720 ns reference versus 292920 ns
candidate (+0.41%) for total GPU cost. This aggregate is a diagnostic summary,
not a replacement for the per-run p95 gate. Total GPU p95 spans 302464–521888 ns
in the reference and 313440–583584 ns in the candidate. Candidate run 5 also has
+7.72% total median, with GBuffer and FXAA intervals increasing about 8% in the
same run. Those unchanged raster workloads moving together suggest a broader
execution disturbance, but do not establish its cause. The second complete
cohort did not resolve the unstable tails. Stable timing acceptance remains open.

Authoring median increases from a six-run median of 30400 ns to 32575 ns
(+7.15%, 2175 ns); author p95 increases from 41800 to 48250 ns (+15.43%).
These trigger the plan's investigation thresholds. Source inspection identifies
additional explicit resource metadata/parameter composition and an extra net
graph pass in the migrated production route; it does not isolate their individual
cost. The cost is recorded rather than hidden by a relaxed budget. Compile median
changes 22 to 23 us (+4.55%), recording median 38.5 to 37 us (-3.90%). Some paired
p95 values exceed +10%; the full populations above preserve that evidence.
Further stable-tail measurements and finer authoring attribution are required
before accepting the performance gate.

The CPU probe route's logical batches fall from 9 to 3. The larger scene GPU
route captures retain 11 passes/20 dependencies without clouds and 14/30 with
clouds, versus 10/19 and 13/29 previously, and now use four logical batches.
Texture transitions change 15 to 22 without clouds, 32 to 39 for compute clouds,
and 18 to 27 for fragment clouds: formerly native-hidden attachment/sample
handoffs are explicit, including two additional depth handoffs for the fragment
route. Distinct graph/raster callbacks remain observable; no states are recoupled
to suppress those counts.

A test-only counter immediately after successful Vulkan `Queue.submit` measures
one actual native submit for each of seven successful offscreen frames in both
reference and candidate, before output readback. Matched Release reports and raw
logs are `ReferenceSceneNative.xml` / `.log` and `CandidateSceneNative.xml` /
`.log` in the same baseline directory. The counter excludes output-resource
creation and readback. Thus native submissions do not increase; logical batch
counts alone would not prove this. The candidate regression asserts one native
submit and at most four scene logical batches.

All twelve runs retain 69984000 active bytes, 120315648 active bytes including
shadow targets, and the unchanged 268435456-byte retained-memory ceiling.
The fixture's correctness and memory gates pass. Five real Vulkan injected
raster failures preserve the original result and transactional publication;
declared aliases, optional producer culling, graphics fallback, async handoffs,
split barriers, terminal joins and pool reuse have separate contract coverage.
The required Debug all build and 97/98 affected targets pass; the sole failure
is the existing Windows Metal target-policy mismatch. No supported Metal host
or frozen Vulkan 1.4.325 receipt is available. These backend/hardware gates and
stable performance acceptance remain open; this receipt does not complete the plan.


## Test Host-Policy Follow-up

The historical 97/98 affected run above exposed an unconditional Metal-success
expectation on Windows, rather than shared-state contamination. The follow-up
registers that success/cook test only on Apple hosts and validates explicit
compile rejection with no shader products on non-Apple hosts. The isolated new
case and the entire affected MaterialCompilerTests target pass; reports are
`Build/NativeTestResults/Win64-Debug-DurinEditor/MetalHostPolicyIsolated.xml` and
`MetalHostPolicyAffected.xml`. This test-only fix does not alter the measured
renderer or compiler policy, rerun the original 98-target selection, or close the
supported-host Metal gate.
