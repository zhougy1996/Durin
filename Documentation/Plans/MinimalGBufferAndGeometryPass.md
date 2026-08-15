# Minimal GBuffer and Geometry Pass Plan

Summary: Freeze and implement the smallest measured geometry-buffer contract for every supported opaque and masked primitive family before deferred lighting begins.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

M1 completed on 2026-08-15. Scene lighting now targets scene-linear
`RGBA16_FLOAT`, one display transform owns SDR output, and the existing scene
target set costs 24 bytes per pixel. At 1920x1080 one retained extent is
49,766,400 bytes before any GBuffer attachment is added. Production readback,
cross-view isolation, native-window Present/resize/toggle behavior, full native
coverage, full build, hidden-window editor smoke, and RTX 3090 display budgets
pass.

M2 is active only for geometry transport. Stage 0 must inventory consumers and
freeze attachment formats, reconstruction tolerances, bytes, and RTX 3090
thresholds before an opaque draw is rerouted. No GBuffer layout or normal
encoding is selected merely by this plan's activation.

## Goal

Publish and implement one minimal, evidence-backed GBuffer that deterministically
encodes every currently supported opaque and masked StaticMesh, SplineMesh,
SkeletalMesh, and Terrain draw. Preserve the forward renderer as the production
lighting owner during this milestone, provide explicit debug/reconstruction
evidence, and establish qualified inputs for M3 without introducing a second
material or lighting model.

## Scope

- Inventory every material and geometric field required by M3 directional,
  environment, emissive, shadow, and later GTAO/contact consumers.
- Compare stored and reconstructed world position, geometric normal, shading
  normal, tangent handedness, base color, metallic, roughness, ambient
  occlusion, emissive, material class, effective opacity, and primitive
  identity candidates.
- Select attachment formats, channel packing, clear values, load/store actions,
  layouts, transitions, byte accounting, and cache retention.
- Add an opaque/masked geometry pass for StaticMesh, SplineMesh, SkeletalMesh,
  and Terrain using shared material decode and deformation inputs.
- Add GBuffer debug views, readback, deterministic fixtures, and temporary
  forward-versus-geometry A/B evidence.
- Cover per-view ownership, resize, alternating extents, resource failure,
  retry, shader/device invalidation, reload, and shutdown.
- Freeze RTX 3090 1920x1080 bandwidth, memory, and geometry-pass GPU gates.

## Non-Goals

- Deferred directional, environment, emissive, local-light, or shadow
  evaluation; those belong to M3 and M4.
- Routing translucent, particle, water, hair, unlit, or editor-assistance
  surfaces through the GBuffer.
- GTAO, decals, clustered/tiled lighting, revised contact shadows, or a render
  graph.
- Expanding supported light counts or changing BRDF, material authoring,
  deformation, LOD, culling, or shadow-selection behavior.
- Keeping an unrestricted second generic opaque renderer after the later M4
  rollout gate.

## Design Decisions and Invariants

### Data contract

- Every stored bit has a named consumer and a measured stored-versus-
  reconstructed comparison. Unused attachment channels are not a design
  invitation.
- Geometric-surface identity remains independent of authored normal maps. M5
  may consume the selected stable geometric normal or equivalent signal.
- Material decode, tangent-space normal application, PBR parameters, emissive,
  and alpha-mask decisions reuse shared shader facilities. The geometry pass
  changes transport, not material semantics.
- Opaque and masked draws are eligible. Mask rejection occurs before GBuffer
  publication; alpha-blended surfaces remain forward.
- World-position storage is not assumed. Stage 0 compares depth reconstruction
  against explicit position storage over perspective, orthographic,
  constrained-aspect, large-coordinate, and reversed-Z fixtures.

### Pass and ownership

- `FSceneRenderer` owns pass ordering. Renderer-owned size-keyed resources own
  GBuffer allocation and never alias another view's current extent.
- The production forward lighting pass remains authoritative in M2. The new
  geometry pass may run only in qualification/A-B modes until M3 consumes it.
- Scene Color stays HDR and display mapping remains unchanged. GBuffer debug
  views are scene-domain diagnostics mapped through the existing display path.
- Transitions are explicit and use existing graphics/transfer ownership. M2
  does not require a render graph, transient allocator, async compute, or a
  second queue.

