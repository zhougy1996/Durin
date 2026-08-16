# Deferred Directional Lighting

Summary: Defines isolated and production deferred lighting ownership, composition, failure, ABI, lifecycle, memory, and qualification contracts.

Modules: Renderer, RenderCore

The deferred lighting path evaluates opaque and masked Lit material records
over the [minimal GBuffer](GBuffer.md). It supports both the isolated
qualification target and the production composition route. Every solid Lit
view requires deferred opaque ownership; no complete forward Lit route or
migration fallback remains.

## Ordering and ownership

The isolated qualification route records:

```text
directional shadow -> GBuffer -> deferred directional qualification
                   -> selected production or special scene -> display
                   -> editor assistance
```

`FDeferredDirectionalLightingRenderer` owns the shader/pipeline payload and a
size-keyed `RGBA16_FLOAT` target cache. The result is isolated, never presented,
and finishes graphics-shader-readable for explicit qualification capture. A
failed or disabled diagnostic leaves the selected scene output authoritative.
Translucency, skybox drawing, production contact visibility, and editor
assistance are not part of this
target.

The production route records:

```text
directional shadow -> GBuffer (owns D32)
                   -> optional contact visibility
                   -> sky/clear Scene Color bootstrap
                   -> deferred Lit opaque/masked into Scene Color
                   -> retained forward Unlit + sorted translucent
                   -> display -> editor assistance
```

Production writes only load-preserving `RGBA16_FLOAT` Scene Color; it does not
allocate or copy the isolated deferred target. Static/Spline, Skeletal, and Terrain cache
separate retained-forward pipeline variants against the load-preserving render
pass ABI, and SkyBox owns a bootstrap-layout pipeline variant for the same
reason. This keeps Vulkan render-pass dependencies compatible while
preserving Scene Color and GBuffer depth. Retained draws finish depth in the
ordinary writable state expected by contact and the next view.

`FSceneRenderer::RenderScene_RenderThread` is the sole scene-color composition
entry point. After common shadow, GBuffer, GTAO, and resource preparation, a
solid Lit view records the production sequence above. Unlit, wireframe, and
other explicitly named special view modes select the special-forward stage
inside the same entry point; resource failure never changes the view mode or
lighting owner.

Missing GBuffer, lighting, or retained-forward resources report
`RendererResourcesUnavailable` and never expose partial HDR. Enabled and
unavailable counters diagnose production deferred outcomes. Main, auxiliary,
preview, thumbnail, Present, and offscreen views retain independent prepared
inputs and sequential isolation.

## Inputs and fixed ABI

The fragment shader binds, in order, four GBuffer attachments, D32, irradiance,
prefiltered environment, the BRDF LUT, the environment sampler, the selected
directional-shadow array and comparison sampler, one 176-byte deferred-view
uniform, the existing 768-byte forward-lighting uniform, GTAO native raw,
native filtered, contact visibility, and full-resolution resolved inputs. The selected
directional, three-cascade shadow, and four local records remain byte-identical
to forward.

The deferred-view uniform contains four projection rows, one view-to-world
matrix, clear color, inverse viewport size, diagnostic identity, contact
enabled/debug controls, and native-half diagnostic selection. Position is
reconstructed analytically from D32. Constrained views use
the centered fitted viewport for GBuffer, deferred, and forward drawing; the
shader derives viewport-local NDC while loading absolute attachment pixels.
This prevents letterbox regions or an earlier same-size view from becoming
valid lighting inputs.

Missing environment resources bind the same black cube/black LUT fallback as
forward. Missing or disabled shadow input uses a complete fallback binding and
a disabled shadow record, so the shader does not sample stale data. A partial
GBuffer never runs deferred lighting. A zero-candidate view may run against the
freshly cleared current-view attachments and produces the clear color.

## Shared lighting semantics

`Lighting/SurfaceLighting.slang` owns the surface frame, directional and local
BRDF calls, point/spot inverse-square and range/cone attenuation, split-sum
environment evaluation, and final direct + environment + emissive composition.
The evaluator consumes the four selected local records in their unchanged
ascending-ID order; point and spot share that fixed budget, while rejected and
overflow records never enter the payload. `Lighting/DirectionalShadow.slang`
remains the sole
shadow receiver implementation for forward and deferred. The GBuffer changes
transport precision only; it does not select another material or lighting
model.

