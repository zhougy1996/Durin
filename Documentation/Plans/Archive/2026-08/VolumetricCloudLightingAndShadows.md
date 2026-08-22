# Volumetric Cloud Lighting and Shadows Plan

Summary: Implement production directional scattering, self-transmittance, ambient contribution, and bounded cloud shadows.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

P3 completed on 2026-08-23 and published stable half-resolution production
tiers, a full-resolution reference route, transactional history, and 4K image,
timing, and memory evidence. The existing cloud marcher already consumes the
selected prepared directional light and evaluates a minimal nested light ray,
but its `ambient + lightTransmittance` term is only the P1 form-visibility
approximation. It has no declared phase convention, no production ambient
source, and no cloud-shadow contribution on opaque receivers.

P4 completed on 2026-08-23. It selected and implemented a fixed first-version
Henyey-Greenstein-relative phase response, the existing authored optical
properties, and one full-resolution screen-space `R8_UNORM` receiver-visibility
target regenerated per eligible Lit view. The target traces the same density
field from reconstructed opaque receiver positions toward the prepared
directional light and multiplies only the surface directional term. The frozen
fixtures pass the inline and threaded RTX 3090 / Vulkan 1.4.325 image, timing,
work, memory, fallback, recovery, and release gates. The lasting runtime
contract is published and P5 is now the next eligible roadmap milestone.

## Goal

Deliver deterministic production lighting for the active global cloud layer:
directional single scattering with bounded self-transmittance, an explicit
ambient/sky approximation, and one bounded cloud-shadow path that modulates
the selected directional light on supported opaque receivers without changing
existing light or geometric-shadow ownership.

## Scope

- Consume the same stable prepared directional-light selection used by surface
  lighting, including direction, scene-linear color/intensity, enabled state,
  and deterministic fallback when no eligible light exists.
- Define the cloud phase, extinction, self-transmittance, ambient, premultiplied
  radiance, and cutoff conventions shared by CPU reference, compute, and
  fragment paths.
- Add the minimum reflected physical-intent properties justified by the frozen
  model; keep sampling counts, target extents, update cadence, and cache policy
  Renderer-owned.
- Produce one Renderer-owned cloud-shadow representation, integrate it into
  the established directional surface-light receiver path, and preserve the
  existing geometric directional-shadow result.
- Define invalidation/update behavior for light, cloud, camera/receiver,
  quality, viewport, scene, shader, and device changes.
- Add lighting/shadow component diagnostics, route/failure reasons, work and
  memory counters, deterministic captures, and complete-route GPU timing.
- Qualify compute and forced-fragment cloud routes, forward/hybrid scene routes,
  supported depth conventions, fitted/offscreen/Present views, both command
  executors, recovery, release, and editor runtime smoke.

## Non-Goals

- Sky-atmosphere LUTs, aerial perspective, multiple scattering, local-light
  volumetrics, volumetric fog, local cloud volumes, or multiple cloud layers.
- Replacing directional-light scene ownership, prepared-light selection,
  cascaded geometric shadows, contact shadows, or surface BRDF ownership.
- A general-purpose volumetric shadow framework, async compute, or Render Graph
  migration.
- Editor quality controls, specialized Details layout, presets, previews,
  procedural generation, and persistent debug-view UI; those remain P5.
- Final cross-feature production closure, long-duration/multi-view soak, or
  roadmap completion; those remain P6.

## Design Decisions and Invariants

- `FPreparedLightView::Directional.front()` remains the single selected light
  for both clouds and surfaces. Cloud code cannot independently select a light
  or read reflected components on the render thread.
- Cloud radiance remains scene-linear premultiplied RGB with transmittance in
  alpha, and composition remains `Cloud.rgb + Cloud.a * Scene.rgb`.
- The P3 `Performance`, `High`, `Epic`, and `Reference` tiers remain the shipped
  quality identities and `High` remains the default. P4 may add tier-owned
  lighting/shadow work only after Stage 0 records exact budgets; it cannot
  serialize dispatch internals into cloud content.
- Compute and fragment producers implement identical lighting algebra. Missing
  optional lighting/shadow resources select an explicit bounded fallback and
  never fail an otherwise renderable view.
- Cloud self-transmittance is evaluated from the same density field and world
  coordinate model as the view ray. Stage 0 must select and record the phase
  function, parameter domains, ambient source, and energy bounds before shader
  or component ABI changes.
- The selected phase is the Henyey-Greenstein response relative to isotropic,
  `P(cosTheta) = (1-g^2) / (1+g^2-2g*cosTheta)^(3/2)`, with Renderer-owned
  fixed `g = 0.35`. Direct radiance is
  `Tview * (1-Tstep) * P * Tlight * LightRadiance`; ambient radiance is
  `Tview * (1-Tstep) * Ambient * AmbientRadiance`. Both remain finite and use
  the existing extinction and cutoff units.
