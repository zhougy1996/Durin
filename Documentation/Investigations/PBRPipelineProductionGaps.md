# PBR Pipeline Production Gaps

**Status:** Open; remaining findings verified against current source
**Last reviewed:** 2026-08-15

## Scope And Verdict

This investigation records the production-path gaps found while reviewing the
current StaticMesh metallic/roughness PBR pipeline. The shader closure itself
contains the intended Cook-Torrance GGX direct-light model, tangent-space normal
mapping, and split-sum studio environment lighting. The remaining production
gaps begin at render-target precision, render-pass execution, and scene/editor
ownership rather than material publication, scene import, or BRDF stabilization.

The findings are ordered by user-visible severity and dependency:

| Priority | Issue | Status | Implementation boundary |
| --- | --- | --- | --- |
| P1 | LDR Scene Color clips PBR radiance before post-processing | Resolved by HDR Scene Color and display mapping | Renderer display contract |
| P2 | Material static properties do not control render passes | Verified, intentionally deferred limitation | Material roadmap milestone 4 |
| P2 | Lighting and IBL remain preview-scale and visually disconnected | Verified scope limitation | Future lighting/environment plan |
| P3 | Removed ambient and rim-light controls remain exposed | Verified stale API/editor state | Bounded cleanup task |

## Verified Findings

### P1 — LDR Scene Color clips PBR radiance before post-processing

**Status:** Resolved 2026-08-15 by the
[HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
contract.

The StaticMesh shader outputs finite, unclamped scene-linear RGB. Scene Color
and its contact-shadow-preserving copy now use `RGBA16_FLOAT`; exact Vulkan
half-float readback proves representative values above one survive the store.
Copy and FXAA both apply the same per-view exposure and ACES fitted display
transform before the existing SDR output.

**Resolution:** direct specular, studio-environment, and Emissive radiance up
to the authored range remain scene-linear through scene and optional contact
composition. The post-process shader maps them once to display-linear SDR and
the sRGB attachment owns the sole transfer encode.

The lasting format, exposure, curve, alpha, FXAA, cache, ordering, and failure
rules are owned by the linked runtime contract.

### P2 — Material static properties do not control render passes

**Status:** Verified, intentionally deferred limitation; already owned by
Material System roadmap milestone 4.

Every StaticMesh graphics pipeline disables alpha blending, enables depth test
and depth writes, and disables back-face culling. Opacity is carried only in
output alpha, while OpacityMask and its threshold do not discard fragments or
change coverage. Blend mode, two-sided state, depth-write policy, and mask
threshold participate in identities without producing their promised visible
pass behavior.

**Impact:** Masked and Translucent materials render as opaque depth writers;
one-sided materials pay two-sided raster cost; backfaces are not supplied a
two-sided-normal policy; and there is no depth-only or shadow-depth behavior.

**Required ownership:** select the existing roadmap milestone rather than
creating a competing task. The implementation boundary must cover opaque,
masked, and translucent passes; culling and depth-write policy; sorting;
depth-only/shadow-depth behavior; and pass/permutation identity.

### P2 — Lighting and IBL remain preview-scale and visually disconnected

**Status:** Verified scope limitation; not a regression against the frozen PBR
surface contract.

The scene exposes only the first registered directional light to StaticMesh
rendering. There are no point/spot lights, shadows, light culling, or attenuation
paths. All lit surfaces use the hidden built-in studio IBL independently of the
visible scene SkyBox; changing SkyBox rotation, tint, intensity, or texture
therefore does not change material reflection or diffuse environment lighting.
AO uniformly multiplies both diffuse and specular IBL.

**Impact:** metal reflections can visibly disagree with the background,
multiple lights are ignored, contacts are unshadowed, and AO can over-darken
metallic/specular response. The fixed IBL remains useful for deterministic
material judgment but is not a scene-lighting solution.

**Candidate direction:** keep the deterministic preview environment, but give
level rendering an explicit SkyLight/environment selection and reflection
contract before adding probes or multiple-light scalability. Specular occlusion
should be selected independently from diffuse AO rather than retaining one
uniform multiplier by accident.

### P3 — Removed ambient and rim-light controls remain exposed

**Status:** Verified stale API/editor state; selected PBR behavior removed these
terms.

`FDirectionalLightSceneData` and `DDirectionalLightComponent` still expose
AmbientIntensity and RimLightIntensity, and the rendered-thumbnail preview
scene still assigns both. `FStaticMeshLightingUniform` uploads only direction,
color/intensity, and view position; the shader has no ambient or rim inputs.

**Impact:** editor/API callers can change values that have no rendering effect,
and preview code implies fill/rim lighting that is actually supplied only by
the fixed studio IBL and key light.

**Required direction:** remove the stale fields, setters, reflected controls,
and preview assignments, or rename/redefine the preview contract around its
actual IBL behavior. Reintroducing non-physical scalar ambient/rim terms is not
the selected correction.

## Validation Ordering

1. Plan HDR output and material render passes as separate architectural units.
2. Expand scene lighting only after environment ownership and scalability
   requirements are selected.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [PBR Material Surface historical plan](../Plans/Archive/2026-08/PBRMaterialSurface.md)
- [Ready-to-Use Static Model Import historical plan](../Plans/Archive/2026-08/ReadyToUseStaticModelImport.md)

## Relevant Implementation

- `Engine/Shaders/Slang/StaticMeshBasePass.slang` and
  `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`: GPU and CPU PBR
  references;
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`,
  `Engine/Shaders/Slang/PostProcess.slang`, and
  `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`:
  LDR scene target and copy/FXAA output;
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp` and
  `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`:
  single-light and fixed-environment selection.
