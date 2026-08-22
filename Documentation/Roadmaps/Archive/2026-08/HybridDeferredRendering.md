# Hybrid Deferred Rendering Roadmap

Summary: Evolve the current forward renderer into an HDR-first hybrid renderer with deferred opaque lighting, retained forward special-surface paths, and measured screen-space grounding.

Last reviewed: 2026-08-16

Status: Archived
Completed: 2026-08-16

## Current Status

Required milestones M1-M5 completed on 2026-08-16. Scene lighting remains HDR
until one display transform; opaque and masked Lit surfaces have one production
deferred owner for the qualified directional plus four-local-light tier; Unlit,
translucent, specialized, and editor-assistance surfaces retain their explicit
forward or display-domain owners. The minimal four-attachment GBuffer carries
independent shading and geometric normals, while deferred lighting shares the
forward BRDF, environment, attenuation, and shadow facilities.

The completed
[Ground Truth Ambient Occlusion](../../../Plans/Archive/2026-08/GroundTruthAmbientOcclusion.md) plan
closes M5 with deterministic full-resolution horizon occlusion and non-temporal
bilateral stabilization. GTAO multiplies only material AO at the deferred
environment-light input, degrades optional failure to factor one per view, and
passes image, diagnostic, lifecycle, memory, aggregate/runtime, and
validation-enabled RTX 3090 gates. Its selected run measured raw
`556,976/560,672 ns`, bilateral `209,472/212,928 ns`, combined GTAO
`766,800/1,059,520 ns`, and production total `595,008/639,584 ns` median/p95
at 1920x1080.

The later deferred-contact plan selected one M6 consumer after this roadmap
completed. Contact shadows now use GBuffer receiver identity and geometric
normal, produce optional single-channel visibility before deferred lighting,
and no longer retain post-scene HDR subtraction targets. Scalable lights and
decals still have no selected product requirement.

## Outcome

- Scene lighting and scene-domain effects retain HDR radiance until one
  explicit display transform writes the existing SDR outputs.
- Opaque and masked geometry use one measured deferred lighting path once it
  reaches parity; translucent and specialized surfaces use retained forward
  paths over the same HDR scene color.
- Forward and deferred lighting consume shared material decode, BRDF,
  environment, light attenuation, and shadow functions rather than diverging
  visual models.
- The geometry buffer exposes only evidence-backed attributes and a stable
  geometric-surface signal needed by selected consumers.
- Wall-corner grounding is owned primarily by a bounded GTAO-class ambient
  occlusion pass. Contact shadows remain optional fine detail and become
  normal-aware only after the required depth/normal inputs exist.
- Every default-path change is gated by deterministic images, runtime
  lifecycle behavior, memory accounting, and RTX 3090 GPU evidence.

## Scope

- HDR scene-color storage, manual exposure, tone mapping, SDR output, FXAA
  ordering, and editor/offscreen consistency.
- A minimal geometry-buffer contract for opaque and masked StaticMesh,
  SkeletalMesh, SplineMesh, and Terrain draws.
- Deferred directional lighting and the currently supported local-light tier.
- Shared forward/deferred material and lighting functions.
- Migration policy, diagnostic modes, A/B comparison, resource recovery, and
  target-GPU qualification.
- A depth/normal-aware ambient-occlusion foundation for stable corner and
  contact grounding.
- Evidence-gated extensions for more lights, decals, and revised contact
  shadows.

## Non-Goals

- Replacing all forward rendering or routing alpha-blended surfaces through a
  conventional deferred geometry buffer.
- Starting deferred rendering solely to hide shadow-map light leaks, defective
  geometry, or the current contact-shadow artifact.
- Requiring a render graph, transient allocator, asynchronous compute, bindless
  resources, or a second GPU queue before the first deferred vertical slice.
- HDR monitor output, PQ/HLG transfer functions, wide-gamut output, automatic
  exposure, bloom, or color grading in the first milestone.
- Ray tracing, virtualized geometry, mesh shaders, or a visibility-buffer
  renderer.
- Multi-view or perspective-coverage contact-shadow tracing. The roadmap does
  not spend that cost to compensate for screen-space visibility limits.
- Expanding the supported light count without a separate measured entry gate.

## Program Decisions and Invariants

### Hybrid path ownership

