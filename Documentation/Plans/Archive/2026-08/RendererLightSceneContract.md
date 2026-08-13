# Renderer Light Scene Contract Plan

Summary: Add renderer-owned directional, point, and spot light families with deterministic per-view selection and one bounded forward-light payload shared by StaticMesh and SkeletalMesh.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

Stage 4 qualification completed on 2026-08-12. The implementation owns directional,
point, and spot proxies, generic FIFO mutation, typed SceneInfo membership,
runtime actors/components, conservative local bounds, deterministic per-view
selection, conservation counters, and one reflected 320-byte forward payload
shared by StaticMesh and SkeletalMesh. CPU falloff references and focused scene,
material/Vulkan, and world tests pass. The representative 1920x1080 target-GPU
fixture measured a 1.376288 ms single-directional median and a 1.978080 ms
`1 + 4` median across 120 post-warm-up frames. Its 0.601792 ms incremental
Scene Color cost passes the frozen 1.0 ms gate.

The production budget is selected as at most one directional light and four
local lights per view, with point and spot lights sharing the local budget. The
original `1 + 8` candidate measured a 1.748 ms incremental Scene Color median
on the target GTX 1060 and was rejected against the 1.0 ms gate. A later
profiling plan may evaluate wider tiers, but none can replace the default
without recorded target-GPU evidence and a reviewed plan revision. The small
fixed payload establishes measured overflow evidence for any later per-object,
tiled, or clustered-lighting decision.

## Goal

Allow one immutable view render to consume renderer-owned snapshots for
directional, point, and spot lights, conservatively reject local lights outside
the view, deterministically select a bounded set, upload that set once, and use
the same light payload for StaticMesh and SkeletalMesh opaque, masked, and
translucent draws.

After the plan, light add/update/remove and component retirement are ordered
without render-thread object reads; zero-light, one-light, and multi-light
results are deterministic; point range and spot cone falloff have visible
meaning; and M6 can select a directional light from the prepared set without
reworking light ownership or view preparation.

## Scope

- Directional, point, and spot scene data, proxy kinds, proxy types, SceneInfo
  classification, typed `FScene` membership, and generic ordered mutation.
- Runtime point- and spot-light components and actors that publish detached
  proxies with stable `FLightSceneId` values and serializable authored values.
- Finite color, intensity, position, direction, range, and cone-angle
  normalization at the game/render boundary.
- Conservative local-light influence bounds and per-view frustum rejection.
- A command-local prepared-light view with deterministic ordering, fixed
  budgets, overflow handling, and conservation counters.
- One reflected, fixed-size forward-light uniform allocated once per view and
  shared by StaticMesh and SkeletalMesh base-pass draws.
- Additive PBR direct-light accumulation for all three families, including
  bounded point attenuation and spot cone attenuation.
- Main, auxiliary, window-backed, offscreen, fixed-aspect, Lit/Unlit,
  Solid/Wireframe, and editor-preview compatibility.
- Focused ownership, ordering, falloff, selection, ABI, shader, image, Vulkan,
  lifecycle, and multi-view validation.
- Lasting Runtime Rendering documentation and Rendering Capability Expansion
  roadmap status updates.

## Non-Goals

- Directional, point, or spot shadows. M6 owns the first directional shadow
  path; later evidence-gated plans own local-light shadows.
- Clustered or tiled light assignment, compute light culling, light volumes,
  deferred lighting, a GBuffer, or a render graph.
- Per-object light lists, probe blending, baked lighting, lightmaps, cookies,
  IES profiles, volumetric scattering, or photometric exposure/calibration.
- A public light-family or renderer-feature registration API.
- Light editor icons, gizmos, details customization, placement menus, or other
  editor-specific authoring UX beyond reflected runtime properties and normal
  actor/component serialization.
- Changing environment-lighting ownership or treating environment maps as
  entries in the direct-light budget.
- Redefining material shading models, adding new blend modes, or changing the
  established surface-pass ordering.
- A persistent GPU light buffer, bindless descriptors, storage-buffer light
  transport, or asynchronous upload work for the selected small budget.

## Design Decisions and Invariants

### Light families remain explicit scene classifications

- `ELightSceneProxyKind` has `Directional`, `Point`, and `Spot` values. It is
  independent of shadow participation, material shading, primitive family,
  and view/output policy.
- `FLightSceneProxy` is the detached Engine-facing base. Concrete proxies own
  copied family data; they never retain an actor, component, reflected asset,
  or editor object.
