# View Local Light Resources Plan

Summary: Separate the visible CPU light set from GPU admission and give forward and deferred passes a counted local-light buffer with an explicit budget.

Last reviewed: 2026-11-11

Status: Active
Completed:

## Current Status

Stage 0 audited Durin at `1dd30a3a5` and the installed UE 5.8 desktop renderer.
The [previous plan](LocalLightViewRequirements.md) established pass demand.
The [forward lighting contract](../Runtime/Rendering/Features/ForwardLighting.md)
describes the shipping ABI. Stage 1 implements the shared view buffer. Stage 2 qualifies 32 records with a per-view override. The owner deferred selection of a new default until there is a representative target workload and hardware. Ordinary views continue to use four.

## UE 5.8 correspondence

Source locations are relative to `Engine/Source/Runtime/Renderer/Private/` in the local UE installation.

| Boundary | UE implementation | Consequence for Durin |
| --- | --- | --- |
| Gather and order | `FSceneRenderer::GatherAndSortLights` checks view relevance while gathering. `FSortedLightSetSceneInfo` holds sorted records and boundaries such as `ClusteredSupportedEnd` and `UnbatchedLightStart`. See `LightRendering.cpp` and `LightSceneInfo.h`. | Keep one CPU source of visible lights for this single-view submission. Keep component IDs for reproducibility. UE's sort keys also select rendering methods. Durin currently has one local shading method per pass. |
| Extract common light properties | The forward grid setup calls `LightProxy->GetLightShaderParameters`. Classic deferred calls the same proxy interface from `GetDeferredLightParameters`, then adds view- and pass-specific fields. See `LightGridInjection.cpp` and `LightRendering.cpp`. | Keep point/spot validation and GPU record packing in one place. Position, direction, range, radiance and cone must have identical meaning across consumers. |
| Bind forward resources | `PrepareForwardLightData` consumes the sorted set. `FForwardLightUniformParameters` and `FForwardLightingViewResources` expose per-view parameters and buffer views. Grid injection produces cell headers and indices into light data. See `SceneRendering.h` and `LightGridInjection.cpp`. | Give passes an explicit local-record resource and count. A per-cell index list is an additional acceleration structure with its own construction cost and overflow policy. |
| Bind deferred resources | Classic deferred has a per-light `FDeferredLightUniformStruct` containing `FLightShaderParameters` plus controls such as shadow channels. When clustered deferred is enabled and the grid populated, `RenderLights` sends the supported range to the clustered pass and advances the classic start index. See `LightRendering.h` and `LightRendering.cpp`. | Durin's existing fullscreen deferred pass and forward surface shaders both iterate the local records. They can share one upload today. A future per-light or clustered pass can consume the same CPU set through its own adapter. |

## Scope and interfaces

Before Stage 1, `GatherAndSortLights_RenderThread` sorts visible point and spot lights together by ID and copies only the first four into `FPreparedLightView::Local`. A fifth light can be the only one illuminating a receiver and disappear before either pass sees it. Four is also embedded in the CPU array, the Slang uniform and both shader loops (one emitted by `MaterialProgramGenerator.cpp`). Raising one constant leaves other boundaries in place.

Proposed Renderer-private interfaces:

| Interface | Owner and contract |
| --- | --- |
| `GatherAndSortLights_RenderThread(Scene, Requirements, Telemetry) -> FPreparedLightView` | Preserve call shape and existing demand/frustum policy. Return all valid, visible local records in stable order. Keep separate counts for visible, admitted and over-budget lights by family. Preserve the existing public selected count as admitted count. |
| `BuildLocalLightRecords(Lights, View, MaxRecords)` | Pack a bounded prefix once using the existing 64-byte point/spot encoding and camera-relative position. Return packed records and overflow accounting. Clamp to the checked backend buffer range and the view budget before sizing the upload. Stage 1 uses four as a compatibility policy. |
| `FResolvedLighting` and pass parameters | Replace the single raw uniform range with common view constants plus a read-only local-record range and count. Resource preparation owns allocation/readiness. RDG declares the upload and every consuming read. Create one view-local buffer for both routes. A zero count uses an empty binding or a valid shared dummy where reflection requires it. |
| C++/Slang view parameters and shared shading helper | Move the local array out of `FForwardLightingUniform`. Give the remaining constants a consumer-neutral name. Removing 256 bytes yields 544 logical bytes if the remaining fields retain their layout. Centralize the counted loop and call it from generated forward materials and fullscreen deferred. Pass adapters bind only parameters required by the permutation. |