- Opaque and masked draws are the deferred migration domain. During
  qualification they may render through forward and deferred A/B fixtures, but
  the production endpoint has one generic opaque owner.
- Translucent and specialized forward surfaces composite into the HDR scene
  target after deferred opaque lighting and before display mapping. They do not
  sample an already tone-mapped scene color as their lighting destination.
- Unlit and editor-assistance surfaces keep their established semantic owners.
  Editor grid, gizmos, icons, and overlay lines remain after display mapping
  and anti-aliasing where their crisp SDR appearance is intentional.

### Shared shading contract

- Material decode, tangent-space normal application, Cook-Torrance BRDF,
  environment lighting, local-light attenuation, and directional-shadow
  sampling are shared shader facilities. A deferred shader may change data
  transport, not the selected lighting equations.
- The geometry-buffer plan must define how emissive, ambient occlusion,
  shading normal, geometric-surface identity, material class, and alpha-related
  behavior cross the pass boundary. It may not infer a layout from unused
  attachment channels.
- A stable geometric normal or an equivalently reliable geometric-surface
  signal must be available independently of authored normal maps before a
  normal-aware contact-shadow revision can become supported.

### Sequencing and resource ownership

- HDR Scene Color and display mapping land before a production GBuffer. A
  deferred base pass must never be built around an LDR lighting destination.
- HDR Stage 0 owns the explicit ordering decision with Compute FXAA because
  both plans modify `FPostProcessRenderer`, size-keyed intermediates, output
  ordering, and FXAA color semantics.
- The first deferred slice uses explicit Renderer-owned targets and transitions.
  A later render-graph plan requires independent complexity or lifetime
  evidence.
- Size-keyed caches have byte accounting and bounded retention. Attachment
  count or cache-entry count alone is not accepted as a memory budget.

### Quality roles

- Shadow maps remain the authoritative direct-visibility solution. GTAO may
  darken indirect/environment lighting at corners but does not rewrite direct
  shadow visibility.
- Contact shadows remain a short, bounded, optional detail term. They do not
  guarantee off-screen casters, fully repair main-shadow leaks, or conceal
  geometry gaps.
- Contact visibility stays default-off, consumes standard-Lit receiver identity
  plus geometric normal, and uses bounded grazing confidence. Deferred inputs
  improve ownership and rejection but do not provide off-screen or multilayer
  coverage automatically.

### Rollout and fallback

- NVIDIA GeForce RTX 3090 at 1920x1080 is the qualification adapter from
  2026-08-15. Each child plan freezes absolute GPU thresholds before later
  implementation changes are evaluated. Earlier GTX 1060 measurements remain
  historical evidence only and are not acceptance gates for new milestones.
- Failure of optional GTAO, contact, decals, or scalable light culling degrades
  only that feature. Failure of a required geometry buffer or deferred-light
  payload selects a deliberately retained compatible path during migration or
  reports the view unavailable after the opaque-forward path is retired; it
  never consumes stale targets from another view.
- Main, auxiliary, preview, thumbnail, Present, and offscreen views retain
  per-view state and deterministic output. A successful main view cannot mask
  a failed auxiliary view.

## Current Foundations and Gaps

| Area | Existing foundation | Program gap |
| --- | --- | --- |
| Material surface | Versioned PBR material representation, opaque/masked/translucent passes, tangent-space normals, direct BRDF, emissive, and studio IBL | Separate data transport from lighting without forking material or BRDF behavior |
| Lighting | One directional plus four local lights, stable per-view selection, fixed ABI, shadows, and target-GPU evidence | Evaluate supported lights after geometry and preserve forward translucency parity |
| Render targets | Renderer-owned size-keyed Scene Color/depth targets, MRT support, sampled depth, explicit layouts, and resource recovery | Replace the LDR scene intermediate, account bytes, then select a minimal GBuffer |
| Post process | Full-screen copy/FXAA, Present/offscreen variants, editor-assistance ordering, and a planned compute FXAA route | Add one display transform and reconcile FXAA domain/intermediate ownership |
| Screen-space inputs | Sampled D32 depth and an opt-in bounded contact-shadow pass | No stable normal/material signal, GTAO, temporal policy, or same-surface rejection |
| Diagnostics | Shadow/contact modes, view counters, GPU timestamp infrastructure, and Vulkan image fixtures | Add GBuffer, lighting-path, AO, memory, and forward/deferred comparison evidence |

