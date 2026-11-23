# HDR Scene Color and Display Mapping

Summary: Define the scene-linear HDR intermediate and the single deterministic transform into SDR viewport outputs.

Modules: RenderCore, Renderer, RHI

Last reviewed: 2026-11-20

## Color Domains and Formats

Scene-domain RGB is finite, non-negative, scene-linear radiance. `SceneColor`
uses `RGBA16_FLOAT`; values above one remain representable until post process.
Its alpha channel carries the established effective opacity. Directional
contact shadows are applied as scalar deferred visibility before Scene Color
composition, so no directional-copy or corrected-color intermediate remains.
Scene depth remains `D32`.

Eligible opaque/masked Base Pass draws seed Emissive RGB and alpha one into
SceneColor alongside three RGBA8 GBuffer records. Background clears retain the
view clear color and alpha. Bootstrap loads this seed; sky tests against the clear far depth and preserves
covered receivers. Deferred RGB adds only
lighting and preserves destination alpha. Forward Unlit and translucent
materials apply their Emissive once through existing forward composition.
No standalone Emissive attachment remains. Emissive/MaterialInputs debug snapshots the seed
before lighting only on request. Exposure is still applied solely by the final
display transform; the HDR seed and lighting are exposure-independent.

Present and offscreen outputs remain `SRGBA8_UNORM`. The post-process shader
writes bounded display-linear RGB. The sRGB attachment performs the only
transfer encoding; shader code must not encode sRGB itself.

## Display Transform

Each immutable `FSceneView` owns one manual `ExposureEV`. Its default and
non-finite fallback are `0.0`; authored values are clamped to `[-16, 16]`.
The CPU computes `exposureScale = exp2(ExposureEV)` and packs it with inverse
target size and Bloom constants in a per-draw uniform. Sequential views never share
semantic exposure state.

For each RGB channel, the selected ACES fitted transform is:

```text
x = max(sceneLinear * exposureScale, 0)
mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
displayLinear = finite(mapped) ? clamp(mapped, 0, 1) : 0
```

At EV 0 the scalar goldens are `0 -> 0`, `0.18 -> 0.26689893`,
`1 -> 0.80379748`, `4 -> 0.97341710`, and `64 -> 1`. The transform is
monotonic over non-negative finite scene radiance. Final alpha is independent
of RGB: finite effective opacity is clamped to `[0, 1]`, and a non-finite alpha
becomes zero.

## FXAA and Composition Order

The graphics copy and FXAA shaders both own the required display transform.
The copy path maps its center sample. The FXAA path maps every sampled scene
value before luma, direction, and resolve calculations, so FXAA operates on
bounded display-linear RGB. Low-contrast neighborhoods return the mapped
center color before directional sampling. This fused route adds no display-sized
intermediate. FXAA keeps the mapped center alpha.

All scene draws and optional contact-shadow composition finish in HDR. Display
mapping and optional FXAA then write the SDR output. An optional per-view
[PostProcess material](../Materials/MaterialSceneInputs.md#per-view-slot-and-preview)
runs between completed HDR composition and that display transform, with a
separate HDR output and local failure bypass. Bloom reads the selected HDR
result after this material slot. Editor grid, gizmos,
overlay lines, and icons load that output afterward, preserving their crisp
SDR design. Present/offscreen selection changes only the final resource state,
not color processing.

## Bloom

`FSceneViewPostProcessSettings::bEnableBloom` opts a view in. The Level Editor
exposes the setting under **Post Processing > Bloom** and exposes its resolution
under **Bloom Resolution**. The Material Editor preview exposes both controls.
Bloom defaults to off, and diagnostic outputs bypass the effect.

RDG extracts highlights from the fitted view into a half-resolution HDR target
by default. The quarter-resolution option uses a wider extraction footprint
and starts one level lower, with both options ending at up to 1/32 resolution
before upsampling. The filter uses exposed luminance with a threshold of 1.0
and a soft knee of 0.5 while preserving scene-linear RGB in the pyramid. The
levels are frame-transient and stop growing in count once both dimensions
reach one pixel.

The display pass samples the reconstructed result at 0.15 strength and adds
it to scene RGB before applying exposure and the ACES fit. The Copy and FXAA
paths both apply this composition, preserving scene alpha and the fitted
view's letterbox. The effect adds no full-resolution HDR output. A view with
Bloom disabled declares no Bloom passes or textures.

## Resources, Retention, and Failure

Copy and FXAA prepare typed vertex and fragment references from the published
global shader map for each view. The draw retains those references and uses
the same pipeline initializer for precaching and recording. The RHI cache
reuses matching shaders, pipeline layout and render state across frames.
A successful publication makes the replacement shaders available to subsequent
views. A failed reload keeps the previous shaders and pipelines usable.

Scene Color and depth cost 12 bytes per pixel (8 + 4), or `24,883,200` bytes
at 1920x1080. They are frame-transient graph targets.
Allocation and retention follow [frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes);
feature byte costs do not define independent cache quotas. The optional material
slot adds one RGBA16F output (8 bytes/pixel); the disabled slot adds none.

## Validation Contract

CPU goldens cover curve values, monotonicity, exposure canonicalization,
finiteness, and alpha. Layout tests freeze HDR scene and SDR output formats.
Scene Color permits explicit source copies so development validation can
capture it without making the production target CPU-readable. Vulkan readback freezes exact half-float values above one before
display mapping, including production emissive and contact on/off paths,
per-view exposure separation, output alpha, editor assistance ordering, shader
refresh, and device lifecycle.
Qualification image hashes are display-contract-specific and must be reviewed
when this transform intentionally changes.

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DisplayMapping.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Shaders/Slang/PostProcess.slang`