Directional-light selection, cascade data (448 bytes) and environment controls have independent consumers. Keep their existing representation for this migration. Audit the shadow renderer's zeroed fallback uniform, forward wireframe, sorted translucency, retained geometry, spline, isolated deferred and prepare-only paths. Record any remaining fallback allocation separately from the main-view upload. A full split of shadow/environment bindings needs evidence of independent lifetime or cost.

## Implementation Stages

### Stage 0: Audit sharing and capacity

- [x] Trace the UE and Durin producer, binding and shader-consumer boundaries above. Identify both sources of shader loops and the shadow fallback.

### Stage 1: Separate visible records from pass bindings

- [x] Introduce the interfaces above and migrate CPU producers, generated shaders, fixed shaders, binding metadata and tests together. Inspect shader/material cache keys and cooked library compatibility. Invalidate or version stale layouts wherever source hashing alone cannot detect the change.
- [x] Keep four admitted lights while establishing parity. Freeze reference pixels and selected IDs on the parent revision. At 0, 1 and 4 mixed point/spot lights, compare final color, isolated Local and forward-only Lit output. Require identical selected IDs and byte-identical deterministic captures. For any backend rounding difference, record the affected channel and establish a justified tolerance before accepting it.
- [x] With six visible lights, expect six CPU-visible, four admitted, two overflow and stable admitted IDs under reversed publication order. Alternate two views with different frusta and a zero-light view. Include an offscreen source whose range reaches the view, disabled culling and large world coordinates.
- [x] On a hybrid Lit frame verify one logical local-record upload of `64 * admitted` bytes, shared by deferred and lit translucent draws. On ordinary Unlit verify zero local traversal and zero per-view local-record upload. Check shadow-only fallback, prepare-only recording zero draws, allocation failure and retry without stale bindings. Reflect field offsets, descriptor types and record stride on Vulkan and Metal. Exercise old shader-cache/cooked artifacts against the new layout and rerun the affected material and rendering tests.

#### Stage 1 qualification (Win64, Vulkan)

The reference uses parent `b751c0494` with the capture fixture only. Set `DURIN_LOCAL_LIGHT_CAPTURES` to separate directories on each revision and run `DevTool.bat test VolumetricCloudSceneVulkanTests`. Compare the raw 96 x 64 captures. Final and Lit wireframe use RGBA8. Isolated Local reads the actual RGBA16F deferred attachment. The fixed camera sees a single Lit receiver. Additional masked, Unlit and Lit translucent surfaces enter for the subsequent hybrid and failure tests.

| Visible / uploaded | Final SHA-256 prefix | Local SHA-256 prefix | Forward SHA-256 prefix |
| --- | --- | --- | --- |
| 0 | e58fd1d7b1e3726d | bccf5468013e4067 | 7842eb82ef35ef12 |
| 1 | 53c2bd1ef19eeb98 | 08afed1b098fbef2 | 36aaab621f837a6a |
| 4 | 2333851ebc231fd2 | 59754fdf77412931 | 4c9041fd526c88b6 |

All nine files match byte for byte. Each column changes at both count transitions. The admitted fixture IDs on both builds are the ordered prefix of 23000, 23001, 23002, 23003 (point, spot, point, spot). An exploratory overlapping-geometry fixture differed in 135 RGBA8 bytes in wireframe even with zero lights. Its final RGBA8 images matched at every count. One four-light isolated Local half-float differed by 1 ULP. The cause of these mixed-fixture differences is unconfirmed. The single-receiver fixture removes overlapping surfaces from the image parity test. Mixed-geometry checks remain in the same executable.