## Milestone Map

| Milestone | Requirement | Dependencies | Deliverable | Entry gate | Exit gate | Child plan |
| --- | --- | --- | --- | --- | --- | --- |
| M1. HDR Scene Color and display mapping | Required; completed 2026-08-15 | Current post-process and PBR contracts | HDR scene intermediate, fixed exposure, selected tone mapper, SDR output, and consistent view ordering | PBR clipping is verified; compute/post-process overlap is identified | Passed: values above one survive production scene/contact rendering; display, lifecycle, view isolation, full build/native aggregate, editor smoke, and frozen RTX 3090 memory/GPU gates pass | [HDR Scene Color and Display Mapping](../../../Plans/Archive/2026-08/HDRSceneColorAndDisplayMapping.md) — Completed |
| M2. Minimal GBuffer contract and geometry proof | Required; completed 2026-08-15 | M1 complete | Measured attachment layout, opaque/masked geometry pass, debug views, and forward A/B fixture | Met 2026-08-15: HDR output is stable and the child plan owns the required field inventory | Passed: all supported opaque/masked primitive families encode deterministic data within frozen reconstruction, bandwidth, memory, lifecycle, and RTX 3090 budgets | [Minimal GBuffer and Geometry Pass](../../../Plans/Archive/2026-08/MinimalGBufferAndGeometryPass.md) — Completed |
| M3. Deferred directional lighting parity | Required; completed 2026-08-15 | M2 complete | Full-screen directional/IBL/emissive composition using shared shading and shadow code | Met 2026-08-15: GBuffer data, reconstruction error, ownership, lifecycle, and cost are qualified | Passed: forward/deferred references meet frozen tolerances across materials, cascades, views, lifecycle, memory, and RTX 3090 gates | [Deferred Directional Lighting](../../../Plans/Archive/2026-08/DeferredDirectionalLighting.md) — Completed |
| M4. Deferred opaque production parity and rollout | Required; completed 2026-08-15 | M3 complete | Current local-light tier, retained forward translucency, supported primitive parity, and one default opaque owner | Met 2026-08-15: directional slice, shared inputs, lifecycle, memory, and representative fixtures are stable | Passed: `1 + 4` lighting, retained composition, views/lifecycle, memory, aggregate/runtime, and RTX 3090 gates pass with deferred as the one generic opaque owner | [Hybrid Renderer Production Rollout](../../../Plans/Archive/2026-08/HybridRendererProductionRollout.md) — Completed |
| M5. Depth/normal grounding | Required; completed 2026-08-16 | M4 complete | GTAO-class indirect occlusion with documented non-temporal policy | Met 2026-08-15: stable D32, geometric/shading normals, HDR indirect-light composition, view/lifecycle, memory, and GPU seams are published | Passed: grounding, non-interference, diagnostics, view/lifecycle, memory, aggregate/runtime, and RTX 3090 gates pass | [Ground Truth Ambient Occlusion](../../../Plans/Archive/2026-08/GroundTruthAmbientOcclusion.md) — Completed |
| M6. Scalable and optional consumers | Evidence-gated | M4 complete; M5 inputs where applicable | Tiled/clustered lights, decals, and/or normal-aware contact-shadow revision | A measured scene or product feature exceeds the required path's capability | The selected extension passes its own image, fallback, memory, and GPU gates | Create separate plans only for selected consumers |

## Child Plan Boundaries

| Plan | Owns | Does not own | Activation state |
| --- | --- | --- | --- |
| HDR Scene Color and Display Mapping | Scene/intermediate formats, exposure, tone mapping, SDR conversion, FXAA/display ordering, target cache cost | GBuffer, deferred lights, automatic exposure, bloom, HDR displays | Completed 2026-08-15 |
| Minimal GBuffer and Geometry Pass | Attribute inventory, packing, attachment layouts, geometry writes, reconstruction, debug modes | Lighting rollout, GTAO algorithm, translucent migration | Completed 2026-08-15 |
| Deferred Directional Lighting | Shared directional/IBL/emissive evaluation and forward comparison | Local-light scaling, generic post-process graph | Completed 2026-08-15 |
| Hybrid Renderer Production Rollout | Existing local-light parity, forward translucency composition, primitive coverage, opaque-owner retirement | More-than-supported light tiers, GTAO, decals | Completed 2026-08-15 |
| Ground Truth Ambient Occlusion | Indirect occlusion, denoise/history policy, edge behavior, composition | Direct-shadow repair or contact-shadow replacement claims | Completed 2026-08-16 |
| Optional consumer plans | One measured extension each | Bundled renderer modernization | Deferred contact visibility completed 2026-08-16; other candidates remain declined |

