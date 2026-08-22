# Volumetric Cloud Lighting and Shadows Plan

Summary: Implement production directional scattering, self-transmittance, ambient contribution, and bounded cloud shadows.

Last reviewed: 2026-08-23

Status: Active
Completed:

## Current Status

P3 completed on 2026-08-23 and published stable half-resolution production
tiers, a full-resolution reference route, transactional history, and 4K image,
timing, and memory evidence. The existing cloud marcher already consumes the
selected prepared directional light and evaluates a minimal nested light ray,
but its `ambient + lightTransmittance` term is only the P1 form-visibility
approximation. It has no declared phase convention, no production ambient
source, and no cloud-shadow contribution on opaque receivers.

P4 is now active at Stage 0. Before implementation, the plan must freeze the
lighting equations, the single receiver representation and update policy, the
authored-versus-renderer-owned parameter boundary, named diagnostic outputs,
and image/performance gates on the existing RTX 3090 / Vulkan 1.4.325
qualification device. No P4 implementation checklist is complete yet.

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
- Cloud shadowing multiplies only the selected directional-light contribution
  on eligible opaque/masked receivers. It does not darken emissive,
  environment/ambient, clear color, sky, or the cloud's own composite, and it
  combines multiplicatively with—not instead of—geometric directional shadow.
- Stage 0 must select exactly one first-version receiver representation from a
  measured screen-space or light/world-space candidate, then freeze its format,
  extent, fitted-viewport behavior, update/invalidation policy, filtering,
  fallback identity, and retained-memory ceiling.
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

- [ ] Freeze the directional single-scattering equation, phase convention and
  parameter range, extinction units, self-transmittance sample placement,
  ambient source/scale, energy bounds, and no-light fallback against a CPU
  reference.
- [ ] Select the first-version cloud-shadow receiver representation from
  measured screen-space and light/world-space candidates; record format,
  extent, coordinate mapping, filtering, update cadence, invalidation keys,
  fallback identity, and memory ceiling.
- [ ] Freeze the integration point and algebra with existing geometric
  directional shadows for deferred and retained-forward opaque/masked
  receivers, including background, sky, emissive, Unlit, translucent, and
  disabled-feature exclusions.
- [ ] Decide whether the existing authored extinction/ambient fields are
  sufficient; name and bound any new physical-intent properties before changing
  reflection, serialization, proxy, or shader ABI.
- [ ] Freeze deterministic fixtures for light rotation/intensity/color,
  forward/back scattering, dense/thin clouds, cloud motion, ambient-only,
  self-shadow, receiver shadow, fitted viewport, camera cut, and absent/failed
  resources.
- [ ] Record per-tier CPU-reference image metrics and RTX 3090 / Vulkan 1.4.325
  median/p95 GPU, sample-work, target/history/shadow bytes, and update/reuse
  budgets before accepting an implementation.

#### Acceptance Gate

- One bounded model and one receiver representation are selected; equations,
  ABI ownership, update/failure policy, fixtures, numeric image gates, and
  performance/memory budgets are explicit enough for implementation without
  an open architectural choice.

### Stage 1: Implement production cloud lighting

- [ ] Extend immutable Engine-to-Renderer cloud parameters only with Stage 0's
  selected physical intent and preserve serialization, duplication, validation,
  publication-revision, and generic Details behavior.
- [ ] Implement the frozen phase, self-transmittance, and ambient equations in
  the CPU reference, Slang shared helper, compute route, and fragment route.
- [ ] Derive cloud history identity/invalidation from every lighting input that
  changes reconstructed radiance without invalidating stable camera motion.
- [ ] Add exact lighting work counters and named no-light/ambient/fallback
  diagnostics without exposing backend objects or editor dependencies.
- [ ] Add contract and shader tests for parameter validation, uniform layout,
  CPU/GPU algebra, compute/fragment parity, mutation, and deterministic fallback.

#### Acceptance Gate

- The selected directional light produces bounded reference-matched cloud
  radiance across the frozen fixture matrix; compute and fragment agree within
  the recorded tolerance, mutations invalidate only required history, and
  missing light/resources preserve the declared fallback.

### Stage 2: Add bounded cloud shadows to surface receivers

- [ ] Implement the selected cloud-shadow producer, target/views/sampler,
  transitions, filtering, update/reuse key, timing/capture seams, and bounded
  cache in Renderer-owned code.
- [ ] Integrate cloud visibility into the shared directional-light receiver
  semantics used by deferred and supported retained-forward opaque/masked
  surfaces while preserving geometric-shadow and contact-shadow ownership.
- [ ] Preserve fitted-viewport, forward/reversed depth, offscreen/Present,
  main/auxiliary view isolation, and no-cloud/disabled-shadow identity behavior.
- [ ] Implement complete-or-last-known-good replacement, failure injection,
  retry, resize, shader reload, manual/device invalidation, explicit release,
  and orderly shutdown.
- [ ] Add component captures proving light/cloud motion, receiver alignment,
  shadow combination, exclusions, fallback, and compute/fragment scene parity.

#### Acceptance Gate

- Eligible receivers show stable, correctly aligned cloud shadows under the
  frozen light/cloud/camera matrix; excluded terms remain unchanged, failed or
  disabled production is identity, retained memory stays within its ceiling,
  and all resources recover and release without partial scene output.

### Stage 3: Qualify the complete P4 route

- [ ] Run the frozen per-tier image sequence against the CPU/reference output
  and report error/outlier metrics for cloud radiance and receiver visibility.
- [ ] Measure complete cloud lighting plus shadow production/receiver cost,
  update versus reuse cost, sample work, active/retained bytes, and median/p95
  GPU time on the named 4K qualification device.
- [ ] Run focused contracts, shader contracts, Vulkan integration, scene Vulkan
  coverage, and qualification explicitly on inline and threaded executors.
- [ ] Run affected aggregate tests, the full build, documentation validation,
  and a bounded DurinEditor runtime smoke following repository workflows.
- [ ] Revise a tier, fallback, update policy, or budget only through a recorded
  evidence-backed decision before closing a failed gate.

#### Acceptance Gate

- Every shipped tier passes the frozen image, timing, sample-work, memory,
  mutation, fallback, recovery, and release gates on both executors; `High`
  remains production-ready or an explicit measured replacement is recorded.

### Stage 4: Publish the lasting contract and complete P4

- [ ] Publish lighting equations, selected-light ownership, shadow receiver and
  update semantics, diagnostics, budgets, fallbacks, invalidation, lifecycle,
  and qualification evidence under Runtime rendering documentation.
- [ ] Update the cloud roadmap to mark P4 complete and make P5's authoring/editor
  plan eligible only after all authored properties, debug outputs, quality tiers,
  and diagnostics are stable.
- [ ] Set this plan to `Completed` only after all prior acceptance gates and
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

- [Volumetric Cloud Rendering roadmap](../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud spatial rendering](../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Volumetric cloud temporal reconstruction](../Runtime/Rendering/VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud scene contract](../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Deferred directional lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Directional shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Implementation plan rules](AGENTS.md)
- [Build and run workflow](../Agents/BuildAndRun.md)
- [Testing workflow](../Agents/Testing.md)

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