- One Renderer-private `FLightSceneInfo` owns the strong `FLightSceneId`, the
  proxy, explicit kind, and conservative influence bounds needed by view
  preparation. `FScene` maintains one authoritative map plus typed directional,
  point, and spot views.
- `IScene` exposes generic `AddOrReplaceLight` and `RemoveLight` mutation.
  Replacement atomically detaches old typed membership and publishes the new
  complete entry; a null/invalid proxy publishes nothing.
- Ordinary component mutations retain the existing single-producer FIFO render
  command order. No light revision is introduced because this plan adds no
  independently completed light work and no reclaimed GPU slot handle.

### Runtime publishers share lifecycle, not rendering state

- A common runtime light-component base owns the stable scene ID, color,
  intensity, registration/removal, hidden-owner behavior, property-change
  publication, and transform-dirty path. Concrete components build their own
  detached proxy values.
- Directional light publishes a normalized world direction. Point light
  publishes world position and finite positive range. Spot light publishes
  world position, normalized world direction, range, inner angle, and outer
  angle.
- Authored color channels and intensity are finite and non-negative. Local
  range is finite and greater than zero. Cone angles satisfy
  `0 <= inner <= outer < 90` degrees; equal angles use a documented hard edge
  rather than an undefined smooth-step denominator.
- Invalid authored/transient values are normalized before proxy publication or
  rejected as a complete candidate. The render thread never repairs values by
  reading their originating component.
- Point and spot actor/component types are runtime serialization owners only in
  this plan. Editor visualization and workflow remain separately selectable
  work.

### Per-view preparation is immutable and bounded

- `FPreparedLightView` is produced after fitting the immutable view and before
  the first Scene Color draw. It owns copied prepared values; draw execution
  does not retain `FLightSceneInfo*` or query `FScene` again.
- Directional lights have no spatial rejection. Point and spot lights use a
  conservative sphere centered at the light position with the authored range;
  a sphere outside the view frustum is rejected. Fine cone/frustum or
  per-object tests are deferred until counters justify them.
- The hard per-view budget is one directional light and four local
  lights total. Point and spot entries compete in one local budget so the
  shader cost cannot grow by filling two independent maxima.
- The current renderer has no per-object, tiled, or clustered assignment: every
  selected light is evaluated by every Lit fragment. The `1 + 4` limit is a
  pixel-cost control, not merely a uniform-buffer capacity choice.
- Eligible entries are ordered by `FLightSceneId` within the directional and
  combined-local lists. The first entries within each budget are selected;
  remaining entries are overflow-rejected. Scene map/vector iteration order is
  never an output decision.
- Each view records submitted counts by family, invalid/disabled rejection,
  frustum rejection, selected counts, overflow counts, and packed bytes.
  Conservation equations are asserted and included in the existing per-view
  counter snapshot.
- Sequential views prepare independently from the same scene snapshot. Target
  dimensions or prior view results never key or mutate light selection.

### The forward payload is one small per-view ABI

- The selected payload contains view position and counts, one directional
  record, and four packed local records. A local record contains
  position/inverse range, direction/type, color/intensity, and spot cone terms.
- CPU and Slang representations use explicit 16-byte fields and arrays, with
  compile-time size/alignment checks and shader-reflection coverage. Counts are
  integer fields; type and count data are not inferred from NaNs or sentinel
  vectors.
- Point and spot lights share one local-record layout. Point records use the
  explicit point kind and neutral cone terms; spot records use the spot kind
  and validated cosine-space cone terms.
- `FSceneRenderer` allocates one dynamic uniform range after light preparation
  and passes that same range to both geometry-family executors for the view.
  Draw code no longer allocates or rebuilds identical lighting data per
  section.
- Dynamic-uniform allocation retains the current prepared-page invariant and
  fail-fast behavior. The plan does not add a nullable allocation API; the
  resulting range is validated before binding and can never fall back to a
  previous view's lights.
- The lighting payload adds no persistent RHI resource, cache, retry slot, or
  invalidation owner. Existing shader/PSO invalidation remains with the Static
  and Skeletal feature renderers.

### Direct lighting has one deterministic meaning

- Directional contribution preserves the current sign convention: the proxy
  direction points along emitted light travel and the surface-to-light vector
  is its negation.
- Point and spot lights use the surface-to-light vector and an inverse-square
  term with a finite near-distance floor. A smooth range window reaches exactly
  zero at the authored range; fragments at the light position produce a finite
  fallback and never NaN/Inf.
