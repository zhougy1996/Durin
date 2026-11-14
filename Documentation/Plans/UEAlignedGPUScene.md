# UE-Aligned GPU Scene Plan

Summary: Introduce persistent Primitive and Instance scene data, shared mesh-command bindings, and pass-local instance submission using UE-aligned interfaces.

Last reviewed: 2026-11-14

Status: Active
Completed:

## Current Status

Stages 1-4 correctness validation and workspace integration compilation are
complete. Stage 5 has retired the remaining supported-path uniform uploader,
dense per-primitive fallback array, culling-only stage descriptor and adapters.
Owning runtime contracts now describe the implemented ID-based submission path.
Windows Vulkan correctness and diagnostic workload measurements are recorded
below. Release/Tracy-off sampling is prepared with 30 warm-ups and 120 samples
per mode/view; the user cannot currently guarantee exclusive GPU ownership, so
Release timing acceptance remains open. Metal and native independent-queue
qualification remain explicit deferred tasks, as requested.
Stage 6 primary interface and grouping alignment is implemented and locally
validated against the inspected UE 5.8.2 model; its receipt follows.
Stage 7 is implemented and validated: it unifies dynamic allocation with scene storage and moves primitive command
associations into SceneInfo. Its final-input validation receipt follows; historical
candidate sizes, dual-domain dispatch and capacity figures below describe earlier stages.
Stage 8 is implemented and validated: shared CPU/GPU ordered runs, submission-scoped dynamic context
reuse and structural pipeline lookup identity. Its final-input receipt follows;
source pipeline retention described in Stage 7 is superseded by canonical-only retention.
Stage 9 is implemented and validated: consolidated submission selection, explicit collector records and one
attribute upload phase, cached pipeline identity and scene-owned SceneInfo.
Its rules supersede Stage 8's packed-byte record identity and shared SceneInfo lifetime.
Default culling policy is unchanged. Stages 1-9 make no timing speedup claim.
Stage 10 implements and validates the user-selected first performance batch: counted reusable
instance-ID uploads, combined cached-command lookup records, and pass-scoped
contiguous instance runs. Its before/after observations follow below; they are
diagnostic and do not close Stage 5's exclusive Release timing gate.
Implemented rules live in [GPUScene storage and submission](../Runtime/Rendering/Scene/GPUScene.md).

### Stage 1 validation receipt

Tested inputs are base `b747a0eef` plus the Stage 1 source/fixture changes, hashed
in `Build/Qualification/UEAlignedGPUSceneStage1Inputs.json`. Subsequent changes
are documentation only and do not invalidate these runtime results. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, application-hosted tests off.
The sandboxed initial build hit the documented MSVC C1902 PDB-service failure;
the same target built successfully outside the sandbox. No clean build was needed.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 154/154, including nine new GPUScene cases: packing/large origins, hidden/default/empty instances, stable reorder, one-payload patches, scene mutation hooks, abandoned/compile-failed retries, allocation rejection, growth/recovery and generation/GPU-safe reuse. Log: `Build/.agent-state/logs/20261112-192356-437508-17872-RendererSceneContractTests.log`. |
| `RenderContractTests` | 246/246 RenderCore/RDG/RHI contract cases. Log: `Build/.agent-state/logs/20261112-192412-740631-24828-RenderContractTests.log`. |
| `SandboxGameplayTests` | 13/13; Sandbox consumer compiled and passed. Log: `Build/.agent-state/logs/20261112-192440-461925-34220-SandboxGameplayTests.log`. |
| `RoadSceneIntegrationTests` | 5/5; RoadWeaver consumer compiled and passed. Log: `Build/.agent-state/logs/20261112-192527-731592-7744-RoadSceneIntegrationTests.log`. |
| `StaticMeshRenderPreparationVulkanTests FGPUSceneVulkanTests.*` | 1/1: ten successful native submissions and one rejected graph; 30 exact buffer-to-texture readbacks match Primitive/Instance/payload mirrors and zero capacity padding through patches, growth and recovery. Log: `Build/.agent-state/logs/20261112-192250-391512-30736-StaticMeshRenderPreparationVulkanTests.log`. |
| Existing Vulkan render regressions | 3/3: `FMaterialCustomDataVulkanTests.*`, `FMaterialAnimationVulkanTests.CachedCommandsInvalidateResourceChangesAndReuseNumericUpdates`, and `FStaticMeshRenderPreparationVulkanTests.HitProxyIdsRespectDepthBackgroundAndCacheInvalidation`. Log: `Build/.agent-state/logs/20261112-192704-915188-9196-StaticMeshRenderPreparationVulkanTests.log`. |

Native Vulkan used NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI,
with `VK_LAYER_KHRONOS_validation` loaded; the passing logs contain no validation
errors. CPU fixtures exercise inline replay and deliberately delayed completion.
No Metal execution or timing qualification was performed on this Windows host.
The final `all`/cross-backend shader migration gates remain assigned to Stage 5.
Affected-test discovery expands new test sources to the entire Engine test
project; the selected owning suites and both workspace consumer targets bound
Stage 1's actual behavior/API changes without running unrelated asset/editor tests.

This plan selects the architecture below. Runtime documents continue to describe
implemented behavior until the corresponding migration and validation land.
The completed indirect-culling work does not establish GPUScene support; its
historical scope and qualification remain valid for its tested implementation.


### Stage 2 validation receipt

Tested inputs are base `ac1bb114e` plus Stage 2 source/fixture changes, recorded
in `Build/Qualification/UEAlignedGPUSceneStage2Inputs.json`. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`; builds run outside the
sandbox for the established MSVC PDB-service limitation. Application-hosted
tests remain off. Later documentation edits do not invalidate these receipts.

| Selection | Result and evidence |
| --- | --- |
| `StaticMeshRenderPreparationVulkanTests` | 21/21: exact packed-buffer readbacks, direct/grouped/individual-reference pixels, selected-instance ObjectPosition in fragments, WPO/custom channels/masked shadows, Local/Spline mirrored normals, picking and approximately 1e12-origin depth, resource/numeric cache changes and independent/custom VF fallback. Log: `Build/.agent-state/logs/20261112-202056-727674-1556-StaticMeshRenderPreparationVulkanTests.log`. |
| `MaterialCompilerTests` | 103/103, including active/reserved stage order, stage sharing, selected pixel scene-buffer reflection, strict dependency closure and detached/Cook round trips. Log: `Build/.agent-state/logs/20261112-202154-641504-37196-MaterialCompilerTests.log`. |
| `MaterialCookTests` | 6/6: Game five-stage products, package loading, binary/version rejection and stripped authoring data. Log: `Build/.agent-state/logs/20261112-202356-169136-29088-MaterialCookTests.log`. |
| `MaterialCompileLifecycleTests` | 29/29. Log: `Build/.agent-state/logs/20261112-202422-570002-7476-MaterialCompileLifecycleTests.log`. |
| `RendererSceneContractTests` | 155/155, including persistent direct-ID buffer reuse, scoped dynamic records, unsupported factory fallback and unchanged persistent uploads. Log: `Build/.agent-state/logs/20261112-202502-918745-6136-RendererSceneContractTests.log`. |
| `RenderContractTests` | 246/246. Log: `Build/.agent-state/logs/20261112-202507-805874-38740-RenderContractTests.log`. |
| `RenderShaderContractTests` | 72/72: fixed vertex resource/layout ABI and exact descriptor checks. Also repaired a pre-existing deferred-light reflection assertion that omitted its existing LocalLights binding. Log: `Build/.agent-state/logs/20261112-202804-279518-40380-RenderShaderContractTests.log`. |
| `RenderShaderCookIntegrationTests` | 1/1. Log: `Build/.agent-state/logs/20261112-202818-838743-9576-RenderShaderCookIntegrationTests.log`. |
| `SandboxGameplayTests` / `RoadSceneIntegrationTests` | 13/13 and 5/5. Logs: `Build/.agent-state/logs/20261112-202833-438892-14904-SandboxGameplayTests.log`, `Build/.agent-state/logs/20261112-202849-009975-30464-RoadSceneIntegrationTests.log`. |

The incremental workspace `all` build passed in 27.27 seconds after the shared
ABI migration. Log: `Build/.agent-state/logs/20261112-202903-792412-4784-cmake.log`.
This covers remaining editor/tool/provider compilation, not unexecuted behavior
or a clean rebuild. Reuse the receipt only while relevant inputs remain unchanged.

Native Vulkan uses NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI with
`VK_LAYER_KHRONOS_validation`; the passing full-suite log has no validation errors.
CPU ownership/retirement fixtures use inline recording and delayed GPU completion.
Metal execution and native Metal compilation are unavailable on this Windows host;
its rejection policy is tested, while cross-backend qualification remains Stage 5.
No timing qualification or rendering speedup is claimed. Affected-test discovery
expands shared Engine/RenderCore/Renderer changes to unrelated asset/editor suites;
the selected suites cover scene access, mesh submission, material products and all
three workspace projects. The workspace-wide build checks remaining API consumers.



### Stage 3 focused validation receipt

Tested inputs are base `4086c3536` plus the Stage 3 changes recorded in
`Build/Qualification/UEAlignedGPUSceneStage3Inputs.json`, profile `windows-msvc-x64`,
preset `Win64-Debug-DurinEditor`, application-hosted tests off. Builds run outside
the sandbox for the established MSVC PDB-service limitation. Documentation-only
closeout edits do not invalidate these results.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 156/156. Explicit stable Instance IDs are independent of cache BatchId; invalid persistent references reject. Effective-state tests cover separately-created pipeline/declaration wrappers, binding/stream/bias/winding differences and ordered CPU runs without translucent merging. Log: `Build/.agent-state/logs/20261112-210327-358105-35056-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. Direct/CPU/GPU pixels match across Forward/GBuffer, custom channels, WPO, masks, mirrors and picking. Receiver native draws fall from 64/3/65 to one for compatible runs; the 65-instance case grows scene storage and rebinds correctly. Shadow comparison verifies 64 to one. Small groups also use CPU merging with GPU culling requested. Log: `Build/.agent-state/logs/20261112-210031-296373-16512-StaticMeshRenderPreparationVulkanTests.log`. |

The full Vulkan receipt preceded a final two-line ownership correction: retain
both the source lookup pipeline and the bucket's canonical pipeline. Scene
contracts were rerun for the final inputs, and the focused native cache
invalidation/numeric-update regression passed again (1/1), log
`Build/.agent-state/logs/20261112-210332-898519-27496-StaticMeshRenderPreparationVulkanTests.log`.
The full pixel/draw-count receipt remains applicable to unchanged merging,
scene-data, fixtures and shader inputs; the focused rerun covers the ownership delta.

Workspace `all` compilation passed, including Engine, Sandbox, RoadWeaver and
remaining editor/tool consumers. Initial broad log:
`Build/.agent-state/logs/20261112-210049-752105-37884-cmake.log` (49.02 seconds).
Final incremental confirmation after the ownership fix:
`Build/.agent-state/logs/20261112-210334-775924-40740-cmake.log` (1.73 seconds).
These are incremental compile/link checks, not a clean build or consumer runtime tests.

