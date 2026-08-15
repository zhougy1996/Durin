# Scene Render Entry Point and Forward Opaque Retirement Plan

Summary: Consolidate production scene rendering behind `RenderScene_RenderThread` and retire the duplicate Lit opaque and masked forward path while preserving explicit retained-forward and special-view ownership.

Last reviewed: 2026-08-16

Status: Active
Completed:

## Current Status

The completed hybrid-deferred rollout makes `DeferredRequired` the explicit
product route for main, auxiliary, offscreen, preview, and thumbnail Lit solid
views. Eligible Lit opaque and masked StaticMesh, SplineMesh, SkeletalMesh, and
Terrain records already have one production owner in GBuffer plus deferred
lighting. Unlit opaque/masked records and the globally sorted translucent list
compose through retained forward pipelines after deferred lighting.

The remaining orchestration does not reflect that ownership. `RenderView`
calls `RenderHybridScene_RenderThread` for a complete deferred view and calls
`RenderScene_RenderThread` only when the hybrid path was not selected or did
not complete. As a result, the generically named scene entry point still means
"complete forward scene", while the production scene path bypasses it.
`ForwardReference` remains the value default of `FSceneViewRenderOptions`, and
`DeferredWithForwardFallback` remains reachable by failure-injection tests even
though no product caller selects either state.

This plan retains `RenderScene_RenderThread` as the one scene-composition entry
point, moves the production hybrid sequence behind it, removes the migration
route state machine and the ordinary Lit solid opaque/masked forward variant,
and preserves the forward work still required by translucency, Unlit and
wireframe views, shadow depth, SkyBox bootstrap, and explicitly named special
surfaces.

## Goal

Make every rendered view enter `RenderScene_RenderThread` exactly once for
scene-color composition, with eligible Lit solid opaque/masked ownership fixed
to deferred rendering and every remaining forward responsibility explicit.
Remove the complete Lit solid forward reference and migration fallback without
changing qualified production images, retained-forward ordering, view-mode
behavior, failure semantics, lifecycle behavior, memory ceilings, or RTX 3090
performance gates.

## Scope

- Establish `RenderScene_RenderThread` as the sole scene-color composition
  entry point after shadow, GBuffer, GTAO, and common resource preparation.
- Fold or decompose `RenderHybridScene_RenderThread` into named bootstrap,
  deferred-opaque, retained-forward, and special-view stages owned by the
  unified entry point.
- Remove `ForwardReference`, `DeferredWithForwardFallback`, and their fallback
  counters, branches, defaults, tests, and resource preparation.
- Reject an incomplete required deferred payload without clearing, presenting,
  or replacing it with a complete forward Lit view.
- Stop constructing ordinary Scene Color pipelines for the exact combination
  of Lit view mode, Lit material, solid opaque/masked base pass, and
  non-retained rendering.
- Preserve GBuffer and shadow use of opaque/masked preparation buckets.
- Preserve retained-forward Unlit opaque/masked and globally sorted
  translucent rendering for Static/Spline, Skeletal, and Terrain families.
- Preserve Unlit view mode, wireframe, diagnostics, SkyBox, contact shadows,
  display mapping, FXAA, editor assistance, Present, and offscreen behavior.
- Replace live complete-forward A/B coverage with shared-lighting tests,
  retained-forward fixtures, deferred production references, and reviewed
  immutable historical goldens where continued comparison is justified.
- Publish the resulting entry-point and surface-ownership contract in the
  authoritative runtime rendering documentation.

## Non-Goals

- Removing forward translucency, Unlit surfaces, wireframe, shadow-depth
  rendering, SkyBox bootstrap, or editor-assistance rendering.
- Removing opaque/masked draw classifications or preparation buckets used by
  GBuffer, shadow, Unlit, and special-view rendering.
- Changing the four-attachment GBuffer layout, material packing, position
  reconstruction, shared BRDF, fixed `1 + 4` light tier, directional shadows,
  GTAO algorithm, contact shadows, tone mapping, or FXAA.
- Adding deferred translucency, decals, tiled/clustered lighting, a render
  graph, transient allocation, asynchronous compute, or new material models.
- Renaming `RenderScene_RenderThread`; the selected outcome deliberately keeps
  that name as the renderer's scene-composition abstraction.
- Keeping a runtime complete-forward renderer solely to regenerate comparison
  images after its production ownership has ended.

## Design Decisions and Invariants

### Unified scene entry point