- Spot angular attenuation compares the normalized light-to-surface direction
  with the normalized spot direction in cosine space. It is one inside the
  inner cone, zero at and beyond the outer cone, and smooth between them; equal
  angles form a deterministic hard edge.
- Each selected light calls the shared PBR direct-light function and
  contributions add before environment lighting and emissive. Environment
  lighting and ambient occlusion remain outside the direct-light loop.
- Zero selected lights produce zero direct contribution while preserving the
  established environment/emissive result. Unlit materials and Unlit view mode
  do not acquire direct-light work.
- Existing directional `AmbientIntensity` and editor-preview
  `RimLightIntensity` serialization are preserved as compatibility values but
  do not multiply or duplicate the new direct-light accumulation.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Scene ownership | Strong `FLightSceneId`, detached `FDirectionalLightSceneProxy`, `FLightSceneInfo`, FIFO mutation, and component-retirement coverage exist. | Generalize the proxy/SceneInfo kind contract and authoritative typed storage to point and spot without adding revisions or object reads. |
| Runtime publishers | Directional actor/component values serialize and recreate render state on registration, transform, property, and visibility changes. | Share lifecycle ownership and add production point/spot publishers and data validation. |
| View preparation | Primitive visibility and prepared geometry are command-local, immutable, deterministic, and counter-backed. | Add light-family preparation, local influence culling, budget selection, copied payload values, and conservation diagnostics. |
| Base-pass lighting | StaticMesh and SkeletalMesh use the same shader entry and PBR direct-light function. | Replace the single directional record and per-draw allocation with bounded multi-light accumulation and one per-view buffer. |
| RHI transport | Reflected dynamic uniform ranges, recorded parameter retention, prepared-page allocation, and Vulkan descriptor validation are stable. | Freeze and validate one fixed forward-light ABI; no new RHI resource type or compute path is needed. |
| Local-light appearance | PBR accepts an arbitrary normalized surface-to-light vector and radiance. | Define point range and spot cone attenuation, finite edge behavior, and image/readback fixtures. |
| Validation | Scene ownership, material PBR, static/skeletal rendering, multi-view counters, Vulkan execution, reload, and editor smoke fixtures exist. | Add multi-family mutation, overflow, falloff, shared-payload, zero-light, multi-view, and visible image evidence. |

## Implementation Stages

### Execution record (2026-08-12)

- Baseline commit: `deccf05d69c6856dee22447583bfb5048547261f` on `feat/render`.
- Baseline: `RendererSceneContractTests` 8/8, `MaterialTests` 78/78,
  `WorldTests` 97/97.
- Target: NVIDIA GeForce GTX 1060 6GB, 1920x1080, RHI GPU timestamp median
  after warm-up; accepted `1 + 4` incremental Scene Color cost is 1.0 ms across
  at least 120 frames.
- ABI: view `float4`, count `uint4`, two directional `float4` values, and four
  four-`float4` local records; 16-byte alignment, 320 bytes total, leaving
  4,193,984 bytes in the current 4 MiB Vulkan dynamic-upload page before other
  allocations.
- Falloff: near floor `0.05`, range window `(1 - (distance/range)^2)^2` for
  `distance < range` and zero otherwise; spot uses cosine-space smoothstep,
  with an equal-angle hard edge that excludes the exact boundary.
- Focused working results: `RendererSceneContractTests` 11/11,
  `MaterialTests` 79/79, and `WorldTests` 98/98.
- Qualification: default-granularity `test all` passed after one
  isolated Core concurrency rerun; `build --target all`, hidden eight-tick
  Sandbox editor smoke, documentation validation, and the 24-package DAST v4
  asset baseline passed on `Win64-Debug-DurinEditor`.
- Mixed-light image evidence: `SkeletalMeshRenderResourcesVulkanTests` renders
  the same Lit geometry with zero lights and with one directional, one point,
  and one spot light; readbacks differ and selected-family counters are 1/1/1.
- Lasting contract: [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md).

### Stage 0: Freeze light data, budgets, falloff, and baselines

- [x] Inventory every directional-light publisher, `IScene` mutation, SceneInfo
  consumer, prepared-view field, per-draw uniform allocation, shader parameter,
  serialization fixture, and renderer test double affected by generic light
  mutation.
- [x] Record the exact CPU and Slang field layout for the one-directional and
  four-local payload, including byte size, alignment, reflected binding,
  and the current backend uniform-range margin.