- CPU contracts cover six mixed lights published in both ascending and descending ID order. All six remain visible. The packed intensities identify IDs 1 through 4, with one point and one spot over budget. Alternating frusta select IDs {1,3}, {2}, {}, then disabling culling yields {1,2,3}. ID 3 lies outside the side plane with an overlapping influence. The large-world test packs relative coordinates at a view origin of magnitude 2^40.
- On a mixed Lit frame the graph capture identifies one 128-byte write and reads of that resource ID by production deferred and sorted translucency. Ordinary Unlit visits zero point/spot lights and queues zero bytes. Preparation records zero draws. Forced local allocation failure returns resources unavailable and the next submission succeeds with the expected two records. Shadow preparation keeps its separate zeroed view constants.
- Offline Slang reflection of `DeferredDirectionalLighting.ProductionFragmentMain` and the generated opaque Lit material fragment for SPIR-V and Metal gives the same 544-byte view block, shadow offset 64, environment offsets 512/528 and read-only structured records with offsets 0/16/32/48, stride 64. Vulkan logical slot 19 maps to Metal buffer 2 in direct MSL. Generated forward uses logical slot 30 (direct MSL buffer 1). Native pixel qualification ran on Vulkan.
- Material pass contract 12 replaces 11 in shader-map and cook identities. Decoding an artifact with the prior contract fails with a recook diagnostic. The deferred shader set uses a new runtime request name, including the cook inventory, so a source-free runtime cannot satisfy it with the old set.

Affected test receipts under `Build/.agent-state/logs/`: renderer scene contracts 136/136, renderer scene views 31/31, material compiler 103/103 (including rejection of pass contract 11), shader builder 36/36, cooked shader library 8/8, static mesh Vulkan 20/20 and the Vulkan scene-route fixture 1/1. The last fixture checks graph uses and outcomes across 92 submissions when capture is enabled. Raw images, Slang reflection and backend output are under `Build/AgentValidation/ViewLocalLightResources/Stage1/`.

Two broader suites also reported asset-unload failures. On this revision and the parent, `FMaterialFunctionCookTests.CookFingerprintsNestedFunctionsWithoutProducingRuntimeFunctionPackages` fails at `UnloadPackage(WrapperPath)` (line 89), and `FEditorTextureSmokeTests.OrdinaryGraphRendersReloadsAndResavesDeterministically` fails at `UnloadPackage(MaterialPath)` (line 216). With those two cases excluded, all remaining 90 editor and 5 cook tests passed. Parent receipts are `20261111-141025-812301-20060-MaterialCookTests.log` and `20261111-141133-544185-36740-EditorRenderingTests.log`.

### Stage 2: Qualify a larger local-light budget

- [x] Snapshot the renderer-private record budget per view, retain the four-light baseline, and exercise a 32-record candidate with the same shader ABI and backend range check.
- [x] Verify lights 5, 8, 32 and 33 on actual receivers, including a Lit translucent surface, mixed point/spot records, cone edges, overflow and readmission.
- [x] Characterize the four- and 32-record profiles at 0/4/8/32 visible lights in sparse and high-overlap layouts. Retain raw CPU, GPU and allocation observations.
- [ ] Select a shipping budget and decide whether to prototype a light grid after specifying representative hardware, frame-time targets and acceptable regressions. The owner explicitly deferred this decision on 2026-11-11. Measurements on this workstation are descriptive.

#### Correctness

The native scene fixture renders one receiver at 96 x 64 through three routes: the opaque isolated deferred Local target (RGBA16F), opaque forward-only Lit wireframe (RGBA8), and sorted Lit translucency (RGBA8). The first four IDs have visible influence bounds clear of the receiver. The fifth is a spot that illuminates it. Whole-image equality against the fifth alone catches both a missing contribution and accidental contributions from the filler lights. The four-record budget gives the zero-light image. Removing only the fifth at budget 32 returns to that image. Its 15-degree hard cone, 15-to-40-degree soft cone, and reversed cone distinguish boundary pixels from unlit pixels.

At 5/8/32 visible lights the candidate uploads 320/512/2048 bytes. At 33 it still uploads 2048 bytes. The last light also reaches the receiver and its additional contribution is absent until removing the fifth admits it. After readmission it reproduces the single-light image. The CPU test encodes each sorted ID in its packed intensity and verifies every admitted record and both families' overflow counts across sequential budgets 32, 4, 0, 5, 8, 33 and 64.

#### Workstation characterization

Win64 Debug, Vulkan, NVIDIA GeForce GTX 1060 6 GB, 1920 x 1080, fixed camera and identical opaque, masked and Lit translucent geometry. Each layout mixes point and spot lights. Sparse centers span an 8 x 4 grid with range 26. Dense lights share a center and range 200. Both profiles use the Stage 1 counted-list implementation. Each pair reverses budget order on alternating iterations, with four warm-ups and 32 measured frames per configuration. This desktop run provides a within-host comparison. Exclusive GPU ownership was not verified.