`RenderView` continues to own immutable view preparation, output acquisition,
directional-shadow preparation, GBuffer/GTAO production, post process, contact
shadows, and editor-assistance ordering. After required scene inputs are
prepared, it calls `RenderScene_RenderThread` exactly once. It does not select
between complete hybrid and complete forward scene functions.

`RenderScene_RenderThread` consumes a private prepared-input value whose
references are valid only for the current command and view. The input carries
Scene Color, directional direct, D32, complete deferred parameters when the
view requires them, and the already prepared geometry state. It must not look
up a previous view's payload or trigger an implicit route change.

The function returns a scene result that distinguishes success, required
renderer-resource unavailability, and required environment unavailability.
It does not encode failure as a request to rerender through another lighting
owner.

### Surface and view ownership

For a Lit solid view, `RenderScene_RenderThread` records exactly:

```text
SkyBox/clear HDR bootstrap
  -> deferred Lit opaque/masked
  -> retained-forward Unlit opaque/masked
  -> one globally sorted translucent list
```

Eligible Lit opaque/masked records may not be submitted to an ordinary forward
Scene Color pass. Missing GBuffer, deferred, or retained-forward resources fail
the required view before partial Scene Color can become authoritative.

For Unlit, wireframe, or another explicitly named special view, the same entry
point selects a dedicated special-forward stage. That stage may draw a Lit
material with Unlit view semantics; removing Lit solid forward ownership must
not remove those shader or pipeline variants. Special-view selection is based
on immutable view mode, not resource failure.

### Retained forward boundary

The forward material, geometry, and shared-lighting facilities remain because
translucent and special surfaces require them. Retained-forward render passes
load HDR Scene Color, directional direct, and GBuffer depth and never clear or
display-map those attachments. Cross-family translucent sorting remains owned
by `PreparedView.TranslucentGeometry` and is traversed once.

The removable pipeline domain is narrowly defined as:

```text
RenderMode == Lit
RasterMode == Solid
MaterialShadingModel == Lit
Pass in {Opaque, Masked}
RenderTargetLayout == ordinary SceneTargets
```

GBuffer pipelines, directional-shadow pipelines, retained-forward pipelines,
translucent pipelines, Unlit material pipelines, and special-view variants are
outside that domain.

### Route and diagnostic cleanup

Product and test callers no longer select a `HybridOpaqueRoute`. The absence
of a route option makes required deferred ownership structural rather than a
caller convention. Development GBuffer, deferred-component, and GTAO debug
modes remain explicit isolated diagnostics; they do not reinstate a complete
forward result.

Remove hybrid fallback counters whose only meaning was route migration.
Retain GBuffer, deferred, GTAO, draw-family, resource-failure, active-byte, and
retained-byte counters that diagnose the production implementation.

### Parity and failure evidence

Before deleting the live forward reference, freeze the exact tests and image
artifacts that still use it. Each must be classified as one of:

- production behavior, migrated to `DeferredRequired` expectations;
- shared material/lighting semantics, moved to focused CPU/shader tests;
- retained-forward behavior, covered by mixed-scene contribution fixtures;
- historical visual evidence, retained as a reviewed immutable golden; or
- obsolete migration/fallback behavior, removed with the route.

Required deferred failure returns `RendererResourcesUnavailable` for only the
affected view. Optional GTAO failure continues with factor one. Missing
environment resources continue using the qualified black fallback where the
current contract permits it. No failure may expose partially bootstrapped HDR
or stale attachments.

## Current Foundations and Gaps

| Area | Current foundation | Refactor gap |
| --- | --- | --- |
| Production selection | Every product Lit solid entry point selects `DeferredRequired` | Selection is repeated by callers and the low-level value default is still `ForwardReference` |
| Scene orchestration | `RenderHybridScene_RenderThread` records the qualified production composition | Production bypasses the generically named `RenderScene_RenderThread` |
| Complete forward scene | `RenderScene_RenderThread` draws SkyBox, all opaque/masked families, then sorted translucency | It still permits duplicate Lit opaque/masked ownership and also mixes in valid special-view responsibilities |
| Retained forward | Dedicated load-preserving pipelines draw Unlit opaque/masked and sorted translucency | Traversal and resource preparation remain partially duplicated between hybrid and complete-forward paths |
| Pipeline cache | Keys distinguish shadow, ordinary scene, GBuffer, and hybrid-retained layouts | Ordinary Lit solid opaque/masked forward variants remain constructible |
| Tests | Vulkan fixtures cover forward/deferred parity, mixed composition, failures, views, and lifecycle | Four test sources still select `ForwardReference`; one still selects migration fallback |
| Documentation | Runtime contracts state that production Lit opaque/masked is deferred-only | They still describe the test-only complete forward and migration route as runtime states |