- [x] Name the target GPU, resolution, representative opaque/overdraw scene,
  GPU-timing method, and accepted incremental lighting-time budget. Record the
  current single-directional baseline before multi-light implementation.
- [x] Freeze finite-value normalization, local range semantics, cone angle
  semantics, inverse-square near floor, smooth range equation, angular equation,
  and exact boundary results at zero distance, range, inner cone, and outer
  cone.
- [x] Freeze local influence bounds, frustum-plane boundary behavior, stable-ID
  ordering, shared local-budget competition, and all conservation equations.
- [x] Record the existing one-directional-light pixels and zero-light fallback
  as compatibility baselines before changing the shader ABI.
- [x] Add or identify fixtures for mixed families, over-budget input, hidden and
  retired components, sequential unequal views, static and skeletal receivers,
  masked/translucent surfaces, range edges, and cone edges.
- [x] Record the baseline commit, affected targets, decisions, exceptions, and
  focused baseline results in the Stage 0 handoff.

#### Acceptance Gate

- The mutation API, scene field ownership, payload ABI, budgets, ordering,
  culling, falloff, failure behavior, and validation fixtures are exact with no
  unresolved design choice before C++ implementation.
- The fixed payload fits the current dynamic-uniform contract with documented
  margin, and no compute, storage-buffer, clustered, or new RHI dependency is
  required.
- Target hardware, resolution, workload, timing method, current single-light
  baseline, and the `1 + 4` acceptance budget are frozen before shader work.
- Existing scene-contract, material, StaticMesh, SkeletalMesh, world, and
  Vulkan baselines pass or any pre-existing failure is recorded.

### Stage 1: Generalize light publication and typed scene ownership

- [x] Add `ELightSceneProxyKind`, point/spot scene data and proxies, generic
  light-proxy access, and family-safe `FLightSceneInfo` classification/bounds.
- [x] Replace directional-only `IScene` add/remove/read APIs with generic
  `AddOrReplaceLight` and `RemoveLight` ownership transfer; update every test
  double atomically.
- [x] Make `FScene` maintain one identity map plus authoritative directional,
  point, and spot SceneInfo views with complete attach, replacement, rollback,
  detach, release, and wrong-kind diagnostics.
- [x] Introduce the common runtime light-component lifecycle and migrate
  directional light without changing its serialized properties, defaults,
  hidden-owner behavior, or output.
- [x] Add point- and spot-light actors/components with stable IDs, reflected
  authored values, transform/property publication, serialization, duplication,
  registration, removal, and scene-release behavior.
- [x] Add focused scene and world tests for every family, same-ID replacement,
  remove/add recreation, update-after-removal behavior, hidden state, invalid
  candidates, component retirement, and deterministic typed membership.
- [x] Record the stage handoff and focused validation evidence.

#### Acceptance Gate

- Every live light has exactly one Renderer-owned SceneInfo/proxy pair and one
  correct typed membership; replacement/removal leaves no stale membership.
- Render-thread light state contains no actor/component/asset pointer, and all
  three families obey the documented FIFO lifecycle without a new revision.
- Directional compatibility tests remain unchanged, while point and spot
  values and actors round-trip through runtime level serialization.

### Stage 2: Prepare deterministic per-view light sets and diagnostics

- [x] Add `FPreparedLightView` and a private preparation path that copies
  eligible directional, point, and spot proxy values before Scene Color.
- [x] Validate/normalize prepared inputs, conservatively frustum-cull point and
  spot influence spheres, and preserve explicit boundary inclusion.
- [x] Sort eligible directional and combined-local entries by stable identity,
  select the fixed budgets, and reject/count overflow without depending on map
  or insertion-vector order.
- [x] Extend `FViewRenderCounters`, snapshot emission, and focused observers
  with submitted, invalid/disabled, culled, selected, overflow, and byte counts
  by the selected family granularity.
- [x] Assert submitted-family and eligible/culled/selected/overflow conservation
  at preparation completion and after the command-local snapshot is emitted.
- [x] Cover perspective/orthographic views, boundary spheres, camera motion,
  hidden entries, mixed-family overflow, remove/add recreation, two sequential
  views with different dimensions/frusta, and repeated identical views.
- [x] Record the stage handoff and focused validation evidence.

#### Acceptance Gate

- Each view owns a complete immutable light list with at most one directional
  and four combined local entries, and all counters reconcile.