- Cloud shadowing multiplies only the selected directional-light contribution
  on eligible opaque/masked receivers. It does not darken emissive,
  environment/ambient, clear color, sky, or the cloud's own composite, and it
  combines multiplicatively with—not instead of—geometric directional shadow.
- Stage 0 must select exactly one first-version receiver representation from a
  measured screen-space or light/world-space candidate, then freeze its format,
  extent, fitted-viewport behavior, update/invalidation policy, filtering,
  fallback identity, and retained-memory ceiling.
- The selected receiver representation is a full-output-resolution,
  viewport-fitted `R8_UNORM` screen-space visibility target. Opaque depth
  reconstructs the receiver, 4/6/8/8 midpoint density samples are traced to the
  cloud-slab exit for `Performance`/`High`/`Epic`/`Reference`, and the result is
  `exp(-opticalDepth * LightExtinction)`. It is regenerated for every eligible
  Lit view; no cross-frame reuse or camera-dependent cache key exists.
- Fragment and compute shadow targets each retain at most `16 MiB`; at 4K one
  target is 8,294,400 bytes and retaining both route families is 16,588,800
  bytes. The complete cloud retained ceiling is `224 MiB`.
- All P4 resources remain Renderer-owned, use complete-or-last-known-good
  publication, participate in resource-coordinator generations, and release on
  manual/device invalidation and shutdown without a device-idle wait.
- Diagnostic modes and qualification capture/timing seams are renderer inputs,
  not editor persistence. P5 may present them without redefining their meaning.

## Current Foundations and Gaps

| Area | Existing foundation | P4 gap |
| --- | --- | --- |
| Light selection | Prepared views deterministically select directional lights; cloud preparation already copies the first selected light direction and scene-linear color/intensity. | Formalize disabled/missing-light behavior and prove cloud/surface selection parity under mutation. |
| Cloud lighting | CPU, compute, and fragment marchers share extinction, a bounded nested light ray, ambient scalar, and premultiplied output. | Replace the minimal visibility approximation with a frozen phase/self-transmittance/ambient contract and parity evidence. |
| Surface receivers | Forward and deferred lighting share directional-light and geometric-shadow semantics; contact visibility and GTAO have explicit ownership. | Add a cloud visibility factor at the shared directional receiver boundary without affecting other lighting terms. |
| Reconstruction | P3 provides four stable tiers, half-resolution temporal reconstruction, depth-aware composition, diagnostics, and 4K reference comparison. | Define how lighting/shadow mutation invalidates history and how added work changes per-tier timing and image gates. |
| Resource lifecycle | Renderer resource slots, explicit transitions, bounded caches, retry, invalidation, and release are established. | Select the shadow representation, transitions, cache ceiling, failure identity, and update/reuse rules. |
| Authoring | The reflected cloud component persists physical density/extinction/ambient intent and generic eligibility diagnostics. | Add only parameters required by the selected production model; defer specialized presentation and presets to P5. |

## Implementation Stages

### Stage 0: Freeze lighting, shadow, and qualification contracts

- [x] Freeze the directional single-scattering equation, phase convention and
  parameter range, extinction units, self-transmittance sample placement,
  ambient source/scale, energy bounds, and no-light fallback against a CPU
  reference.
- [x] Select the first-version cloud-shadow receiver representation from
  measured screen-space and light/world-space candidates; record format,
  extent, coordinate mapping, filtering, update cadence, invalidation keys,
  fallback identity, and memory ceiling.
- [x] Freeze the integration point and algebra with existing geometric
  directional shadows for deferred and retained-forward opaque/masked
  receivers, including background, sky, emissive, Unlit, translucent, and
  disabled-feature exclusions.
- [x] Decide whether the existing authored extinction/ambient fields are
  sufficient; name and bound any new physical-intent properties before changing
  reflection, serialization, proxy, or shader ABI.
- [x] Freeze deterministic fixtures for light rotation/intensity/color,
  forward/back scattering, dense/thin clouds, cloud motion, ambient-only,
  self-shadow, receiver shadow, fitted viewport, camera cut, and absent/failed
  resources.
- [x] Record per-tier CPU-reference image metrics and RTX 3090 / Vulkan 1.4.325
  median/p95 GPU, sample-work, target/history/shadow bytes, and update/reuse
  budgets before accepting an implementation.