## Implementation Stages

### Stage 0: Freeze ownership, callers, and retirement evidence

- [ ] Inventory every `ForwardReference`, `DeferredWithForwardFallback`,
      `RenderHybridScene_RenderThread`, and `RenderScene_RenderThread` caller.
- [ ] Inventory ordinary, hybrid-retained, GBuffer, shadow, translucent, Unlit,
      and special-view pipeline keys for Static/Spline, Skeletal, and Terrain.
- [ ] Classify every forward/deferred A/B and migration-fallback test using the
      parity categories in this plan; record the replacement owner for each.
- [ ] Freeze production Lit solid, mixed retained-forward, Unlit view,
      wireframe, Present/offscreen, preview, thumbnail, resize, reload, and
      required-failure reference results before changing orchestration.
- [ ] Freeze the pre-refactor ordinary Lit opaque/masked pipeline creation
      count and render-thread CPU/GPU evidence needed to prove retirement does
      not add work; retain the existing published memory and RTX 3090 gates.
- [ ] Specify the prepared-input value and scene-result enum at the
      `RenderView`/`RenderScene_RenderThread` boundary, including nullability,
      current-view identity, and failure mapping.

#### Acceptance Gate

- Every removable runtime branch, pipeline variant, counter, test, and
  documentation statement has one recorded disposition; every retained
  forward or special-view responsibility has an explicit owner and frozen
  reference before implementation begins.

### Stage 1: Route production composition through `RenderScene_RenderThread`

- [ ] Add the private prepared scene-input/result contract and make
      `RenderView` call `RenderScene_RenderThread` exactly once after common
      shadow, GBuffer, GTAO, and resource preparation.
- [ ] Move the SkyBox/clear bootstrap, deferred evaluation, retained Unlit
      opaque/masked draws, and globally sorted translucent traversal behind the
      unified entry point.
- [ ] Decompose or remove `RenderHybridScene_RenderThread` so it cannot remain
      a parallel scene entry point.
- [ ] Preserve post-scene contact-shadow, HDR capture, display/FXAA, Present or
      offscreen transition, and editor-assistance ordering.
- [ ] Preserve per-view counters and prove main, auxiliary, preview, thumbnail,
      Present, and offscreen views do not share prepared scene inputs.

#### Acceptance Gate

- Every production Lit solid view enters `RenderScene_RenderThread` once and
  produces the frozen deferred/mixed-scene references with unchanged pass
  ordering, counters, output ownership, and per-view isolation; no product path
  calls a second complete scene renderer.

### Stage 2: Separate special views and remove migration routing

- [ ] Add an explicit internal special-forward stage for Unlit, wireframe, and
      other named non-production-deferred view modes while retaining
      `RenderScene_RenderThread` as their public scene entry point.
- [ ] Remove `EHybridOpaqueRoute`, `FSceneViewRenderOptions::HybridOpaqueRoute`,
      all product/test assignments, and the value-default forward selection.
- [ ] Remove whole-view forward fallback, hybrid fallback counters, and
      migration-only resource branches.
- [ ] Convert required-path failure fixtures to assert one unavailable view,
      no partial/stale output, and continued independence of later views.
- [ ] Prove optional GTAO failure still binds factor one without selecting a
      different scene path.

#### Acceptance Gate

- View mode is the only selector between deferred Lit solid composition and
  explicit special-forward composition. Resource failure never selects a
  complete forward Lit renderer, and all supported special views retain their
  frozen output and lifecycle behavior.

### Stage 3: Retire ordinary Lit opaque/masked forward variants

- [ ] Prevent shader and graphics-pipeline construction for Lit material,
      Lit solid view, opaque/masked, ordinary Scene Targets combinations in
      Static/Spline, Skeletal, and Terrain renderers.
- [ ] Remove execution loops and helper branches reachable only by the retired
      complete Lit solid forward path.
- [ ] Preserve opaque/masked preparation for GBuffer, directional shadows,
      Unlit surfaces, and special views; preserve translucent preparation and
      one cross-family sorted traversal.
- [ ] Add focused assertions/tests proving production Lit opaque/masked records
      are consumed once by GBuffer and never by retained/special forward.
- [ ] Verify the retired ordinary pipeline creation count reaches zero while
      GBuffer, shadow, retained-forward, translucent, Unlit, and special-view
      pipeline coverage remains nonzero where the fixture demands it.