Production deferred lighting also consumes the optional
[ground truth ambient occlusion](GroundTruthAmbientOcclusion.md) factor. It
multiplies decoded material AO only at the environment-light input; a disabled
or unavailable payload uses factor one and does not affect direct light,
shadows, emissive, alpha, retained forward surfaces, or display mapping.

Valid standard-lit pixels write scene-linear RGB and decoded effective opacity.
Invalid flags, failed reconstruction, and background depth retain immutable
`View.ClearColor`. Unlit and translucent surfaces remain excluded and continue
through their established forward owners.

## Diagnostics and lifecycle

Per-view counters report enabled, unavailable, failed, and diagnostic routes
plus exact output bytes. Capture and GPU-timing seams are installed only by
qualification callers. Component modes expose decoded material, directional,
local, environment, emissive, alpha, and final HDR terms; directional-shadow
diagnostics continue to use the shared shadow diagnostic identities.

Injected shader, pipeline, and target failures plus GBuffer unavailability,
missing-environment fallback, missing/disabled-shadow fallback, resize, reload,
device invalidation, explicit release, and shutdown follow Renderer resource-
coordinator generations. Target publication is complete-or-null. The cache
retains the current extent and evicts oldest other extents above `64 MiB`;
recorded commands retain their RHI references across eviction.

## Memory and RTX 3090 qualification

The target costs 8 bytes per pixel: `16,588,800` bytes at 1920x1080. Four such
extents fit under the `64 MiB` cache ceiling and five do not. M1 + M2 + M3 cache
ceilings total `384 MiB`; one active 1920x1080 route including scene targets,
GBuffer, deferred color, and one SDR output costs `107,827,200` bytes.

The validation-enabled qualification uses NVIDIA GeForce RTX 3090, driver
591.86, Vulkan 1.4.325, `Win64-Debug-DurinEditor`, 1920x1080, 30 warm-up frames,
and 120 measured frames. The four-family Lit fixture uses one Medium-filter
directional shadow and the complete black environment fallback. The selected
run measured:

| GPU interval | Median | p95 | Frozen gate median/p95 |
| --- | ---: | ---: | ---: |
| GBuffer | 79,968 ns | 80,768 ns | 350,000 / 500,000 ns |
| Deferred directional | 200,896 ns | 201,952 ns | 300,000 / 450,000 ns |
| GBuffer + deferred | 280,864 ns | 282,208 ns | 600,000 / 800,000 ns |

M4 Stage 1 extends the same fixture to the selected `1 + 4` tier without
changing the ABI or target. Its selected run measured GBuffer
`79,840/80,704 ns`, isolated deferred `254,384/255,552 ns`, and their sum
`334,272/335,904 ns` median/p95. The M4 lighting gate is
`450,000/650,000 ns`. M4 Stage 2 then composed that evaluator directly into
production Scene Color. The isolated target remains only for component
diagnostics and qualification; it does not provide a complete forward A/B
scene.

The final M4 production qualification uses the same adapter, driver, Vulkan
version, validation, profile, extent, and `30 + 120` sampling policy. Its mixed
fixture adds retained Unlit and translucent StaticMesh draws to the four Lit
geometry families. Because one command list forbids overlapping timestamp
queries, each interval is sampled in an otherwise identical production run;
the tracked total is the per-sample sum of shadow, GBuffer, complete hybrid
Scene Color, and FXAA intervals. The selected run measured:

| Production interval | Median | p95 | Frozen gate median/p95 |
| --- | ---: | ---: | ---: |
| GBuffer geometry | 80,864 ns | 81,696 ns | 350,000 / 500,000 ns |
| Deferred `1 + 4` into Scene Color | 254,880 ns | 255,872 ns | 450,000 / 650,000 ns |
| Sky/clear + retained forward | 8,832 ns | 10,240 ns | 300,000 / 500,000 ns |
| FXAA display | 67,648 ns | 68,288 ns | 100,000 / 120,000 ns |
| Shadow-through-display tracked total | 424,752 ns | 426,784 ns | 1,350,000 / 2,100,000 ns |

The production extent is exactly `91,238,400` bytes, or `141,570,048` bytes
including the directional-shadow array. Scene-target plus GBuffer size caches
retain at most `320 MiB`; the isolated deferred target is not allocated by a
production view and remains outside that active/retained production count.

Forward/deferred HDR and displayed references pass the frozen decoded-record,
quantization, shadow-discontinuity, background, alpha, and above-one-radiance
gates across primitive, material, filter, cascade, projection, aspect, extent,
and view-route matrices.
