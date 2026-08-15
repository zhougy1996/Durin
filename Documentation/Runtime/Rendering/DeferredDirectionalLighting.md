# Deferred Directional Lighting

The M3 deferred directional path is a qualified, opt-in lighting reference over
the [minimal GBuffer](GBuffer.md). It proves that opaque and masked material
records can reproduce the existing directional, shadow, environment, emissive,
and opacity semantics without changing the authoritative forward Scene Color.
Production opaque ownership remains forward until the M4 rollout plan closes.

## Ordering and ownership

One requested view records passes in this order:

```text
directional shadow -> GBuffer -> deferred directional qualification
                   -> unchanged forward Scene Color -> contact/display
                   -> editor assistance
```

`FDeferredDirectionalLightingRenderer` owns the shader/pipeline payload and a
size-keyed `RGBA16_FLOAT` target cache. The result is isolated, never presented,
and finishes graphics-shader-readable for explicit qualification capture. A
failed or disabled route leaves forward output authoritative. Local lights,
translucency, skybox drawing, contact composition, and editor assistance are not
part of this target.

## Inputs and fixed ABI

The fragment shader binds, in order, four GBuffer attachments, D32, irradiance,
prefiltered environment, the BRDF LUT, the environment sampler, the selected
directional-shadow array and comparison sampler, one 160-byte deferred-view
uniform, and the existing 768-byte forward-lighting uniform. The lighting copy
zeros only the local-light count; its selected directional and three-cascade
shadow records remain byte-identical to forward.

The deferred-view uniform contains four projection rows, one view-to-world
matrix, clear color, inverse viewport size, diagnostic identity, and reserved
zero. Position is reconstructed analytically from D32. Constrained views use
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

`Lighting/SurfaceLighting.slang` owns the surface frame, directional BRDF call,
split-sum environment evaluation, and final directional + environment +
emissive composition. `Lighting/DirectionalShadow.slang` remains the sole
shadow receiver implementation for forward and deferred. The GBuffer changes
transport precision only; it does not select another material or lighting
model.

Valid standard-lit pixels write scene-linear RGB and decoded effective opacity.
Invalid flags, failed reconstruction, and background depth retain immutable
`View.ClearColor`. Unlit and translucent surfaces remain excluded and continue
through their established forward owners.

## Diagnostics and lifecycle

Per-view counters report enabled, unavailable, failed, and diagnostic routes
plus exact output bytes. Capture and GPU-timing seams are installed only by
qualification callers. Component modes expose decoded material, directional,
environment, emissive, alpha, and final HDR terms; directional-shadow
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

Forward/deferred HDR and displayed references pass the frozen decoded-record,
quantization, shadow-discontinuity, background, alpha, and above-one-radiance
gates across primitive, material, filter, cascade, projection, aspect, extent,
and view-route matrices.