The existing
[Directional Contact Shadows](../../../Plans/Archive/2026-08/DirectionalContactShadows.md) plan
remains separate and completed after the roadmap. Its GBuffer-aware deferred
visibility is an opt-in detail term, not an M1-M4 dependency.

## Program Validation Matrix

| Contract | M1 | M2 | M3 | M4 | M5-M6 |
| --- | --- | --- | --- | --- | --- |
| Scene-linear radiance and SDR display references | Required | Regression | Regression | Regression | Regression |
| Attachment format, byte, cache, and lifetime accounting | Required | Required | Regression | Required | Required per extension |
| StaticMesh/SplineMesh/SkeletalMesh/Terrain opaque and masked behavior | Baseline | Encode | Directional proof | Full parity | Regression |
| Translucent and special forward composition | Ordering | Ordering | Representative | Required | Regression |
| Directional/local light and shadow parity | Baseline | Inputs | Directional | `1 + 4` required | No direct rewrite |
| Main/auxiliary/preview/thumbnail/Present/offscreen isolation | Required | Required | Required | Required | Required |
| Resource failure, retry, resize, reload, and shutdown | Required | Required | Required | Required | Required |
| RTX 3090 1920x1080 GPU and memory qualification | Required | Required | Required | Required | Required per selected default |

Child plans follow the root
[build and run](../../../Development/Build/BuildAndRun.md) and
[testing](../../../Agents/Testing.md) workflows rather than duplicating
commands here.

## Risks and Control Gates

- **Bandwidth growth:** HDR and GBuffer attachments can dominate the current
  lighting cost. M1 and M2 freeze byte budgets before implementation and report
  cache-retained bytes, not only per-frame allocation counts.
- **Two-renderer divergence:** A/B paths are temporary qualification tools.
  M3 and M4 require shared shader functions, and M4 cannot close while two
  unrestricted opaque production paths remain.
- **Color regression:** Changing Scene Color format without a defined display
  transform shifts every viewport. M1 uses CPU/shader reference values and
  captured view classes before deferred work begins.
- **Post-process plan collision:** Compute FXAA and M1 touch the same owner.
  M1 Stage 0 must sequence or rebaseline the compute plan before implementation.
- **GBuffer overdesign:** M2 must tie every stored bit to a required consumer
  and compare stored versus reconstructed alternatives on image and bandwidth.
- **Screen-space overclaim:** GTAO and contact cannot observe absent geometry.
  Diagnostics and acceptance captures include disocclusion, screen edges,
  grazing walls, thin geometry, and off-screen casters.
- **Feature-family gaps:** M4 does not switch the default opaque owner until all
  currently supported primitive families and shadow/lighting combinations have
  explicit results.

## Completion Criteria

- Required milestones M1-M5 pass their exit gates and their lasting contracts
  are published under `Documentation/Runtime/Rendering/`.
- Opaque and masked production rendering has one default deferred owner, while
  retained forward surface categories and their composition order are explicit.
- Current `1 + 4` lighting, directional shadows, environment lighting,
  emissive, material passes, view classes, recovery, and resize/reload behavior
  pass deterministic parity and target-GPU gates.
- Corner grounding is supplied by the selected indirect-occlusion path;
  contact shadows remain optional and honestly bounded.
- Each M6 candidate is completed or explicitly declined with evidence. No
  unselected optional milestone blocks roadmap completion.
- Active child plans are completed, lasting rules have moved to owning runtime
  documents, and roadmap/plan/documentation validation passes.

## Related Documentation

- [PBR Pipeline Production Gaps](../../../Investigations/PBRPipelineProductionGaps.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Material System Roadmap](../../MaterialSystem.md)
- [Compute Shader Pipeline Roadmap](ComputeShaderPipeline.md)
- [Ground Truth Ambient Occlusion](../../../Runtime/Rendering/GroundTruthAmbientOcclusion.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/PostProcess.slang`
- `Engine/Shaders/Slang/ContactShadow.slang`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