Gather/sort and record packing are render-thread CPU intervals. GPU timestamps cover the production deferred pass and the sorted translucent forward pass (including native pass boundaries). Frame time measures wall time from RenderView to GPU idle, excluding strict preparation, output allocation and pixel readback. The allocation column is the Vulkan device-local allocator byte delta around creation of the local buffer. It excludes staging, the shared empty descriptor and the common 544-byte uniform. Each 0/4-light budget pair produces identical pixels. Above four lights the two profiles shade different contributions. Enabling translucent timing also reproduces the untimed image at each candidate count/layout.

| Layout | Visible | Budget | Gather / pack median (us) | Upload / allocated (B) | Deferred GPU median (ms) | Forward translucent GPU median (ms) | Completed frame median / P95 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sparse | 0 | 4 | 14.20 / 3.45 | 0 / 0 | 0.332 | 0.154 | 10.951 / 14.826 |
| sparse | 0 | 32 | 14.30 / 3.30 | 0 / 0 | 0.332 | 0.154 | 10.373 / 13.143 |
| sparse | 4 | 4 | 32.75 / 6.65 | 256 / 256 | 0.578 | 0.365 | 11.463 / 17.606 |
| sparse | 4 | 32 | 33.00 / 6.75 | 256 / 256 | 0.580 | 0.364 | 11.263 / 14.735 |
| sparse | 8 | 4 | 47.90 / 6.65 | 256 / 256 | 0.727 | 0.364 | 11.790 / 16.741 |
| sparse | 8 | 32 | 47.15 / 7.35 | 512 / 512 | 0.977 | 0.570 | 11.888 / 14.788 |
| sparse | 32 | 4 | 122.30 / 6.80 | 256 / 256 | 0.578 | 0.364 | 11.414 / 18.859 |
| sparse | 32 | 32 | 119.55 / 12.05 | 2048 / 2048 | 2.310 | 1.801 | 15.197 / 18.554 |
| dense | 0 | 4 | 13.35 / 3.30 | 0 / 0 | 0.417 | 0.196 | 10.637 / 17.039 |
| dense | 0 | 32 | 13.55 / 3.75 | 0 / 0 | 0.418 | 0.196 | 11.661 / 17.049 |
| dense | 4 | 4 | 32.50 / 6.40 | 256 / 256 | 0.735 | 0.468 | 12.609 / 17.805 |
| dense | 4 | 32 | 32.75 / 6.50 | 256 / 256 | 0.734 | 0.468 | 12.094 / 16.929 |
| dense | 8 | 4 | 46.75 / 7.15 | 256 / 256 | 0.989 | 0.625 | 16.063 / 31.077 |
| dense | 8 | 32 | 49.40 / 7.95 | 512 / 512 | 1.407 | 0.981 | 16.676 / 21.800 |
| dense | 32 | 4 | 124.05 / 7.40 | 256 / 256 | 1.321 | 0.795 | 19.456 / 25.300 |
| dense | 32 | 32 | 134.40 / 13.10 | 2048 / 2048 | 5.394 | 3.924 | 27.361 / 43.206 |

Set `DURIN_LOCAL_LIGHT_SAMPLES` to a writable CSV filename and run `DevTool.bat test VolumetricCloudSceneVulkanTests` to repeat the measurements. The [512 raw samples](../../Engine/Tests/Native/EngineTests/Data/ViewLocalLightResources/Stage2-GTX1060.csv) include all individual pass timings for recomputing medians and nearest-rank P95.

The counted shader visits every admitted record at every shaded receiver pixel. A spatial index could skip out-of-range lights in the sparse layout. The dense layout has co-located spheres that cover the same receivers, so sphere-based binning removes zero of those 32 entries in a covered cell. These samples do not measure index construction or supply a cross-device frame target.

Validation receipts in `Build/.agent-state/logs/`: `20261111-142509-193900-31388-RendererSceneContractTests.log` (137/137), `20261111-142608-363198-45548-VolumetricCloudSceneVulkanTests.log` (all three receiver routes), `20261111-142422-683557-29968-VolumetricCloudSceneVulkanTests.log` (paired timing and pixel checks), and `20261111-142644-894340-35344-StaticMeshRenderPreparationVulkanTests.log` (20/20). Both scene qualification runs completed without Vulkan validation errors. Native GPU coverage for Stage 2 is Vulkan on Windows.