#### Acceptance Gate

- No runtime or test can construct or execute an ordinary Lit solid
  opaque/masked forward Scene Color pipeline. All retained surface categories,
  special views, shadows, and four supported geometry families pass their
  frozen references without duplicate draws or missing coverage.

### Stage 4: Qualify and publish the consolidated renderer

- [ ] Run focused unit, shader-reflection, layout, rendering, failure,
      lifecycle, and cross-view tests through the repository testing workflow.
- [ ] Run the native aggregate, required build validation, and editor runtime
      smoke through the repository build-and-run workflow.
- [ ] Re-run the validation-enabled RTX 3090 1920x1080 production fixture and
      prove GBuffer, GTAO, deferred, retained-forward, display, total-frame,
      active-byte, and retained-byte results remain within their published
      gates.
- [ ] Compare pre/post pipeline creation and render-thread evidence; require
      zero retired ordinary Lit opaque/masked pipelines, no new production
      target, no increased cache ceiling, and no additional scene traversal.
- [ ] Update `DeferredDirectionalLighting.md`, `ForwardLighting.md`, `GBuffer.md`,
      and other directly affected runtime contracts to describe the unified
      entry point and remove migration-route language.
- [ ] Complete documentation validation, record final evidence, and mark this
      plan complete only after all required gates pass.

#### Acceptance Gate

- Image, ownership, view-mode, failure, lifecycle, aggregate/build, editor
  runtime, memory, pipeline-retirement, and RTX 3090 gates pass with
  `RenderScene_RenderThread` as the sole scene-composition entry point and no
  complete Lit solid forward renderer remaining.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Unified entry | Main, auxiliary, preview, thumbnail, Present, and offscreen views each call one scene-composition entry point |
| Lit opaque ownership | StaticMesh, SplineMesh, SkeletalMesh, and Terrain opaque/masked records write GBuffer and deferred output exactly once |
| Retained forward | Unlit opaque/masked and cross-family sorted translucent contributions preserve HDR depth, blending, sorting, and alpha |
| Special views | Unlit and wireframe views retain frozen geometry, material, depth, and editor-assistance behavior without a Lit solid forward route |
| Lighting | No light, directional, `1 + 4`, shadows, environment fallback, emissive-above-one, material AO, and GTAO preserve qualified results |
| Optional composition | GTAO disable/failure, contact on/off, FXAA on/off, and diagnostics do not change scene ownership |
| Required failure | GBuffer, deferred shader/pipeline, retained-forward, and uniform failures return the documented result without partial, stale, or fallback output |
| Lifecycle | Alternating views/extents, resize, shader reload, manual retry, device invalidation, explicit release, and shutdown retain current-view isolation |
| Pipeline retirement | Ordinary Lit solid opaque/masked forward Scene Target pipeline creation and execution counts are exactly zero |
| Memory/performance | No new production target or cache ceiling; active/retained bytes and RTX 3090 intervals remain within the published M4/M5 gates |

## Definition of Done

- `RenderScene_RenderThread` is the only scene-color composition entry point
  used by `RenderView` for production and supported special views.
- `RenderHybridScene_RenderThread`, `EHybridOpaqueRoute`, complete-view forward
  fallback, and migration-only counters no longer exist.
- Lit solid opaque/masked Static/Spline, Skeletal, and Terrain records have no
  constructible ordinary forward Scene Color pipeline and one deferred owner.
- Retained-forward Unlit/translucent, Unlit and wireframe views, SkyBox,
  shadows, diagnostics, contact shadows, display, FXAA, and editor assistance
  retain their documented behavior.
- Required failures never present partial or stale HDR and never change the
  scene lighting owner; optional GTAO failure remains factor one.
- Focused, aggregate, build, runtime, memory, and target-GPU validation passes,
  and lasting ownership/orchestration rules are published in Runtime docs.

## Deferred Follow-ups

- Deferred or order-independent translucency.
- A dedicated material-shader permutation reduction beyond the retired
  ordinary Lit opaque/masked domain.
- Tiled/clustered local lights, decals, render-graph integration, or transient
  target allocation.
- Renaming lower-level retained-forward or special-view helpers if later code
  evidence shows their names remain ambiguous; this plan keeps
  `RenderScene_RenderThread` deliberately.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [Hybrid Renderer Production Rollout](HybridRendererProductionRollout.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Ground Truth Ambient Occlusion](../Runtime/Rendering/GroundTruthAmbientOcclusion.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/Lighting/SurfaceLighting.slang`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