- Local lights outside the view do not enter the payload; boundary and overflow
  choices are deterministic across runs and independent of container order.
- Multiple sequential views consume the same scene snapshot without sharing or
  mutating prepared light results.

### Stage 3: Upload once and execute multi-light base-pass shading

- [x] Add the fixed CPU lighting-uniform representation, explicit layout
  assertions, packing from `FPreparedLightView`, one-time dynamic allocation per
  view, and range validation before binding.
- [x] Update the Slang lighting ABI and reflection declarations for integer
  counts, directional records, packed local records, and explicit point/spot
  family selection.
- [x] Move direct-light accumulation into shared shader helpers with the Stage
  0 range, inverse-square, cone, finite-fallback, and exact edge contracts.
- [x] Change StaticMesh and SkeletalMesh pass/draw APIs to consume the same
  prepared uniform range and remove all per-section light reconstruction and
  allocation.
- [x] Preserve shader/PSO key completeness, masked discard, translucent order,
  environment lighting, emissive, Unlit behavior, and all existing material
  texture bindings.
- [x] Add CPU reference tests for directional/point/spot accumulation and
  Vulkan image/readback scenes for zero/one/multiple lights, range/cone edges,
  mixed families, static/skeletal receivers, and view/output variants.
- [x] Profile the full-screen `1 + 4` representative scene on the Stage 0 target
  GPU and compare its incremental lighting time with the frozen budget. Record
  `2 + 16` only as deferred capacity; do not widen the implemented arrays or
  selection limit in this plan.
- [x] Exercise invalid uniform ranges, shader/PSO creation failure, manual
  retry, shader invalidation, device invalidation, and shutdown without a stale
  or previous-view payload fallback.
- [x] Record the stage handoff and focused validation evidence.

#### Acceptance Gate

- StaticMesh and SkeletalMesh visibly accumulate the same selected lights from
  one per-view payload in opaque, masked, and translucent execution.
- The current single-directional and zero-light baselines remain compatible;
  point range and spot cone edges match CPU references without NaN/Inf.
- The `1 + 4` representative scene meets the frozen target-GPU lighting-time
  budget; failure keeps M5 open and requires reducing the limit or selecting a
  bounded light-assignment optimization.
- Reflected ABI, descriptor lifetime, failure recovery, resource reload, and
  Vulkan validation are clean across main, auxiliary, present, and offscreen
  paths.

### Stage 4: Qualify M5 and publish the lasting contract

- [x] Remove the obsolete single-directional getter, prepared-view field,
  per-draw uniform code, and compatibility branches after every consumer and
  test double has migrated.
- [x] Publish light Proxy/SceneInfo ownership, family data, mutation, bounds,
  selection, budget, payload, falloff, diagnostics, and failure behavior under
  `Documentation/Runtime/Rendering/`.
- [x] Update the Rendering Capability Expansion roadmap with M5 completion and
  the exact M6 entry state; do not select shadow quality or memory budgets in
  this plan.
- [x] Run the focused native targets, relevant Vulkan integration targets,
  documentation validation, and the repository-required aggregate validation
  for the cross-target Engine/Renderer/RenderCore/shader change.
- [x] Because point/spot runtime actors and lighting output are user-visible,
  complete a successful full `all` build and validation-enabled editor smoke
  from the same Agent Build Profile, including a mixed-light static/skeletal
  scene, multiple view paths, reload, and orderly shutdown.
- [x] Record the final handoff with baseline, working set, symbols, decisions,
  open questions, test/build/runtime evidence, and verified editor executable.

#### Acceptance Gate

- M5's ownership, multi-view, budget, falloff, image, lifecycle, failure,
  diagnostics, Vulkan, build, and editor-smoke rows all pass.
- Directional, point, and spot lights are detached renderer-owned snapshots;
  the forward renderer uses one bounded prepared payload shared by both
  production geometry families.
- Lasting documentation is authoritative, the roadmap M5 exit gate is closed,
  and M6 can start after selecting one directional-shadow quality and memory
  budget without revisiting M5 scene or view contracts.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Add/update/remove by family | FIFO replacement, removal, remove/add recreation, and typed membership are deterministic | `RendererSceneContractTests` |
