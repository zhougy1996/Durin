# Hybrid Renderer Production Rollout Plan

Summary: Promote the qualified deferred opaque slice into production, add the current local-light tier, preserve forward special-surface composition, and retire duplicate generic opaque ownership only after parity and RTX 3090 gates pass.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

M1-M3 of the
[Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
are complete. HDR Scene Color and display mapping, the four-attachment minimal
GBuffer, and the isolated deferred directional/environment/emissive/shadow
slice have lasting contracts. The validation-enabled RTX 3090 M3 fixture
measures `200,896/201,952 ns` isolated deferred and
`280,864/282,208 ns` GBuffer + deferred median/p95 at 1920x1080. All supported
geometry families, material transport, shadow filters/cascades, views,
lifecycles, memory, aggregates, full build, and editor smoke pass.

Production remains forward. Stage 0 must freeze the mixed-scene reference,
opaque/special-surface ordering, migration fallback, retirement condition, and
absolute RTX 3090 budgets before local-light or default-route implementation.

Stage 0 is complete. The selected production route reuses M1 Scene Color and
M2 depth/GBuffer without adding a display-sized production target. Shadow depth
executes first; the GBuffer clears and owns D32; a sky/clear bootstrap loads
that depth; deferred standard-lit opaque/masked pixels load and overwrite Scene
Color; retained forward unlit opaque/masked and globally sorted translucent
draws load Scene Color and depth; contact, the single display transform, and
SDR editor assistance remain last. A three-state migration contract and an
exact retirement gate prevent duplicate unrestricted opaque ownership. The
mixed `1 + 4` fixture, inherited image tolerances, byte ceilings, and absolute
RTX 3090 gates are frozen below. The current forward fixture measures
`201,664 ns` directional and `312,096 ns` for `1 + 4`, a `110,432 ns` local-
light increment; Stage 1 may now implement isolated local lighting.

Stage 1 is complete. `Lighting/SurfaceLighting.slang` now owns the unchanged
point/spot distance, inverse-square, range-window, hard/smooth cone, and PBR
orchestration. Forward calls that helper in the original four-record order;
the isolated deferred pass consumes the same unmodified 768-byte lighting
range and exposes a separate Local diagnostic. Point-only, spot-only, four-
light overlap, fifth-light overflow, invalid-record rejection, no-light, and
combined forward/deferred HDR references pass. The four-family RTX 3090 run
measures GBuffer `79,840/80,704 ns`, deferred `1 + 4`
`254,384/255,552 ns`, and combined `334,272/335,904 ns` median/p95. Shader ABI,
directional/shadow, skeletal, terrain, `fast-all`, and full-build gates pass;
production output and ownership remain forward for Stage 2.

Stage 2 is complete. Per-view `ForwardReference`,
`DeferredWithForwardFallback`, and `DeferredRequired` routes now select an
authoritative production composition without a renderer-global toggle. The
production route writes valid deferred Lit opaque/masked pixels directly into
the M1 Scene Color and directional-direct attachments, then uses dedicated
load-preserving Static/Spline, Skeletal, and Terrain pipeline variants for
retained Unlit and globally sorted translucent draws. Sky/clear bootstrap,
contact shadows, the one display transform, FXAA, and SDR editor assistance
retain their established owners. Validation-enabled Vulkan references cover
mixed opaque/Unlit/translucent output, sky/environment, contact, above-one
emissive HDR, all four view roles, Present/offscreen, every geometry family,
and injected GBuffer allocation failure. Optional migration rerenders the
whole forward view with exact output on failure; required mode returns
`RendererResourcesUnavailable` and cannot present partial work. Stage 3 may
now make the route the production default and retire unrestricted Lit forward
ownership.

Stage 3 is complete. Main window, auxiliary/offscreen viewport, camera preview,
and rendered-asset thumbnail product callers now select `DeferredRequired`;
no production caller selects migration fallback or the forward reference.
`ForwardReference` remains a value-default test-only A/B route, while the only
remaining `DeferredWithForwardFallback` caller is the injected-failure
qualification. StaticMesh, SplineMesh deformation, SkeletalMesh skinning, and
Terrain all run through required deferred ownership on consecutive frames;
retained forward submission filters opaque/masked records to Unlit and keeps
the existing cross-family translucent list. Required mixed-scene references
cover mirrored/two-sided and masked material transport, local/directional
lighting, shadow/contact, sky, views, Present/offscreen, and alternating
extents. Focused validation-enabled Vulkan suites, `fast-all`, and the full
build pass without partial output, duplicate lighting, or silent fallback.
Stage 4 may now run the final aggregate, smoke, performance, memory, and
publication gates.

Stage 4 is complete. The validation-enabled RTX 3090 production fixture
measures GBuffer `80,864/81,696 ns`, deferred `1 + 4`
`254,880/255,872 ns`, Sky/retained forward `8,832/10,240 ns`, FXAA
`67,648/68,288 ns`, and the tracked shadow-through-display frame
`424,752/426,784 ns` median/p95. Active production intermediates plus output
are exactly `91,238,400` bytes (`141,570,048` with shadow), and the production
cache ceiling remains `320 MiB`. Focused qualification/product tests,
`fast-all`, the ordinary native aggregate, full build, native-window matrix,
hidden-editor smoke, and documentation validation pass. Lasting ownership,
ordering, failure, lighting, lifecycle, memory, and performance contracts are
published; M5 now owns the stable depth/normal/indirect-light input seam.

## Frozen Stage 0 Contract

### Eligibility, ordering, and attachment ownership

Only `Lit` opaque and masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain
records are production-deferred. Every translucent draw, every `Unlit`
opaque/masked draw, SkyBox, and editor-assistance operation remains forward.
Wireframe and unsupported render modes remain on their existing forward route
and cannot silently enter the production-deferred state.

The production order is fixed:

1. Prepare one immutable view/light snapshot and render the selected
   directional-shadow candidate.
2. Clear the four GBuffer colors and D32 once, then write eligible Lit
   opaque/masked geometry. GBuffer is the sole production depth writer for
   those records.
3. Begin the Scene Color bootstrap with `RGBA16_FLOAT` Scene Color and
   `R11G11B10_FLOAT` directional direct cleared and D32 loaded. Draw the SkyBox,
   or retain `View.ClearColor` where no sky exists.
4. Load Scene Color and run the deferred full-screen evaluator. It writes only
   valid standard-lit records, leaves sky/clear background untouched, writes
   effective opacity, and writes the directional-direct term required by the
   existing contact-shadow pass.
5. Load Scene Color, directional direct, and D32. Draw retained Unlit
   opaque/masked surfaces, then the existing cross-family back-to-front
   translucent list. Their established depth, blend, and sorting state is
   unchanged; no display transform occurs here.
6. Run contact-shadow composition when enabled, then exactly one copy/FXAA
   display transform, then load the SDR result for editor assistance.

The production deferred evaluator renders directly into M1 Scene Color. The
isolated M3 `RGBA16_FLOAT` target remains qualification-only and is never copied
into production Scene Color. This avoids another full-screen copy and another
8 bytes-per-pixel production allocation. Explicit render-target layouts own
the bootstrap-clear, deferred-load, and retained-forward-load transitions;
no pass samples and attaches the same texture simultaneously.

### Migration and retirement state machine

| State | Intended stages | Required-path failure | Generic Lit opaque/masked owner |
| --- | --- | --- | --- |
| `ForwardReference` | 0-1 and A/B tests | Existing forward result | Forward only; deferred may run isolated |
| `DeferredWithForwardFallback` | 2 migration opt-in | Discard partial work, clear the authoritative targets, and rerender the complete view through forward | Deferred when complete, otherwise one whole-view forward fallback |
| `DeferredRequired` | 3-4 production default | Return `RendererResourcesUnavailable`; never present partial or stale HDR | Deferred only |

The state is copied into immutable per-view render options. It is not a
renderer-global toggle, and sequential main/auxiliary/preview/thumbnail views
may select independently. Diagnostic GBuffer/deferred captures remain isolated
and cannot replace the selected production output.

Generic Lit opaque/masked forward ownership is retired only after all four
families pass the Stage 3 image, mutation, multi-view, Present/offscreen,
lifecycle, and performance matrices; `DeferredRequired` is the default; and
the same commit removes `DeferredWithForwardFallback` from production callers.
After retirement, forward geometry entry points must filter to Unlit,
translucent, wireframe, or another explicitly named special-surface contract.
A test-only `ForwardReference` A/B route may remain, but no product path may
select it and no failure may select it implicitly.

### Frozen mixed-scene references

The canonical scene uses a perspective 1920x1080 timing view and deterministic
320x180 HDR/display readbacks. It tiles one opaque or masked receiver from each
supported family, adds a mapped-normal mirrored/two-sided StaticMesh, an
emissive-above-one masked edge, one Unlit opaque occluder, two overlapping
translucent surfaces from different families, a SkyBox/environment pair, and
clear background. Stable light IDs and authored values are:

| ID | Family | Position or direction | Color / intensity | Range / cone |
| ---: | --- | --- | --- | --- |
| 10 | Directional | direction `(0.35, 0.20, -1.0)` | `(1, 1, 1) / 3` | casts Medium single-map shadow; cascade variant is also captured |
| 20 | Point | `(-1.5, -0.75, 2.0)` | `(1, 0.15, 0.05) / 5` | range `6` |
| 21 | Spot | `(1.5, -0.75, 3.0)`, direction `(-0.15, 0.10, -1.0)` | `(0.05, 0.2, 1) / 8` | range `8`, inner/outer `25/40` degrees |
| 22 | Point | `(-0.5, 1.25, 1.5)` | `(0.1, 1, 0.2) / 3` | range `4` |
| 23 | Spot | `(1.25, 1.25, 2.5)`, direction `(0.10, -0.20, -1.0)` | `(1, 0.6, 0.1) / 6` | range `7`, inner/outer `20/45` degrees |

References include no-light, each family isolated, directional-only,
point-only, spot-only, all-four overlap, fifth-local overflow, invalid local
records, no/black/custom environment, no/single/cascaded shadow, opaque behind
translucent, Unlit in front of and behind Lit, emissive above one, masked edge,
sky versus clear background, contact on/off, FXAA on/off, perspective,
orthographic, constrained aspect, alternating extent, and every supported view
route. The selected four local records are ordered by light ID; overflow never
changes their payload.

Decoded finite HDR channels inherit the M3 bound
`max(0.002, 0.002 * abs(reference))`; opacity remains within `1/510`.
Forward/deferred displayed valid RGB pixels retain mean/p99/maximum absolute
byte-error gates `1/3/18`, with alpha at most one byte. Shadow-stable pixels and
diagnostic classes are byte-identical; changed classification is permitted
only within the existing two-pixel dilated discontinuity mask. Background and
ownership masks are exact. Translucent/Unlit isolated contributions, sorting,
contact toggles, and editor-assistance pixels are byte-identical because those
owners are retained rather than reimplemented.

### Frozen memory and RTX 3090 budgets

No new production target is permitted. At 1920x1080, M1 Scene Color/depth/
directional-direct/contact targets cost `49,766,400` bytes, the M2 GBuffer costs
`33,177,600`, and one SDR output costs `8,294,400`: the active production
extent is exactly `91,238,400` bytes. The existing directional-shadow array is
`50,331,648` backend/logical bytes, making the active renderer intermediates
plus output `141,570,048` bytes. Environment assets and mesh/material resources
are pre-existing scene inputs and are reported separately.

The production size-cache ceiling remains M1 `192 MiB` plus M2 `128 MiB`, or
`320 MiB`. During Stage 1-2 A/B, the qualification-only M3 target may retain up
to `64 MiB`, so the temporary combined ceiling is `384 MiB`; it is not part of
the default production route or the retirement-state active byte count.

Qualification uses NVIDIA GeForce RTX 3090, driver 591.86, Vulkan 1.4.325,
validation enabled, `Win64-Debug-DurinEditor`, 1920x1080, 30 warm-up frames,
and 120 measured frames. Every interval must be non-zero and meet:

| GPU interval | Median gate | p95 gate |
| --- | ---: | ---: |
| GBuffer geometry | `350,000 ns` | `500,000 ns` |
| Deferred `1 + 4` lighting into Scene Color | `450,000 ns` | `650,000 ns` |
| Sky + retained Unlit/translucent composition | `300,000 ns` | `500,000 ns` |
| Display copy / FXAA | `30,000 / 100,000 ns` | `40,000 / 120,000 ns` |
| Shadow-through-display tracked production frame, FXAA enabled | `1,350,000 ns` | `2,100,000 ns` |

The baseline evidence on this adapter is M3 GBuffer `79,968/80,768 ns` and
directional deferred `200,896/201,952 ns` median/p95. The existing forward
fixture measures `201,664 ns` directional, `312,096 ns` for `1 + 4`, and a
`110,432 ns` local increment; Medium shadow depth measures `24,928 ns` median.
M1 display evidence is copy `<=30,000/40,000 ns` and FXAA
`<=100,000/120,000 ns`. Stage 4 records fresh medians/p95s for every interval;
these gates cannot be raised without an explicit plan revision and evidence.

## Goal

Make deferred lighting the one production owner for eligible opaque and masked
StaticMesh, SplineMesh, SkeletalMesh, and Terrain surfaces, while retaining
forward translucency, unlit/special surfaces, sky, contact/display processing,
and editor assistance in their established composition domains.

## Scope

- Evaluate the existing selected local-light tier of up to four point/spot
  lights through shared attenuation and PBR helpers after the GBuffer.
- Compose qualified deferred opaque HDR into production Scene Color, then draw
  retained forward surfaces in an explicitly frozen order.
- Preserve main/auxiliary/preview/thumbnail and Present/offscreen behavior,
  diagnostics, recovery, memory accounting, and target-GPU evidence.
- Remove or narrowly product-gate the duplicate generic forward opaque route
  after production parity and fallback gates pass.

## Non-Goals

- Increasing the selected local-light count, adding tiled/clustered culling,
  local-light shadows, decals, GTAO, or revised contact shadows.
- Migrating translucency, particles, water, hair, unlit materials, skybox, or
  editor assistance into the GBuffer.
- Introducing a render graph, transient allocator, bindless resources,
  asynchronous compute, or another GPU queue.

## Program Invariants

- Material decode, BRDF, local attenuation, directional shadow, environment,
  emissive, and alpha semantics have one shared shader owner.
- A view never composes partial or stale GBuffer/deferred resources. During
  migration, required-path failure selects the explicitly retained compatible
  fallback; after retirement it reports the view unavailable.
- Translucent and special forward draws see scene-linear deferred opaque color
  and execute before the one display transform. Editor assistance remains SDR.
- Size caches publish complete-or-null payloads and report exact active and
  retained bytes. Default-route changes require frozen image and GPU budgets.
- No stage leaves two unrestricted feature-equivalent generic opaque owners.

## Stages

### Stage 0: Freeze the production rollout contract and budgets

- [x] Inventory current opaque, masked, unlit, translucent, sky, contact,
      post-process, and editor-assistance ordering for every primitive family.
- [x] Freeze one representative `1 directional + 4 local` mixed scene with
      no-light, family-isolated, overlap, translucent-over-opaque, unlit,
      emissive, shadow, environment, and background references.
- [x] Select the production Scene Color composition mechanism, GBuffer/depth
      ownership, load/store/transitions, diagnostic route, migration flag,
      failure fallback, and exact forward-opaque retirement condition.
- [x] Freeze per-view/cache byte ceilings plus validation-enabled RTX 3090
      1920x1080 warm-up/sample counts and absolute geometry, lighting,
      translucent-composition, display, and total-frame median/p95 gates.

#### Acceptance Gate

- The production route, fallback/retirement state machine, every mixed-scene
  reference, byte, and GPU threshold are explicit before implementation.

### Stage 1: Add the current local-light tier to isolated deferred lighting

- [x] Move any remaining point/spot orchestration into shared helpers without
      changing forward accumulation order or the fixed 768-byte lighting ABI.
- [x] Evaluate the selected four local records in the isolated deferred pass
      with the existing inverse-square/range-window and spot-cone semantics.
- [x] Add directional/local/environment/emissive/final diagnostics and
      directional-only, point-only, spot-only, overlap, overflow, and invalid-
      record HDR/display references across all GBuffer primitive families.
- [x] Prove injected shader/pipeline/target failures, partial-GBuffer rejection,
      missing-environment fallback, invalid-light rejection, reload, retry,
      device generation, resize, and alternating-view isolation.

#### Acceptance Gate

- Isolated deferred `1 + 4` output meets frozen parity, lifecycle, memory, and
  GPU gates while production output remains unchanged.

### Stage 2: Compose deferred opaque HDR with retained forward surfaces

- [x] Route valid deferred opaque lighting into production HDR Scene Color with
      explicit transitions and no second display transform.
- [x] Draw retained unlit, translucent, sky, and special surfaces in the frozen
      order; preserve depth, blending, sorting, contact, FXAA/display, and
      editor-assistance contracts.
- [x] Prove translucent-over-opaque, emissive above one, opacity/blend extremes,
      sky/environment separation, contact toggles, and assistance ordering for
      main, auxiliary, preview, thumbnail, Present, and offscreen views.
- [x] Keep the migration fallback independently diagnosable and prove required-
      resource failure never presents partial deferred output.

#### Acceptance Gate

- The mixed production image meets every frozen reference and fallback gate;
  retained forward surfaces compose over deferred opaque HDR exactly once.

### Stage 3: Switch every eligible primitive and retire duplicate opaque ownership

- [x] Enable production deferred opaque/masked routing for StaticMesh,
      SplineMesh, SkeletalMesh, and Terrain, including deformation, LOD,
      batching, mirrored/two-sided, mapped-normal, and masked-edge cases.
- [x] Run A/B telemetry through the frozen scene/view matrix, resolve every
      explained difference, and reject any unsupported silent fallback.
- [x] Retire generic forward opaque/masked drawing or document and narrowly
      test the explicit product requirement that retains it.
- [x] Prove repeated frames, resize, reload, scene/material/light mutation,
      multi-view ordering, Present/offscreen, and shutdown cannot duplicate,
      omit, or reuse opaque lighting.

#### Acceptance Gate

- Eligible opaque/masked surfaces have one production owner and the retained
  forward path contains only explicitly listed special-surface responsibilities.

### Stage 4: Qualify and publish the production hybrid renderer

- [x] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the ordinary
      native aggregate, full build, native-window matrix, and hidden-editor
      startup/runtime/shutdown smoke through root workflows.
- [x] Capture the validation-enabled RTX 3090 production timing/memory matrix
      with adapter, driver, profile, extent, warm-up/sample count, median, p95,
      active bytes, retained bytes, and comparison to frozen gates.
- [x] Publish lasting production ordering, ownership, shared lighting,
      fallback/retirement, diagnostics, lifecycle, memory, and performance
      contracts; update the roadmap and downstream M5 input seam.
- [x] Re-review GTAO entry requirements and activate its dedicated plan only
      after this plan's exit gate.

#### Acceptance Gate

- Image, feature, ownership, fallback, lifecycle, aggregate/full-build,
  runtime, documentation, memory, and RTX 3090 gates pass with one generic
  opaque production owner.

## Definition of Done

- All Stage 0-4 acceptance gates pass and every completed stage is committed
  with this plan's exact provenance.
- Deferred owns eligible production opaque/masked lighting including the
  current `1 + 4` tier; retained forward responsibilities are explicit.
- Duplicate generic opaque ownership is removed or justified by a narrow,
  tested product requirement.
- M5 receives stable depth, geometric/shading normal, HDR indirect-light
  composition, view, lifecycle, memory, and performance inputs.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
- [Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [HDR Scene Color and Display Mapping](../../../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
