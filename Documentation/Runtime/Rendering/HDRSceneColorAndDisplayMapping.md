# HDR Scene Color and Display Mapping

Summary: Define the scene-linear HDR intermediate and the single deterministic transform into SDR viewport outputs.

Modules: RenderCore, Renderer, RHI

Last reviewed: 2026-08-16

## Color Domains and Formats

Scene-domain RGB is finite, non-negative, scene-linear radiance. `SceneColor`
uses `RGBA16_FLOAT`; values above one remain representable until post process.
Its alpha channel carries the established effective opacity. Directional
contact shadows are applied as scalar deferred visibility before Scene Color
composition, so no directional-copy or corrected-color intermediate remains.
Scene depth remains `D32`.

Present and offscreen outputs remain `SRGBA8_UNORM`. The post-process shader
writes bounded display-linear RGB. The sRGB attachment performs the only
transfer encoding; shader code must not encode sRGB itself.

## Display Transform

Each immutable `FSceneView` owns one manual `ExposureEV`. Its default and
non-finite fallback are `0.0`; authored values are clamped to `[-16, 16]`.
The CPU computes `exposureScale = exp2(ExposureEV)` and packs it with inverse
target size in one 16-byte per-draw uniform. Sequential views never share
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
bounded display-linear RGB. This fused route adds no display-sized
intermediate. FXAA keeps the mapped center alpha.

All scene draws and optional contact-shadow composition finish in HDR. Display
mapping and optional FXAA then write the SDR output. Editor grid, gizmos,
overlay lines, and icons load that output afterward, preserving their crisp
SDR design. Present/offscreen selection changes only the final resource state,
not color processing.

## Resources, Retention, and Failure

`FPostProcessRenderer` publishes copy and FXAA shader/pipeline payloads
transactionally. A missing payload reports renderer resources unavailable;
there is no raw HDR-to-SDR fallback. Same-device last-known-good shader/manual
payloads follow the renderer resource coordinator contract, while device
invalidation clears dependent resources before retry.

One scene-target extent accounts for 12 bytes per pixel: 8 Scene Color and
4 depth. The size-keyed cache keeps
the current extent and evicts oldest other extents while retained payload bytes
exceed `96 MiB`. Recorded commands retain their own RHI references, so cache
eviction cannot invalidate in-flight work. One 1920x1080 extent is
`24,883,200` bytes and the budget retains at least three such extents.

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