The frozen fixture uses thin and dense imported volumes over planar and curved
opaque receivers, light elevations 15/45/80 degrees, neutral/red/blue light,
0/0.5/2 intensity, static/translation/rotation/cut camera sequences,
ambient-only and disabled-light cases, fitted odd extents, both depth
conventions, and injected shader/target/pipeline failure. Against the CPU
reference, final linear RGB mean error remains bounded by the P3
0.08/0.06/0.04 production-tier gates. Complete 4K median/p95 budgets are
20/32, 26/40, 32/48, and 60/80 ms for
`Performance`/`High`/`Epic`/`Reference`; shadow-only median/p95 budgets are
4/32, 6/32, 8/32, and 12/32 ms. Repeated inline calibration kept shadow
medians below 0.9 ms but produced shared-queue p95 observations from 9.5 to
25.5 ms; the common p95 gate bounds that scheduling tail without weakening the
tier-specific median work gate.
Complete retained cloud memory must remain below
224 MiB, and every report separates shadow update cost and bytes; reuse is not
a shipped P4 route.

During Stage 3, the proposed standalone CPU visibility-image gate was replaced
before closure by a stronger route-boundary check that matches the shipped
quantized representation: a non-identity receiver fixture reads back compute
and forced-fragment `R8_UNORM` output and requires every fitted-view pixel to
agree within one quantization level. The shader reflection/layout contract,
analytic `[0,1]` exponential bound, identity fallback, exact sample work, and
scene integration remain separately asserted. This avoids maintaining a second
CPU implementation of depth reconstruction as a false oracle while directly
testing the two production producers.

The accepted 2026-08-23 inline/threaded 4K runs reported stable production-tier
mean RGB errors of 0.0348179/0.0162224/0.0031851 and outlier fractions of
1.31761%/0.76421%/0.0430464%. Production tiers retained 149,299,200 bytes and
`Reference` retained 215,654,400 bytes. Inline shadow medians were
0.694/0.912/1.141/1.198 ms; threaded medians were
0.524/0.761/0.959/0.829 ms. All declared median, p95, work, image, and memory
gates passed.

#### Acceptance Gate

- One bounded model and one receiver representation are selected; equations,
  ABI ownership, update/failure policy, fixtures, numeric image gates, and
  performance/memory budgets are explicit enough for implementation without
  an open architectural choice.

### Stage 1: Implement production cloud lighting

- [x] Extend immutable Engine-to-Renderer cloud parameters only with Stage 0's
  selected physical intent and preserve serialization, duplication, validation,
  publication-revision, and generic Details behavior.
- [x] Implement the frozen phase, self-transmittance, and ambient equations in
  the CPU reference, Slang shared helper, compute route, and fragment route.
- [x] Derive cloud history identity/invalidation from every lighting input that
  changes reconstructed radiance without invalidating stable camera motion.
- [x] Add exact lighting work counters and named no-light/ambient/fallback
  diagnostics without exposing backend objects or editor dependencies.
- [x] Add contract and shader tests for parameter validation, uniform layout,
  CPU/GPU algebra, compute/fragment parity, mutation, and deterministic fallback.

#### Acceptance Gate

- The selected directional light produces bounded reference-matched cloud
  radiance across the frozen fixture matrix; compute and fragment agree within
  the recorded tolerance, mutations invalidate only required history, and
  missing light/resources preserve the declared fallback.

### Stage 2: Add bounded cloud shadows to surface receivers

- [x] Implement the selected cloud-shadow producer, target/views/sampler,
  transitions, filtering, update/reuse key, timing/capture seams, and bounded
  cache in Renderer-owned code.
- [x] Integrate cloud visibility into the shared directional-light receiver
  semantics used by deferred and supported retained-forward opaque/masked
  surfaces while preserving geometric-shadow and contact-shadow ownership.
- [x] Preserve fitted-viewport, forward/reversed depth, offscreen/Present,
  main/auxiliary view isolation, and no-cloud/disabled-shadow identity behavior.
- [x] Implement complete-or-last-known-good replacement, failure injection,
  retry, resize, shader reload, manual/device invalidation, explicit release,
  and orderly shutdown.
- [x] Add component captures proving light/cloud motion, receiver alignment,
  shadow combination, exclusions, fallback, and compute/fragment scene parity.

#### Acceptance Gate

- Eligible receivers show stable, correctly aligned cloud shadows under the
  frozen light/cloud/camera matrix; excluded terms remain unchanged, failed or
  disabled production is identity, retained memory stays within its ceiling,
  and all resources recover and release without partial scene output.

### Stage 3: Qualify the complete P4 route

- [x] Run the frozen per-tier image sequence against the reference output and
  report cloud-radiance error/outlier metrics plus non-identity one-R8-level
  compute/fragment receiver-visibility parity.
- [x] Measure complete cloud lighting plus shadow production/receiver cost,
  update versus reuse cost, sample work, active/retained bytes, and median/p95
  GPU time on the named 4K qualification device.
