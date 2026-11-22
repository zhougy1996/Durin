# Single Layer Water Material Plan

Summary: Add UE 5.8-compatible water material outputs, desktop deferred water rendering and a qualified default material for finite Gerstner surfaces.

Last reviewed: 2026-11-23

Status: Completed
Completed: 2026-11-23

## Current Status

Stages 0–5 are complete. Stage 0 was reviewed against local UE 5.8.2 CL 56702186. On 2026-11-23 the owner selected a source-derived contract and analytic baselines in place of UE captures and pixel parity. The material contract, render pass ownership, and plugin defaults are published in the owning documents.

Stage 1 validation used Win64 Debug DurinEditor from base revision `c364d7527` with this stage's changes. The integrated Cook fixture saved and reopened Opaque and Masked materials, loaded their Win64 Game packages with the authored source tree unavailable, matched shader entries and layouts, and changed instance absorption with zero new material compilation requests. `MaterialCompilerTests` passed 125 cases, `MaterialCookTests` passed 7, `RendererSceneContractTests` passed 180, `MaterialEditingPersistenceTests` passed 44, and `SandboxGameplayTests` passed 15. `RoadViewportQualificationTests` compiled. The build ran outside the sandbox after sandboxed MSVC hit `C1902`. Test logs are under `Build/SingleLayerWater/Stage1/`. Vulkan attachment execution and pixel readbacks belong to Stage 2 and later.

The existing Water plugin supplies finite geometry, WPO, normals and queries. See [water surfaces](../Runtime/World/WaterSurfaces.md), [material scene inputs](../Runtime/Rendering/Materials/MaterialSceneInputs.md) and the [GBuffer contract](../Runtime/Rendering/Pipeline/GBuffer.md).

Stage 2 validation used Win64 Debug DurinEditor from base revision `2fe24d81d` with this stage's changes on NVIDIA GeForce RTX 3090, Vulkan 1.4.351, driver 616.64. `WaterSurfaceRenderingVulkanTests` passed all 3 cases. The offset 86 × 64 fixture verified exact display matches for nearest-water ownership in both draw orders, foreground preservation, masked holes and sorted translucency with ForwardZ and ReversedZ. Captures verified the snapshot, depth, shading and sorted order, three read-only shading inputs and no water resources or passes in the baseline. `RendererSceneContractTests` passed 180, `MaterialCompilerTests` passed 125 and `MaterialCookTests` passed 7. RDG captures, RGBA8 readbacks and logs are under `Build/SingleLayerWater/Stage2/`. Cloud selection follows its completion result. The numeric lighting and reflection gates are pending.

Stage 3 validation used Win64 Debug DurinEditor from base revision `12a53150f` with this stage's changes on the same RTX 3090. In the 320 × 240 linear HDR fixture, ForwardZ and ReversedZ each returned RGB 0.606445 / 0.135254 / 0.018311 at the 25/100/200 cm beds. At 100 cm the view and vertical-path factors independently returned 0.367676. The same Vulkan suite checked clear channels and open depth, near-zero thickness, RGB extinction, pure scattering, both phase signs, Opacity endpoints, shoreline scaling, the FromF0 offset, explicit IOR=1, foreground rejection and viewport clamp. It passed all 4 cases, `MaterialCompilerTests` passed 125, and `MaterialCookTests` passed 7. Raw HDR readbacks, independent path values and logs are under `Build/SingleLayerWater/Stage3/`. Positive view depth follows the projection's forward axis, which is X for Durin's perspective matrix. Reflection composition remains Stage 4.

Stage 4 validation used Win64 Debug DurinEditor from base revision `15f6a4caa` on the same RTX 3090. In a 96 × 96 RGBA16F fixture with a fixed six-face sky, center red was 0.000000 with reflection disabled, 0.012833 at normal incidence and roughness 0.1, 0.217285 at grazing incidence and roughness 0.1, and 0.007214 at normal incidence and roughness 0.7. HDR alpha stayed 0.699707. Reflection preserved opaque foreground and masked pixels, and adding rear water produced identical output. `WaterSurfaceRenderingVulkanTests` passed 5 cases. `MaterialEditorInteractionTests` passed 77 routine cases and the dedicated Vulkan comparison. With the editor's Studio Cube, fixed bed, camera and material, the preview and direct renderer submission produced identical 128 × 128 RGBA8 readbacks. Raw readbacks and logs are under `Build/SingleLayerWater/Stage4/`.

