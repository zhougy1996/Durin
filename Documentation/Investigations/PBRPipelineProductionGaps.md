# PBR Pipeline Production Gaps

**Status:** Open; findings verified against current source
**Last reviewed:** 2026-08-06

## Scope And Verdict

This investigation records the production-path gaps found while reviewing the
current StaticMesh metallic/roughness PBR pipeline. The shader closure itself
contains the intended Cook-Torrance GGX direct-light model, tangent-space normal
mapping, and split-sum studio environment lighting. The highest-priority defect
is earlier in the data path: stable material-proxy resolution silently drops
most v2 surface values before the Renderer decodes the binding.

The findings are ordered by user-visible severity and dependency:

| Priority | Issue | Status | Implementation boundary |
| --- | --- | --- | --- |
| P0 | Material proxies drop most v2 PBR values | Correction implemented; validation expanding | Bounded corrective task |
| P1 | Scene import drops parsed glTF PBR data | Verified end-to-end gap | Import follow-up plan or staged task after channel decision |
| P1 | LDR Scene Color clips PBR radiance before post-processing | Verified, intentionally deferred limitation | HDR/post-process plan |
| P1 | GGX denominator floor distorts low-roughness direct highlights | Verified numerical-quality defect | Bounded corrective task after reference selection |
| P2 | Material static properties do not control render passes | Verified, intentionally deferred limitation | Material roadmap milestone 4 |
| P2 | Lighting and IBL remain preview-scale and visually disconnected | Verified scope limitation | Future lighting/environment plan |
| P3 | Removed ambient and rim-light controls remain exposed | Verified stale API/editor state | Bounded cleanup task |

The P0 correction should land before using rendered output to validate any
other PBR change. Fixing import, BRDF, or texture behavior first would leave its
production result hidden by proxy defaults.

## Verified Findings

### P0 — Material proxies drop most v2 PBR values

**Status:** Correction implemented; focused native and Vulkan validation pass,
with the complete parity matrix still being expanded.

The implementation now normalizes every canonical local value once during
render-safe publication and applies every exact v2 identity and type through a
shared direct/proxy compilation policy. Focused coverage compares complete base
and instance representations, invalid-value and texture fallback behavior, all
UV roles, inherited/coalesced publication behavior, and isolated Vulkan images
for Metallic, Roughness, Normal, and Emissive. The remaining validation work is
to make every texture role and every instance override independently explicit
in the parity matrix before resolving this finding.

`DMaterial::BuildMaterialLocalRenderLayer` publishes every material definition,
and `DMaterialInstance::BuildMaterialLocalRenderLayer` publishes every valid
override. `FMaterialRenderProxy::Resolve_RenderThread`, however, applies those
values through `ApplyLocalParameter`, whose accepted cases are limited to:

- BaseColor;
- every Vector2 value, currently the UV scale and offset fields;
- BaseColorTexture;
- Opacity;
- the obsolete v1 SpecularStrength and Shininess identities.

All other recognized local parameters return success without changing the
representation. Metallic, Roughness, Normal, AmbientOcclusion, Emissive,
OpacityMask, every UV-channel scalar, and seven of the eight texture roles are
therefore silently ignored. A base proxy begins from the v2 ErrorMaterial
payload, after which BaseColor and static properties make the result look like
an ordinary material while the ignored fields retain fallback values:

```text
Metallic = 0
Roughness = 0.5
Normal = (0, 0, 1)
AmbientOcclusion = 1
Emissive = (0, 0, 0)
OpacityMask = 1
UV channels = 0
Normal/Metallic/Roughness/AO/Emissive/Opacity/Mask textures = null
```

StaticMesh production draws resolve exactly this proxy representation through
`FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread` before
`FStaticMeshRenderer` decodes the v2 binding. Direct calls to
`DMaterialInterface::GetRenderData` compile the complete v2 representation and
therefore do not reproduce the production result.

**Impact:** level, preview, and thumbnail rendering cannot reliably display
non-default metallic, roughness, normal, AO, emissive, mask, UV-channel, or
non-base-color texture values. Instance overrides for the same fields also
appear to succeed while producing no scene change. BaseColorTexture publication
additionally captures a texture reference without the usage/sRGB validation
performed by `GetRenderData`, so the two compilation paths disagree in both
field coverage and resource validation.