- [x] Run focused contracts, shader contracts, Vulkan integration, scene Vulkan
  coverage, and qualification explicitly on inline and threaded executors.
- [x] Run affected aggregate tests, the full build, documentation validation,
  and a bounded DurinEditor runtime smoke following repository workflows.
- [x] Revise a tier, fallback, update policy, or budget only through a recorded
  evidence-backed decision before closing a failed gate.

#### Acceptance Gate

- Every shipped tier passes the frozen image, timing, sample-work, memory,
  mutation, fallback, recovery, and release gates on both executors; `High`
  remains production-ready or an explicit measured replacement is recorded.

### Stage 4: Publish the lasting contract and complete P4

- [x] Publish lighting equations, selected-light ownership, shadow receiver and
  update semantics, diagnostics, budgets, fallbacks, invalidation, lifecycle,
  and qualification evidence under Runtime rendering documentation.
- [x] Update the cloud roadmap to mark P4 complete and make P5's authoring/editor
  plan eligible only after all authored properties, debug outputs, quality tiers,
  and diagnostics are stable.
- [x] Set this plan to `Completed` only after all prior acceptance gates and
  repository validation pass.

#### Acceptance Gate

- Runtime documentation is authoritative, P4 evidence is reproducible, no
  required checklist remains open, and the roadmap accurately exposes P5 as
  the next eligible milestone.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Selected directional light -> cloud | Stable selected-light identity; direction/intensity/color/enable mutation; no-light fallback; CPU/compute/fragment agreement. |
| Density -> lit cloud radiance | Thin/dense, forward/back scattering, self-transmittance, ambient-only, extinction/cutoff bounds, finite HDR output, and per-tier reference error. |
| Cloud/light -> shadow representation | Coordinate alignment, update/reuse/invalidation, fitted/odd extents, camera/light/cloud motion, filtering, target format, work, and retained bytes. |
| Cloud shadow -> surface lighting | Deferred and retained-forward opaque/masked receivers, geometric-shadow multiplication, background/sky/emissive/Unlit/translucent exclusions, forward/reversed depth, and no-cloud identity. |
| Temporal history -> lighting mutation | Stable camera motion remains eligible; radiance-changing light/cloud policy mutations reject history; failed frames retain last-known-good state. |
| Resource lifecycle -> recovery | Shader/target/pipeline/view/sampler failure, retry, resize, reload, manual/device invalidation, queued commands, release, and shutdown. |
| Quality -> complete cost | Four tiers at 4K report median/p95 lighting-plus-shadow GPU time, update/reuse cost, sample structure, image metrics, target/history/shadow bytes, and fallback route. |
| Scene route -> final image | Compute/fragment, inline/threaded, hybrid/forward-supported, fitted/offscreen/Present, main/auxiliary, post-process continuity, and editor runtime smoke. |

## Definition of Done

- Directional phase response, self-transmittance, ambient contribution, and
  receiver cloud shadows follow one documented bounded model in CPU reference,
  compute, fragment, deferred, and supported retained-forward routes.
- Light/cloud mutations, temporal history, shadow reuse, failure, recovery,
  invalidation, and release are deterministic and observable through named
  diagnostics and exact work/memory counters.
- The frozen per-tier image and 4K GPU/memory gates pass on RTX 3090 / Vulkan
  1.4.325 for inline and threaded execution, with focused, aggregate, full-build,
  documentation, and Editor-smoke validation complete.
- Lasting runtime behavior is published, the roadmap marks P4 complete, and P5
  can consume stable properties, quality tiers, diagnostics, and debug outputs
  without redefining runtime semantics.

## Deferred Follow-ups

- P5 owns quality selection UI, specialized cloud Details, presets,
  volume/cloud previews, persistent diagnostic presentation, and any richer
  source adapter or procedural generation justified by production assets.
- P6 owns cross-plan production qualification, long-duration and multi-view
  closure, combined reload/invalidation sequences, package/cook closure, and
  roadmap completion.
- Sky atmosphere, aerial perspective, multiple scattering, local lights,
  volumetric fog, local/multiple cloud layers, async compute, and Render Graph
  remain outside this roadmap milestone.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud spatial rendering](../../../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Volumetric cloud temporal reconstruction](../../../Runtime/Rendering/VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Deferred directional lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Directional shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Renderer resource recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Implementation plan rules](../../AGENTS.md)
- [Build and run workflow](../../../Agents/BuildAndRun.md)
- [Testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/VolumetricCloudSceneProxy.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudScenePreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudScenePreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudSpatialRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudSpatialRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.h`
- `Engine/Shaders/Slang/VolumetricCloud.slang`
- `Engine/Shaders/Slang/Lighting/SurfaceLighting.slang`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp`