### Failure and rollout

- GBuffer payloads publish complete-or-null. A missing required attachment,
  shader, pipeline, or primitive input disables the qualification path for
  that view and cannot reuse stale data.
- A failure in M2 never breaks the production forward output. Counters and
  diagnostics distinguish unavailable, rejected, skipped, and successful
  geometry views.
- Cache policy is byte-based. Stage 0 freezes both per-extent bytes and peak
  retained bytes before implementation.

## Current Foundations and Gaps

| Area | Existing foundation | M2 gap |
| --- | --- | --- |
| Scene targets | HDR color, depth, directional direct, contact color, explicit layouts, 24 B/px accounting | No geometry attachments, semantic layout, or retained-byte gate |
| Materials | Versioned PBR representation and shared opaque/masked/translucent shader paths | No pass-boundary packing or decode parity proof |
| Geometry | StaticMesh, SplineMesh, SkeletalMesh, and Terrain production draws | No one geometry-pass contract spanning all four families |
| Position/normal | Reversed-Z depth and tangent-space shading normals | No qualified reconstruction tolerance or stable geometric-surface signal |
| Lifecycle | Transactional Renderer resource slots and size-keyed caches | No GBuffer payload failure/retry/resize evidence |
| Diagnostics | View counters, Vulkan readback, image fixtures, GPU timestamps | No attachment debug modes, reconstruction heatmaps, or M2 timing seam |

## Implementation Stages

### Stage 0: Freeze consumers, layout candidates, and budgets

- [ ] Inventory each M3-M5 consumer and map it to exact source material,
      primitive, view, depth, or identity data.
- [ ] Inventory current StaticMesh, SplineMesh, SkeletalMesh, and Terrain vertex
      outputs, deformation paths, material bindings, mask behavior, and
      available stable primitive identifiers.
- [ ] Compare depth-derived position against stored position and compare normal
      encodings over perspective, orthographic, constrained-aspect,
      large-coordinate, mirrored-tangent, grazing, and authored-normal fixtures.
- [ ] Select one attachment/channel layout with explicit formats, clear values,
      alpha/material-class rules, layouts, transitions, and debug meanings.
- [ ] Freeze numeric per-pixel bytes, one-extent bytes, maximum retained bytes,
      and RTX 3090 1920x1080 median/p95 geometry-pass budgets before production
      implementation.
- [ ] Freeze deterministic tolerances and reference captures for all supported
      primitive/material families, masked edges, reconstruction, view classes,
      and forward A/B output.

#### Acceptance Gate

- Every stored or reconstructed field has a named consumer, encoding,
  tolerance, and failure rule; one layout and numeric memory/GPU budget are
  selected with no unresolved position, normal, identity, or packing choice.

### Stage 1: Establish resources, layouts, and shared shader transport

- [ ] Add semantic GBuffer render-target layouts and exact format/load/store/
      final-state tests without changing the production scene pass.
- [ ] Add byte-accounted, size-keyed complete-or-null GBuffer resources with
      bounded retention and current-view isolation.
- [ ] Factor shared material decode, tangent-space normal application, mask
      rejection, and geometric-normal preparation for forward and geometry
      writers without changing selected shading equations.
- [ ] Add typed geometry-pass shader parameters and pipeline payloads for each
      required primitive vertex factory and material class.
- [ ] Inject attachment, shader, and pipeline failures; prove same-generation
      suppression, manual retry, reload, device invalidation, and release.

#### Acceptance Gate

- Layouts, byte accounting, shared shader transport, and transactional payloads
  pass focused CPU/RHI tests while production rendering remains unchanged.

### Stage 2: Encode every opaque and masked primitive family

- [ ] Record the geometry pass for visible opaque and masked StaticMesh draws,
      preserving LOD, section, material, culling, and mask behavior.
- [ ] Add SplineMesh deformation parity without a separate material or packing
      path.
- [ ] Add SkeletalMesh palette/deformation parity with coherent pose and bounds
      snapshots.
- [ ] Add Terrain patch/LOD/batching parity and large-coordinate coverage.
- [ ] Keep translucent, special forward, unlit, and editor-assistance ownership
      unchanged and prove they do not publish GBuffer records.