Stage 5 validation used Win64 Debug DurinEditor and DurinGame from base revision `e024c6136` with Stage 5 changes on the same RTX 3090. The regenerated default uses two-sided Opaque routing, Opacity 0 and FromF0 refraction. Its scattering and absorption settings are recorded in [Water Surfaces](../Runtime/World/WaterSurfaces.md#material-resources). Reopened authored optics match the recipe. Editor and Game query probes retained the 5 mm position, 0.02 normal-vector and 2 cm tessellation tolerances. In both startup probes, enabled Water was admitted at Providers and disabled Water stayed unregistered. The normal Cook published 3 packages. The repeated Cook hit all 3 and reused 434,767 bytes. The Game probe loaded the level and accepted water program from the Cook without loading ShaderBuild. The Win64 Debug export verified 30 ticks of the source-free Game startup with the cooked water level.

The enabled Vulkan suite passed 5 Editor cases and 2 cooked Game cases. All four 192 × 108 component readbacks matched byte-for-byte. Animation at 0.75 s changed 10,185 pixels and replay at 0 s reproduced the original. The ordinary StaticMesh test confirmed the Water module was unloaded. Its intersecting opaque and sorted translucent fixtures matched the CPU-displaced reference in all 5,504 pixels for both depth conventions and changed 1,188 pixels compared with flat water. `MaterialCookTests` passed 7 cases, including authored Refraction stripping and installed program equivalence. Evidence is under `Build/SingleLayerWater/Stage5/`. Qualification covers the source-derived contract and the Win64 Vulkan fixtures described here.

## Goal

Qualify the above-water, non-Substrate UE 5.8 desktop deferred path on Win64 Vulkan. An ordinary StaticMesh with a water material and the plugin's animated surface must use the same rendering path. Engine owns material semantics, Renderer owns scene sampling and pass placement, and Water owns its asset, geometry and queries. First delivery uses full-resolution scene inputs and sky environment reflections. Underwater views and split underwater translucency, SSR/Lumen, tiled classification, indirect dispatch, downsampled copies and mobile shading are follow-up scopes.

## UE 5.8 reference contract

Reference paths below are relative to the locally installed UE 5.8 Engine directory. Use the legacy material branch as the comparison target. Stage 0 records the installation build and numeric reference values.

| Material input | Type and unconnected value | Authoring and evaluation |
|---|---|---|
| ScatteringCoefficients | RGB, zero | Nonnegative scattering per channel in inverse centimetres |
| AbsorptionCoefficients | RGB, zero | Nonnegative absorption per channel in inverse centimetres |
| PhaseG | Scalar, zero | Clamp to [-1, 1]. Positive values favor forward scattering |
| ColorScaleBehindWater | RGB, one | Nonnegative multiplier on background radiance. Fade from one with saturate(0.02 × water depth in cm) |

Expose these four pixel inputs on a dedicated material output expression in the order above. Keep the ordinary surface root for BaseColor, Normal, Roughness, Metallic, Specular, Opacity, WPO and applicable surface inputs. Admit Surface / SingleLayerWater with Opaque or Masked. Activate Opacity in both cases. Water visibility is 1 − Opacity. Masked also evaluates OpacityMask and rejects uncovered samples before attachment writes. Preserve UE's diagnostic when scattering, absorption and phase are all disconnected, including the case where only ColorScaleBehindWater is connected. Reference authoring in `Source/Runtime/Engine/Public/Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h` and the corresponding Compile implementation in `Private/Materials/MaterialExpressions.cpp` under the same Engine module.

The following quantities define the numerical comparison. Reference `Shaders/Private/SingleLayerWaterShading.ush`, `SingleLayerWaterCommon.ush`, `BasePassPixelShader.usf` and `SingleLayerWaterComposite.usf` in that shader directory.

1. At the surface pixel, sample positive view depth from the scene without water. Compute the refracted UV with the material refraction settings, view and surface normal. Apply UE's shallow-depth fade, viewport bounds and depth/mask-aware sample weights. When the distorted taps are all invalid, use the original UV and its matching depth and color weights.
2. For an above-water view, take the positive difference between selected background view depth and surface view depth, expressed in cm, as thickness d. For each RGB channel use extinction e = absorption + scattering, transmittance T = exp(−e × d), and integrated scattering weight saturate(scattering × (1 − T) / max(e, 1e−5)). Preserve the source's coefficient units when applying its epsilon.
3. Use the dielectric F0 derived from Specular for the water IOR used by the refracted lighting ray. Evaluate UE's Schlick phase for the main directional light along that ray, including its shadow factor. Use isotropic phase for ambient illumination. Reconstruct the background world position and separately attenuate incident light to the water bed by exp(−e × positive vertical surface-to-bed distance in cm).
4. Combine the sampled, incident-light-attenuated background with ColorScaleBehindWater and T. Apply the above-water environment BRDF complement to scattered light, and water visibility to the volume contribution. Accumulate the surface lighting and reflection terms once, using the same linear HDR exposure convention throughout.

## Implementation Stages

### Stage 0: Freeze parity fixtures and pass contracts

- [x] Trace desktop full-quality legacy water in local UE 5.8.2, CL 56702186, branch `++UE5+Release-5.8`. The baseline uses above-water shading, full-resolution inputs, one mono perspective view and one sample per pixel.
- [x] Select the authoring modes, sampling rules and frame-resource contract below against Durin revision `9279b161a`.
- [x] Freeze the numeric oracles and reproducible scene recipe below. These are design inputs to the Durin implementation gates. Actual Vulkan readbacks belong to Stages 2–4.

#### Refraction and depth contract

| Refraction method | Durin authoring contract | Initial viewport offset |
|---|---|---|
| None (default) | Omitted method on existing assets resolves here. Volume absorption and scattering still run. | Zero |
| Index of Refraction | Scalar Refraction root, default 1.0. Default depth bias is 0 cm. | View-space unit normal XY × (root IOR − 1) |
| Index of Refraction from F0 | Select explicitly for the default water asset. Offset depends on Specular. | View-space unit normal XY × (water IOR − 1) |

The lighting ray in **all three modes** uses dielectric F0 = 0.08 × Specular and IOR = 2 / (1 − sqrt(min(F0, 0.99))) − 1. A scalar root changes only the explicit IOR offset. Keep the root inactive for None and FromF0 and retain its connection across mode edits. UE internally packs XY data and depth bias in Z. Durin can generate these operands without exposing a float3 pin. Pixel Normal Offset and explicit 2D Offset remain outside the initial supported modes. Source anchors under the UE Engine directory are `Source/Runtime/Engine/Private/Materials/Material.cpp` (constructor), `Shaders/Private/MaterialTemplate.ush` (GetMaterialRefraction), `DistortionCommon.ush` (both distortion functions) and `ShadingCommon.ush` (dielectric helpers).

For the reference mono projection let V be the active view size, B the backing texture extent, U the unscaled view size, p the horizontal projection scale and a = U.x/U.y. Multiply the initial offset componentwise by V/B, U, (p, −a×p), and 0.00023. For original-pixel depth separation h in cm and bias b = −RefractionDepthBias, multiply by 4 × saturate(h/max(1, clamp(abs(b)/2, 0, 50)) + b) × saturate(h/30). Apply the sign in the established view-to-texture convention once. Clamp both lookups to the active view's texel rectangle inset by 0.55 texel per side, or its center on a one-texel axis. UE supplies U and p through SetupDistortionParams in `Source/Runtime/Renderer/Private/DistortionRendering.cpp`.

Use 1 Durin water world unit = 1 m = 100 cm. Reconstruct positive view Z for **each** of four device-depth taps using the active projection and its texel center. Reject foreground taps where background Z plus the maximum absolute screen derivative of surface Z is below surface Z. Normalize surviving bilinear weights when their sum exceeds 1e−5. The selected footprint supplies both linear background Z and HDR color. Retry at the original UV when the offset footprint has no valid taps. UE's optional refraction-culling mask defaults to white when unavailable. Use full weights for this feature and reserve Durin's coverage mask for visibility/composition.

Convert view-depth difference to cm once for the integral. For incident-light attenuation, use positive **world Z** difference in cm, reconstructing at the original surface pixel with the selected background depth as UE does. Treat NormalZ and ReversedZ symmetrically. The projection's clear value identifies an open background, never a zero-depth bed. Durin policy for open sky is the infinite-column limit per channel (positive extinction gives T=0, zero extinction gives T=1). Use a separate sky sample for this case, prefer finite valid bed taps in mixed footprints, and use zero thickness if both lookups fail solely due to foreground rejection. This avoids an arbitrary far plane and preserves sky through a perfectly clear channel.

#### Durin frame and resource ownership

Insert the water interval between cloud composition and the sorted translucency work in [SceneRenderer.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp) and [SceneColorRendering.cpp](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneColorRendering.cpp). Use the [existing attachment layouts](../../Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp) as format and depth-state precedents. Names below describe proposed RDG roles. All stored outputs are view-local.

| Order | Reads | Writes and attachment actions |
|---|---|---|
| 1. Background | Opaque GBuffer/depth, lighting, sky, retained opaque geometry, optional clouds | Existing passes produce HDR and D32. Select successful cloud composite color when present, otherwise base scene color. Exclude water from ordinary opaque/retained draws. |
| 2. Snapshot | Selected HDR and opaque device depth | Write/store complete active rects of distinct RGBA16_FLOAT color and R32_FLOAT device-Z textures. They stay immutable through water shading. |
| 3. Nearest water | Mesh/VF, WPO, mask and original scene depth attachment | Load D32. Use strict nearer test (Less for NormalZ, Greater for ReversedZ), write/store winning depth. Clear R8_UNORM coverage to 0 and store 1 on accepted samples. Mask holes and opaque foreground retain original depth. |
| 4. Water shading | Both snapshots, lighting/shadows, read-only winning D32 and coverage | Rasterize the same WPO and mask with Equal depth, coverage required. Load HDR, replace covered RGB and preserve alpha. Clear/store two RGBA16_FLOAT surface records for octahedral world normal XY and roughness, and RGB specular F0, respectively. Direct specular and volume lighting belong here. |
| 5. Sky reflection | Coverage, surface records, depth, filtered environment and BRDF | Load/store HDR with additive RGB and preserved alpha for covered pixels. Reading environment and attachments as separate resources avoids scene-color feedback. |
| 6. Sorted transparency and postprocess | Completed HDR and scene depth including nearest water | Preserve existing sorted blending, downstream depth policy, postprocess and editor order. No visible candidates bypass steps 2–5 without allocating water resources. |

Use deterministic mesh submission order for exactly coincident faces with different materials (the Equal color pass's last writer wins). Distinct depths must be order independent. All readers depend on the preceding completion result as well as the declared texture accesses. Lit material preview uses this graph with a fixed opaque bed and sky. Hit-proxy/wireframe keep ordinary displaced geometry and mask semantics. GBuffer-only diagnostics show the opaque background. Verify attachment usage and MRT/PSO compatibility on Vulkan during Stage 2.

#### Analytic acceptance baseline

Use a 320 × 240 linear RGBA16_FLOAT view, horizontal FOV 60°, camera at (0,0,3) m looking down −Z with screen up +X, water plane 6 × 6 m at z=0 and normal +Z. Place three opaque white emissive beds, each 1 m wide along X and 6 m along Y, at (x,z) = (−1,−0.25), (0,−1), (+1,−2) m. Add a 0.12 × 0.8 × 0.7 m opaque post centered at (0.65,0,−0.15) and a second 1 × 1 m water plane at z=−0.1. UE equivalents multiply all coordinates by 100. Use fixed unit exposure, black sky, zero direct/ambient lighting, stationary geometry and no AA, fog, reflection or postprocess transfer for absorption readback. Material settings are Opacity 0, Metallic 0, Specular 0.25, Roughness 0.1, FromF0, scattering zero, absorption 0.01 per cm in all channels and background scale one.

| Probe | Expected result |
|---|---|
| Bed centers, top-left pixel coordinates (160,195), (160,120), (160,45) respectively | For 25/100/200 cm, view T = 0.778801 / 0.367879 / 0.135335. Unit bed radiance after both paths = 0.606531 / 0.135335 / 0.018316 in each channel. |
| Refraction, isolated arithmetic | Specular 0.25 yields F0 0.02 and IOR 1.329431. For view normal XY=(0.5,0), full-frame view, bias zero and h≥30 cm, horizontal offset is +26.8772 texels. At h=15 cm it halves. None and explicit IOR=1 give zero offset. |
| Scattering, isolated arithmetic | For scattering=absorption=0.005 per cm and d=100 cm the integrated weight is 0.316060. At dot=+0.5, UE Schlick phase for g=−0.5/0/+0.5 gives 0.095317 / 0.079577 / 0.021784. Use the shader's signed light and refracted-ray directions. |
| Visibility and rejection | Both submission orders yield the front depth at center. Post top is above water and remains white. Mask holes preserve bed color and depth. Translate the viewport origin and tilt the normal toward the post to exercise edge clamp and rejected taps. |

For scalar float32 probes use absolute error ≤2e−5, for offsets ≤0.1 texel, and for linear HDR channels absolute error ≤max(0.002, 0.01 × |expected|). These are engineering thresholds for float32 math and half-float storage. Depth ordering, selected fallback and coverage are exact classifications at interior pixels. Sample before tonemapping and separate both transmittance factors in failure reports. Keep diagnostic captures under `Build/SingleLayerWater/` when implementation gates run.

### Stage 1: Compile and persist the water material interface

Dependency: Stage 0. Outcome: one authorable water material with deterministic compiled outputs.

- [x] Append the shading model to the serialized model enum. Implement configuration admission, root activation, typed output expression discovery, connection diagnostics and pixel code generation. Define missing/duplicate water output diagnostics. Support the agreed Refraction input. Reuse ordinary vertex evaluation for WPO and static selection for active bindings.
- [x] Carry pass usage, compiled entry identity and resource layout through shader selection, PSO preparation, editor Apply/Save, instance overrides and source-free DMAT admission. Preserve old material identifiers and check whether the derived artifact version needs invalidation. Follow [shader stage selection](../Runtime/Rendering/Materials/MaterialShaderStages.md).
- [x] **Acceptance**: compile Opaque and Masked examples, including simultaneous Opacity and OpacityMask. Verify defaults with partially connected inputs and exercise illegal blend/domain, invalid output combinations and type errors. Compare authored, reopened and source-free cooked values/entry layouts. Change an instance coefficient without a shader rebuild. Existing DefaultLit, Unlit and ClearCoat fixtures still pass.

### Stage 2: Route water and preserve the scene behind it

Dependency: Stage 1. Outcome: one visible water layer with stable read-only background inputs.

- [x] Add dedicated water depth and shading mesh passes, selection and PSO collectors. Exclude these draws from ordinary GBuffer and retained-forward color ownership. After opaque lighting, sky, retained opaque geometry and applicable background cloud composition, snapshot HDR color and device depth. Run the water mesh passes before sorted translucency and postprocess. Stage 4 adds reflection composition after shading using the persisted surface records.
- [x] Declare snapshots, final scene depth and surface coverage explicitly in RDG. Bind per-view snapshots only to water shading. Ensure the nearest visible surface owns color, depth and reflection inputs independent of draw order. Carry masked rejection and displaced geometry through the coverage and shading paths. Stage 3 reconstructs positive view depth from the snapshot.
- [x] **Acceptance**: RDG capture identifies each producer and consumer with no attachment feedback. A diagnostic water shader reproduces the pre-water background. Overlap in either submission order, opaque occluders, mask holes, offset view rectangles and sorted transparency in front/behind obey the specified depth policy. A view with zero water draws allocates no water targets and schedules no water work.

### Stage 3: Implement refraction and volume lighting

Dependency: Stage 2. Outcome: linear HDR transmission and in-scattering satisfy the above-water analytic baseline.

- [x] Implement the four algorithm steps above in shared Slang helpers. Keep the view-depth and vertical light paths independently inspectable in the fixture. Use existing directional light/shadow and sky inputs. Provide a controlled reflection-disabled comparison.
- [x] **Acceptance**: a 1 m pure-absorption sample with coefficient 0.01 per cm gives view T = exp(−1) before the separate bed-light factor. Check zero coefficients, zero thickness, independent RGB extinction, pure scattering, both signs of PhaseG, both Opacity endpoints, shoreline color scaling and clear depth. Compare against the Stage 0 numeric oracles and tolerances. The foreground post and viewport edge exercise the distorted-tap fallback without foreground color bleeding.

### Stage 4: Composite surface reflections and qualify material preview

Dependency: Stage 3. Outcome: the nearest water surface reflects the sky with the expected angle and roughness response.

- [x] Consume coverage and the selected surface normal, roughness and dielectric response in reflection composition. Assign direct specular and sky specular to explicit owners. Reuse the available filtered sky environment and apply the Stage 0 preview policy.
- [x] **Acceptance**: compare Durin normal and grazing views, two roughness values and reflection-disabled/enabled captures with a fixed sky. Covered pixels receive one reflection contribution, masked holes and opaque foreground preserve their baseline values, and a rear water mesh cannot add a second reflection. Editor preview and runtime show the same response with matched lights and background.

### Stage 5: Migrate default water and close integration

Dependency: Stage 4. Outcome: the shipping plugin asset exercises the qualified path.

- [x] Update the default water recipe and authored asset together. Preserve WPO, analytic normals, primitive parameters, bounds and CPU query behavior. Set explicit optical coefficients, Opacity and refraction values. Exercise a plain StaticMesh with the same shading model while the Water plugin is disabled.
- [x] **Acceptance**: validate existing water displacement/query fixtures plus editor and cooked Vulkan rendering of the animated surface. Check displaced silhouettes, intersecting opaque geometry and sorted transparency against the flat reference cases. Check load preparation and source-free startup for the migrated default. Record tested revisions, selections, settings, numerical results, capture locations and remaining parity limits. Publish implemented contracts in their owning material, renderer and water documents.

Use the [build workflow](../Agents/BuildAndRun.md) and [test selection workflow](../Agents/Testing.md) to validate changed owners and workspace consumers at each stage. Keep raw readbacks, captures and timings under ignored Build output or retrievable CI artifacts. Record acceptance summaries in this plan.
