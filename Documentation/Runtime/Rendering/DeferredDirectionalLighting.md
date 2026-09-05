# Deferred Directional Lighting

Summary: Defines isolated and production deferred lighting ownership, composition, failure, ABI, lifecycle, memory, and qualification contracts.

Modules: Renderer, RenderCore

The deferred lighting path evaluates opaque and masked Lit material records
over the [minimal GBuffer](GBuffer.md). It supports both the isolated
qualification target and the production composition route. Every solid Lit
view requires deferred opaque ownership; no complete forward Lit route or
migration fallback remains.

## Ordering and ownership

The complete frame schedule is owned by
[Renderer Frame Preparation](RendererFramePreparation.md#render-graph-frame-schedule).
Deferred lighting consumes completed GBuffer/D32 and optional visibility/AO
results before retained forward surfaces, cloud composition, and translucency.

`FDeferredDirectionalLightingRenderer` owns shader/pipeline payloads. The
isolated diagnostic branch declares an `RGBA16_FLOAT` graph target, finishes
it graphics-shader-readable for capture, and leaves selected scene output
authoritative on failure. Sky, translucency, and editor assistance are outside
that isolated target.

Production writes only load-preserving `RGBA16_FLOAT` Scene Color; it does not
allocate or copy the isolated deferred target. Static/Spline caches separate
retained-forward pipeline variants against the load-preserving render
pass ABI, and SkyBox owns a bootstrap-layout pipeline variant for the same
reason. This keeps Vulkan render-pass dependencies compatible while
preserving Scene Color and GBuffer depth. Retained draws finish depth in the
ordinary writable state expected by contact and the next view.

The frame pipeline/composer owns production ordering. Solid Lit views require
deferred opaque ownership; Unlit, wireframe, and explicitly named special modes
select special-forward work in the same graph. Resource failure never changes
the view mode or lighting owner.

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
coordinator generations.
Allocation and retention follow [frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes);
feature byte costs do not define independent cache quotas.

## Memory and Qualification

An isolated deferred target costs 8 bytes per pixel, or `16,588,800` bytes at
1920x1080. Production renders directly into Scene Color and allocates no
isolated target. The qualification fixture's Scene Color/depth, GBuffer,
half-resolution GTAO, and SDR output subtotal is `69,984,000` bytes, or
`120,315,648` bytes with the directional-shadow array. These are named fixture
subtotals, not a frame-wide allocation ceiling or a count of every optional
cloud/contact/debug resource.

[`GBufferQualificationTests`](../../../Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp)
qualifies the mixed Static/Spline Lit fixture plus retained
Unlit/translucent surfaces on NVIDIA GeForce
RTX 3090, driver 591.86, Vulkan 1.4.325, validation enabled,
`Win64-Debug-DurinEditor`, 1920x1080, 30 warm-up and 120 measured frames.
The synchronized production intervals and quiet-GPU requirement follow
[GBuffer qualification](GBuffer.md#diagnostics-and-qualification).

| Production interval | Median gate | p95 gate |
| --- | ---: | ---: |
| GBuffer geometry | 350,000 ns | 500,000 ns |
| Deferred `1 + 4` into Scene Color | 450,000 ns | 650,000 ns |
| Retained scene work | 300,000 ns | 500,000 ns |
| FXAA display | 100,000 ns | 120,000 ns |
| Shadow-through-display total | 1,350,000 ns | 3,000,000 ns |

The AO composition median increment is at most `75,000 ns`. Total p95 must
also be within 125% of its median; unstable samples require a quiet rerun.
The tests retain decoded-record, quantization, shadow-discontinuity, background,
alpha, and above-one-radiance comparisons across primitive, material, filter,
cascade, projection, aspect, extent, and view-route variants.

Historical isolated/migration runs belong to the
[Deferred Directional Lighting plan](../../Plans/Archive/2026-08/DeferredDirectionalLighting.md)
and [Hybrid Deferred Rendering roadmap](../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md).
