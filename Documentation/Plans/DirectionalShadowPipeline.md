# Directional Shadow Pipeline Plan

Summary: Add one bounded directional shadow-depth pass and forward-lighting sampling path shared by StaticMesh, SplineMesh, SkeletalMesh, and Terrain.

Last reviewed: 2026-08-13

Status: Completed
Completed: 2026-08-13

## Current Status

Stages 0-6 and this plan are complete. M2 through M5 of the
[Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
are stable, the production renderer already prepares deterministic view-local
geometry and one selected directional light, and the RHI exposes D32 depth
targets, sampled texture views, comparison samplers, depth bias, and explicit
attachment-to-shader-read transitions.

The selected entry candidate is one 2048x2048 D32 shadow map for the first
prepared directional light. It has a 16 MiB logical texture budget, covers at
most 256 world units from the receiving camera, uses one hardware linearly
filtered comparison sample, and is regenerated for every rendered view. The
target-GPU qualification gate is a median combined shadow-depth and base-pass
sampling increment no greater than 2.0 ms across 120 measured 1920x1080 frames
after warm-up on the NVIDIA GeForce GTX 1060 6GB. Stage 0 must freeze the exact
receiver/caster fitting math, depth-bias values, fixtures, and measurement
procedure before implementation proceeds; evidence may tighten or reject the
candidate only through a recorded plan revision.

The roadmap predates the production Terrain path. This plan deliberately adds
Opaque and Masked Terrain patches to the first shadow caster and receiver set
so that a current production geometry family does not become an undocumented
exception. Point/spot shadows, cascades, persistent shadow contents, and
translucent shadowing remain outside M6.

Stage 0 froze the production candidate at baseline commit
`0e755edb2d69e8c082be4478b797bffc03791951`. The affected inventory follows
the component -> `FDirectionalLightSceneData` -> Light Proxy/SceneInfo ->
`FPreparedLightView` path; `FForwardLightingUniform` and
`StaticMeshBasePass.slang`; the StaticMesh/Spline, SkeletalMesh, and Terrain
preparation/execution paths; `RenderTargetLayouts`; renderer coordinator
release/retry/device invalidation; `FViewRenderCounters`; RendererScene,
RendererSceneView, RHI resource-view/transition, Vulkan RHI, material, and the
three geometry-family Vulkan fixtures; and the main, auxiliary, preview,
offscreen/present, fixed-aspect, post-process, and editor-assistance view paths.

The frozen fitting contract reconstructs the eight Vulkan zero-to-one clip
corners from the inverse fitted view-projection matrix. Perspective far corners
are clamped along rays from `ViewLocation`; orthographic far corners are
clamped along each near-to-far segment. Both use the nearer authored far extent
or 256 world units. Light-space forward is the normalized authored direction
of travel, the preferred up axis is world +Z, and directions within `1e-4` of
parallel use world +Y. XY encloses the receiver with a two-texel guard on every
edge, then its center is snapped to whole 2048-map texels. The caster volume
uses the same guarded XY interval and extends 256 units opposite light travel
from the receiver minimum depth. Boundary contact and invalid finite bounds are
conservatively included, with invalid bounds counted separately. Degenerate or
non-finite inverse, basis, extent, or matrix construction disables the shadow
for that view rather than publishing an identity matrix.

The frozen raster/sample constants are fill mode; authored two-sided/mirrored
culling; `Less` depth write; constant bias `1.25`, slope bias `1.75`, clamp
`4.0`; receiver depth bias `0.0005`; and one linear `LessOrEqual` comparison
sample from a clamp-to-border opaque-white sampler. The clip-to-texture mapping
is `(x,y) * 0.5 + 0.5` with zero-to-one Z unchanged. Outside/non-finite
coordinates are fully lit. Opaque and Masked Local StaticMesh, SplineMesh,
SkeletalMesh, and Terrain cast; Translucent never casts; camera Wireframe does
not change filled shadow rasterization.

The qualification fixture is the NVIDIA GeForce GTX 1060 6GB (driver 560.94),
1920x1080, identical disabled/enabled scenes, 30 warm-up frames, then 120
measured frames. It reports Shadow Depth and Scene Color medians separately and
requires their combined enabled-minus-disabled median increment to be at most
2.0 ms. Failed/retry frames are excluded from the timing window and reported
separately. Logical target bytes are `2048 * 2048 * 4 = 16,777,216`; backend
allocation bytes are recorded independently and do not redefine this budget.
The no-shadow baseline owns zero shadow-target bytes.

Baseline evidence on 2026-08-13: the Debug `all` build and plan validator
passed; RHI resource-view 3/3, RHI transition 6/6, RendererSceneContract 12/12,
VulkanRHIIntegration 55/55, and the StaticMesh, SkeletalMesh, Terrain, and
Material Vulkan image targets 1/1 each passed. Existing reference images and
Vulkan validation were clean. The built editor remained healthy through an
eight-second smoke run. The implementation will add named shadow fixtures and
record target allocation/performance evidence in Stage 6.

Final qualification on 2026-08-13 passed the frozen GTX 1060 6GB fixture. A
30-frame warm-up followed by 120 measured 1920x1080 frames produced a disabled
Scene Color median of 24,608 ns, enabled Scene Color median of 30,560 ns,
Shadow Depth median of 25,120 ns, and combined increment of 31,072 ns (0.031
ms), below the 2.0 ms gate. Logical and Vulkan backend target allocations were
both 16,777,216 bytes. Focused RHI, shader ABI, scene/view, light, material,
StaticMesh/SplineMesh, SkeletalMesh, Terrain, resource reload, Cook, and Vulkan
tests passed; the 53-target `fast-all` set, Debug `all`, Shipping DurinGame
`all`, all-plan validation, and the final editor smoke passed. Lasting behavior
is documented in [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md).

## Goal

Render one deterministic directional shadow map before Scene Color, sample it
while evaluating the selected directional light, and preserve the current
forward renderer's scene ownership, view isolation, material policy, resource
recovery, and viewport behavior.

After this plan, StaticMesh, SplineMesh, SkeletalMesh, and Terrain Opaque or
Masked surfaces cast and receive directional shadows through their existing
vertex deformation and material contracts. Masked shadow coverage matches the
base pass, camera-visible receivers can be shadowed by relevant off-camera
casters, sequential views cannot consume one another's shadow contents, and
all feature failure paths fall back to the existing unshadowed directional
lighting result without a whole-device idle wait.

## Scope

- One renderer-owned shadow configuration for the first directional light
  selected by `FPreparedLightView`, including an authored enable value copied
  through the existing Light Proxy/SceneInfo snapshot.
- One fixed-resolution D32 depth texture with sampled and depth-attachment
  views, a comparison sampler, a depth-only render-target layout, and explicit
  access/layout transitions.
- One camera-relative receiver volume, deterministic light-space orthographic
  projection, texel stabilization, conservative caster search volume, and
  value-only prepared shadow view.
- Independent light-space visibility and LOD preparation. Camera visibility is
  not reused as caster visibility because off-camera primitives can affect
  visible receivers.
- Opaque and Masked shadow participation for Local StaticMesh, SplineMesh,
  SkeletalMesh, and Terrain vertex domains. Translucent sections do not cast in
  this milestone.
- Authored two-sided state, mirrored winding, skeletal palette deformation,
  spline deformation, terrain height deformation, and exact material opacity
  mask/threshold behavior in shadow depth.
- Directional-light shadow sampling in the shared forward-lighting contract;
  only the selected directional direct-light contribution is attenuated.
- Per-view shadow submitted, culled, prepared, resource, draw, triangle,
  masked, target-byte, failure, and GPU-time diagnostics.
- Main, auxiliary, present, offscreen, fixed-aspect, Lit/Unlit,
  Solid/Wireframe, post-process, and editor-assistance compatibility.
- Shader reload, manual retry, device invalidation, scene release, resize,
  sequential-view reuse, and normal renderer shutdown behavior.
- Focused RHI, Vulkan, Renderer, material, geometry-family, image, performance,
  full-build, and editor-smoke qualification.

## Non-Goals

- Cascaded shadow maps, virtual shadow maps, variance/moment maps, contact
  shadows, ray-traced shadows, screen-space shadows, or temporal shadow
  accumulation.
- Point-light or spot-light shadows, omnidirectional targets, shadow atlases,
  or per-light scheduling beyond the selected directional light.
- Persistent shadow contents, dirty-region updates, scene/light revision
  tracking, cross-frame temporal history, or a cache keyed by scene identity.
- Percentage-closer filtering kernels wider than the selected single hardware
  linear comparison sample, blocker search, PCSS, or filter-radius authoring.
- Translucent, colored, dithered, or opacity-weighted shadow casting.
- A public pass registry, Render Graph, deferred renderer, clustered lighting,
  GPU-driven submission, meshlets, or asynchronous compute.
- General material shader authoring, new blend modes, or changes to the
  established Opaque, Masked, and Translucent base-pass meaning.
- A permanent second visibility, material, resource, or vertex-factory system
  created only for shadows.
- A user-facing global shadow-quality menu. The first production tier is a
  frozen renderer contract; later tiers require measured product evidence.

## Design Decisions and Invariants

### Selection and authored state

- `FPreparedLightView` remains the authoritative per-view light selection. At
  most its first directional entry can own the shadow for that view; overflow
  directional lights neither render nor sample shadow state.
- Directional-light scene data gains a finite value-owned `bCastShadows`
  property, defaulting to enabled for ordinary runtime lights. It crosses the
  existing component -> proxy -> SceneInfo -> prepared-light FIFO boundary and
  never requires a render-thread component read.
- The absence of a selected directional light, disabled shadow casting, an
  invalid direction, or unavailable shadow resources produces the existing
  unshadowed light payload. Zero-shadow behavior is explicit and fully lit,
  never an unbound descriptor or partially initialized matrix.
- M6 adds no per-primitive authored shadow toggle. Opaque and Masked pass
  policy supplies caster eligibility; a later product requirement may add a
  detached primitive flag without changing pass ownership.

### Selected quality, memory, and performance tier

- The entry candidate is one 2048x2048, one-mip, one-layer, one-sample D32
  texture. Its logical texel storage is exactly 16 MiB. Runtime diagnostics
  also record the backend allocation when that value is available; hidden
  alignment must not be represented as logical texel bytes.
- The map covers the camera receiver region out to at most 256 world units and
  searches conservatively for casters along the incoming light direction. The
  exact receiver reconstruction, caster extrusion, guard band, and degenerate
  fallback are frozen in Stage 0 golden tests.
- Sampling uses a D32 sampled view plus a clamp-to-border comparison sampler
  with an opaque-white border and `LessOrEqual` comparison under Durin's
  ordinary zero-to-one forward-depth convention. One linear comparison sample
  is the entire M6 filter tier.
- The qualification fixture measures a shadow-disabled baseline and the
  shadow-enabled candidate under identical scene, view, light, build, GPU,
  resolution, warm-up, and 120-frame capture conditions. The median combined
  increment must be no greater than 2.0 ms on the GTX 1060 6GB at 1920x1080.
  Shadow-depth and Scene Color timings are also reported separately.
- A quality candidate that misses either the 16 MiB logical target budget or
  the 2.0 ms performance gate cannot become the default. Stage 0 or Stage 6
  records the replacement tier and repeats all affected qualification.

### Shadow matrix and visibility

- The receiving volume is derived from the fitted `FSceneView`, not raw output
  dimensions, so fixed-aspect black bars do not enlarge or shift the shadow.
  Perspective and orthographic views have explicit golden inputs.
- Camera frustum reconstruction uses the existing zero-to-one clip convention.
  The far receiving extent is clamped to the selected shadow distance while
  respecting a nearer authored camera far plane. Invalid or non-invertible
  view/projection data disables the shadow for that view and increments a
  diagnostic; it does not publish an identity shadow matrix.
- The light view uses a deterministic up-axis fallback for directions parallel
  to the preferred axis. The orthographic XY extent encloses the receiver
  volume plus the frozen filter/precision guard band. Its XY origin is snapped
  to whole shadow texels before the world-to-shadow matrix is published.
- Caster discovery starts from authoritative typed SceneInfo collections and
  uses conservative world bounds against a light-space caster volume. It never
  starts from the camera-visible primitive lists. Boundary-contact bounds
  remain included; invalid finite-bounds cases use the documented conservative
  inclusion or rejection counter selected in Stage 0.
- StaticMesh shadow LOD uses the existing deterministic projected-size policy
  evaluated for the shadow view and its 2048x2048 viewport. SplineMesh follows
  the same static resource selection, SkeletalMesh remains at its current LOD0
  contract, and the current Terrain path remains single LOD. Shadow selection
  never mutates the camera base-pass LOD result.
- A culling-disabled shadow comparison seam remains available to focused tests
  and diagnostics. It is immutable per preparation invocation and is not a
  process-global renderer toggle.

### Pass and material behavior

- `FDirectionalShadowRenderer` is a Renderer-private feature owner composed
  explicitly by `FSceneRenderer`. It owns shadow shaders, pipelines, sampler,
  target/view slots, failure state, invalidation, retry, and release.
- All shadow draw resources are prepared before `BeginRenderPass`. Shadow pass
  execution consumes only complete immutable prepared draws and performs no
  asset/component discovery or resource creation.
- The pass runs before Scene Color. It has no color attachment, clears depth to
  1.0, writes D32 with `Less`, stores the result, and ends in
  `ShaderReadOnly`/`GraphicsShaderRead` for base-pass sampling.
- Opaque draws need no material color evaluation. Masked draws use the same
  resolved opacity, opacity-mask texture, sampler, scalar composition, UVs,
  and strict threshold comparison as their base pass. Unsupported or
  incomplete material bindings use the existing ErrorMaterial/failure policy
  rather than a shadow-only approximation.
- Shadow depth always rasterizes filled triangles even when the camera view is
  Wireframe. The camera's Solid/Wireframe choice affects visible Scene Color,
  not physical caster coverage.
- Authored two-sided state and mirrored-transform front-face selection remain
  effective. Shadow depth forces depth test and write, disables color writes,
  disables blending, and applies the frozen constant/slope bias. These values
  participate in complete shadow PSO identity.
- Local StaticMesh, SplineMesh, SkeletalMesh, and Terrain keep their existing
  vertex deformation math. Skeletal shadow and base execution share the
  frame-local uploaded palette range; the shadow pass does not charge a second
  palette allocation for the same prepared pose.
- Translucent draws never enter the M6 shadow-depth list, even when authored to
  write base-pass depth. Receiver shading may still sample a shadow when the
  translucent material is Lit; that changes only the selected directional
  direct-light term and preserves established blending and depth behavior.

### Forward-lighting sampling

- The forward-lighting ABI gains one complete directional-shadow record:
  world-to-shadow matrix, enabled flag, texel/filter facts required by the
  selected tier, and any frozen receiver bias. CPU layout, Slang reflection,
  alignment, byte size, and backend uniform-range margin are asserted together.
- The sampled texture and comparison sampler bind beside the shared per-view
  lighting uniform. StaticMesh, SkeletalMesh, and Terrain use one common shadow
  helper; no geometry family owns a divergent shadow equation.
- World positions outside the shadow XY range, before the near depth, or past
  the far depth are fully lit. The helper rejects non-finite projected values
  and never samples an undefined coordinate.
- Only the selected directional light's direct BRDF contribution is multiplied
  by the shadow factor. Ambient, environment lighting, emissive, point/spot
  lights, editor rim assistance, and Unlit output are unchanged.
- Shader and descriptor identity includes shadow participation. A disabled or
  failed shadow uses a complete fallback binding and valid zero-shadow uniform,
  not optional stale descriptor state from an earlier view.

### Multi-view, lifetime, and failure

- One fixed-resolution target may be reused by sequential main, auxiliary,
  preview, present, and offscreen views. Every shadow-enabled view clears and
  regenerates the contents before its Scene Color pass; no view may sample
  contents prepared for another view.
- Fixed-aspect bars remain outside Scene Color, post-process, and editor
  assistance and do not affect shadow target dimensions. Resize does not
  recreate the fixed shadow target, but the fitted receiving volume is
  recomputed for the new view.
- Target, view, sampler, shader, and PSO creation are complete-or-null.
  Allocation failure publishes no cache tombstone and remains retryable through
  the established resource-slot policy. Shader reload retains last-known-good
  resources until a complete replacement publishes.
- A failed shadow feature never makes otherwise valid Scene Color fail.
  The renderer binds the complete fully-lit fallback, records the failure, and
  continues with unshadowed directional lighting.
- Device invalidation and renderer release destroy shadow-owned resources
  through the existing coordinator/deferred RHI lifetime. The plan adds no
  `WaitIdle`, command-list flush, or whole-device synchronization to ordinary
  invalidation, view rendering, or shutdown.
- Recorded commands retain every shadow target/view, sampler, pipeline,
  material texture/sampler, geometry buffer, and palette range required after
  recording. CPU prepared views remain command-local and do not outlive their
  owning borrowed scene/render-data protocol.

## Current Foundations and Gaps

| Area | Existing foundation | M6 gap |
| --- | --- | --- |
| Light ownership | Directional, point, and spot values are detached Proxy/SceneInfo snapshots; one directional light is deterministically selected per view. | Directional scene data has no shadow enable value, shadow matrix, or depth resource binding. |
| Geometry preparation | StaticMesh/SplineMesh, SkeletalMesh, and Terrain have typed SceneInfo inputs, deterministic base-pass buckets, complete resource preparation, and counters. | There is no independent light-space caster visibility, shadow LOD, caster list, or shared auxiliary-pass draw contract. |
| Material policy | Opaque, Masked, and Translucent behavior, two-sided state, mirrored winding, texture/sampler resolution, and strict mask threshold are production paths. | Masked coverage has no depth-only shader/pass proof; Translucent exclusion is not yet represented by a shadow list. |
| Vertex domains | Local, spline, skeletal, and terrain deformation execute in production base passes; skeletal palettes are bounded frame-local ranges. | Shadow depth needs the same deformations without duplicating palette uploads or base-pass renderer ownership. |
| RHI state | D32, depth attachments, sampled views, comparison samplers, value raster/depth state, depth bias, explicit layouts/access, and depth-only render-target validation primitives exist. | No production depth-only target layout and no Vulkan write-transition-compare-sample qualification exist. |
| Scene orchestration | `FSceneRenderer` completes all preparation before Scene Color and composes Renderer-private feature owners with coordinated invalidation. | There is no shadow owner or pass between view preparation and Scene Color, and shadow failure must become non-fatal. |
| Lighting shader | One reflected 320-byte forward payload is uploaded once and shared by all production geometry families. | The ABI and shared lighting helpers do not bind/project/sample a shadow map. |
| Targets and memory | Scene targets use bounded caches and explicit logical counters; fixed-size renderer resources use creation slots. | No 16 MiB shadow target accounting, backend allocation evidence, or sequential-view content rule exists. |
| Validation | Renderer/RHI/Vulkan tests cover view math, resource views, transitions, materials, static/skeletal/terrain rendering, invalidation, reload, multi-view output, and readback. | No shadow math goldens, depth sampling proof, masked caster image, off-camera caster case, or target-GPU shadow profile exists. |

## Implementation Stages

### Stage 0: Freeze quality, fitting, bias, fixtures, and baselines

- [x] Inventory every affected directional-light publisher, Proxy/SceneInfo
  value, prepared-light field, forward-light uniform, shader declaration,
  geometry-family draw path, render-target layout, invalidation hook, counter,
  test double, serialization fixture, and editor/runtime viewport path.
- [x] Freeze the 2048x2048 D32, 16 MiB, 256-unit, one-linear-comparison-sample
  candidate and document exact logical/backend byte accounting.
- [x] Freeze perspective and orthographic receiver reconstruction, camera far
  clamping, caster extrusion, guard band, preferred/fallback light up axes,
  texel snapping, clip-to-texture transform, and invalid-matrix fallback with
  numeric golden cases.
- [x] Select constant depth bias, slope bias, clamp, receiver bias, comparison
  direction, and border result using acne, peter-panning, grazing-angle,
  contact, mirrored, two-sided, thin masked, and large-coordinate fixtures.
- [x] Freeze caster policy: Local StaticMesh, SplineMesh, SkeletalMesh, and
  Terrain Opaque/Masked participate; Translucent does not; shadow rasterization
  remains filled under camera Wireframe.
- [x] Freeze the GTX 1060 fixture, baseline/candidate toggles, 1920x1080 output,
  warm-up, 120-frame measurement window, separate Shadow Depth/Scene Color
  timestamps, combined 2.0 ms median gate, and failure/retry measurement rules.
- [x] Record baseline focused test counts, full-build status, Vulkan validation
  status, reference images, logical/actual target bytes, and clean editor smoke.

#### Acceptance Gate

- The entry quality and memory candidate, matrix convention, caster volume,
  bias/filter values, geometry participation, fixtures, counters, and profile
  method are unambiguous and reproducible. No Stage 1 code begins while two
  incompatible fitting, comparison, or failure policies remain open.

### Stage 1: Qualify sampled shadow-depth RHI contracts

- [x] Add a depth-only shadow render-target layout with D32 clear/store,
  `Undefined`/`None` entry, and `ShaderReadOnly`/`GraphicsShaderRead` exit.
- [x] Validate creation of a single-sample D32 texture carrying both
  `DepthStencilTargetable` and `ShaderResource`, plus exact sampled and
  depth-attachment views.
- [x] Create and validate the frozen clamp-to-border, opaque-white,
  `LessOrEqual`, linear comparison sampler contract through public RHI values
  and Vulkan mapping.
- [x] Prove a no-color depth pipeline with depth write/test, color-write
  absence, complete raster/depth-bias PSO identity, and nullable creation.
- [x] Add focused transition validation for depth attachment writes followed by
  fragment-shader comparison reads, including repeated sequential write/read
  cycles and incompatible access/usage rejection.
- [x] Add Vulkan readback or sampled-output coverage that distinguishes lit,
  shadowed, boundary, and opaque-white-border comparison outcomes under the
  selected zero-to-one convention.

#### Acceptance Gate

- Public RHI and Vulkan tests deterministically write and comparison-sample the
  selected D32 resource, reject invalid descriptors/transitions, preserve full
  PSO equality/hash behavior, and complete with validation layers clean.

### Stage 2: Prepare one deterministic directional shadow view

- [x] Extend directional scene/prepared data with detached authored shadow
  enable state and update add/replace/remove, serialization, property-change,
  component-retirement, and test-double coverage.
- [x] Add value-only shadow configuration, selected-light identity, receiver
  volume, caster volume, light view/projection/world-to-shadow matrices, texel
  scale, and fallback state to the prepared scene view.
- [x] Implement the frozen perspective/orthographic receiver reconstruction,
  light-axis fallback, orthographic fitting, guard band, texel snapping, caster
  extrusion, and finite-value validation with Stage 0 golden cases.
- [x] Classify authoritative StaticMesh/SplineMesh, SkeletalMesh, and Terrain
  SceneInfo inputs independently against the shadow caster volume; keep a
  culling-disabled comparison seam and conserved submitted/culled/rejected
  outcomes.
- [x] Select shadow-view LOD/resources and build immutable Opaque/Masked caster
  records with stable sort/state keys, material snapshots, vertex-domain facts,
  primitive identity, and triangle counts.
- [x] Demonstrate that an object outside the camera frustum but inside the
  conservative caster volume is prepared, while a caster unable to affect the
  receiving volume is rejected.

#### Acceptance Gate

- Matrix and visibility tests reconcile every candidate, camera and light
  motion is deterministic at sub-texel and texel boundaries, relevant
  off-camera casters survive, shadow preparation does not mutate base-pass
  visibility/LOD, and no component or asset object is read on the render
  thread.

### Stage 3: Execute Opaque and Masked shadow depth

- [x] Add Renderer-private `FDirectionalShadowRenderer` ownership for target,
  views, sampler, shaders, pipelines, material resources, creation/retry state,
  counters, invalidation, and release.
- [x] Add Local/Spline, Skeletal, and Terrain shadow vertex entry points that
  reuse production deformation and coordinate conventions without copying base
  renderer orchestration.
- [x] Add an Opaque depth-only program and a Masked depth program whose opacity
  composition, textures, samplers, UVs, and strict threshold match the base
  material contract.
- [x] Prepare every shader, pipeline, material resource, geometry binding,
  height resource, and palette range before the shadow render pass begins;
  execution consumes only `ResourcesPrepared` draws.
- [x] Share one frame-local skeletal pose palette allocation between shadow and
  base-pass execution and prove no duplicate upload or range-budget charge.
- [x] Execute the shadow pass before Scene Color with frozen filled raster,
  cull/winding, depth write/test, bias, viewport/scissor, clear/store, and final
  shader-read state.
- [x] Add focused execution counters and deterministic images for Opaque,
  Masked, mirrored, two-sided, spline-deformed, GPU-skinned, and terrain
  casters.

#### Acceptance Gate

- All selected geometry families emit correct D32 caster coverage; Masked
  pixels exactly match base-pass coverage; resource and draw counters conserve;
  Wireframe views retain filled shadows; palette uploads remain bounded; and
  Vulkan validation reports no render-pass, pipeline, descriptor, or lifetime
  errors.

### Stage 4: Sample the shadow in shared forward lighting

- [x] Extend the C++ and Slang forward-lighting ABI with one aligned shadow
  record and assert field offsets, total byte size, reflection bindings, and
  uniform-range margin.
- [x] Bind the sampled depth view, comparison sampler, and a complete fully-lit
  fallback through StaticMesh, SkeletalMesh, and Terrain base-pass resource
  preparation and execution.
- [x] Implement one shared world-to-shadow projection and comparison helper
  with finite checks, XY/depth range rejection, frozen bias, and fully-lit
  outside behavior.
- [x] Apply the result only to the selected directional direct-light term;
  preserve point/spot accumulation, ambient/environment, emissive, preview rim,
  Unlit, material surface, blend, depth, and output behavior.
- [x] Verify Lit Opaque, Masked, and Translucent receivers across Local/Spline,
  Skeletal, and Terrain paths, plus Unlit and zero/disabled/overflow
  directional-light cases.
- [x] Prove descriptor and shader identity cannot reuse shadow-enabled bindings
  for a later shadow-disabled or failed view.

#### Acceptance Gate

- All production geometry families share one shadow equation and produce
  deterministic receiver results. Disabled, absent, out-of-range, Unlit, and
  failed-shadow cases match the established unshadowed baseline, while only
  the selected directional direct-light contribution is visibly attenuated.

### Stage 5: Qualify multi-view, recovery, diagnostics, and lifetime

- [x] Render main, auxiliary, window-backed, offscreen, fixed-aspect, asset
  preview, and editor-assistance views sequentially at different dimensions
  and settings; verify each shadow-enabled view regenerates contents before
  sampling and bars/overlays remain outside the shadow composition.
- [x] Exercise camera/light transform changes, enable toggles, add/replace/
  remove ordering, selected-light changes, component retirement, primitive
  removal, material replacement, skeletal pose replacement, terrain revision,
  and scene release without stale shadow state.
- [x] Exercise target/view/sampler/shader/PSO/material/palette failure,
  last-known-good shader reload, manual retry, device invalidation, and renderer
  shutdown; verify fallback rendering and later recovery.
- [x] Publish conserved per-view shadow counters for light selection, receiver
  validity, submitted/culled/rejected/prepared casters by family, Opaque/Masked
  draws and triangles, resource attempts/results, target logical/actual bytes,
  fallback reason, and Shadow Depth/Scene Color GPU timing.
- [x] Verify recorded commands retain all referenced resources and ordinary
  mutation, invalidation, sequential rendering, and shutdown introduce no
  whole-device idle wait or renderer-thread read of reflected objects.

#### Acceptance Gate

- Sequential views never sample stale contents, every failure produces a
  complete unshadowed frame and remains diagnosable/recoverable, counters
  reconcile, resource releases are balanced, and Vulkan validation remains
  clean through reload, invalidation, scene teardown, and shutdown.

### Stage 6: Qualify the production tier and close M6

- [x] Run the frozen target-GPU fixture against identical disabled/enabled
  captures, report Shadow Depth, Scene Color, combined median increment, target
  logical/actual bytes, draws, triangles, and material/family mix across 120
  post-warm-up 1920x1080 frames.
- [x] Pass the 2.0 ms combined median gate and 16 MiB logical texture budget, or
  record and requalify a reviewed replacement tier before enabling production
  defaults.
- [x] Complete focused RHI, Vulkan, scene/view, light, material, static/spline,
  skeletal, terrain, renderer-resource, reload, image, and lifecycle coverage
  selected according to the repository testing guidance.
- [x] Complete Debug and Shipping qualification required by affected targets,
  the repository full `all` build, representative Cook/asset baseline, and a
  clean editor smoke with Vulkan validation enabled.
- [x] Publish lasting directional-shadow ownership, fitting, caster, material,
  shader, target, view, failure, diagnostics, and performance contracts under
  `Documentation/Runtime/Rendering/`.
- [x] Update the Rendering Capability Expansion roadmap with M6 completion,
  dispose every conditional branch as completed, active, or explicitly
  deferred with activation evidence, and close the roadmap only when its full
  completion criteria pass.

#### Acceptance Gate

- StaticMesh/SplineMesh, SkeletalMesh, and Terrain cast and receive stable
  deterministic directional shadows under the selected production tier;
  masked coverage, off-camera casters, camera/light motion, multi-view reuse,
  invalidation, Vulkan validation, full build, editor smoke, performance, and
  memory gates pass; lasting contracts are published and M6 is complete.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Directional-light lifecycle | Authored enable, add/replace/remove, transform, visibility, selection, retirement, serialization, and scene release remain FIFO and detached | `RendererSceneContractTests`, `WorldTests` |
| Matrix convention | Perspective/orthographic reconstruction, zero-to-one depth, fallback up axis, fitting, guard band, texel snapping, invalid matrices, and large coordinates match frozen goldens | Renderer shadow-math focused tests |
| Caster visibility | Camera-visible and relevant off-camera casters are included; irrelevant light-space bounds are culled; contacts remain conservative; comparison mode reconciles | Renderer shadow-preparation tests |
| LOD and vertex domains | Static/Spline shadow LOD is deterministic; Skeletal LOD0 and palette deformation, spline deformation, and terrain height deformation match production geometry | Renderer preparation tests and Vulkan images |
| Surface policy | Opaque writes depth, Masked matches strict base coverage and authored culling, Translucent does not cast, Lit translucent may receive | Material focused tests and Vulkan images |
| Sampled depth RHI | D32 dual usage, exact views, comparison sampler, depth-only layout, bias identity, attachment-to-read transition, and repeated cycles validate | `RHITests`, `VulkanRHITests` |
| Forward lighting | Only the selected directional direct-light term is shadowed; local lights, IBL, ambient, emissive, rim, and Unlit remain unchanged | Forward-lighting unit tests and mixed-light image readback |
| Multi-view/output | Main/auxiliary, present/offscreen, preview, different dimensions, fixed aspect, post-process, and editor assistance consume freshly generated view-local contents | Renderer scene-view tests and Vulkan sequential-view images |
| Failure recovery | Nullable target/view/sampler/shader/PSO/material/palette failure falls back fully lit; retry, reload, invalidation, and shutdown recover without stale bindings | Renderer resource/failure-injection and Vulkan reload tests |
| Counters and memory | Submitted outcomes conserve; draws/triangles/resources reconcile; 16 MiB logical and backend allocation values are explicit; timings identify both passes | Counter snapshot tests and target-GPU profile |
| Performance | Identical GTX 1060 captures stay within the frozen 2.0 ms combined median increment across 120 measured 1920x1080 frames | Stage 6 qualification record |
| Handoff | Focused suites, affected aggregate targets, full `all` build, representative asset/Cook baseline, and editor Vulkan smoke pass | Repository build/test guidance and Stage 6 evidence |

## Definition of Done

- The first prepared directional light owns one optional, detached, authored
  shadow state and no render-thread component or asset read is introduced.
- One qualified 2048x2048 D32 production shadow tier or a reviewed measured
  replacement passes its explicit memory and GTX 1060 performance budgets.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain Opaque/Masked geometry cast
  through shared scene/material/resource contracts, and all Lit production
  geometry families receive through one shared forward-lighting equation.
- Masked coverage, culling, mirrored winding, two-sided state, spline/skin/
  terrain deformation, off-camera casters, and camera/light motion have focused
  deterministic and Vulkan image evidence.
- Main, auxiliary, present, offscreen, fixed-aspect, preview, post-process, and
  editor-assistance paths remain isolated and correct under sequential target
  reuse.
- Failure, retry, shader/device invalidation, scene release, and shutdown are
  complete, diagnosable, resource-balanced, and require no whole-device idle.
- Per-view diagnostics explain shadow selection, fitting failure, visibility,
  LOD, pass, resource, draw, triangle, target-memory, fallback, and GPU cost.
- Lasting behavior is documented under Runtime Rendering, M6 is marked
  complete in the roadmap, conditional branches are explicitly dispositioned,
  and the Rendering Capability Expansion roadmap completion criteria pass.

## Deferred Follow-ups

- Cascaded directional shadows and wider PCF/PCSS filtering require a named
  outdoor-scale quality target, accepted multi-target memory, and target-GPU
  evidence from the single-map baseline.
- Persistent shadow caching requires measured repeated depth-pass cost plus a
  reviewed scene/light/primitive revision and lifetime model; target-size reuse
  is not content identity.
- Point/spot shadows require explicit light-count, resolution/atlas, update,
  memory, and selection budgets and do not inherit authorization from M6.
- Per-primitive cast/receive controls require a concrete authoring need and a
  detached mutation contract; the first milestone intentionally uses surface
  policy only.
- Translucent/colored shadows require a selected compositing model and material
  semantics rather than entry into the depth-only pass by accident.
- Render Graph, clustered lighting, GPU-driven caster submission, and
  asynchronous work remain independently evidence-gated roadmap branches.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Light Scene Contract Plan](RendererLightSceneContract.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Engine/Public/Engine/LightSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/Components/DirectionalLightComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/DirectionalLightComponent.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ViewPreparationMath.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/VertexFactory/SkeletalMeshVertexFactory.slang`
- `Engine/Shaders/Slang/VertexFactory/TerrainVertexFactory.slang`
- `Engine/Shaders/Slang/Lighting/PBRLighting.slang`
- `Engine/Tests/Native/RHITests/Private/RHIResourceViewValidationTests.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIResourceTransitionValidationTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