Vulkan used NVIDIA GeForce GTX 1060 6GB, API 1.4.312, threaded RHI and
`VK_LAYER_KHRONOS_validation`; no validation errors were reported. CPU fixtures
use inline recording and delayed completion. At the user's request, extended
cached-owner replacement/removal and mixed CPU/GPU partition lifecycle coverage,
Metal execution, timing qualification and broader native consumer tests are
skipped for that commit. The follow-up receipt below covers the additional
Stage 3 lifecycle cases; cross-backend and timing gates remain assigned to Stage 5.

### Stage 3 lifecycle validation receipt

Tested inputs are base `4bfed8fbe` plus the two test-source changes hashed in
`Build/Qualification/UEAlignedGPUSceneStage3ExtendedInputs.json`. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, application-hosted tests
off. This follow-up changes fixtures and documentation only; production source,
shaders and shared APIs are unchanged.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 157/157. The added shared-owner case verifies separately-created equivalent pipelines share a command while retaining independent source infos and lookup pipelines. Removing the canonical source preserves the survivor; replacing the survivor invalidates its old visible handle, releases both old pipelines and does not revive retired slots. Log: `Build/.agent-state/logs/20261112-224726-648201-25668-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. The extended custom-data case uses independent bindings over identical native geometry to form one 67-instance CPU run and GPU groups of 64 and three. Exact color/GBuffer/shadow comparisons pass for direct, CPU-merged, mixed indirect/direct and failed-admission fallback. Native receiver draws are 67, one, four and one respectively. Budget rejection, command-admission failure, retry and source removal are checked. Log: `Build/.agent-state/logs/20261112-225249-614107-19676-StaticMeshRenderPreparationVulkanTests.log`. |

The full native run exposed two existing fixture teardown sequences that closed
render-command admission before queued scene destruction released GPUScene.
Both now flush after resetting scene owners, before shutting down the rendering
thread; the passing full-suite run covers those fixes and the shared test helper.
No runtime workaround or assertion suppression was introduced.

The initial stale DevTool operation was recovered after confirming its old
process tree had exited. Owning test targets and dependencies compiled outside
the sandbox for the established MSVC PDB-service limitation. No new workspace
`all` build was needed for these test-only changes; the earlier workspace receipts
remain historical evidence for their recorded environments.

This run used NVIDIA GeForce RTX 3090, Vulkan 1.4.351, threaded RHI and
`VK_LAYER_KHRONOS_validation`; the passing log contains no validation errors.
Scene contracts use inline recording. Affected-test discovery expands the
GPUScene fixture to unrelated Engine asset/editor suites; the two named targets
cover all changed executable behavior. Stage 3's correctness checkbox is closed.
Metal, broader consumer runtime coverage and timing remain Stage 5 gates;
this receipt does not claim full cross-backend or performance qualification.


### Stage 4 validation receipt

Tested inputs are base `aeaeb77e6` plus the Stage 4 source/fixture changes hashed
in `Build/Qualification/UEAlignedGPUSceneStage4Inputs.json`. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, application-hosted tests off.
Builds ran outside the sandbox for the established MSVC PDB-service limitation.
Later documentation edits do not change these executable inputs.

| Selection | Result and evidence |
| --- | --- |
| `RendererSceneContractTests` | 158/158. Covers ID/owner packing, indexed argument preservation, exact and overflowing budgets, missing/out-of-range scene IDs, instance-frequency rejection, multiple view contexts, graph abandonment, graph compile rejection, allocation failure and fresh-context retry. Dual-owner compute reads, graphics/indirect consumers and independent-compute scheduling dependencies are checked before deliberately rejecting allocation. A successful retry exercises complete CPU fallback; unchanged persistent attributes still upload zero bytes. Log: `Build/.agent-state/logs/20261113-023512-014001-10000-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. Direct/CPU/GPU color, GBuffer and shadow regressions pass. The extended case checks zero/partial/full visibility, mixed CPU/GPU partitioning, persistent plus scoped dynamic owners, 1e12 origins, ordinary/async requests, four failure modes, retry and source removal. 128 candidates across two owners use two indirect draws and 2,600 transient bytes; scene capacity is reported separately. Log: `Build/.agent-state/logs/20261113-023301-145623-40192-StaticMeshRenderPreparationVulkanTests.log`. |
| `RenderContractTests` | 246/246 RDG/RHI contracts, including queue synchronization and terminal completion behavior. Log: `Build/.agent-state/logs/20261113-023525-023578-26748-RenderContractTests.log`. |
| `RenderShaderContractTests` | 72/72 fixed shader/layout/reflection contracts. Log: `Build/.agent-state/logs/20261113-023547-052095-26724-RenderShaderContractTests.log`. |
| `MaterialCompilerTests` | 103/103 generated stage, reflection and persisted-product contracts. Log: `Build/.agent-state/logs/20261113-023648-378576-9896-MaterialCompilerTests.log`. |
| `RenderShaderCookIntegrationTests` | 1/1. Deterministic Cook library construction and runtime admission pass with six ordinary VF requests after retiring the culling graphics request. Log: `Build/.agent-state/logs/20261113-023800-889696-20972-RenderShaderCookIntegrationTests.log`. |
| `RendererRDGAllocatorTests` | 8/8 after retiring the transform allocation observation. Log: `Build/.agent-state/logs/20261113-023822-932719-10696-RendererRDGAllocatorTests.log`. |
| `SandboxGameplayTests` / `RoadSceneIntegrationTests` | 13/13 and 5/5. Logs: `Build/.agent-state/logs/20261113-023835-381711-27432-SandboxGameplayTests.log`, `Build/.agent-state/logs/20261113-023851-910063-37960-RoadSceneIntegrationTests.log`. |

Workspace `all` compilation passed in 19.83 seconds, covering remaining editor,
tool and project consumers of the mesh-pass key, VF virtual interface and telemetry
layout changes. Log: `Build/.agent-state/logs/20261113-023857-686485-4312-cmake.log`.
This is an incremental integration build, not a clean rebuild or application smoke.
Affected-test discovery expands the shared fixtures to unrelated Engine asset/editor
suites. The named targets cover scene submission, shader contracts/Cook, RDG/RHI,
allocation and both workspace consumers; unrelated suites were not executed.

Native Vulkan used NVIDIA GeForce RTX 3090, API 1.4.351, threaded RHI with
`VK_LAYER_KHRONOS_validation`; the passing log reports no Vulkan validation errors.
Its graphics and compute roles share one physical queue. Native async requests
therefore validate that fallback, while CPU contracts validate independent-queue
planning and dependencies. They do not establish native execution on a distinct
compute queue. Metal execution, native independent-queue qualification, broader
workload coverage and timing remain Stage 5 gates. No culling-policy change or
rendering speedup is claimed.


### Stage 5 local validation and remaining qualification

Tested inputs are base `bfc3dd2ea` plus the Stage 5 source/fixture hashes in
`Build/Qualification/UEAlignedGPUSceneStage5Inputs.json`. Profile:
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor`, application-hosted tests off.
Builds run outside the sandbox for the established MSVC PDB-service limitation.
Later documentation edits do not invalidate runtime receipts. Affected-test
analysis expands the shared Engine/test changes to unrelated asset/editor suites;
selected owning suites, both project consumers and the required workspace `all`
build cover this contract migration.

| Selection | Result and evidence in `Build/.agent-state/logs/` |
| --- | --- |
| `RendererSceneContractTests` | 158/158, including committed upload statistics and capability fallback. `20261113-030856-670120-26648-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21, threaded RHI: persistent/scoped IDs, exact readbacks, direct/grouped/indirect parity, Local/Spline, WPO/custom data, hit proxy, independent factory fallback, growth/recovery and failures. `20261113-030901-238633-28136-StaticMeshRenderPreparationVulkanTests.log`. |
| `MaterialCompilerTests` / `MaterialCookTests` | 103/103 and 6/6: six active stages, persisted ordinals, authoring migration and obsolete cooked product rejection. `20261113-030938-063370-8404-MaterialCompilerTests.log`, `20261113-030958-426280-19872-MaterialCookTests.log`. |
| `MaterialCompileLifecycleTests` | 37/37. `20261113-031021-336189-19856-MaterialCompileLifecycleTests.log`. |
| `RenderShaderContractTests` / `RenderShaderCookIntegrationTests` | 72/72 and 1/1. `20261113-031048-077649-12784-RenderShaderContractTests.log`, `20261113-031054-617623-23360-RenderShaderCookIntegrationTests.log`. |
| `RenderContractTests` | 246/246. `20261113-031056-700425-8812-RenderContractTests.log`. |
| `VolumetricCloudSceneVulkanTests` / `RendererResourceReloadVulkanTests` | 1/1 and 2/2, threaded RHI: offscreen/presentation routes, attachment parity and shader/device/manual invalidation. `20261113-031116-384232-33776-VolumetricCloudSceneVulkanTests.log`, `20261113-031132-594715-36792-RendererResourceReloadVulkanTests.log`. |
| `SandboxGameplayTests` / `RoadSceneIntegrationTests` | 13/13 and 5/5. `20261113-031141-524213-24964-SandboxGameplayTests.log`, `20261113-031149-769365-21148-RoadSceneIntegrationTests.log`. |
| Native inline recording | The same Vulkan static mesh, scene-route and resource-reload targets pass 21/21, 1/1 and 2/2 with `DURIN_RHI_EXECUTION=inline`. `20261113-031240-213239-31948-StaticMeshRenderPreparationVulkanTests.log`, `20261113-031252-354459-40872-VolumetricCloudSceneVulkanTests.log`, `20261113-031259-483651-5380-RendererResourceReloadVulkanTests.log`. |
| Workspace `all` build | Incremental build passed in 6.73 seconds after the final API/build-graph migration. `20261113-031326-887845-31716-cmake.log`. Covers remaining editor/tool/provider compilation, not unexecuted behavior or a clean build. |

Native Vulkan uses NVIDIA GeForce RTX 3090, API 1.4.351, driver 616.64, with
`VK_LAYER_KHRONOS_validation`. Native graphics and compute both resolve to queue
(0,0), so scheduled async tests exercise same-queue fallback rather than proving
independent physical queue execution. Native Metal execution/compilation is
unavailable on Windows. Its direct shader fixture has been migrated to the
16-byte ID candidate, 144-byte view uniform and fifth instance-data buffer, but
requires macOS compilation and checked readback before acceptance.

The opt-in diagnostic matrix passed at
`Build/.agent-state/logs/20261113-030817-978437-1724-StaticMeshRenderPreparationVulkanTests.log`.
Set `DURIN_GPUSCENE_SAMPLE_FILE` to an absolute CSV path, then run
`StaticMeshRenderPreparationVulkanTests` with selector
`FMaterialCustomDataVulkanTests.StableInstancesMatchDynamicMaterialsAcrossPassesAndGPUGrouping`.
Raw samples: `Build/Qualification/UEAlignedGPUSceneStage5Samples.csv`; median/p95
summary: `Build/Qualification/UEAlignedGPUSceneStage5SamplesSummary.json`.
There are two warm-up iterations and six measured samples per mode/view, with
alternating direct/CPU-merged/GPU-enabled order (108 samples total). The three
cascades use a positive-depth orthographic camera; shadow timings aggregate all
three passes. Only the first cascade contains this fixture's casters. Culling
remains disabled by default, with the existing 64-instance floor.

