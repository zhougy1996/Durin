# Ground Truth Ambient Occlusion

Summary: Defines production GTAO quality, mapping, resolve, indirect-light composition, diagnostics, failure, memory, lifecycle, and qualification contracts.

Modules: Renderer, RenderCore

## Ownership and ordering

Ground truth ambient occlusion is an optional deterministic screen-space factor
for production solid Lit views with required deferred opaque ownership.
`FSceneViewSettings::AmbientOcclusion.bEnabled` captures the immutable
per-view enable selection and defaults to enabled. The separate immutable
`FSceneViewSettings::AmbientOcclusion.Quality` selects `HalfResolution`, the
production default, or `FullResolution`, the quality reference. Forward-reference,
migration fallback, wireframe, and Unlit views neither allocate nor sample
GTAO. The viewport View menu exposes the toggle and quality under
`Post Processing > GTAO`; the submenu contains an `Enabled` checkbox and the
mutually exclusive quality choices.

Production half resolution records:

```text
directional shadow -> GBuffer/D32 -> representative selector -> half raw GTAO
                   -> half bilateral horizontal/vertical -> full resolve
                   -> sky/clear bootstrap
                   -> deferred direct + environment * material AO * GTAO
                      + emissive
                   -> retained forward -> contact -> display
                   -> editor assistance
```

The full-resolution resolved factor is consumed only by the deferred
environment-light call. Directional and local direct lighting,
directional/contact shadow visibility, emissive, opacity, Unlit and translucent
surfaces, SkyBox/background, display, and editor assistance do not consume it.
The renderer binds an explicit white fallback and disables the texture load
when GTAO is unavailable or disabled; it never darkens Scene Color after
lighting.

## Quality, mapping, and resolve contract

Full resolution retains the established reference path. Its raw pass
reconstructs X-forward view position from production D32 through the shared
analytic perspective/orthographic contract and decodes the GBuffer geometric
normal independently of the authored shading normal. Three azimuthal slices
use four samples per side with fixed deterministic noise. The world radius is
`0.75`, falloff begins at `0.60` radius, projected radius is capped at `96`
pixels, and a sample must protrude more than `0.05` world units into the
receiver's geometric-normal hemisphere.

Half resolution ceil-divides the owning target on each axis. A fitted full
rectangle `[origin, origin + extent)` maps to
`[floor(origin / 2), ceil((origin + extent) / 2))`; integer viewport and
scissor state restrict selector, raw, and filter work to that half-open region.
Partial 2x2 blocks inspect only standard-Lit pixels inside the fitted full
view. The selector is `R8_UNORM`: zero is invalid and values one through four
identify the row-major full pixel. The nearest valid reconstructed view-depth
candidate wins, with row-major order breaking equal-depth ties.

Public RHI cannot read an attachment being written by the same draw, so the
half raw interval first publishes the compact selector in a half-resolution
pass, then runs the same three-slice/four-step horizon evaluation using
selector-directed full-resolution GBuffer samples. The world radius remains
`0.75`; the projected cap is `48` reduced texels. Noise derives from stable
reduced coordinates. This two-draw raw phase avoids rescanning four candidates
for every horizon tap and is part of the qualified production contract.

The separable radius-two bilateral pair runs in the reduced domain with five
fixed Gaussian taps per direction. Center and tap depth/geometric-normal reads
are redirected through the immutable selector. Invalid, out-of-view,
non-standard-Lit, failed-depth, or failed-normal taps receive zero neighbor
weight while the valid center remains authoritative. The acceptance thresholds
remain a `1% + 0.01` view-depth difference and `0.90` geometric-normal dot.

A dedicated full-resolution resolve considers the four nearest reduced
texels. It combines bilinear spatial weights with those depth and normal rules,
normalizes only over accepted candidates, and writes white when none match.
Background and non-standard-Lit destinations also write white. Deferred
production always performs an exact full-pixel load from this resolved R8
target; it never samples the reduced texture directly.

Invalid reconstruction, background, out-of-viewport, non-finite, and rejected
same-surface samples produce no occlusion. Visibility is finite and clamped to
`[0, 1]`. There is no history, motion vector, jitter, reprojection, camera-cut,
convergence, or cross-view state; identical immutable inputs produce identical
output regardless of prior view order.

## Diagnostics and failure

Development modes visualize native-domain Raw visibility, bilateral Confidence
classes, native-domain Filtered visibility, and the full-resolution FinalFactor
product of material AO and resolved GTAO. Diagnostics render through an
isolated deferred target and then the ordinary display transform; they do not
replace production Scene Color. Raw diagnostic mode regenerates the
deterministic native raw signal after filtering while retaining the production
filtered and resolved outputs.

Per-view counters report attempted, enabled, selected half/full quality,
unavailable, raw-pass failure, filter-pass failure, resolve-pass failure,
diagnostic, active bytes, and retained bytes. Capture seams expose native raw
and filtered R8 results. GPU timing seams expose raw, combined bilateral,
resolve, and complete-feature intervals.

Target, shader, pipeline, uniform, sampler, selector, render, filter, or resolve
failure disables GTAO only for the affected view. It never silently falls back
from half to full resolution. Required deferred opaque rendering continues
with factor one. Resources publish complete-or-null against renderer device,
shader, and manual generations. Resize, quality changes, reload, device
invalidation, retry, explicit release, cache eviction, recorded-command
lifetime, and shutdown cannot reuse stale payloads from another quality, view,
or extent.

## Memory and RTX 3090 qualification

Full resolution retains two full-resolution `R8_UNORM` targets and costs two
bytes per pixel: `4,147,200` bytes at 1920x1080. Half resolution owns raw,
scratch, and selector at `ceil(W/2) x ceil(H/2)`, plus one full-resolution
resolved R8 target. Its exact formula is
`3 * ceil(W/2) * ceil(H/2) + W * H`, or `3,628,800` bytes at 1920x1080. Quality
is part of the cache key. The cache retains the current entry and evicts oldest
other extent/quality entries above `32 MiB`; publication remains transactional.

Qualification uses NVIDIA GeForce RTX 3090, driver 591.86, Vulkan 1.4.325,
validation enabled, `Win64-Debug-DurinEditor`, threaded RHI execution,
1920x1080, 30 warm-up frames, and 120 measured frames. The 2026-08-16
same-process run measured full feature `681,808/697,248 ns`, half feature
`397,568/400,032 ns`, half resolve `116,416/117,600 ns`, and
shadow-through-display production total `468,672/483,552 ns` median/p95 with
FXAA enabled and editor assistance disabled. Half median was `58.3%` of full.
Inline RHI execution also passes the correctness and lifecycle matrix;
production p95 gates belong to the normal threaded mode.

The frozen gates are:

| GPU interval | Median gate | p95 gate |
| --- | ---: | ---: |
| Full reference feature | `850,000 ns` | `1,100,000 ns` |
| Half feature | `600,000 ns` | `900,000 ns` |
| Half resolve | `150,000 ns` | `250,000 ns` |
| Deferred composition increment | `75,000 ns` | `125,000 ns` |
| Shadow-through-display, GTAO + FXAA | `2,000,000 ns` | `3,000,000 ns` |

Half feature median must additionally be at most `65%` of the same-run full
feature median. All image, isolation, lifecycle, memory, and timing gates
passed, so half resolution is the shipped default while full resolution
remains the opt-in reference.

The screen-space result cannot observe off-screen casters and does not repair
direct-shadow leaks. Shadow maps remain authoritative for direct visibility;
the separate contact-shadow path remains an opt-in near-field detail term.
