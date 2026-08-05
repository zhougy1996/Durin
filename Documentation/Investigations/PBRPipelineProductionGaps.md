# PBR Pipeline Production Gaps

**Status:** Open; findings verified against current source
**Last reviewed:** 2026-08-06

## Scope And Verdict

This investigation records the production-path gaps found while reviewing the
current StaticMesh metallic/roughness PBR pipeline. The shader closure itself
contains the intended Cook-Torrance GGX direct-light model, tangent-space normal
mapping, and split-sum studio environment lighting. The remaining production
gaps begin at scene import, render-target precision, and low-roughness BRDF
stabilization rather than material-proxy publication.

The findings are ordered by user-visible severity and dependency:

| Priority | Issue | Status | Implementation boundary |
| --- | --- | --- | --- |
| P1 | Scene import cannot represent glTF texture rotation or per-binding samplers | Partially remediated import gap | Material UV/sampler layout follow-up |
| P1 | LDR Scene Color clips PBR radiance before post-processing | Verified, intentionally deferred limitation | HDR/post-process plan |
| P1 | GGX denominator floor distorts low-roughness direct highlights | Verified numerical-quality defect | Bounded corrective task after reference selection |
| P2 | Material static properties do not control render passes | Verified, intentionally deferred limitation | Material roadmap milestone 4 |
| P2 | Lighting and IBL remain preview-scale and visually disconnected | Verified scope limitation | Future lighting/environment plan |
| P3 | Removed ambient and rim-light controls remain exposed | Verified stale API/editor state | Bounded cleanup task |

## Verified Findings

### P1 — Scene import cannot represent glTF texture rotation or per-binding samplers

**Status:** Partially remediated; the remaining loss is explicit and warned.

Scene import now maps metallic, roughness, emissive, opacity, and AO factors;
all five glTF texture semantics; UV channel, scale, and offset; alpha mode and
cutoff; and double-sided state onto generated material instances. It creates
semantic-specific Color/sRGB, Normal/linear, and DataMask/linear outputs. The
selected packed-channel policy derives deterministic linear assets with
metallic B, roughness G, AO R, and base-color alpha copied into the material
contract's sampled R channel. Normal scale and glTF's multiplicative emissive
factor are baked into semantic derivatives where the material surface does not
expose the same operation.

Derived outputs participate in stable planning identity, semantic/color-space
deduplication, mounted-source publication, rollback, and in-place reimport.
Material instances may persist a validated static-property override so imported
alpha and two-sided state survive without importer-specific base materials.

The remaining gap is narrower: `KHR_texture_transform` rotation and per-binding
sampler filter/wrap state cannot be represented by the current material
render layout, which contains only UV channel/scale/offset and one shared
material sampler. Import emits an explicit warning for either case rather than
silently claiming parity.

**Validation gap:** the end-to-end import fixture proves generated material
values, all semantic texture usages and color spaces, packed-channel pixels,
UV channel/scale/offset, static properties, and stable in-place reimport. It
does not yet provide rendered-image acceptance for those inputs, failure
injection across the expanded seven-texture graph, UV rotation, or independent
samplers.

**Candidate direction:** introduce a versioned material render layout with
per-role rotation and sampler state, then add rendered acceptance images that
isolate dielectric/metal, low/high roughness, normal, AO, emissive, masked,
rotated-UV, and sampler-wrap inputs.

### P1 — LDR Scene Color clips PBR radiance before post-processing

**Status:** Verified, intentionally deferred limitation; requires a plan rather
than a local format substitution.

The StaticMesh shader outputs finite, unclamped scene-linear RGB, but Scene Color
is `SRGBA8_UNORM`. Hardware stores therefore clip values above one before the
post-process pass samples them. The only current post-process choices are a
fullscreen copy and FXAA; neither applies exposure or tone mapping.

**Impact:** direct specular values above one, bright studio-environment samples,
and Emissive values authored up to 64 lose their dynamic range on first store.
Highlights become flat white, no later bloom or exposure stage can recover
them, and roughness/metallic comparisons are biased by clipping. The sRGB
transfer itself is valid; the loss is caused by using the final LDR format as
the scene-linear intermediate.

**Candidate direction:** select an HDR Scene Color format, exposure ownership,
tone mapper, post-process ordering, final sRGB conversion, FXAA domain, and
editor-assistance composition contract together. Main viewports, offscreen
camera views, material previews, and thumbnails must retain deterministic
output. This work should not be represented as a one-line switch to RGBA16F.

Acceptance evidence must include values above one surviving the scene pass,
exposure/tone-map reference values, stable SDR output, emissive response, and
consistent present/offscreen rendering.

### P1 — GGX denominator floor distorts low-roughness direct highlights

**Status:** Verified numerical-quality defect; the replacement stabilization
and antialiasing reference need selection.

Both the shader and CPU reference compute the GGX distribution as
`alpha2 / max(PI * distributionTerm^2, 1e-5)`. At the aligned-light peak,
`distributionTerm = roughness^4`, so the floor changes the distribution for
roughness below approximately 0.205 rather than merely preventing a divide by
zero.

| Perceptual roughness | Ideal aligned `D` | Current aligned `D` | Retained peak |
| ---: | ---: | ---: | ---: |
| 0.045 | about 77,625 | about 0.41 | less than 0.001% |
| 0.10 | about 3,183 | 10 | about 0.3% |
| 0.20 | about 199 | 160 | about 80% |
| 0.50 | about 5.09 | about 5.09 | 100% |

**Impact:** smooth materials receive an artificially weak and broad-looking
directional highlight. Environment prefiltering does not apply the same clamp,
so direct and image-based responses disagree as roughness approaches zero.

**Validation gap:** frozen CPU references cover roughness 0.5 and 1.0. The
extreme-input test combines zero roughness with degenerate light/view vectors
and therefore returns zero before it can validate the aligned low-roughness
peak. Rendered tests do not isolate a low-roughness sweep.

**Candidate direction:** retain finite handling while moving stabilization to a
term and magnitude that do not redefine the supported roughness range. Select
the intended direct-specular antialiasing policy separately from the BRDF
epsilon. Update CPU/GPU parity values and add aligned/off-axis sweeps at the
minimum, 0.1, 0.2, 0.5, and 1.0 roughness values.

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

1. Complete glTF UV rotation and per-binding sampler representation.
2. Correct low-roughness GGX stabilization against explicit numeric and image
   references.
3. Plan HDR output and material render passes as separate architectural units.
4. Expand scene lighting only after environment ownership and scalability
   requirements are selected.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [PBR Material Surface historical plan](../Plans/Archive/2026-08/PBRMaterialSurface.md)
- [Ready-to-Use Static Model Import historical plan](../Plans/Archive/2026-08/ReadyToUseStaticModelImport.md)

## Relevant Implementation

- `Engine/Shaders/Slang/StaticMesh.slang` and
  `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`: GPU and CPU PBR
  references;
- `Engine/Source/Editor/StandardAssetImport/Private/GltfSceneAdapter.cpp` and
  `SceneImport.cpp`: normalized PBR input and incomplete output mapping;
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`,
  `Engine/Shaders/Slang/PostProcess.slang`, and
  `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`:
  LDR scene target and copy/FXAA output;
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp` and
  `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`:
  single-light and fixed-environment selection.
