# Ground Truth Ambient Occlusion

Summary: Defines production GTAO quality, indirect-light composition, diagnostics, failure, view lifecycle, memory, and qualification contracts.

Modules: Renderer, RenderCore

## Ownership and ordering

Ground truth ambient occlusion is an optional full-resolution screen-space
factor for solid Lit views whose opaque route is `DeferredRequired`.
`FSceneViewSettings::bEnableGroundTruthAmbientOcclusion` captures the immutable
per-view selection and defaults to enabled. Forward-reference, migration
fallback, wireframe, and Unlit views neither allocate nor sample GTAO.

Production records:

```text
directional shadow -> GBuffer/D32 -> raw GTAO -> bilateral horizontal/vertical
                   -> sky/clear bootstrap
                   -> deferred direct + environment * material AO * GTAO
                      + emissive
                   -> retained forward -> contact -> display
                   -> editor assistance
```

The filtered factor is consumed only by the deferred environment-light call.
Directional and local direct lighting, directional/contact shadow visibility,
emissive, opacity, Unlit and translucent surfaces, SkyBox/background, display,
and editor assistance do not consume it. The renderer binds an explicit white
fallback and disables the texture load when GTAO is unavailable or disabled;
it never darkens Scene Color after lighting.

## Quality contract

The raw pass reconstructs X-forward view position from production D32 through
the shared analytic perspective/orthographic contract. It decodes the GBuffer
geometric normal independently of the authored shading normal. Three
azimuthal slices use four samples per side with a fixed 4x4 pixel rotation.
The world radius is `0.75`, falloff begins at `0.60` radius, projected radius
is capped at `96` pixels, and a sample must protrude more than `0.05` world
units into the receiver's geometric-normal hemisphere. Invalid reconstruction,
background, out-of-viewport, non-finite, and rejected same-surface samples
produce no occlusion. Visibility is finite and clamped to `[0, 1]`.

Stabilization is spatial and deterministic, not temporal. A separable
radius-two bilateral filter uses five fixed Gaussian taps per direction. A
non-center tap is accepted only when it remains in the fitted viewport, both
pixels are standard Lit, reconstructed view-depth difference is at most
`1% + 0.01` world units, and geometric-normal dot is at least `0.90`. The
center always contributes. There is no history, motion vector, jitter,
reprojection, camera-cut, convergence, or cross-view state; identical immutable
inputs produce identical output regardless of prior view order.

## Diagnostics and failure

Development modes visualize Raw visibility, bilateral Confidence classes,
Filtered visibility, and the FinalFactor product of material AO and GTAO.
Diagnostics render through an isolated deferred target and then the ordinary
display transform; they do not replace production Scene Color. Raw diagnostic
mode regenerates the deterministic raw signal after filtering so the two-target
production ping-pong contract remains unchanged.

Per-view counters report attempted, enabled, unavailable, raw-pass failure,
filter-pass failure, diagnostic, active bytes, and retained bytes. Capture
seams expose raw and filtered R8 results before and after the ping-pong filter;
GPU timing seams expose raw and combined bilateral intervals.

Target, shader, pipeline, uniform, sampler, render, or filter failure disables
GTAO only for the affected view. Required deferred opaque rendering continues
with factor one. Resources publish complete-or-null against renderer device,
shader, and manual generations; resize, reload, device invalidation, retry,
explicit release, and shutdown cannot reuse stale payloads from another view
or extent. Recorded commands retain their RHI references through cache
eviction.

## Memory and RTX 3090 qualification

Raw and scratch are full-resolution `R8_UNORM` targets. They cost exactly two
bytes per pixel: `4,147,200` bytes at 1920x1080. The size cache retains the
current extent and evicts oldest other extents above `32 MiB`; eight 1080p
pairs fit and nine do not. Production scene targets, GBuffer, GTAO, and one SDR
output total `95,385,600` active bytes, or `145,717,248` bytes including the
directional-shadow array. Scene-target, GBuffer, and GTAO cache ceilings total
`352 MiB`.

Qualification uses NVIDIA GeForce RTX 3090, driver 591.86, Vulkan 1.4.325,
validation enabled, `Win64-Debug-DurinEditor`, 1920x1080, 30 warm-up frames,
and 120 measured frames. The selected production integration run measured raw
`556,976/560,672 ns`, bilateral `209,472/212,928 ns`, combined GTAO
`766,800/1,059,520 ns`, and shadow-through-display production total
`595,008/639,584 ns` median/p95. The frozen gates are:

| GPU interval | Median gate | p95 gate |
| --- | ---: | ---: |
| Raw GTAO | `600,000 ns` | `900,000 ns` |
| Bilateral horizontal + vertical | `250,000 ns` | `400,000 ns` |
| Deferred composition increment | `75,000 ns` | `125,000 ns` |
| GTAO feature total | `850,000 ns` | `1,100,000 ns` |
| Shadow-through-display, GTAO + FXAA | `2,000,000 ns` | `3,000,000 ns` |

The screen-space result cannot observe off-screen casters and does not repair
direct-shadow leaks. Shadow maps remain authoritative for direct visibility;
the separate contact-shadow path remains an opt-in near-field detail term.
