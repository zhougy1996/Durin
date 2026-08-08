# Material Render Pass Policies Plan

Summary: Give Opaque, Masked, and Translucent StaticMesh materials distinct deterministic base-pass behavior through explicit Renderer preparation and value-based RHI graphics state.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

This plan is the active M2 child of the
[Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
and the execution boundary for Material System milestone 4's visible surface
policies. M1 is complete: StaticMesh entries arrive through typed
`FPrimitiveSceneInfo` storage with stable identity, transform, bounds, and
detached material proxies.

The material v3 snapshot already identifies blend mode, shading model, mask
threshold, two-sided state, and depth-write policy. Those identities do not yet
change visible behavior. `FStaticMeshRenderer` visits typed StaticMesh entries
and LOD 0 sections in scene order, but every pipeline disables blending,
enables depth test/write, disables culling, and uses the same draw loop.
`StaticMeshBasePass.slang` calculates opacity and opacity mask but never rejects
masked fragments, while Vulkan expands four Boolean pipeline fields into fixed
backend policy.

Stage 0 must freeze the effective pass, state, winding, sorting, and validation
contracts against current view and section semantics before the first RHI or
shader migration.

## Goal

Make every current StaticMesh material static property produce one documented,
testable base-pass meaning:

- Opaque sections draw without blending.
- Masked sections reject fragments below their resolved mask threshold without
  becoming translucent.
- Translucent sections use straight-alpha blending and deterministic
  back-to-front ordering.
- Two-sided, depth-write, mirrored-transform winding, Lit/Unlit, and
  Solid/Wireframe policies compose through explicit effective graphics state.
- RHI and Vulkan represent that state with cohesive values instead of a growing
  collection of feature-specific Booleans.

After the plan, one view-local preparation step classifies every drawable LOD 0
section into explicit base-pass work, and execution consumes those prepared
items without resolving material pass identity a second time.

## Scope

- Freeze the effective base-pass policy for `EMaterialBlendMode`,
  `EMaterialDepthWritePolicy`, `bTwoSided`, mirrored transforms, view render
  mode, and raster mode.
- Add the minimum cohesive RHI rasterizer, depth, and color-blend value
  descriptors required by those policies, including equality/hash identity and
  Vulkan translation.
- Make graphics-pipeline creation, diagnostics, and Renderer cache keys include
  every effective state that changes a pipeline.
- Add a Renderer-private prepared StaticMesh section item and Opaque, Masked,
  and Translucent view-local buckets.
- Keep LOD 0 and current no-culling behavior while deriving section identity,
  transform, material snapshot, pipeline key, and translucent sort key once.
- Implement masked shader coverage rejection from the resolved OpacityMask
  constant/texture and static threshold.
- Implement straight-alpha translucent color/alpha blending, depth policy, and
  stable back-to-front ordering.
- Apply authored two-sided and one-sided culling correctly for normal and
  mirrored transforms.
- Preserve main, auxiliary, present, offscreen, fixed-aspect, Lit/Unlit,
  Solid/Wireframe, post-process, editor-assistance, material fallback, resource
  retry/invalidation, and multi-view behavior.
- Add focused public-RHI, Vulkan, shader, preparation, ordering, lifecycle, and
  rendered-output coverage.

## Non-Goals

- Frustum culling, LOD selection, visibility counters, broad state sorting, or
  reusable per-view draw lists beyond the minimum M2 section buckets; M3 owns
  those extensions.
- Depth-only resources, shadow-map resources, shadow caster preparation, or
  shadow-depth execution; M6 owns the first auxiliary geometry pass.
- A deferred renderer, GBuffer, order-independent transparency, per-pixel
  linked lists, weighted blending, depth peeling, or per-triangle sorting.
- Point/spot lights, multi-light payloads, light culling, or shadows.
- Material graph compilation, new authored blend modes, custom blend equations,
  alpha-to-coverage, dithered transitions, decals, or separate transmission.
- Stencil, depth bias, conservative rasterization, independent blend per MRT,
  or color-write policies without a selected M2 consumer.
- Public pass, material, renderer, or pipeline registration.
- Moving material identity ownership out of Engine or feature-resource ownership
  out of `FStaticMeshRenderer`.

## Design Decisions and Invariants

### Pass classification is per section

A StaticMesh proxy can bind different materials to different sections, so pass
membership is derived from each resolved section material rather than from the
primitive kind or proxy. For one `FSceneView`, preparation walks the typed
StaticMesh SceneInfo view and current LOD 0 sections once. Each valid section
produces one prepared item containing stable primitive identity, section index,
SceneInfo transform/bounds reference or copied facts, render data, resolved
material snapshot, effective pipeline key, and sort facts.

The item enters exactly one Opaque, Masked, or Translucent bucket. Missing or
invalid material data continues to resolve to the complete ErrorMaterial before
classification, so failure cannot create an unclassified or partially bound
draw. M2 preparation remains view-local and ephemeral; it neither mutates
`FScene` membership nor creates a second registry.

### M2 remains a forward base pass

All three buckets continue to target the current forward Scene Color/depth
layout. M2 does not introduce a GBuffer or an A/B deferred path. Blend mode and
masked shader identity provide the selected pass/permutation dimensions;
Solid/Wireframe and mirrored winding provide pipeline dimensions. The current
platform shader output identity remains authoritative, there is no selected
material-quality axis, and `FLocalVertexFactory` is the only production vertex
factory in scope. New platform, quality, or vertex-factory permutation axes are
added only with a concrete consumer in a later milestone.

### Visible surface policy is fixed

| Blend mode | Color blending | Depth test | `Automatic` depth write | Coverage |
| --- | --- | --- | --- | --- |
| Opaque | Disabled | Less | Enabled | All produced fragments |
| Masked | Disabled | Less | Enabled | Discard when resolved mask is below threshold |
| Translucent | Straight alpha | Less | Disabled | All produced fragments |

Explicit `Enabled` or `Disabled` depth-write policy overrides the blend-mode
default. Straight-alpha color uses `SrcAlpha` and `OneMinusSrcAlpha`; alpha uses
`One` and `OneMinusSrcAlpha`, both with Add. Opaque and Masked retain the shader
output alpha currently carried into Scene Color even though blending is off.

Masked coverage compares the saturated product of the OpacityMask constant and
OpacityMask texture sample against the static shader-map threshold. Opacity and
BaseColor alpha do not implicitly enter the mask. Threshold equality is kept;
only values strictly below the threshold are discarded.

### Culling and winding are effective pipeline state

Two-sided materials use no culling. One-sided materials cull back faces. The
effective front-face winding combines the repository's clockwise vertex
contract with the local-to-world determinant sign so mirrored transforms show
the same authored side rather than disappearing or exposing the reverse side.
Winding parity therefore participates in the Renderer pipeline key; it is not
hidden in a shader-only normal correction.

Solid and Wireframe use the same blend, depth, cull, winding, mask, and pass
policy. Raster mode remains a pipeline choice and does not bypass authored
material semantics.

### Translucent order is deterministic and deliberately bounded

Translucent items sort back-to-front by squared view-space distance from the
camera origin to the transformed section-bounds center. This center-based
object/section metric is the accepted M2 baseline; it does not claim correct
per-triangle ordering for intersecting translucent geometry. Equal distance
uses ascending `FPrimitiveSceneId::Value`, then section index as the stable
tie-break. Invalid section bounds fall back to the primitive world-bounds
center and finally the transformed local origin, so every accepted item has a
finite key.

Preparation is repeated independently for each view. No bucket or sort result
is shared between main and auxiliary views, different dimensions, or different
camera transforms.

### RHI state uses cohesive value descriptors

M2 replaces the relevant graphics-pipeline Booleans with value objects:

- rasterizer state: polygon mode, cull mode, and front-face winding;
- depth state: test enable, write enable, and compare operation; and
- one color-attachment blend state: enable, color/alpha factors and operations,
  plus the current complete color-write mask.

The exact enum set is limited to the values consumed by current rendering and
the three M2 policies. Stencil and depth bias do not enter merely to make the
descriptors look future-complete. Descriptor equality/hash, pipeline cache
identity, debug diagnostics, failure injection, and Vulkan mapping must agree.
Complete-or-null PSO creation and Renderer-owned reuse remain unchanged.

### Existing ownership and failure behavior remain intact

Prepared items borrow SceneInfo/proxy/render-data/material state only for the
render command that creates and executes them. No new asynchronous publication,
revision, wait, or resource fence is introduced. StaticMesh render-data and
material-proxy lifetimes remain governed by their existing component and asset
protocols.

Shader, pipeline, sampler, default-texture, and environment failures retain the
current isolated slot/retry/diagnostic behavior. Failure to resolve or create
one prepared item's resources skips only that item and cannot corrupt another
bucket or later view.

## Current Foundations and Gaps

| Area | Foundation | Gap owned by this plan |
| --- | --- | --- |
| Scene input | Typed `FPrimitiveSceneInfo` access, stable identity, transform, and bounds are complete from M1. | No per-section pass item or view-local bucket exists. |
| Material identity | v3 snapshots carry blend, shading, mask threshold, two-sided, and depth-write identity. | Static properties do not yet select visible pass or graphics state. |
| Shader | Base pass evaluates PBR, Opacity, and OpacityMask and compiles blend-mode/threshold macros. | Masked fragments are never discarded and all modes share execution behavior. |
| Pipeline cache | Shader maps key blend/shading/threshold; pipelines key material pipeline identity and retain solid/wireframe payloads. | Effective blend/depth/cull/winding state is fixed and mirrored winding is not a cache dimension. |
| Public RHI | Graphics initializer exposes alpha-blend, back-face-cull, depth-test/write, polygon mode, and topology. | Boolean state cannot express or validate the selected policies as cohesive values. |
| Vulkan | Backend creates valid fixed pipelines and already maps the current straight-alpha Boolean path. | Cull/winding/depth/blend values and their key coverage are not explicit or fully tested. |
| Validation | Material identity, shader reflection, opaque rendering, lifecycle, main/auxiliary output, and Vulkan tests exist. | No mask edge, blend equation, depth override, face winding, stable translucent ordering, or per-view bucket coverage exists. |

## Implementation Stages

### Stage 0: Freeze effective policy and baseline

- [ ] Record every material identity resolution, StaticMesh section iteration,
  shader macro, pipeline-cache, public-RHI initializer, Vulkan mapping, and
  rendered-output call site affected by M2.
- [ ] Record current Scene Color/depth formats, load/store order, viewport and
  scissor behavior, output alpha expectations, and present/offscreen paths.
- [ ] Freeze the prepared-item fields, three bucket owners, item validity
  checks, fallback timing, and per-view lifetime.
- [ ] Freeze the blend table, depth override table, mask expression and edge
  comparison, cull/winding mapping, and Solid/Wireframe composition.
- [ ] Freeze the translucent metric, invalid-bounds fallback, stable tie-break,
  and accepted center-sorting limitation with representative fixtures.
- [ ] Enumerate the minimal RHI enum/descriptor values and every initializer,
  equality/hash, diagnostic, failure-injection, backend, and test migration.
- [ ] Identify focused baselines for opaque output, material fallback,
  multi-section binding, mirrored transforms, main/auxiliary views,
  fixed-aspect output, resource retry, and Vulkan validation.
- [ ] Record baseline commit, working set, symbols, decisions, open questions,
  and validation outcome in the stage handoff.

#### Acceptance Gate

- Every material static-property combination maps to one pass, shader policy,
  effective RHI state, pipeline key, and validation owner.
- No unresolved blend, mask, depth, cull, winding, sort, alpha-output, or
  M2/M3/M6 boundary choice remains before RHI changes begin.
- Baseline focused tests pass or pre-existing failures are recorded without
  attribution to M2.

### Stage 1: Introduce value-based graphics state

- [ ] Add the selected rasterizer, depth, and color-blend value descriptors to
  the public RHI graphics-pipeline initializer.
- [ ] Replace current Boolean call sites atomically and preserve behavior for
  StaticMesh, SkyBox, post-process, editor assistance, and test pipelines.
- [ ] Include every descriptor value in pipeline equality/hash or backend cache
  identity and in relevant diagnostic identity text.
- [ ] Map the descriptors exactly into Vulkan rasterization, depth/stencil, and
  color-blend attachment state.
- [ ] Reject invalid or unsupported descriptor combinations before publishing a
  graphics pipeline.
- [ ] Extend public-RHI and Vulkan focused tests for opaque and straight-alpha
  blend, depth write/compare, no/back cull, clockwise/counter-clockwise winding,
  solid/wireframe, cache separation, and nullable failure.
- [ ] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Renderer and backend contain no migrated feature Boolean whose meaning is
  duplicated by the new descriptors.
- Distinct effective state produces distinct compatible pipelines, while equal
  state retains existing Renderer/backend reuse behavior.
- Existing fixed-state output and complete-or-null failure behavior remain
  unchanged before material policies are enabled.

### Stage 2: Prepare and classify StaticMesh section work

- [ ] Add the Renderer-private prepared section item and view-local Opaque,
  Masked, and Translucent buckets.
- [ ] Resolve each valid LOD 0 section material exactly once before bucket
  insertion, preserving ErrorMaterial fallback and per-slot binding behavior.
- [ ] Derive effective blend, depth, cull, winding, raster, shader, and pipeline
  identity from the material, SceneInfo transform, and view settings.
- [ ] Compute finite section-center sort facts with the documented primitive-
  bounds and local-origin fallbacks.
- [ ] Keep M2 preparation free of frustum rejection, LOD choice, broad
  state-group sorting, persistent frame caches, and duplicate scene membership.
- [ ] Make execution consume prepared items rather than rescan typed SceneInfo
  collections or re-resolve pass identity.
- [ ] Add focused multi-section classification, fallback, replacement,
  removal, mirrored parity, deterministic membership, and independent
  multi-view preparation tests.
- [ ] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Every accepted section appears exactly once in its selected bucket with one
  complete resolved material and effective pipeline identity.
- Preparation retains no component, asset, or reflected object and does not
  outlive the render command that owns its borrows.
- Opaque-only scenes preserve existing draw count, ordering, fallback,
  lifecycle, and output before Masked/Translucent behavior is enabled.

### Stage 3: Execute Opaque and Masked policies

- [ ] Execute Opaque then Masked buckets with blending disabled and the resolved
  depth-write, cull, winding, and raster state.
- [ ] Add the masked shader coverage branch using the static blend-mode and
  threshold identity without adding dynamic material lookup.
- [ ] Preserve the exact OpacityMask constant/texture/UV/sampler binding and
  threshold equality behavior.
- [ ] Ensure one-sided normal and mirrored transforms cull the intended authored
  face while two-sided materials cull neither face.
- [ ] Keep Lit/Unlit and Solid/Wireframe output orthogonal to mask, cull, and
  depth policy.
- [ ] Add focused threshold-below/equal/above, textured mask, depth-enabled/
  disabled, front/back/mirrored face, fallback, main/auxiliary, fixed-aspect,
  and Vulkan readback/image coverage.
- [ ] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Masked fragments change color and depth coverage exactly at the documented
  threshold while Opaque output remains unchanged.
- Two-sided and mirrored one-sided behavior is correct in Solid and Wireframe
  under both Lit and Unlit views.
- Pipeline creation, shader reload, resource retry, and Vulkan validation remain
  clean for both buckets.

### Stage 4: Execute deterministic Translucent policy

- [ ] Sort Translucent items by descending documented distance and stable
  primitive/section tie-break for each view independently.
- [ ] Execute Translucent after Opaque and Masked with straight-alpha state,
  depth test, and resolved Automatic/Enabled/Disabled depth writes.
- [ ] Preserve source alpha through Lit and Unlit shading and verify the defined
  Scene Color alpha equation.
- [ ] Validate overlapping distances, equal-key ties, multiple primitives,
  multiple sections/materials, camera motion, mirrored/two-sided state, and
  remove/add recreation.
- [ ] Validate main/auxiliary cameras with different transforms and dimensions,
  present/offscreen targets, fixed-aspect scissor, post-process, editor
  assistance, Solid/Wireframe, and resource invalidation/retry.
- [ ] Add Vulkan readback/image baselines for blend color/alpha, order,
  occlusion by opaque/masked depth, and all three depth-write policies.
- [ ] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Translucent output matches the documented blend and depth equations and is
  deterministic for all stable-key fixtures.
- Each view prepares its own correct order; no camera or size state leaks into
  another view.
- Opaque and Masked coverage/order remain unchanged when translucent work is
  present or absent.

### Stage 5: Consolidate contracts and qualify M2

- [ ] Remove obsolete fixed-state, unclassified section-loop, duplicate
  material-resolution, and feature-Boolean paths.
- [ ] Add diagnostics or focused assertions for invalid pass kind, incomplete
  prepared item, non-finite sort key, unsupported RHI state, bucket mismatch,
  and execution outside the owning view lifetime.
- [ ] Update Runtime Rendering documentation with lasting pass, mask, blend,
  depth, cull/winding, preparation, ordering, and RHI-state contracts.
- [ ] Update both Rendering Capability Expansion and Material System roadmaps
  with M2/milestone-4 completion evidence and retained M3/M6 boundaries.
- [ ] Run focused Material, StaticMesh, Renderer, shader, public-RHI, and Vulkan
  suites; relevant main/auxiliary/offscreen rendering tests; plan validation;
  and a successful full `all` build using repository guidance.
- [ ] Record the final handoff with baseline commit, working set, symbols,
  decisions, open questions, and validation results.

#### Acceptance Gate

- Opaque, Masked, and Translucent have distinct documented and tested visible
  behavior for every current material static property.
- StaticMesh execution consumes complete prepared section items through
  authoritative typed scene input without a second scene registry or RTTI scan.
- Public RHI, Vulkan mapping, Renderer pipeline keys, and diagnostics agree on
  all effective graphics state.
- Existing material/resource lifetimes, fallback, multi-view behavior,
  viewport composition, recovery, and Vulkan validation remain clean.
- The Rendering Capability Expansion M2 exit gate is satisfied, and M3/M6 can
  extend preparation and auxiliary passes without redefining base surfaces.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Classification | Mixed sections, fallback material, static-property replacement, invalid section/material | Every valid LOD 0 section enters exactly one complete bucket item. |
| Mask coverage | Constant/texture mask below, equal, above threshold; UV/sampler variants | Color and depth coverage match the static threshold in all view modes. |
| Blend policy | Known source/destination colors and alpha; zero/partial/full opacity | Vulkan readback matches straight-alpha color and alpha equations. |
| Depth policy | Automatic/Enabled/Disabled across three blend modes and overlapping geometry | Occlusion and later draws observe the documented depth writes. |
| Cull/winding | Front/back faces, two-sided, normal/mirrored transform, solid/wireframe | The authored visible side remains stable under transform parity. |
| Ordering | Near/far/equal/intersecting centers, multiple sections/materials, remove/add, camera motion | Translucent items use deterministic per-view back-to-front order. |
| Multi-view | Main/auxiliary, differing cameras/dimensions/settings, fixed aspect | Preparation and sorting do not leak between sequential views. |
| RHI/Vulkan | Descriptor validation, equality/hash, pipeline separation, exact Vulkan mapping | Every effective state creates a compatible complete pipeline or null. |
| Failure/lifetime | ErrorMaterial, shader/PSO/sampler failure, retry/invalidation, proxy replacement/removal, scene release | Failure stays item-local and no prepared borrow survives its command. |
| Composition | Lit/Unlit, Solid/Wireframe, present/offscreen, post-process, editor assistance | Existing viewport order and output ownership remain intact. |
| Documentation/build | Changed/all-plan validators, focused suites, relevant Vulkan tests, full `all` build | Lasting contracts are authoritative and M2 is ready for dependents. |

## Definition of Done

- Every implementation stage passes its acceptance gate with a recorded
  handoff and validation evidence.
- Every current StaticMesh LOD 0 section is prepared once into Opaque, Masked,
  or Translucent work with complete material and effective pipeline state.
- Mask, blend, depth, cull, winding, view render mode, and raster mode have one
  visible meaning across main, auxiliary, present, offscreen, and fixed-aspect
  views.
- Translucent order is deterministic under the documented center metric and
  stable tie-break, with its intersecting-geometry limitation explicit.
- Public RHI exposes cohesive selected state, Vulkan maps it exactly, and all
  cache/diagnostic identities cover it.
- Existing material/static-mesh lifetimes, fallback, resource recovery,
  post-process/editor composition, and Vulkan validation remain qualified.
- Lasting behavior is moved to Runtime Rendering documentation, both roadmaps
  record completion, plan validation passes, and the required build/test
  evidence is recorded.

## Deferred Follow-ups

- M3 frustum culling, LOD selection, reusable prepared draw lists, state-group
  sorting, and performance counters.
- M6 depth-only/shadow-depth resources, caster policy, bias, filtering, and
  lighting sampling.
- Additional blend modes, alpha-to-coverage, dithered transitions, decals,
  transmission, refraction, and order-independent transparency.
- Per-triangle or per-pixel translucent ordering if measured content exposes
  unacceptable center-sort artifacts.
- Independent MRT blend/color masks when a selected deferred or effect pass
  supplies a concrete consumer.
- Material graph/pass permutation compilation in Material System milestone 5.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshMaterialTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