**Deterministic reproduction:** create a base material with non-default
Metallic, Roughness, AO, Emissive, UV channel, and one non-base-color texture.
Compare `TryGetMaterialRenderV2Binding(Material->GetRenderData())` with the
binding obtained from `Material->GetMaterialRenderProxy()->Resolve_RenderThread`.
The direct binding contains the authored values; the proxy binding retains the
fallbacks above.

**Validation gap:** current proxy tests compare complete payloads only while the
PBR fields retain defaults, then exercise BaseColor and Opacity changes. The
rendered thumbnail test changes BaseColor together with Roughness, so image
inequality does not isolate roughness. Its eight-texture assertion only requires
the combined image to differ; BaseColorTexture alone satisfies that condition.

**Required direction:** use one canonical local-value compilation policy for
direct render data and proxy publication. Every exact-v2 uniform and resource
field must be applied by identity and type, with the same finite-value, range,
normalization, integer UV-channel, texture-usage, and sRGB validation rules.
Render-thread resolution must continue to consume render-safe values without
reading reflected objects.

Acceptance evidence must compare direct and proxy bindings after changing each
v2 constant, each texture role, each UV channel/scale/offset, base and instance
values, inherited updates, invalid textures, and rapid coalesced publications.
At least one Vulkan image test must vary only Metallic, only Roughness, only
Normal, and only Emissive.

### P1 — Scene import drops parsed glTF PBR data

**Status:** Verified end-to-end gap; packed-channel strategy remains unresolved.

`GltfSceneAdapter` parses metallic and roughness factors, emissive factor,
normal scale, occlusion strength, alpha mode, double-sided state, and BaseColor,
MetallicRoughness, Normal, Occlusion, and Emissive texture bindings into
`FImportedMaterial`. Scene-output planning creates only a BaseColor/sRGB texture
output. Material preparation then maps only BaseColor, Opacity, and
BaseColorTexture onto the generated material instance.

**Impact:** a supported glTF file can contain valid metallic/roughness PBR data
that survives adapter normalization but is absent from the immediately
renderable imported assets. Imported metal renders as the proxy/default
dielectric surface even after the P0 proxy defect is corrected unless this
mapping is completed.

The current v2 surface contract samples the R channel for every scalar texture,
while glTF packs roughness in G and metallic in B. Completing import therefore
requires an explicit choice between:

- deriving separate linear DataMask assets/channels during import; or
- defining a new material-layout version with per-role channel selection.

The chosen path must also address one source image used by both sRGB and linear
semantics, normal-scale representation, occlusion strength, alpha mode,
double-sided state, reimport identity, rollback, and texture deduplication.

**Validation gap:** adapter fixtures prove normalized PBR data, but no
end-to-end import fixture proves that generated material assets retain and
render every supported factor, semantic, channel, UV transform, and static
property.

**Candidate direction:** prepare a follow-up to the archived
Ready-to-Use Static Model Import plan after selecting the packed-channel
contract. Its rendered acceptance images must isolate dielectric/metal,
low/high roughness, normal, AO, emissive, and masked input, and must run only
after proxy parity is proven.

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

1. Add a failing direct-versus-proxy v2 parity test and correct the P0 proxy
   path.
2. Re-run isolated rendered material-role coverage so later baselines observe
   authored PBR values.
3. Select and execute the glTF packed-channel contract.
4. Correct low-roughness GGX stabilization against explicit numeric and image
   references.
5. Plan HDR output and material render passes as separate architectural units.
6. Expand scene lighting only after environment ownership and scalability
   requirements are selected.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [PBR Material Surface historical plan](../Plans/Archive/2026-08/PBRMaterialSurface.md)
- [Ready-to-Use Static Model Import historical plan](../Plans/Archive/2026-08/ReadyToUseStaticModelImport.md)

## Relevant Implementation

- `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp` and
  `MaterialInstance.cpp`: complete local-layer publication;
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`:
  direct v2 compilation and texture validation;
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`:
  incomplete local-value application and render-thread resolution;
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp` and
  `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`:
  production proxy consumption;
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