All timing values below are diagnostic medians in microseconds. CPU preparation
measures render-thread `PrepareViewResources` wall time across retries, excluding
initial collection, readback and full frame time. This Debug/Tracy run has no
exclusive quiet-lane admission and cannot establish a speedup or a regression.
D/C/G denote direct, CPU-merged and GPU-enabled modes; the three-instance G case
uses its normal CPU fallback.

| Workload | CPU preparation D/C/G | GBuffer GPU D/C/G | Culling GPU G | Shadow GPU D/C/G |
| --- | --- | --- | --- | --- |
| Static small (3) | 1301.6 / 1451.1 / 1489.5 | 7.168 / 6.928 / 7.152 | 0 | 8.368 / 7.152 / 7.008 |
| Static large (256) | 20286.4 / 25361.3 / 25064.6 | 36.032 / 25.568 / 24.384 | 17.216 | 38.720 / 32.592 / 34.192 |
| Dynamic large (256) | 23285.4 / 25253.8 / 26504.0 | 67.472 / 51.984 / 50.336 | 31.264 | 77.328 / 70.912 / 70.256 |
| Two views (64 each) | 6279.4 / 7890.7 / 7961.6 | 9.568 / 8.272 / 8.000 | 7.312 | 12.000 / 11.408 / 11.328 |
| Three shadow cascades (64) | 6456.7 / 8153.7 / 8272.4 | 8.928 / 7.328 / 7.920 | 6.432 | 26.432 / 25.472 / 27.232 |

Steady native geometry/shadow draws are N/N in D and 1/1 in C/G for N=3,256,64.
Static, camera-only and additional-view attribute uploads are zero. One dynamic
instance transform/custom edit commits exactly 816 bytes in three ranges once
per iteration, charged to the first rendered mode; remaining modes upload zero.
Steady samples rebuild no scene buffers and create no retained commands. Current
attribute GPU capacity/CPU mirror capacity are 107296/54464 bytes for small, then
213792/160960 bytes retained through the later smaller workloads. This explicitly
shows high-water capacity retention. GPU culling transient storage is 5140 bytes
for 256 or 1300 for 64; shadow backend target allocation is 50331648 bytes in
both candidates because the existing target always reserves three array layers.
These are component capacities, not total
resident memory; retired generations, allocator/map overhead and other frame
resources are excluded.

