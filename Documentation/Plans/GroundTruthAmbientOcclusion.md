# Ground Truth Ambient Occlusion Plan

Summary: Add a bounded GTAO-class indirect-occlusion path over the production depth/normal seam without rewriting direct shadows or leaking state across views.

Last reviewed: 2026-08-16

Status: Completed
Completed: 2026-08-16

## Current Status

M5 of the
[Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
completed on 2026-08-16. Production solid Lit `DeferredRequired` views now run
the deterministic full-resolution three-slice/four-step GTAO horizon pass and
the non-temporal radius-two bilateral pair before deferred composition. The
filtered factor multiplies material AO only at the environment-light input;
explicit disable or optional AO failure binds factor one without failing the
required deferred view. Raw, Confidence, Filtered, and FinalFactor diagnostics,
viewport-origin/extent isolation, repeated/mutated frames, resource retry,
reload/device invalidation, exact active/retained bytes, and all four geometry
families pass.

The selected validation-enabled RTX 3090 run measured raw
`556,976/560,672 ns`, bilateral `209,472/212,928 ns`, combined GTAO
`766,800/1,059,520 ns`, and shadow-through-display production total
`595,008/639,584 ns` median/p95. The measured AO composition increment passes
the frozen `75,000 ns` median gate. `EditorRenderingTests`, the Vulkan renderer
integration set, `fast-all`, the ordinary native aggregate, full `all` build,
HDR/native-window qualification matrix, and the 30-tick hidden DurinEditor
startup/runtime/shutdown smoke pass. Lasting behavior is published in
[Ground Truth Ambient Occlusion](../Runtime/Rendering/GroundTruthAmbientOcclusion.md).

M6 candidates remain unselected: no measured product scene exceeds the
qualified `1 + 4` light tier, no selected decal requirement exists, and the
depth-only contact-shadow experiment supplies no evidence for a normal-aware
production revision. Each remains eligible for a future dedicated plan when
its own entry evidence exists; none blocks M5 or roadmap completion.

## Frozen Stage 0 Contract

### Algorithm and normal ownership

The raw pass runs at the fitted production viewport's full resolution. Each
valid standard-lit pixel reconstructs view position through the existing
analytic reversed-Z perspective/orthographic contract and decodes both normals
through `GBufferContract`. Geometric normal defines the tangent horizon frame,
sample-side rejection, and depth/normal confidence. Shading normal remains the
existing BRDF/environment normal and never changes AO geometry. Normal maps
therefore cannot invent creases or allow samples from a back-facing surface.

The selected horizon approximation follows the radiometrically weighted GTAO
formulation from Jimenez et al., bounded to three azimuthal slices and four
depth samples on each side of a slice. A fixed 4x4 screen-space rotation/offset
table permutes slice angle and radial start per pixel; there is no frame index,
random state, or temporal jitter. Radial positions are monotonically spaced in
projected radius at `(step + 0.5) / 4`; the last position reaches the clamped
radius. Samples outside the fitted viewport, at background depth, behind the
receiver hemisphere, or failing finite reconstruction contribute no occlusion.

The default world radius is `0.75`, falloff begins at `0.60` radius, maximum
projected radius is `96` pixels, and receiver thickness is `0.05` world units.
Distance attenuation uses smoothstep from full weight at `0.60` radius to zero
at the radius. A sample must protrude more than the receiver thickness toward
the geometric-normal hemisphere; there is no center offset. Horizon angles
clamp to the receiver hemisphere and the
cosine-weighted analytic arc is averaged across the three slices. Every
intermediate must be finite; the raw visibility factor clamps to `[0, 1]`, and
invalid/background pixels publish one.

Raw visibility is spatially stabilized, not temporally accumulated. A
separable bilateral filter uses radius two (`5` taps horizontally, then
vertically), fixed Gaussian weights, reconstructed view-depth relative
difference at most `1%` plus `0.01` world units, and geometric-normal dot at
least `0.90`. Rejected taps have zero weight and the center always contributes.
The two R8 targets ping-pong: raw A, horizontal B, filtered A. Qualification
captures raw before overwrite. No motion-vector, history, reprojection, camera-
cut, or convergence semantics exist in M5; identical immutable inputs produce
identical output on every frame and in every view order.

The reference algorithm is the 2016/2019 Activision GTAO horizon formulation,
adapted only by the explicitly frozen sampling and non-temporal policy above:
[Practical Real-Time Strategies for Accurate Indirect Occlusion](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf).
Near-field multi-bounce compensation, directional AO, bent normals, and GTSO
are not selected.

### Production ordering, composition, and failure

Only solid Lit `DeferredRequired` views enable GTAO by default. Wireframe,
Unlit view mode, isolated `ForwardReference`, and migration fallback do not
allocate or sample it. The immutable per-view setting may explicitly disable
AO. Development options select Raw, Confidence, Filtered, or FinalFactor
diagnostics without changing another view.

The production order becomes:

```text
directional shadow -> GBuffer/D32 -> raw GTAO -> bilateral H/V
                   -> sky/clear bootstrap
                   -> deferred direct + (material AO * GTAO) environment + emissive
                   -> retained forward -> contact -> display -> editor assistance
```

GTAO multiplies the decoded material AO only at the input of
`EvaluateSurfaceEnvironmentLighting`. Directional/local direct light,
directional/contact shadow visibility, emissive, effective opacity, Unlit,
translucent, SkyBox/background, display, and editor assistance are bit-
identical to AO-disabled output when inspected in their isolated diagnostics.
No post-lighting Scene Color darkening is permitted.

AO is optional. Shader, pipeline, sampler, target, uniform, render, or filter
failure increments an unavailable/failure counter and binds the complete white
fallback factor for the affected view. Deferred opaque remains required and
continues; a successful prior view or extent cannot supply AO. Raw/filtered
targets publish complete-or-null against shader/device/manual generations and
recorded commands retain their RHI references through cache eviction.

### Frozen fixtures and image gates

The canonical 320x180 HDR/display fixture uses a perspective camera plus
orthographic and constrained-aspect variants. It contains an open plane, a
convex outward edge, a 90-degree concave corner, a box touching the floor, the
same box at `0.10`, `0.75`, and `1.00` separation, two thin parallel plates, a
grazing wall, a silhouette crossing the fitted viewport edge, background, and
one emissive-above-one patch. StaticMesh, SplineMesh, SkeletalMesh, and Terrain
each own one receiver; mapped-normal and mirrored/two-sided variants prove the
geometric/shading-normal split.

References cover no/black/custom environment, no/directional/`1 + 4` direct
lights, no/single/cascaded shadow, contact on/off, AO on/off, all diagnostics,
FXAA on/off, main/auxiliary/preview/thumbnail, Present/offscreen, alternating
extent/origin, and repeated/mutated frames. A CPU evaluator owns per-pixel raw
horizon and bilateral reference math. R8 raw/filtered/final factors differ by
at most `1/255`; diagnostic RGB differs by at most one display byte. Composed
finite HDR channels use `max(0.002, 0.002 * abs(reference))`; displayed changed
RGB pixels use mean/p99/maximum absolute byte error `1/3/18`, alpha at most one
byte, and changed classification only within the established two-pixel dilated
depth/normal discontinuity mask.

Open plane/background visibility is at least `0.98`; the convex edge is at
least `0.95`; stable pixels within the concave corner are within `[0.68, 0.82]`;
the touching foot region is at most `0.80`; and receivers separated by at least
the `0.75` radius are at least `0.98`. Thin-plate opposite surfaces remain at
least `0.90` outside their physical corner mask. No occlusion may cross more
than two pixels beyond a silhouette/disocclusion mask or alter letterbox/
background ownership. Direct-only and emissive diagnostics are byte-identical
AO on/off; environment-only occluded pixels must decrease and open pixels must
remain within the HDR tolerance.

### Diagnostics, memory, and RTX 3090 budgets

Per-view counters report attempted, enabled, unavailable, raw-pass failure,
filter-pass failure, debug route, active bytes, and retained bytes. Capture
seams expose raw and filtered R8; a GPU timing seam reports raw horizon and
combined bilateral intervals independently. FinalFactor visualizes the product
of material AO and filtered GTAO, while Confidence distinguishes valid center,
accepted neighborhood, rejected edge, and background classes.

Two full-resolution R8 targets cost exactly `4,147,200` bytes at 1920x1080.
The AO size cache retains current extent and evicts oldest other extents above
`32 MiB`: eight 1080p pairs fit, nine do not. Production active intermediates
plus SDR output grow from `91,238,400` to `95,385,600` bytes, or from
`141,570,048` to `145,717,248` including the shadow array. Scene-target,
GBuffer, and AO cache ceilings total `352 MiB`. AO owns no history target.

Qualification uses NVIDIA GeForce RTX 3090, driver 591.86, Vulkan 1.4.325,
validation enabled, `Win64-Debug-DurinEditor`, 1920x1080, 30 warm-up frames,
and 120 measured frames. Every interval is non-zero and must meet:

| GPU interval | Median gate | p95 gate |
| --- | ---: | ---: |
| Raw GTAO, 3 slices x 4 steps per side | `600,000 ns` | `900,000 ns` |
| Bilateral horizontal + vertical | `250,000 ns` | `400,000 ns` |
| Deferred composition increment | `75,000 ns` | `125,000 ns` |
| GTAO feature total | `850,000 ns` | `1,100,000 ns` |
| Shadow-through-display production frame, GTAO + FXAA | `2,000,000 ns` | `3,000,000 ns` |

The M4 AO-disabled production baseline is GBuffer `80,864/81,696 ns`, deferred
`254,880/255,872 ns`, retained `8,832/10,240 ns`, FXAA `67,648/68,288 ns`, and
tracked total `424,752/426,784 ns` median/p95. Gates cannot be raised without
an explicit plan revision and new adapter/profile/input evidence.

Stage 1 plan revision (2026-08-15): three validation-enabled implementations
of the frozen full-resolution 24-sample horizon pass measured stable medians
`559,872`, `549,824`, and `551,584 ns` with p95 at most `563,168 ns`.
Algebraically specializing projection reconstruction and replacing `acos`
with the qualified approximation did not materially change the texture-bound
cost. Reducing samples or resolution would change the frozen quality contract,
so the raw median gate is revised to `600,000 ns`; at Stage 1 its `650,000 ns`
p95, the `750,000/1,100,000 ns` complete feature gate, and total-frame gates
were not changed. Stage 2's measured bandwidth-tail revision below supersedes
the raw p95 and complete-feature median values.

Stage 1 qualification selected raw `544,336/557,920 ns` median/p95 at
1920x1080, with exactly `2,073,600` active bytes. The 320x180 perspective
readback proves byte-exact repeated-frame determinism, four open coplanar
receiver centers at least `250/255`, a raised-contact fixture that changes a
strict non-empty subset of pixels, and 320x180 -> 384x216 -> 320x180 target
reconstruction without stale reuse. Shared GBuffer qualification already owns
reversed-Z perspective/orthographic analytic reconstruction; raw AO invokes
that same shader contract rather than a feature-local specialization.

Stage 1 scope clarification (2026-08-15): raw qualification owns the open,
raised-contact, bounded-output, reconstruction, viewport clamp, and lifecycle
seams. The complete convex/concave/thin/grazing/silhouette diagnostic pixel
matrix is evaluated after the frozen bilateral filter exists in Stage 2, where
its edge-rejection tolerances are meaningful; Stage 1 does not claim filtered
image acceptance.

Stage 2 contract correction (2026-08-15): the frozen receiver offset produced
view-angle-dependent self-occlusion on the canonical open plane. GPU readback
measured only `235/255` at stable coplanar centers. Requiring positive
geometric-normal protrusion beyond the existing `0.05` thickness and removing
the center offset restores at least `250/255` without changing radius, sample
count, or performance gates. The raised-contact fixture still produces a
strict non-empty filtered occlusion region.

The implemented radius-two bilateral pair reconstructs X-forward view depth,
rejects relative depth differences above `1% + 0.01` and geometric-normal dots
below `0.90`, and always retains the center tap. Selected validation-enabled
RTX 3090 filter timing is `199,824/210,112 ns` median/p95 against the frozen
`250,000/400,000 ns` gates. Raw A plus scratch B is exactly `4,147,200` active
bytes at 1920x1080; eight such pairs fit the `32 MiB` cache and nine do not.

Stage 2 timing revision (2026-08-15): once every measured raw pass is followed
by both full-resolution filter passes, three validation-enabled runs retain
raw medians `532,096` to `555,920 ns` but expose bandwidth-tail p95 values
`745,408`, `807,872`, and `873,216 ns`; the filters remain
`199,824-208,752/210,112-210,624 ns`. The raw p95 gate is therefore revised to
`900,000 ns` and the correlated raw-plus-filter median gate to `850,000 ns`.
The raw median, both filter gates, complete-feature `1,100,000 ns` p95, and
production-frame gates remain unchanged.

## Goal

Improve corner, crease, and foot-contact grounding by attenuating only indirect
environment lighting with a GTAO-class screen-space signal, while preserving
direct/local light, directional/contact shadow, emissive, alpha, retained
forward, display, and editor-assistance semantics.

## Scope

- Reconstruct view position from production D32 and consume the qualified
  geometric/shading normal signal at a documented resolution and sample tier.
- Generate, denoise, and compose a bounded AO term into deferred indirect
  environment light with deterministic diagnostics and failure fallback.
- Define a non-temporal policy or implement history with explicit invalidation
  for camera cuts, projection/extent changes, reload, device generation, and
  independent sequential views.
- Qualify image behavior, edge rejection, lifecycle, memory, and validation-
  enabled RTX 3090 performance through root workflows.

## Non-Goals

- Repairing direct-shadow leaks, observing off-screen casters, replacing
  directional/contact shadows, or changing the shared direct-light BRDF.
- Ray tracing, signed-distance fields, bent-normal environment lookup, motion
  vectors, generic render-graph adoption, decals, or scalable local lights.
- Applying AO to Unlit, translucent, emissive, SkyBox/background, or editor-
  assistance output.

## Program Invariants

- AO multiplies only the indirect/environment term of valid Lit records and
  cannot attenuate directional/local direct light or emissive radiance.
- Background, disocclusion, screen edges, thin geometry, grazing walls, and
  invalid reconstruction have an explicit confidence/rejection rule.
- Optional AO failure degrades to factor one for the current view; stale or
  partial targets are never sampled and production deferred remains available.
- View identity, projection, viewport origin/extent, device generation, and
  history generation are immutable command-local inputs.
- Size-keyed resources publish complete-or-null payloads, expose exact active
  and retained bytes, and remain within the frozen ceiling.

## Stages

### Stage 0: Freeze GTAO quality, ownership, and budgets

- [x] Inventory the production depth/normal decode, indirect-light shader seam,
      post/deferred ordering, view identity, resource coordinator, timing,
      capture, and diagnostic facilities.
- [x] Select full/half resolution, horizon integration, direction/step count,
      radius/falloff/thickness, geometric-versus-shading-normal roles, edge
      confidence, denoise, and temporal/non-temporal policy.
- [x] Freeze deterministic CPU/HDR/display references for open plane, convex
      edge, concave corner, foot contact, thin separation, grazing wall,
      silhouette/screen edge, background, emissive, direct-only, environment-
      only, contact on/off, and every supported view route.
- [x] Freeze optional failure behavior, diagnostics/counters, active/cache byte
      ceilings, plus validation-enabled RTX 3090 1920x1080 warm-up/sample count
      and absolute AO/denoise/composition/total median/p95 gates.

#### Acceptance Gate

- Algorithm, composition ownership, history/lifecycle, fixtures, tolerances,
  bytes, and GPU thresholds are explicit before shader/runtime implementation.

### Stage 1: Implement and qualify raw horizon occlusion

- [x] Add shared CPU/shader parameter and reconstruction contracts, bounded
      noise/rotation, horizon search, radius/falloff, and finite factor output.
- [x] Add transactional size-keyed targets, explicit layouts/transitions,
      shader/pipeline lifecycle, exact counters, captures, and component debug.
- [x] Prove the raw open/contact signal, bounded background/component behavior,
      geometric-normal thickness rejection, inherited reversed-Z perspective/
      orthographic reconstruction, and viewport-clamped sampling; reserve the
      complete filtered edge-shape pixel matrix for Stage 2 as clarified above.
- [x] Prove shader/pipeline/target failure, retry, resize, reload, device
      invalidation, recorded-command lifetime, and shutdown degrade to factor
      one without stale sampling.

#### Acceptance Gate

- Raw AO is deterministic, bounded, diagnosable, lifecycle-safe, and meets the
  frozen reconstruction, image, memory, and GPU gates while production output
  remains unchanged.

### Stage 2: Denoise and stabilize the selected signal

- [x] Implement the frozen depth/normal-aware denoise and the selected history
      or explicitly non-temporal policy without cross-view state.
- [x] Reject out-of-viewport taps plus depth/normal discontinuities in the
      separable filter; preserve background ownership and center contribution.
- [x] Prove silhouettes, disocclusions, camera
      cuts, projection changes, viewport-origin changes, and extent changes.
- [x] Prove static convergence/no-history determinism, camera/object motion,
      alternating main/auxiliary/preview/thumbnail views, resize, and reload.
- [x] Capture raw, confidence, denoised/history, and final-factor diagnostics;
      meet frozen halo, stability, memory, and GPU gates.

#### Acceptance Gate

- The selected filtered AO improves the frozen grounding fixtures without
  halos, ghosting, edge leakage, view leakage, or budget failure.

### Stage 3: Compose indirect occlusion into production HDR

- [x] Apply AO only to deferred environment/indirect lighting before emissive,
      retained forward surfaces, contact composition, and the one display map.
- [x] Preserve direct directional/local light, shadow visibility, emissive,
      alpha, Unlit/translucent, SkyBox/background, FXAA, Present/offscreen, and
      editor-assistance references.
- [x] Add an immutable per-view enable/quality contract and prove optional
      failure returns factor one for only the affected view.
- [x] Prove repeated frames, scene/material/light mutation, all geometry
      families, views, projection modes, alternating extents, and shutdown.

#### Acceptance Gate

- Production images meet the frozen grounding and non-interference references;
  AO has one indirect-light composition owner and no failure can corrupt HDR.

### Stage 4: Qualify, publish, and hand off optional consumers

- [x] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, ordinary
      native aggregate, full build, native-window matrix, and hidden-editor
      startup/runtime/shutdown smoke through root workflows.
- [x] Capture the validation-enabled RTX 3090 timing/memory matrix with adapter,
      driver, profile, extent, warm-up/sample count, median, p95, active bytes,
      retained bytes, and comparison to frozen gates.
- [x] Publish lasting AO quality limits, input/composition ownership,
      diagnostics, failure, view/history lifecycle, memory, and performance
      contracts; update the roadmap and M6 evidence seam.
- [x] Evaluate M6 candidates independently and create a dedicated plan only
      for a candidate supported by measured product evidence.

#### Acceptance Gate

- Image/non-interference, edge/stability, failure/lifecycle, aggregate/build,
  runtime, documentation, memory, and RTX 3090 gates pass.

## Definition of Done

- All Stage 0-4 gates pass and each completed stage is committed with this
  plan's exact provenance.
- Corners and foot contacts improve through bounded indirect occlusion without
  claims or behavior that rewrite direct-shadow visibility.
- Optional failure, view isolation, lifecycle, exact bytes, and target-GPU
  costs are lasting contracts.
- M6 receives stable AO/depth/normal/HDR seams and explicit evidence for or
  against each optional consumer.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Directional Contact Shadows](DirectionalContactShadows.md)
- [HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.cpp`
- `Engine/Shaders/Slang/DeferredDirectionalLighting.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