| Component/actor lifecycle | Reflected values serialize; registration, transform, visibility, duplication, retirement, and scene release publish no stale state | `WorldTests` and scene-contract tests |
| Invalid data | Non-finite color/intensity/position/direction/range/angles never publish a partial SceneInfo or GPU record | Engine and Renderer focused negative tests |
| Local visibility | Point/spot conservative spheres outside the frustum are rejected; plane-boundary spheres remain included | Renderer preparation tests |
| Budget and order | One directional and four combined local maxima; stable-ID selection and overflow counters are container-order independent | Renderer preparation/counter tests |
| Forward-light GPU cost | The `1 + 4` full-screen Lit fixture meets the recorded target-GPU budget; wider tiers remain evidence-gated | GPU timestamp/profile fixture and Stage 3 handoff |
| Multi-view | Sequential unequal views prepare independent lists and buffers from identical scene state | Renderer scene and viewport tests |
| Payload ABI | CPU size/alignment, Slang fields/counts, reflection, and descriptor range agree exactly | `RenderShaderContractTests` and Renderer tests |
| Directional compatibility | Existing one-light pixels and zero-direct-light fallback remain unchanged | material reference and Vulkan image/readback tests |
| Point attenuation | Finite center result, smooth in-range behavior, and exact zero at range match CPU reference | `MaterialTests` and Vulkan image/readback tests |
| Spot attenuation | Inner, transition, outer, equal-angle, transform, and behind-light cases match CPU reference | `MaterialTests` and Vulkan image/readback tests |
| Geometry/pass reuse | StaticMesh and SkeletalMesh opaque, masked, and translucent draws consume one shared prepared payload | Renderer and Vulkan integration tests |
| View/output policy | Lit/Unlit, Solid/Wireframe, main/auxiliary, present/offscreen, fixed aspect, post-process, and assistance ordering remain correct | viewport/editor rendering suites and smoke |
| Failure and reload | Invalid uniform ranges fail before binding; shader/PSO failure remains complete-or-null and retryable; invalidation cannot reuse a previous view's lights | Renderer reload Vulkan tests |
| Shutdown/lifetime | Recorded parameters retain required ranges until replay; scene/device/editor shutdown releases cleanly without device idle as a lifetime substitute | lifetime counters, Vulkan validation, and editor smoke |

## Definition of Done

- Directional, point, and spot runtime publishers create detached Renderer-owned
  proxy/SceneInfo entries with deterministic ordered lifecycle and no render-
  thread reflected-object access.
- Each immutable view conservatively culls local lights, selects no more than
  one directional and four combined local lights by stable identity,
  and emits reconciling diagnostics for every submitted entry.
- StaticMesh and SkeletalMesh share one reflected per-view lighting payload and
  accumulate deterministic directional, point, and spot PBR direct lighting in
  every established surface pass.
- Zero-light, single-directional compatibility, local falloff, cone edges,
  overflow, multi-view, failure/reload, Vulkan, full build, and editor smoke
  qualification pass.
- Lasting contracts are published, Rendering Capability Expansion M5 is marked
  complete, and the only remaining required roadmap milestone is M6.

## Deferred Follow-ups

- Directional shadow target, caster preparation, bias/filtering, sampling,
  cache/update policy, and selected shadow quality/memory budget (M6).
- Point and spot shadows, cookies, IES profiles, volumetrics, photometric units,
  exposure, and light-authoring UX, each behind a concrete product requirement.
- Light priority authoring or a contribution-based overflow policy if real
  scenes exceed the fixed budget and stable-ID overflow is visibly inadequate.
- A two-directional/sixteen-local forward tier only if target-GPU profiling
  meets a named lighting-time budget and a later plan revision accepts its
  worst-case opaque, masked, translucent, and overdraw cost.
- Per-object, tiled, or clustered light assignment if counters show that the
  fixed forward loop is a material GPU cost or required light counts exceed the
  accepted budget; clustered work also waits for Compute Renderer integration.
- Persistent/storage-buffer light transport only if a selected scalable path
  outgrows the small dynamic-uniform ABI.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../../../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../../../Runtime/Rendering/SkeletalMeshRendering.md)
- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Scene Proxy and Info Contract Plan](RendererSceneProxyAndInfoContract.md)
- [Per-View Visibility and LOD Plan](PerViewVisibilityAndLOD.md)
- [Skeletal Mesh Rendering Plan](SkeletalMeshRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Engine/Public/Engine/LightSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/Components/DirectionalLightComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/DirectionalLightComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Actors/DirectionalLightActor.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Public/PBRLighting.h`
- `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/Lighting/PBRLighting.slang`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/NewLevelBaselineTests.cpp`