Stage 5 remains open for Metal inline/threaded correctness, independent physical
queue execution, and equivalent Release/Tracy-off measurements in an exclusive
quiet GPU lane following the [qualification policy](../Development/Build/NativeTestQualification.md#performance-qualification-and-concurrent-agents).
The final two Stage 5 checkboxes therefore remain open; local diagnostic samples
do not satisfy authoritative timing acceptance.


### Stage 5 Release sampling follow-up

The user requested Release performance acceptance while deferring Metal and
independent physical queue validation. The user then confirmed that exclusive
GPU ownership cannot currently be guaranteed. Under the
[quiet-lane policy](../Development/Build/NativeTestQualification.md#performance-qualification-and-concurrent-agents),
Release timing acceptance therefore remains open; this follow-up prepares and
checks the sampling pipeline without claiming an authoritative timing result.

Tested inputs: base `31cff44b4` plus the fixture SHA-256 and exact compile command
in `Build/Qualification/GPUSceneRelease/inputs.json`. Runtime modules, shaders
and material/Cook ABIs are unchanged, so the preceding correctness, consumer and
workspace `all` build receipts remain applicable. The changed fixture is built
and checked in Release and Debug. Release uses `Win64-Release-DurinEditor`, MSVC
19.44.35228.0, `/O2`, `NDEBUG`, and verified `DURIN_WITH_TRACY=0` after explicit
`DURIN_ENABLE_TRACY=OFF` configuration. Sampling uses threaded RHI, validation
off, backend compute-queue policy disabled and production async compute off.
The five existing 192x192 workloads and direct/CPU-merged/GPU-enabled modes are
unchanged. The `release` profile adds 30 warm-up iterations and 120 measured
iterations per mode/view; authoritative collection requires three consecutive
runs. The smaller default diagnostic profile retains two warm-ups/six samples.

`update_ns` measures the dynamic instance edit, custom-data/vector publication
and render-thread drain once per iteration, charged only to the first mode.
It excludes attribute-buffer writes, which occur inside the separately timed
resource-preparation call. It includes scheduling/wait time and cannot be
interpreted as isolated mirror packing cost. Preparation, GBuffer/culling/shadow
GPU durations, attribute upload bytes/ranges, native draws and component capacity
counters retain the measurement scopes documented above.

Release diagnostic execution passed: 1/1 selected case in 63.827 seconds,
with 2160 measured rows after warm-up. Raw artifacts are under
`Build/Qualification/GPUSceneRelease/`: `diagnostic-run-1.csv`,
`diagnostic-run-1.xml`, `inputs.json`, and `summary.json`. The summary contains
median/nearest-rank p95 for every mode/view workload, separate GPU pass times
and measured-pass totals, plus upload/draw/capacity ranges. Full execution log:
`Build/.agent-state/logs/20261113-032306-049234-35396-StaticMeshRenderPreparationVulkanTests.log`.
Release pre-sampling correctness: 1/1,
`Build/.agent-state/logs/20261113-032131-930493-30468-StaticMeshRenderPreparationVulkanTests.log`;
XML `Build/Qualification/GPUSceneReleaseCorrectness.xml`. Debug validation-on
regression: 21/21,
`Build/.agent-state/logs/20261113-032450-710025-37712-StaticMeshRenderPreparationVulkanTests.log`;
XML `Build/Qualification/GPUSceneRelease/debug-regression.xml`.

The following values are diagnostic median/p95 in microseconds, on an Intel
Core i7-12700 and RTX 3090 (driver 616.64, Vulkan 1.4.351, SDK 1.4.357.0).
The GPU column sums each sample's measured GBuffer, culling and shadow durations
before computing percentiles; it excludes lighting/postprocess/full-frame time.
No performance conclusion follows from this nonexclusive single run.

| Workload | D preparation | C preparation | G preparation | G measured GPU passes |
| --- | --- | --- | --- | --- |
| static-small | 132.00/165.30 | 146.30/181.60 | 148.55/182.60 | 18.29/25.18 |
| static-large | 1451.35/1600.30 | 1781.65/1947.10 | 1780.50/1958.00 | 74.00/80.74 |
| dynamic-large | 1645.85/2078.10 | 1871.35/2131.10 | 1979.15/2369.30 | 66.64/73.34 |
| multi-view | 511.25/611.70 | 608.55/730.00 | 604.40/782.80 | 120.82/162.91 |
| shadow-heavy | 520.85/663.90 | 619.95/846.30 | 621.35/752.70 | 141.63/203.90 |

Dynamic edit/publication/drain CPU median/p95 is 219.85/292.70 microseconds. All 120 dynamic edits upload exactly 816 bytes
in three ranges once; all unchanged/static/extra-view samples upload zero.
No steady sample rebuilds a scene buffer or misses a retained command. Native
draw counts and retained component capacities match the prior diagnostic
matrix. Neither sample stability nor lower native draw counts establish an
exclusive lane. Three quiet runs have deliberately not been performed.

For a future exclusive window, configure Tracy off and run the same selector
three times with distinct `DURIN_GPUSCENE_SAMPLE_FILE` CSV and `--report` XML
paths. Set `DURIN_GPUSCENE_SAMPLE_PROFILE=release`,
`DURIN_VULKAN_VALIDATION=off`, `DURIN_RHI_EXECUTION=threaded`,
`DURIN_VULKAN_COMPUTE_QUEUE=disabled` and `DURIN_TEST_KEEP_WORK=1`.
Use direct exact-target execution, since this fixture belongs to an integration
target on Windows; `--mode qualification` filters it out of CTest and discovers
no tests. Local reproduction helper:
`Build/Qualification/RunUEAlignedGPUSceneRelease.ps1 -QuietGpu` (three passes,
only after explicit quiet-lane admission); `-Diagnostic` runs one pass and
cannot satisfy timing acceptance. The helper and raw artifacts stay in ignored
`Build/` per the [evidence policy](../Agents/Documentation.md#validate-changes).

Pending acceptance remains: three consecutive quiet Release passes with raw
CSV/log/XML and median/nearest-rank p95 review. Metal inline/threaded and
independent physical queue execution remain deferred TODOs. No latency threshold,
automatic culling policy or performance acceptance checkbox is changed.


### Stage 6 primary interface alignment

The user authorized semantic changes toward the local UE 5.8.2 implementation
(CL 56702186). Tested inputs are base `9ef5ee68d` plus source/fixture hashes and
UE reference hashes in `Build/Qualification/UEAlignedGPUSceneStage6Inputs.json`.
Local reference root: `D:/Programs/UE_5.8/Engine/Source/Runtime/Renderer`.
The selected boundaries follow `Public/MeshPassProcessor.h` and
`Private/InstanceCulling/InstanceCullingContext.cpp`; byte packing remains the
explicit Durin ABI.

- Add submission-only `FMeshDrawCommandOverrideArgs`/`FMeshDrawCommandSceneArgs`.
  Override instance-buffer byte ranges and indirect argument ranges during
  binding emission/submission. CPU merged entries likewise select their ID
  buffer on visible commands. Neither route clones commands or pass-binding
  arrays for resource replacement. A contract case verifies nonzero offsets,
  immutable source bindings, cached-range restoration and rejected invalid offsets.
- `CollectMeshDrawCommands` adapts prepared providers to a common visible-command
  array. Setup consumes visible entries; submission consumes an array, start/count
  and overrides. Contexts associate with their manager at creation, and RDG
  results expose `InstanceIdOffsetBuffer`/`DrawIndirectArgsBuffer`. Frame storage
  retains borrowed dynamic command values without a self-retaining alias cycle.
- Replace GPU source-wrapper grouping with consecutive effective command/pass
  state comparisons, including actual scene buffers and bias. Independent but
  equivalent descriptions/declarations/buffer-set wrappers group; incompatible
  intervening draws split runs. The native 64+3 independent-provider case now
  emits one 67-instance indirect draw instead of one indirect plus three direct
  draws, with identical output. A synthetic partial-admission case preserves
  complete CPU-run restoration when dispatch fails. Existing policy and limits
  are unchanged.
- Keep ownership explicit: scene attribute storage and scoped dynamic buffer
  owners remain separate in this stage. CPU fallback run storage remains an
  adapter; this is not UE's full in-place visible-array compaction. Contexts are
  owned by callers/graph callbacks instead of an unused manager context list.
  Dynamic allocation-domain unification and source resource-owner migration are
  separate work, not implied by interface-name alignment.

Profile: `windows-msvc-x64`, `Win64-Debug-DurinEditor`, application-hosted tests
off; builds run outside the sandbox for the established MSVC PDB limitation.
Native Vulkan uses RTX 3090, API 1.4.351 with validation enabled. The affected
selection includes unrelated material/editor suites through shared Renderer/test
headers; the selected owning suite, shader contract suite, scene/recovery native
paths and both workspace consumers cover this interface migration. The required
workspace `all` build covers remaining editor/tool/provider compilation. RHI and
material/Cook/shader byte ABIs are unchanged. A final direct-include addition only
makes the already-used GPUScene declaration dependency explicit; the final `all`
and Release builds cover that header, and preceding Debug results remain valid.

| Selection | Result and log under `Build/.agent-state/logs/` |
| --- | --- |
| `RendererSceneContractTests` | 159/159: equivalent independent state grouping, order/binding breaks, partial-admission rollback, nonzero ID-buffer offsets, invalid offsets and source/cache restoration. `20261113-040410-750865-33280-RendererSceneContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21 threaded: exact attributes, direct/CPU/GPU pixels, 67-instance cross-provider grouping, fallback/retry, WPO/custom data, shadows, picking and independent VF. `20261113-040450-277539-35644-StaticMeshRenderPreparationVulkanTests.log`. |
| `RenderShaderContractTests` | 72/72. `20261113-040502-437149-27660-RenderShaderContractTests.log`. |
| `VolumetricCloudSceneVulkanTests` / `RendererResourceReloadVulkanTests` | 1/1 and 2/2 threaded, including scene-route output and resource invalidation/recovery. `20261113-040516-143550-18608-VolumetricCloudSceneVulkanTests.log`, `20261113-040524-946091-8076-RendererResourceReloadVulkanTests.log`. |
| `SandboxGameplayTests` / `RoadSceneIntegrationTests` | 13/13 and 5/5. `20261113-040528-060732-37904-SandboxGameplayTests.log`, `20261113-040529-951010-22296-RoadSceneIntegrationTests.log`. |
| Vulkan inline recording | Static mesh 21/21, scene routes 1/1 and resource reload 2/2 with `DURIN_RHI_EXECUTION=inline`. `20261113-040532-718356-26372-StaticMeshRenderPreparationVulkanTests.log`, `20261113-040544-726080-15228-VolumetricCloudSceneVulkanTests.log`, `20261113-040552-556018-2784-RendererResourceReloadVulkanTests.log`. |
| Workspace `all` | Incremental build passed in 18.99 seconds. `20261113-040620-150142-6432-cmake.log`. |
| Release correctness | Numeric/custom-data direct/CPU/GPU case 1/1 with validation on, `Win64-Release-DurinEditor`, Tracy off. `20261113-040724-788742-16256-StaticMeshRenderPreparationVulkanTests.log`; XML `Build/Qualification/UEAlignedGPUSceneStage6Release.xml`. This is correctness coverage, not timing acceptance. |

Stage 5's earlier performance samples describe their original tested inputs;
Stage 6 changes submission/preparation work and grouping, so those timings cannot
be reused as performance acceptance for this revision. No new timing comparison
is claimed. Metal, native independent-queue and exclusive quiet Release timing
qualification remain open. All scene dirty-commit and GPU-completion retirement
semantics remain in force.

## Goal

Separate scene updates from view/pass preparation. All supported scene mesh draws,
including single-instance direct draws, read persistent scene data through IDs.
Compatible commands share bindings across Primitives. Views prepare visibility,
LOD selection, instance-ID lists and draw overrides without rebuilding or uploading
unchanged scene attributes.

Align the major names, responsibilities and data flow with UE. Do not claim
source compatibility or copy an undocumented UE byte layout. UE public API pages
span versions; the Durin declarations below are selected adaptations, not quoted
UE signatures. Shader packing and backend lowering remain explicit Durin ABIs.

## UE Reference Model

| UE reference | Selected alignment |
| --- | --- |
| [Mesh drawing pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine) | Retained commands, identical bindings for dynamic instancing, scene-wide Primitive data selected by ID; view/pass command setup remains legitimate work. |
| [FPrimitiveSceneShaderData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FPrimitiveSceneShaderData/__ctor?lang=en-US) | Distinguish semantic Primitive parameters from their packed scene-buffer representation. |
| [FInstanceSceneShaderData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FInstanceSceneShaderData) | Packed instance data identifies its Primitive and instance-relative identity; CPU packing and shader decoding agree. |
| [FPrimitiveSceneInfo](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FPrimitiveSceneInfo) | SceneInfo tracks instance allocation and requests GPU-scene updates. |
| [FInstanceCullingContext](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/FInstanceCullingContext?lang=en-US) | Pass-local setup, culling and submission over GPU-scene instances, separate from scene-data ownership. |
| [UE 5.4 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.4-release-notes?application_version=5.4) | Persistent GPU-scene Primitive identity is distinct from a packed CPU scene-list index. |

`FGPUScene`, `FGPUSceneResourceParameters`, and the update lifecycle below adopt
UE vocabulary. Exact private UE allocation/update implementation is not assumed
from public documentation. Durin does not need Nanite, bindless materials, Hi-Z,
native multi-draw, or UE's full scene-extension machinery for this design.

## Ownership and Main Interfaces

`FScene` owns one `FGPUScene` and its authoritative membership, CPU mirrors,
allocators and dirty state. The scene-associated render-resource owner retains
its native buffers through the existing resource registry; device recovery
rebuilds them from current mirrors. No process-wide scene-data singleton is added.

`FPrimitiveSceneInfo` stores a GPU-scene Primitive handle, instance range and
allocation generation. Engine proxies publish source facts; Renderer derives
GPU identity, packing and uploads. `FSceneRenderingService` coordinates execution
but does not merge data from different scenes.

| Interface/type | Responsibility and module |
| --- | --- |
| `FPrimitiveUniformShaderParameters` | View-independent semantic Primitive values; RenderCore value contract, built from Engine producer facts by Renderer. Also supplies any explicit non-GPUScene fallback. |
| `FPrimitiveUniformShaderParametersBuilder` | Checked construction of semantic parameters from transform, bounds, flags and custom data; keep validation/defaults shared with packing and fallback. |
| `FPrimitiveSceneShaderData` | Versioned packing of Primitive parameters; shared CPU/shader layout contract in RenderCore. No native allocation ownership. |
| `FInstanceSceneDataBuffers` | Immutable source snapshot of transforms, stable instance IDs, bounds and custom channels; RenderCore vocabulary published by Engine. It is source data, not a GPU allocation. |
| `FInstanceSceneShaderData` | Packed instance header/transform representation; RenderCore CPU/shader ABI. |
| `FGPUSceneResourceParameters` | Counted Primitive, Instance and payload buffer ranges plus limits; Renderer scene/pass binding and RDG access vocabulary. |
| `FGPUScene` | Renderer-private scene membership, ranges, dirty requests, update transaction and external-buffer access. |
| `FVisibleMeshDrawCommand` | Frame-local command reference plus Primitive/Instance selection, sort/state bucket and submission overrides. |
| `FInstanceCullingManager` | Frame/submission owner for shared culling resources and registered views; no second scene-attribute mirror. |
| `FInstanceCullingContext` | Per-pass command setup, CPU ID-list or GPU-culling construction and submission. |

Illustrative Renderer interface shape; use repository containers/RDG conventions
when implementing these declarations:

```cpp
class FGPUScene
{
public:
    auto AddPrimitive(FPrimitiveSceneInfo& Info) -> void;
    auto RemovePrimitive(FPrimitiveSceneInfo& Info) -> void;
    auto RequestUpdate(FPrimitiveSceneInfo& Info, EGPUSceneDirtyState Dirty) -> void;
    auto Update(FRDGBuilder& GraphBuilder) -> FGPUSceneUpdateResult;
    auto GetShaderParameters(FRDGBuilder& GraphBuilder)
        -> FGPUSceneResourceParameters;
};

// FPrimitiveSceneInfo forwards the request to its owning scene.
auto RequestGPUSceneUpdate(EGPUSceneDirtyState Dirty) -> void;

class FInstanceCullingContext
{
public:
    auto SetupDrawCommands(/* visible commands, view IDs, pass policy */) -> void;
    auto BuildRenderingCommands(/* graph, scene parameters */)
        -> FInstanceCullingDrawParams;
    auto SubmitDrawCommands(/* command list, visible commands, draw params */)
        -> void;
};
```

Dirty categories distinguish Added, Transform, PrimitiveCustomData,
InstanceData, Bounds and Removed. Coalesce requests by handle and revision;
remove wins over pending writes. Keep draw-state invalidation separate.
`Update` returns usable resources and dependency/revision information only for
an admitted transaction; it does not clear dirty state merely during graph setup.
Failed/canceled updates remain pending for the next valid submission.

## Identity and Data Structures

| Identity | Meaning |
| --- | --- |
| `FPrimitiveComponentId` | Existing component publication identity; not a buffer offset. |
| `FPrimitiveInstanceId` | Proposed typed wrapper for the existing stable nonzero uint32 instance ID, scoped to its Primitive; snapshot reordering preserves association. |
| GPU-scene Primitive handle | Internal persistent slot plus CPU generation; independent of CPU scene-list compaction. Shader `PrimitiveId` denotes its validated slot. |
| GPU-scene Instance ID | Current instance-buffer address; source stable ID maps to it. It may change when ranges move. |
| Instance-relative ID | Index inside the current Primitive instance allocation; never the public stable ID. |

Use UE-shaped names at the SceneInfo boundary: `PersistentPrimitiveIndex`
(a typed `FPersistentPrimitiveIndex`), `InstanceSceneDataOffset`,
`NumInstanceSceneDataEntries`, and `InstancePayloadDataOffset`. Keep allocation
generation in CPU ownership metadata, distinct from the shader index. Expose
`GetPersistentIndex()` and `RequestGPUSceneUpdate(...)`; callers must not derive
GPU identity from `GetPrimitiveSceneInfos()` vector positions or `BatchId`.
Packed logical fields use `PrimitiveId`, `InstanceRelativeId`, `InstanceFlags`,
`InstancePayloadDataOffset` and `InstanceCustomDataCount`; offsets have explicit
element/float4 units instead of interchangeable bare byte counts. These are the
selected Durin API names; packing is defined separately from semantic fields.

Allocate one default instance for an ordinary mesh, zero for an explicitly empty
instance snapshot. Preserve the current distinction between empty and cleared
snapshots. CPU handle generations reject stale requests; published GPU ID lists
must also be retired or ordered before allocation reuse. A generation check on
the CPU alone cannot protect an already-recorded GPU reference.

Selected logical data layout:

| Data | Fields/semantics |
| --- | --- |
| Primitive parameters | Primitive-to-world transform/origin, aggregate bounds, flags, instance offset/count, CustomPrimitiveData and explicit payload metadata. |
| Instance source | LocalToPrimitive, stable source ID, local bounds, CustomInstanceData. |
| Packed Instance | PrimitiveId, relative ID, flags, payload offset/count, composed transform/origin and instance bounds needed for shader access/culling. |
| Instance payload | CustomInstanceData, separate from Primitive channels; reserve extension through flags/offsets, not speculative fixed fields. |

`FPrimitiveSceneShaderData` and `FInstanceSceneShaderData` expose explicit
float4-aligned packing/stride constants, with shader `GetPrimitiveData` and
`GetInstanceSceneData` accessors. Do not upload compiler-dependent C++ object
layouts. Specify matrix order, padding, integer bit interpretation and invalid
IDs and verify them on Metal and Vulkan. Begin with uncompressed transforms;
compression is a later measured optimization.

Retain split high/low world origins and existing normal/inverse-transform
semantics. Store world-independent-of-view bounds/origins and translate relative
to the current view in shaders. Component transforms can dirty all composed
instances; changing one instance dirties only its record/payload and affected
aggregate bounds. This cost is explicit, not described as constant-time updates.
Retain the existing 32-float Primitive and Instance channel limits, finite-data
admission, conservative WPO bounds obligation and aggregate component LOD policy.

## Scene Update and Submission Flow

1. Ordered Add/Remove/Transform/CustomData/Instance commands change source state
   and request dirty updates. Full source snapshots may be diffed by stable ID;
   a whole-component CPU publication does not require a whole-scene GPU upload.
2. Before consuming views, commit the current scene revision: allocate/patch
   ranges, coalesce dirty byte ranges and admit uploads. Build from scene source
   facts, including hidden/offscreen members, rather than visibility discovery.
3. Import persistent buffers into each RDG that consumes them. One unchanged
   revision needs no repeated attribute upload. A later scene update between
   views produces a new ordered revision; do not cache an upload decision by
   frame number alone. RDG handles never escape their builder.
4. Prepare per-view visibility, requested/selected LOD and visible commands.
   Each visible command refers to scene-instance IDs/runs. Compatible commands
   can merge after comparison of their complete effective draw state.
5. CPU selection or GPU culling generates instance-ID lists and draw overrides.
   Compute reads persistent bounds; graphics reads the same Instance/Primitive
   records. Candidate IDs, visible IDs, counts and indirect arguments may remain
   transient. Complete transforms/custom channels are not copied per view.

`GetShaderParameters` must declare exact RDG read ranges. Dirty writes precede
compute/graphics readers, including cross-graph submissions and queue transfers.
Separate builders do not infer dependencies on one another: carry the existing
RHI submission/access dependencies explicitly through the scene resource owner.
Preserve CPU preparation/recording joins before mutation. On buffer growth,
initialize the new capacity from current CPU mirrors, publish a new resource generation,
and retire old buffers/bindings only after their recorded/GPU uses complete.
No historical per-view scene versions or unconditional GPU idle waits are added.

Stage 1 deliberately uses a full mirror upload on geometric growth instead of a
GPU copy followed by patches. This initializes unused capacity deterministically
and avoids a second copy/retirement path during initial adoption. Growth bytes
are reported separately by buffer rebuild count; unchanged-capacity updates stay
incremental. Revisit GPU-to-GPU growth copies only with measured growth cost.
The internal RequestUpdate takes a generation-bearing handle; SceneInfo retains
the UE-shaped RequestGPUSceneUpdate forwarding interface. Non-compilation graph
failure discards native storage before retry so a potentially recorded prefix
cannot authorize unsafe reuse or carry an unknown final access state forward.

The scene-resource binding is pass-scoped and late-resolved. Cached mesh commands
must not retain an obsolete raw scene buffer after growth/recovery. Use the
existing resource-table indirection where sufficient; otherwise add a bounded
generation-aware pass rebind, without rebuilding every geometry command.

## Mesh Commands, Shader Access and Fallback

Keep source-owned cache lookup/removal identity separate from instancing identity.
`FCachedMeshDrawCommandInfo` retains Primitive association. Add
`FMeshDrawCommand::MatchesForDynamicInstancing` and a consistent hash/state bucket
over effective PSO, shaders, topology, vertex/index buffers and ranges, resource
bindings and draw state. Hash equality alone is insufficient. Pipeline/binding
wrapper pointer equality alone must not reject equivalent effective state.

Exclude per-object transforms, numeric custom data and source IDs from that
comparison. Preserve differences in determinant/cull parity, depth bias, stencil,
material bindings, per-component streams and deformation resources. Translucent
ordering remains authoritative; shared scene access does not authorize arbitrary
translucent reordering or Spline merging.

Direct and indirect submissions use the same scene-data geometry shaders.
The vertex factory resolves an Instance ID through an instance-frequency ID
stream or the supported backend's equivalent indexed ID buffer; one direct draw
uses one ID. Preserve and qualify the current base-instance interpretation.
Pass a flat Primitive/Instance identity to fragments that need it. Existing
referenced-channel varyings may remain an optimization of these semantics.
WPO, masking and HitProxy must use the selected instance, not a group representative.
Carry picking identity through selected Primitive data or an indexed selection
table so per-draw picking uniforms do not silently defeat sharing.

`UseGPUScene` is a capability/vertex-factory data-access decision.
`bEnableGPUCulling` controls the selection algorithm independently. A small group
or unavailable compute pipeline uses direct/CPU-instanced submission with the
same GPUScene records. The 64-candidate floor is not a GPUScene admission floor.
Providers without scene-data support keep an explicit uniform path and cannot
join GPUScene groups. Dynamic providers use a scoped dynamic allocation/upload
context with the same shader addressing; keep those records alive through use,
without turning view-generated geometry into persistent scene membership.

Use an explicit instance-data access capability in mesh/VF preparation instead
of the GBuffer-specific `bGPUCulling` pipeline distinction. Provider-specific
deformation bindings remain independent. Missing scene data must fail admission
or the graph transaction, never draw partial/stale geometry. No unordered late
redraw is introduced.

## Existing Contract Changes

These are implementation prerequisites, not edits to current runtime claims in
this design-only change. Update each owning document with its migrated code.

| Owning contract / current rule | Required adjustment |
| --- | --- |
| [Scene representation](../Runtime/Rendering/Scene/SceneRepresentation.md): SceneInfo owns transform/bounds and scene commands publish updates | Add scene-owned GPU membership, persistent handles/ranges and RequestGPUSceneUpdate hooks for all source mutations and teardown. Preserve generic Primitive membership and queue ordering. |
| [StaticMesh rendering](../Runtime/Rendering/Scene/StaticMeshRendering.md): per-Primitive uniforms and view-local copied transforms; explicitly excludes GPUScene/instancing | Replace supported-path uniforms with scene IDs and packed scene resources. Keep visibility/LOD/prepared values view-local while sharing only scene-owned attribute data. |
| [Retained batch publication](../Runtime/Rendering/Scene/StaticMeshBatchPublication.md): source batch facts plus downstream Primitive IDs/instance uniforms | Publish instance source data independently from per-section geometry/material descriptions. Keep IDs out of geometry capability tables; attach instance ranges at visible-command construction. Dynamic/custom producers retain explicit admission. |
| [Material custom data](../Runtime/Rendering/Materials/MaterialCustomData.md): one batch per stable instance; 512-byte combined uniform; GPU grouping uses InstanceTransforms | Publish stable instance snapshots through FInstanceSceneDataBuffers, separate Primitive/Instance storage, and remove repeated Primitive channels. Preserve stable-ID APIs, limits, defaults, WPO and pixel semantics. |
| [Material stage selection](../Runtime/Rendering/Materials/MaterialShaderStages.md): optional GPUCullingVertexMain restricted to Local GBuffer | Make scene-data access independent of GPU culling and available to supported Local/Spline passes. Replace the culling-only geometry usage with the unified Local entry contract; migrate persisted masks and reject incompatible cooked products explicitly. |
| [Material system](../Runtime/Rendering/Materials/MaterialSystem.md) and [shader parameters](../Runtime/Rendering/Shaders/ShaderParameters.md): Primitive uniform binding/reflection | Add scene resource parameters and selected-instance access to fixed/generated shaders, strict reflection admission and VF bindings. Update translation/pass/binding compatibility identities and Cook invalidation; no unversioned reinterpretation. |
| [Frame preparation](../Runtime/Rendering/Pipeline/RendererFramePreparation.md): resolved PrimitiveUniforms and source-owned cache bindings | Add an ordered scene-update boundary, scene read parameters and visible instance selection. Retain CPU joins, current-value semantics, frame-local execution and transactional publication. |
| [Resource lifecycle](../Runtime/Rendering/Pipeline/RenderResourceLifecycle.md): source-owned Primitive uniform resources | Scene owns native scene buffers; source owns registration/cache associations. Define replacement generations, scene release, dynamic allocation retirement and recovery from mirrors. |
| [GBuffer](../Runtime/Rendering/Pipeline/GBuffer.md): per-view candidate/transform uploads and graphics permutation | Replace temporary attribute arrays with persistent scene reads; use generic FInstanceCullingContext. Keep default-disabled culling, direct fallback and correctness qualification until new evidence authorizes policy changes. |
| [Directional shadows](../Runtime/Rendering/Features/DirectionalShadows.md): direct per-Primitive uploads/reuse and cascade-local preparation | Read shared scene data in shadow vertices; preserve cascade view/bias and masked-WPO behavior. Add pass-local CPU merging without promising shadow-map content caching. Coordinate the existing [shadow optimization plan](ShadowPreparationOptimization.md), whose exclusion of GPUScene limits that plan, not this one. |
| [RHI resource views/transfers](../Runtime/Rendering/RHI/RHIResourceViewsAndTransfers.md) and [GPU completion](../Runtime/Rendering/RHI/VulkanMemoryAndGPUCompletion.md) | Preserve ordinary FRHIBuffer storage, WriteBuffer patches, counted ranges and completion retirement. Specify scene-level use of these contracts; extend RHI only if a demonstrated ID-fetch/synchronization capability is missing. Do not add a GPUScene-specific RHI resource class. |
| Telemetry in frame/GBuffer/shadow contracts | Separate scene dirty/upload bytes, resource rebuilds, visible IDs, command reuse, merge counts and culling costs. Replace assumptions that PrimitiveUniformUploads represents all instance-data cost; preserve defined legacy counters during migration or rename their consumers together. |

Material/VF/binding API changes require symbol searches in source and test roots
of Engine, Sandbox and RoadWeaver declared by `Durin.dworkspace`; migrate every
consumer, including custom providers. Inspect reflection/schema dependencies
before choosing version bumps. Existing archived acceptance receipts do not
qualify the changed ABI and must not be rewritten as new evidence.

## Implementation Stages

### Stage 0: Specify interfaces and contract migration

- [x] Select UE-aligned owners, identities, public vocabulary and submission flow.
- [x] Identify current contracts to replace or extend and preserved invariants.
- [x] Separate design from implemented runtime claims and define acceptance gates.

Outcome: this reviewable design. Stage 1 depends on this interface direction.

### Stage 1: Add scene storage and incremental updates

- [x] Implement semantic/packed values, stable handles, instance ranges and mirrors.
- [x] Wire scene mutation hooks and full-snapshot diffing into dirty requests.
- [x] Implement ordered patch uploads, failed-update retry, growth and recovery.
- [x] Validate ordinary/empty/multiple instances, offscreen mutations, stable-ID
  reorder/removal, range relocation, generation rejection and GPU-safe reuse.

Outcome: unchanged scene revisions upload zero attribute bytes; one instance
change patches only affected records/payload/bounds. Depends on Stage 0.

### Stage 2: Unify scene-data shader and direct submission

Stage 2 implementation decisions:

- Commit dirty persistent scene data through a dedicated upload graph before
  mesh binding preparation. Geometry graphs import the resulting counted buffers
  and declare reads explicitly. A successful geometry graph registers its terminal
  completion with GPUScene so logical slots wait for actual drawing, not merely
  the preceding upload graph. Scene-data commit is independent of view output.
- `FGPUSceneDynamicContext` prepares submission-owned packed records for unpublished
  or provider-specific transforms. Built-in factories advertise `SupportsGPUScene`;
  other factories retain their explicit uniform vertex contract. Direct draw
  overrides select an instance-ID list through base instance while cached geometry
  arguments remain source values. This establishes ID transport without Stage 3 merging.
- Local/Spline geometry stages serve both direct and indirect submission. Legacy
  material GeometryUsage bit 4 no longer contributes a culling-only stage; new
  materials default to mask 3. Authored load/edit and shader identity normalize
  legacy masks. The reserved stage ordinal remains readable for rejection and
  diagnostics; old binaries are rejected by Pass contract 13, generator revision
  16 and compile-adapter revision 13. The Cook container schema is unchanged because
  the persisted container shape is unchanged.
- Until Stage 4 removes the old culling plan, it still constructs/uploads its
  temporary transform array for existing budget/telemetry semantics; graphics
  selects scene IDs instead of reading that array. Per-group compatibility includes
  the scene-data owner so persistent and scoped dynamic IDs cannot alias.

- [x] Add scene accessors, VF capability and one-instance ID selection.
- [x] Migrate fixed/generated Local and Spline geometry, Forward/GBuffer/Shadow/
  HitProxy consumers, stage selection, reflection, Cook and cache compatibility.
- [x] Provide scoped dynamic scene records and explicit unsupported-provider fallback.
- [x] Validate CPU/shader packing, base instance, WPO/custom channels, masking,
  normals, negative scale, picking and 1e12-origin precision against direct output.

Outcome: supported direct rendering uses persistent scene data with GPU culling
disabled; all affected workspace consumers compile. Depends on Stage 1.

### Stage 3: Share commands and add CPU dynamic instancing

Stage 3 implementation decisions:

UE reference interfaces: [EPrimitiveIdMode](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/EPrimitiveIdMode),
[FMeshDrawCommand](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FMeshDrawCommand),
and [FVisibleMeshDrawCommand](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/FVisibleMeshDrawCommand).
Durin implements the fields required by its current transport; it does not add
unused UE indirect/root-constant override fields merely to copy the complete API.

- Providers select `EPrimitiveIdMode` on their batch and identify the stable
  source instance explicitly. Collection captures `PersistentPrimitiveIndex`;
  submission resolves that typed source reference after the scene update. Remove
  the ComponentId lookup map and all transform/payload repacking comparisons.
  An invalid persistent reference fails admission instead of silently becoming
  dynamic. Standalone consumers without a scene explicitly use scoped storage.
- `MatchesForDynamicInstancing` compares effective shaders, pipeline description,
  layout/declaration, streams/ranges and resource binding values. Hash buckets
  narrow candidates; wrapper addresses and source cache IDs do not decide equality.
  Cached commands remain source-owned; visible commands carry state bucket IDs and
  `PrimitiveIdBufferOffset` and `NumInstances` for submission-local ID ranges/counts.
- Merge consecutive compatible opaque/masked command runs without reordering.
  Keep Spline, sorted translucency, custom unsupported factories and instance-rate
  streams outside this path. Use a separate CPU-selected ID list with counted
  submission ownership and explicit RDG reads; no scene buffer is captured in a
  cached geometry command.
- Preserve original direct commands alongside CPU runs until GPU visibility
  submission is chosen. If a CPU run contains any GPU-admitted member, use original
  commands for its direct remainder; if dispatch fails, use the complete CPU run.
  Stage 4 can unify this selection with its common culling submission context.
  Logical draw conservation remains separate from actual native submission counts.


- [x] Add MatchesForDynamicInstancing/hash buckets, visible instance ranges and
  draw override parameters; keep source cache identity separate.
- [x] Carry explicit persistent/dynamic scene-data provenance from collection into
  visible commands. Remove Stage 2's per-view transform repacking/comparison used
  to distinguish provider overrides; normal scene draws should select validated
  scene IDs without rebuilding attribute data to infer ownership.
- [x] Route compatible opaque/masked and shadow submissions through CPU ID lists.
- [x] Validate equivalent separately-created state merges, incompatible bindings/
  winding/bias do not merge, ordering is preserved, and growth rebinds correctly.

Outcome: different transforms/custom channels share a draw when effective state
matches, including small groups. Depends on Stage 2.

### Stage 4: Generalize GPU instance culling

Stage 4 implementation decisions:

- `FInstanceCullingManager` owns compute-resource readiness and registered view
  snapshots; `FInstanceCullingContext` owns setup, graph construction and submission
  overrides for a mesh pass. The rendering service retains the manager, while
  graph callbacks retain contexts independently of the next view registration.
- Candidates are 16-byte Instance ID/argument/scene-owner records. Compute reads
  the existing 18-float4 instance ABI directly, subtracting view origin high/low
  before adding relative bounds. Per-owner compute passes declare counted scene
  reads and share ordered visibility/argument writes. There is no attribute mirror
  or per-view bounds/transform upload. Invalid packed bounds remain conservative.
- Direct and indirect rendering use the same shader, VF pass and retained command.
  Remove the culling graphics key, shader registration and late binding rebuild.
  Only ID bindings and argument ranges change at submission. Any admitted indirect
  member disables its CPU run for the direct remainder; failed admission restores
  complete CPU runs. Graph failure remains terminal, with fresh contexts on retry.
- Keep the default-disabled policy, 64-candidate floor and 65,536-candidate/4-MiB
  limits. Rebase transient charges to 20 bytes per candidate plus 20 per group;
  report each referenced owner's three attribute-buffer capacities separately
  for persistent and dynamic sources. Capacity observations are not upload costs.
  The shared telemetry type preserves existing GBuffer counter access.

- [x] Add manager/context setup and BuildRenderingCommands/SubmitDrawCommands.
- [x] Migrate GBuffer candidate processing to IDs and persistent bounds reads;
  delete per-view Transforms generation/upload and culling-only graphics shaders.
- [x] Validate zero/partial/full visibility, group budgets, multiple views/graphs,
  CPU fallback, async queue dependencies and allocation/cancellation failure.
- [x] Rebase transient storage budgets and telemetry to the new record sizes;
  account for persistent scene capacity separately from per-view culling cost.

Outcome: CPU selection and GPU culling consume the same scene attributes and
graphics path, without rendering readback. Depends on Stage 3.

### Stage 5: Qualify and retire replaced contracts

Stage 5 implementation decisions:

- Retire the unused blanket primitive-uniform uploader and dense uniform array.
  Explicit unsupported-factory uniforms use a sparse fallback map; hit proxy and
  independent/custom factory consumers remain exercised. Remove obsolete culling
  cache-key padding and GBuffer renderer/view forwarding parameters.
- Remove the reserved culling-stage descriptor and its dead reflection adapter.
  Active stage ordinals 0-5 and pass contract 13 are unchanged; retired ordinal 6
  is never reused. Preserve legacy authored GeometryUsage bit 4 load/edit
  canonicalization, since it migrates persisted assets rather than submitting an
  alternate graphics path. Cooked products with obsolete contracts remain rejected.
- Expose cumulative admitted GPUScene upload bytes/ranges/rebuilds and current
  attribute-buffer/CPU-mirror capacities. Failed or abandoned transactions do not
  increase upload counters. These capacities exclude retired buffers and other
  renderer/allocator memory; report shadow targets and culling transients separately.
- Keep workload sampling opt-in through `DURIN_GPUSCENE_SAMPLE_FILE` in the
  existing numeric-material native fixture. Timing hooks force immediate shadow
  recording; correctness qualification separately covers ordinary recording.

- [x] Remove obsolete supported-path uniforms, arrays, stage bits and adapters;
  retain only documented capability fallbacks with exercised consumers.
- [x] Update the owning runtime contracts listed above with implemented behavior.
- [ ] Complete targeted correctness/native qualification and workspace integration
  gates, recording tested inputs and reusable receipts in this plan.
- [ ] Compare static/dynamic, small/large, multi-view and shadow-heavy workloads;
  record CPU preparation, upload bytes, draw counts, GPU time and retained memory.

Outcome: no duplicate authoritative path for supported scene meshes, no unmeasured
default culling-policy change, and documented acceptance evidence. Depends on Stage 4.

### Stage 6: Align mesh submission interfaces with UE

The user authorized semantic alignment after reviewing local UE 5.8.2 source.
This stage depends on Stage 4's implemented command path; deferred Stage 5
Metal/independent-queue/quiet timing qualification does not block implementation.
Use UE's visible-command setup and draw override/scene-argument boundaries.
Keep Durin's counted resources, terminal graph completion and failure semantics.

- [x] Add `FMeshDrawCommandOverrideArgs` and `FMeshDrawCommandSceneArgs`; apply
  submission overrides without copying retained commands or pass binding arrays.
- [x] Make culling setup/submission consume frame-local visible commands rather
  than prepared StaticMesh objects, and associate contexts with their manager.
- [x] Group culling candidates by effective command/pass state in prepared order,
  replacing source-wrapper pointer identity. Keep the existing opt-in policy,
  admission budgets, scoped attribute owners and complete CPU fallback.
- [x] Migrate consumers, document the resulting contracts and validate native
  direct/CPU/indirect parity, failure/recovery and workspace integration.

Outcome: UE-shaped primary submission interfaces with stable command state and
explicit per-submission arguments. Dynamic GPUScene range unification and
primitive resource-owner restructuring remain separate implementation work.

### Stage 7: Unify dynamic allocation and primitive cache ownership

The user authorized semantic alignment with local UE 5.8.2. Follow GPUScene's
shared persistent/dynamic instance allocation model and PrimitiveSceneInfo's
cached-command association/removal responsibility. This stage depends on Stage 6
and does not waive Stage 5's deferred hardware/timing qualification.

- [x] Allocate scoped dynamic primitive/instance records from the owning GPUScene;
  retain ranges through actual graph use and retire only after GPU completion.
  Standalone editor/preview draws use a transient GPUScene with the same machinery.
- [x] Remove the separate dynamic attribute-buffer domain and per-candidate scene
  owner selection/dispatch. Update budgets, telemetry and explicit graph reads.
- [x] Move cached command associations and invalidation into PrimitiveSceneInfo;
  remove FPrimitiveDrawResources. Keep fallback uniforms source-associated and
  retain source/canonical pipelines in the actual scene command storage.
- [x] Validate shared-domain readbacks, growth, reuse, abandonment/recovery,
  independent scene/preview lifetimes, source invalidation, native output parity
  and workspace integration; update runtime contracts and receipts.

Selected lifetime rules: dynamic allocation leases retain GPUScene independently
of FScene; frame/graph owners retain leases. Freed dynamic slots keep unreferenced
contents and do not cause clearing uploads, but cannot be reused before the last
using graph completes. Persistent identity generations and terminal failure rules
remain unchanged. PrimitiveSceneInfo is shared by scene membership and borrowed
through weak frame references; it is not another RHI resource-owner wrapper.
Scene cache storage owns cache-wide RHI invalidation and pipeline references.

Outcome: one attribute allocation domain per scene and one primitive command
association owner, with explicit submission lifetimes.

### Stage 7 validation receipt

Inputs are base `e3380906c` plus the source, shader and fixture hashes in
`Build/Qualification/UEAlignedGPUSceneStage7Inputs.json`. That inventory also hashes
the inspected local UE 5.8.2 (CL 56702186) GPUScene, PrimitiveSceneInfo and Scene
implementations. UE's collector commits instance/payload slots through GPUScene;
its dynamic context releases those slots. PrimitiveSceneInfo stores command infos
and removes its scene cache associations. Durin now follows those responsibilities,
while retaining its completion-based free-range allocator and shared lifetime leases.

The separate dynamic attribute-buffer uploader, shader-side owner selector and
per-owner dispatch loop are removed. Persistent and dynamic records share one
attribute domain; standalone picking uses a transient GPUScene. Candidates are now
eight bytes. The mixed 64 persistent/64 dynamic native case forms one indirect
draw and charges 1,556 transient bytes instead of two draws/2,600 bytes. Scene
capacity includes dynamic ranges; standalone capacity is reported separately.
These structural observations do not establish a timing improvement.

FPrimitiveDrawResources is removed. SceneInfo retains command locations and fallback
uniform associations; scene cache entries/buckets retain source/canonical pipelines.
Cache storage participates in device invalidation. Detach removes material/cache
associations immediately and invalidates retained weak frame sources, even if an
external reference keeps SceneInfo storage alive.

`test affected --explain` expands to the Engine project's ordinary tests because
native test sources changed. The bounded selection covers the changed Renderer,
shader, recovery and frame contracts plus Editor, Sandbox and RoadWeaver consumers.
The explicit workspace `all` gate covers remaining compilation/ABI integration.
Profile is `windows-msvc-x64`, default `Win64-Debug-DurinEditor`, application-hosted
tests off; native Vulkan uses the current local adapter with validation enabled.
Builds ran outside the sandbox for the established MSVC PDB-service issue.

| Selection | Final-input result and log under `Build/.agent-state/logs/` |
| --- | --- |
| `RendererSceneContractTests` | 163/163. Shared dynamic/persistent allocation, exact IDs, delayed drawing retirement, abandonment, reconstruction, wrong-scene token rejection, game-thread lease release and retained SceneInfo detach. `20261113-044612-157100-536-RendererSceneContractTests.log`. |
| `RenderShaderContractTests` | 72/72. `20261113-044616-939892-38664-RenderShaderContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. Direct/CPU/GPU, GBuffer/shadow, large origins, mixed dynamic/persistent grouping and 45 exact attribute readbacks over 15 successful snapshots. Dynamic release uploads zero clearing bytes and persistent records safely reuse its range. `20261113-044635-953520-24032-StaticMeshRenderPreparationVulkanTests.log`. |
| `RendererResourceReloadVulkanTests`, `VolumetricCloudSceneVulkanTests` | 2/2 and 1/1. `20261113-044648-743211-33676-RendererResourceReloadVulkanTests.log`, `20261113-044658-427206-33644-VolumetricCloudSceneVulkanTests.log`. |
| Project consumers | Sandbox 13/13, RoadWeaver 5/5, Editor rendering 91/91. `20261113-044705-441507-30188-SandboxGameplayTests.log`, `20261113-044707-170850-37412-RoadSceneIntegrationTests.log`, `20261113-044717-266873-10820-EditorRenderingTests.log`. |
| Vulkan inline recording | Same native targets pass 21/21, 1/1 and 2/2 with `DURIN_RHI_EXECUTION=inline`. `20261113-044739-817698-10728-StaticMeshRenderPreparationVulkanTests.log`, `20261113-044751-519824-23748-VolumetricCloudSceneVulkanTests.log`, `20261113-044758-615494-27320-RendererResourceReloadVulkanTests.log`. |
| Workspace `build all` | Passed. `20261113-044818-542684-39896-cmake.log`. |
| Release correctness | 1/1 numeric/custom-data direct/CPU/GPU case, validation on, `Win64-Release-DurinEditor`, Tracy off. `20261113-044913-853484-40300-StaticMeshRenderPreparationVulkanTests.log`; XML `Build/Qualification/UEAlignedGPUSceneStage7Release.xml`. Correctness only; no performance sampling. |

Metal fixtures use the new candidate layout but native Metal execution remains
deferred. Independent physical queues and exclusive quiet Release timing remain
open Stage 5 gates. Earlier timing samples describe older inputs and cannot serve
as Stage 7 performance acceptance. Culling remains disabled by default.

### Stage 8: Unify command grouping and submission identity

The user authorized UE-aligned semantics for the follow-up review findings.
Implement in order: shared CPU/GPU ordered runs, scene-render-scoped dynamic
context, then value-based pipeline identity. Preserve complete CPU fallback,
explicit graph reads and GPU-completion retirement; Stage 5 deferred gates remain.

- [x] Build ordered effective-state runs once and reuse them for CPU instancing
  and GPU admission. Remove pass-local hash buckets and duplicate grouping.
- [x] Share a dynamic context across receiver/cascades in one scene submission,
  reusing identical dynamic records and registering terminal graph use once.
  Independent picking/preview submissions keep independent contexts.
- [x] Replace pipeline-pointer cache identity with structural pipeline identity;
  remove source lookup pipeline retention, keeping the canonical pipeline alive.
- [x] Validate ordering, mixed CPU/GPU fallback, cross-view dynamic reuse,
  changed-fact separation, cache replacement and resource recovery. Run owning
  and affected consumer tests and the workspace all integration build because
  frame/context and command-cache interfaces cross renderer feature boundaries.

### Stage 8 validation receipt

Inputs are base `90beb040b` plus the final source/fixture hashes in
`Build/Qualification/UEAlignedGPUSceneStage8Inputs.json`, which also hashes the
local UE 5.8.2 (CL 56702186) reference files. UE's InstanceCulling setup compacts
commands and selects direct/indirect submission, its SceneRenderer holds the dynamic
context, and minimal pipeline state IDs replace wrapper identity. Durin follows
these responsibilities without introducing another global pipeline-ID registry.

`BuildMeshDrawCommandRuns` now supplies both CPU instancing and GPU admission.
Production forms runs after final component admission; GPU selection reuses the
same members. Pass-local hash buckets and transient state numbering are removed.
CPU fallback remains complete on failed GPU admission/dispatch. The shared dynamic
context interns normalized packed facts within one submission; receiver/cascades
reuse identical records and changed facts preserve earlier snapshots. Independent
preview/picking contexts remain separate. One terminal graph-use registration
replaces scene plus per-view registrations. Scene command lookup compares pipeline
state/layout/shader/declaration values; source wrappers may expire while the shared
canonical pipeline survives until its last command association is removed.

`test affected --explain` selects additional material/editor feature targets from
the Renderer module label. The bounded selection covers changed grouping, pipeline
cache, shader, graph execution/recovery and scene routes, plus Editor, Sandbox and
RoadWeaver consumers. The explicit workspace `all` gate covers other compilation
consumers. Profile: `windows-msvc-x64`, default `Win64-Debug-DurinEditor`, application
tests off; native Vulkan validation enabled. Builds ran outside the sandbox for the
established MSVC PDB-service incompatibility.

| Selection | Final-input result and log under `Build/.agent-state/logs/` |
| --- | --- |
| `RendererSceneContractTests` | 164/164. Shared run membership/ordering and eligibility splits, complete fallback, cross-view dynamic reuse with zero repeated attribute upload, changed-fact snapshot isolation, independent preview domains, canonical-only pipeline lifetime and source invalidation. `20261113-124611-953571-12100-RendererSceneContractTests.log`. |
| `RenderShaderContractTests` | 72/72. `20261113-124620-850470-10612-RenderShaderContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21, including direct/CPU/GPU, mixed admission/fallback, GBuffer/shadow, exact GPUScene readbacks and large-origin output. `20261113-124638-928892-25000-StaticMeshRenderPreparationVulkanTests.log`. |
| Resource recovery and scene routes | 2/2 and 1/1. `20261113-124656-475450-19420-RendererResourceReloadVulkanTests.log`, `20261113-124705-802591-18776-VolumetricCloudSceneVulkanTests.log`. |
| Project consumers | Sandbox 13/13, RoadWeaver 5/5, Editor rendering 91/91. `20261113-124719-584363-28460-SandboxGameplayTests.log`, `20261113-124740-663114-3452-RoadSceneIntegrationTests.log`, `20261113-124800-539070-26308-EditorRenderingTests.log`. |
| Vulkan inline recording | Same native targets pass 21/21, 1/1 and 2/2 with `DURIN_RHI_EXECUTION=inline`. `20261113-124818-607835-16576-StaticMeshRenderPreparationVulkanTests.log`, `20261113-124829-745201-22020-VolumetricCloudSceneVulkanTests.log`, `20261113-124836-701498-29424-RendererResourceReloadVulkanTests.log`. |
| Workspace `build all` | Passed. `20261113-124838-745203-28308-cmake.log`. |
| Release correctness | 1/1 numeric/custom-data direct/CPU/GPU case, validation on, `Win64-Release-DurinEditor`, Tracy off. `20261113-125112-969154-10408-StaticMeshRenderPreparationVulkanTests.log`; XML `Build/Qualification/UEAlignedGPUSceneStage8Release.xml`. Correctness only. |

No new timing samples were collected. The dynamic fact cache and value-based
lookup add CPU bookkeeping, so structural cleanup alone makes no performance
claim. Default culling policy is unchanged. Metal, native independent queues and
exclusive quiet Release timing remain Stage 5 follow-ups.

### Stage 9: Consolidate submission and collector contracts

The user authorized further UE-aligned semantics in review order. Keep one
submission selection state, use explicit collector records instead of packed-byte
identity, stage all views before attribute upload, cache immutable pipeline
identity, and make primitive membership scene-owned with generation-checked borrows.

- [x] Share one run/selection table between CPU and GPU submission; remove copied
  CPU group members, post-dispatch enable mutation and GPU eligibility in CPU equality.
- [x] Introduce explicit collector record identity and collection/upload/binding
  phases; use context as the sole allocation-domain authority, with one attribute
  upload after receiver/cascade collection and independent standalone requests.
- [x] Cache canonical pipeline identity and use the same pipeline equality for
  command sharing and cache lookup.
- [x] Make SceneInfo scene-owned; resolve frame source references through persistent
  generation identity, without retaining removed SceneInfo or inferring lifetime
  from shared ownership. Unpublished sources use dynamic collector records.
- [x] Validate fallback, cancellation, record identity, shared-view growth, cache
  invalidation and source removal. Run owning and consumer coverage, inline Vulkan,
  Release correctness and workspace all integration for the changed frame APIs.
  Stage 5 Metal, independent physical queues and quiet Release timing stay open.

### Stage 9 validation receipt

Inputs are base `348de52f3` plus the source/fixture hashes in
`Build/Qualification/UEAlignedGPUSceneStage9Inputs.json` (including the new shared
submission header). The inventory also hashes local UE 5.8.2, CL 56702186 references:
dynamic-context/collector upload ownership, collector commit and upload state,
PrimitiveSceneInfo cache removal, and minimal pipeline identity.

One shared submission table now owns ordered run members, draw-to-run lookup,
argument selection and dispatch outcome. Copied CPU groups, argument-per-draw
selection and `PrepareForSubmission` repair are removed. GPU-only eligibility
does not split CPU-compatible runs; a run with an ineligible member stays on CPU.
Fallback remains complete when GPU admission/dispatch fails.

`FGPUScenePrimitiveCollector::Commit` accepts logical records and reserves CPU
mirror ranges without native upload. Identity is explicit and immutable: shared
records reuse allocation, while distinct equal-valued records remain distinct.
Packed-byte keys and duplicate identity packing are removed. Receiver/cascade
collectors commit before one attribute upload, then consume the same final buffer
generation during binding. Context is the sole allocation-domain authority.
Standalone requests use a context plus commit/upload convenience entrypoint.

Pipeline snapshots cache immutable identity and both command sharing and cache
lookup use it. Cached entries retain a cheap identity reference; canonical buckets
retain the pipeline. Copies start a fresh snapshot; published snapshots stay
immutable. Scene uniquely owns SceneInfo and frame references resolve membership
by persistent index/generation. SceneInfo shared ownership, `bDetached` and inferred
`bSourceBound` are removed. The render thread freezes source availability together
with each primitive's batch facts before worker tasks start; workers do not resolve
SceneInfo. Removal and slot reuse cannot revive a prior membership reference.

`test affected --explain` includes unrelated material/editor feature suites through
the Renderer label. The bounded selection covers owning grouping/collector/cache,
shader, recovery and scene routes plus Editor, Sandbox and RoadWeaver consumers.
The explicit workspace `all` gate covers remaining compilation consumers of the
changed SceneInfo and frame headers. Profile: `windows-msvc-x64`, default
`Win64-Debug-DurinEditor`, application-hosted tests off, native Vulkan validation on.
Builds use the established sandbox-external MSVC PDB-service workaround.

| Selection | Result and log under `Build/.agent-state/logs/` |
| --- | --- |
| `RendererSceneContractTests` | 164/164. Final declaring headers confirmed by `20261113-132418-858646-20472-RendererSceneContractTests.log`. Covers shared selection/fallback, explicit same-value record separation, staged growth and common final buffers, pipeline identity cache and generation-checked scene removal/reuse. |
| `RenderShaderContractTests` | 72/72. `20261113-132138-447854-11164-RenderShaderContractTests.log`. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21. Direct/CPU/GPU, GBuffer/shadow, exact scene readbacks, shared scene routes and immutable parallel preparation. `20261113-132158-159591-10892-StaticMeshRenderPreparationVulkanTests.log`. |
| Recovery and scene routes | 2/2 and 1/1. `20261113-132210-491431-12060-RendererResourceReloadVulkanTests.log`, `20261113-132220-591871-4936-VolumetricCloudSceneVulkanTests.log`. |
| Project consumers | Sandbox 13/13, RoadWeaver 5/5, Editor rendering 91/91. `20261113-132227-226646-29136-SandboxGameplayTests.log`, `20261113-132228-958520-28692-RoadSceneIntegrationTests.log`, `20261113-132238-889655-26684-EditorRenderingTests.log`. |
| Vulkan inline recording | Same native targets pass 21/21, 1/1 and 2/2 with `DURIN_RHI_EXECUTION=inline`. `20261113-132435-677482-27692-StaticMeshRenderPreparationVulkanTests.log`, `20261113-132453-553242-6076-VolumetricCloudSceneVulkanTests.log`, `20261113-132503-949627-6564-RendererResourceReloadVulkanTests.log`. |
| Workspace `build all` | Passed. `20261113-132506-062751-6708-cmake.log`. |
| Release correctness | 1/1 numeric/custom-data direct/CPU/GPU case, validation on, `Win64-Release-DurinEditor`, Tracy off. `20261113-132702-169224-8908-StaticMeshRenderPreparationVulkanTests.log`; XML `Build/Qualification/UEAlignedGPUSceneStage9Release.xml`. Correctness only. |

The final follow-up changes after the broad regression are direct declaring-header
includes and whitespace only; owning contracts and inline native coverage recompile
those headers, and all builds remaining consumers. Preceding behavioral receipts
remain applicable. No shader ABI or cooked inputs changed in this stage.

Remaining deliberate differences: Durin executes one dedicated attribute upload
graph before native binding construction, uses cached structural pipeline identity
instead of UE's global packed-ID table, and retains completion-based range leases
for standalone and failed-graph recovery. No performance result is claimed. Metal,
independent physical queues and exclusive quiet Release timing remain Stage 5 gates.

### Stage 10: Reuse instance uploads and streamline cached submission

The user selected review items 1, 2 and 4. Align the responsibilities with UE's
retained command metadata, submission-scoped instance data and pass-local setup;
do not change default culling policy or expand this batch into dirty-scene queues.
References: Epic's [mesh drawing pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
and [instance culling context](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Renderer/FInstanceCullingContext)
describe the retained metadata and pass-local setup responsibilities used here.

- [x] Batch receiver/cascade direct ID lists and retain counted immutable upload
  leases. Share equal contents; overwrite only released, GPU-complete storage.
  Bound scene retention and clear native pool state on resource recovery.
- [x] Carry command and source metadata through one cached lookup, including
  invalidation identity, without repeating structural lookup during binding.
- [x] Request only consuming mesh passes, skip absent passes, and represent
  consecutive runs by first command/count without per-run member allocations.
- [x] Validate owning scene/cache/grouping contracts, real Vulkan output and
  recovery, inline/threaded recording, and Editor/Sandbox/RoadWeaver consumers.
- [x] Compare three Release/Tracy-off runs and three default Debug Sandbox runs
  against the same baseline probes. Preserve CSV/log/XML and report median/p95
  and structural allocation observations without promising an FPS recovery.

Baseline inputs are `4086759c1` plus the sampling-only fixture probe. Raw samples
and temporary-probe manifests are under `Build/Qualification/GPUSceneBatch1/before/`.
Stage 5's deferred Metal and independent physical queue gates remain unchanged.

### Stage 10 validation receipt

Inputs are base `4086759c1` plus the final source/fixture hashes in
`Build/Qualification/GPUSceneBatch1/receipt.json`. Reproduction helpers, probe
backups, raw CSV, XML and the aggregate `comparison.json` remain in that ignored
directory. Sampling retains the same CPU intervals on both revisions; readback
captures are disabled only during measured samples, with output parity checked
before/after sampling. The final fixture also records RenderView CPU time and
asserts zero steady instance-ID native creations/uploads after warm-up.

| Selection | Result |
| --- | --- |
| `RendererSceneContractTests` | 165/165; includes shared ID contents, batched collector offsets, pending-reader protection, completed-capacity reuse and recovery, cached metadata invalidation, and contiguous run membership. |
| `StaticMeshRenderPreparationVulkanTests` | 21/21 threaded and 21/21 inline; direct/CPU/GPU color, GBuffer, shadows, mixed fallback and source update output. |
| `RendererResourceReloadVulkanTests` | 2/2 threaded and 2/2 inline. |
| Consumer targets | Editor rendering 91/91, Sandbox gameplay 13/13, RoadWeaver scene integration 5/5. |
| Release sampling | Three runs pass, each with 30 warm-ups and 120 measured samples per mode/view; all measured ID creations and ID upload bytes are zero. Native draw counts match baseline. |
| Documentation | Changed-document validation passes. |

The implementation changes private Renderer preparation APIs. Symbol searches
cover source/tests of all three workspace projects; bounded native selections
cover the owner and actual consumers. Unchanged material compiler/cook/shader
paths selected by the broad Renderer module label do not require new execution.
No foundational header, build graph or shader ABI changed, so this stage does
not repeat earlier stages' workspace-all compilation gate. Builds use the
established sandbox-external MSVC PDB-service workaround.

Release observations use MSVC, RTX 3090 driver 616.64, Vulkan 1.4.351,
`Win64-Release-DurinEditor`, Tracy/validation off, threaded RHI, graphics queue,
and the fixture's 192x192 output. Values below pool three runs; each cell is
RenderView CPU median/nearest-rank p95 in microseconds for CPU dynamic instancing.
These intervals exclude the separate PrepareViewResources call and GPU completion.

| Workload | Before | After |
| --- | --- | --- |
| static-small | 327.50 / 438.80 | 282.25 / 363.70 |
| static-large | 2487.20 / 2962.40 | 2361.10 / 2720.80 |
| dynamic-large | 2489.75 / 2918.90 | 2405.45 / 2920.50 |
| multi-view | 845.85 / 1082.80 | 803.05 / 1034.20 |
| shadow-heavy | 881.75 / 1159.10 | 834.10 / 1059.30 |

Default Debug Sandbox observations use the saved GrayboxStage15 camera,
2916x1866 viewport, Tracy enabled, validation auto and instancing enabled.
Each revision has three 1800-tick runs, taking the last 1200 frames per run.
Average run-mean frame time is 18.7691 ms before and 16.7389 ms after (10.82%
lower); corresponding throughput is 53.28 and 59.74 FPS. Mean run p95 is
22.5024 and 19.1569 ms. Absolute FPS differs from the earlier chat session and
must not be used to claim recovery of the original 130-to-110 report.

These sequential A/B observations are diagnostic: an exclusive quiet lane and
alternating revision order are not established, and GPU clocks/other machine
load are not locked. They do not rebaseline a threshold or accept Stage 5's
Release gate. Dynamic-large CPU p95 is effectively unchanged. This batch makes
no GPU shader-time improvement claim. Temporary Sandbox probes are restored;
only the reusable native sampling instrumentation remains in the fixture.

## Validation and Completion Gates

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection](../Agents/Testing.md). Stage 0 required changed-document and
all-plan validation only; implementation stages require runtime evidence below.

Implementation stages use owning-module and affected-consumer builds/tests.
Final integration requires an `all` build because shared VF, mesh binding and
material stage/Cook ABIs span workspace providers and cannot be established by
Renderer-only coverage. Run this broad gate after the final shared-ABI migration;
reuse it while relevant inputs/environment remain unchanged.

Native qualification covers supported Metal/Vulkan paths and inline/threaded
recording, with validation enabled for correctness. Include direct/CPU-merged/
GPU-culled output comparisons, multiple scenes, interleaved scene changes and
views, buffer growth, teardown/recovery and cross-queue use. Legacy uniform
fallback and old-cooked-product rejection/migration require explicit cases.

Structural acceptance requires zero unchanged Primitive/Instance attribute
uploads on camera-only movement and additional views of the same committed
revision. Visibility/ID/argument uploads are reported separately. Dirty uploads
scale with changed records plus allocator growth, not visible draws times views.
Per-frame command creation and actual native draw counts prove reuse/merging.
Timing comparisons must report persistent memory and scene-update cost as well
as pass cost; fewer draws alone do not establish a speedup.