- [ ] Publish per-family attempts, successful writes, rejects, attachment bytes,
      and view-local diagnostic counters.

#### Acceptance Gate

- Every supported opaque/masked primitive family writes the selected contract
  deterministically; exclusions remain forward and no production-lit image or
  feature owner changes.

### Stage 3: Qualify reconstruction, debug views, and A/B evidence

- [ ] Add channel debug views and reconstruction-error heatmaps through the
      existing HDR/display pipeline.
- [ ] Read back every attachment and verify clear/background, opaque, masked,
      geometric normal, shading normal, material, emissive, AO, identity, and
      alpha rules.
- [ ] Compare reconstructed versus source values against Stage 0 tolerances for
      every primitive family and selected difficult view/geometry fixture.
- [ ] Add temporary forward-versus-GBuffer A/B fixtures that decode the stored
      material inputs without performing deferred lighting.
- [ ] Prove main, auxiliary, preview, thumbnail, Present/offscreen, resize, and
      alternating-size isolation with no stale attachment reuse.

#### Acceptance Gate

- Debug/readback/A-B evidence meets every frozen tolerance across primitive,
  material, view, and lifecycle matrices; no attachment contains unexplained
  data or unowned padding semantics.

### Stage 4: Qualify cost and publish the M3 input contract

- [ ] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the required
      ordinary native aggregate, and the full build through root workflows.
- [ ] Capture the frozen RTX 3090 geometry-pass timing matrix and record
      adapter, driver, profile, warm-up/sample count, median, p95, attachment
      bytes, and peak retained bytes.
- [ ] Run validation-enabled native-window and hidden-editor resize/reload/
      shutdown smoke coverage.
- [ ] Publish the lasting GBuffer encoding, reconstruction, ownership, memory,
      failure, and diagnostic contracts under Runtime Rendering.
- [ ] Re-review M3 against the qualified GBuffer and activate
      `DeferredDirectionalLighting.md` only after this plan's exit gate passes.

#### Acceptance Gate

- Image, reconstruction, primitive-family, lifecycle, focused/aggregate test,
  full-build, runtime, documentation, memory, and RTX 3090 GPU gates pass; the
  stable M3 input contract is published and no M2 qualification route has
  become an accidental second production renderer.

## Validation Matrix

| Contract | Required stages | Validation outcome |
| --- | --- | --- |
| Attachment formats and packing | 0-4 | Exact layout tests and Vulkan readback match the selected semantic channels |
| Position and normal reconstruction | 0, 3-4 | Difficult view/geometry fixtures stay within frozen numeric tolerances |
| Primitive families | 0, 2-4 | StaticMesh, SplineMesh, SkeletalMesh, and Terrain opaque/masked writes pass |
| Material parity | 0, 1-4 | Shared decode, normal, PBR inputs, emissive, AO, mask, and alpha rules agree |
| Excluded surfaces | 2-4 | Translucent, special forward, unlit, and editor assistance do not enter the GBuffer |
| View isolation | 1, 3-4 | Main/auxiliary/preview/thumbnail/Present/offscreen and alternating extents remain independent |
| Recovery | 1-4 | Failure, retry, reload, device invalidation, resize, and shutdown never consume stale attachments |
| Memory and GPU | 0, 4 | Per-extent/retained bytes and RTX 3090 median/p95 meet frozen numeric gates |

## Definition of Done

- All Stage 0-4 gates pass and the lasting contract is published.
- One minimal GBuffer deterministically represents all supported opaque and
  masked primitive families within frozen reconstruction tolerances.
- Stored bits, reconstructed values, resource states, byte costs, and failure
  behavior are explicit and tested.
- Production forward lighting remains unchanged during M2; temporary A/B
  routes are bounded qualification tools.
- M3 activation is evidence-backed and the hybrid-deferred roadmap records M2
  completion.

## Deferred Follow-ups

- Deferred directional/environment/emissive evaluation belongs to M3.
- Existing local lights, forward translucency composition, and opaque-owner
  rollout belong to M4.
- GTAO and any normal-aware contact-shadow revision belong to M5 or a later
  evidence-backed plan.
- Decals, scalable light culling, render graphs, transient allocation, and
  asynchronous compute remain separately gated.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
