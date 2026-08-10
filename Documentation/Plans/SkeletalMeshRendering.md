# Skeletal Mesh Rendering Plan

Summary: Render GPU-skinned SkeletalMesh primitives through the shared scene, visibility, material, pass, viewport, and resource-lifecycle contracts.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

This plan completed the shared Rendering Capability Expansion M4 and Skeletal
Mesh and Animation S3 milestone on 2026-08-11. SkeletalMesh is now the second
production geometry family: detached proxies publish coherent pose and
conservative-bound state, typed visibility feeds immutable prepared views, and
the shared material, pass, viewport, invalidation, and frame orchestration
contracts execute GPU-skinned sections without a parallel renderer or scene
RTTI scan.

The selected slice uses vertex-shader linear-blend skinning with four normalized
influences, one immutable palette snapshot per prepared primitive, and one
frame-local shader-readable storage range per distinct prepared palette.
Animated visibility uses conservative per-palette-bone influence bounds: render
data precomputes a bind-space bound for vertices affected by each palette entry,
then pose evaluation transforms and unions those bounds. This avoids per-frame
CPU vertex skinning while preventing deformation from escaping the culling
bound.

Stage 0 completed on 2026-08-10 against baseline `bf0613f8`. The repository
fixture now publishes independent CPU skinning and conservative-bound goldens
for both mesh instances at reference, interpolated, key, loop, and clamp
samples. The completed RHI plan handoff was re-inspected at `c50a7b2a`, plus
the drawn-range validation fix at `bf0613f8`.

Stage 1 completed on 2026-08-10. `FSkeletalMeshRenderData` now owns the exact
LOD0 geometry, material sections, palette metadata, per-entry influence bounds,
and seven-resource RHI lifecycle. `FSkeletalMeshVertexFactory` publishes a
ten-attribute/five-stream declaration with exact `UShort4` joint indices and
`Float4` weights. Asset initialization, DDC/cooked load, imported-state
exchange, retry, failure retention, fence-backed destruction, and Vulkan
release/reinitialize behavior are covered by `SkeletalAssetTests`,
`SkeletalSceneLifecycleTests`, and
`SkeletalMeshRenderResourcesVulkanTests`.

Stage 2 completed on 2026-08-10. Pose publications now contain matrices and a
finite conservative local bound computed from the render-data influence boxes.
The component resolves positional/name material overrides, creates a detached
skeletal proxy only from a complete pose, and publishes FIFO pose+bound
updates. Scene membership has a dedicated kind, typed collection/accessor,
atomic local/world-bound refresh, centralized visibility classification, and
skeletal candidate counters. `SkeletalAssetTests` and
`RendererSceneContractTests` cover component, material, typed membership,
visibility, transform, and revision/bound coherence.

Stage 3 completed on 2026-08-11. RHI now publishes exact storage alignment and
range limits and provides a bounded frame-local dynamic storage allocator in
inline and RHI-thread modes. Vulkan retains aligned per-frame arenas, rejects
range/frame/chunk overflow deterministically, records host-write state, and
requires the prepared palette range to transition to graphics shader read.
The skeletal vertex factory Slang module skins position, normal, and tangent
from one reflected scalar storage range. The production base-pass variant and
Vulkan validation cover shader reflection, PSO creation, exact range binding,
multiple influences, nonuniform deformation, pose replacement, readback, and
resource retry/release.

Stage 4 completed on 2026-08-11. `FPreparedSkeletalMeshView` freezes LOD0
geometry, transform, pose, palette range, materials, pass state, caster facts,
and complete sort keys before Scene Color. `FSkeletalMeshRenderer` owns its
shader/PSO/sampler retry slots and executes only resource-complete records.
Opaque and Masked remain family-grouped; a command-local combined translucent
list is prepared before the pass and orders both geometry families by distance
and complete stable ties. View counters conserve skeletal candidates, sections,
triangles, state changes, palette uploads/bytes, resource attempts, and draws.
Vulkan coverage renders Opaque, Masked, and Translucent skeletal sections and
keeps the StaticMesh preparation baseline unchanged.

Stage 5 completed on 2026-08-11. Focused qualification covers FIFO pose/bound,
transform, visibility, material, reimport, DDC, cook/runtime-only load,
remove/re-add, shader reload, device invalidation, shutdown, and exact resource
release. One Vulkan frame now renders sequential solid/unlit and fixed-aspect
wireframe/lit views with independent targets while retaining one immutable pose
per prepared view. The glTF, GLB, authored, DDC, cooked runtime, debug editor,
and Shipping game paths agree through the deterministic fixture/cook tests and
the shared Vulkan deformation/readback target. Material, viewport, thumbnail,
StaticMesh, reload, and assistance regressions remain green; measured palette
and draw counters stay within the frozen 64 MiB view budget.

Stage 6 completed on 2026-08-11. The shared draw-sort-key assembly was reduced
to one value helper, production proxy creation has no temporary no-proxy seam,
and lasting contracts are published in Runtime Rendering documentation. Final
qualification passed changed-document, active-plan, and roadmap validators;
the focused Debug Editor native targets recorded below; the Shipping Game
skeletal Vulkan target; a full `Win64-Debug-DurinEditor` `all` build; and a
30-tick hidden-window Sandbox editor smoke. The verified executable is
`Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.

The prerequisite RHI Graphics State and Bindings Plan completed before source
implementation began. Skeletal rendering consumed that stable contract and
added only the consumer-proven transient storage-range capability.

## Goal

Make `DSkeletalMeshComponent` a production scene primitive whose supported
animations render through GPU skinning with deterministic bounds, materials,
surface passes, ordering, diagnostics, resource recovery, and viewport output,
without reading reflected objects on the rendering thread or introducing a
skeletal-only frame renderer.

## Scope

- Skeletal render data built from the existing runtime payload, including
  geometry, influences, sections, material slots, palette metadata, influence
  bounds, a skinning vertex factory, and complete RHI lifecycle.
- Skeletal material defaults and component overrides with StaticMesh-equivalent
  fallback, immutable render-proxy binding, and revisioned updates.
- `FSkeletalMeshSceneProxy`, typed scene membership, palette/bounds mutation,
  and FIFO retirement safety.
- Conservative animated local/world bounds derived from per-palette influence
  bounds and each complete published pose.
- Frame-local dynamic shader-readable storage allocation, exact range/limit
  admission, transitions, reflected binding, and Vulkan validation required by
  the palette path.
- Skeletal vertex shaders for position, normal, and tangent skinning and reuse
  of PBR Opaque, Masked, and Translucent behavior.
- Per-view preparation, deterministic surface ordering, resource preparation,
  execution, and conserved counters alongside StaticMesh.
- Main, auxiliary, present, offscreen, fixed-aspect, Lit/Unlit,
  Solid/Wireframe, post-process, assistance, reload, invalidation, and shutdown
  qualification.
- Runtime documentation and completion updates in both owning roadmaps.

## Non-Goals

- Animation graphs, blending, state machines, retargeting, root motion, IK,
  control rigs, morph targets, cloth, or a CPU-skinning fallback.
- Skeletal LOD generation or selection. The current payload has one geometry
  level; this plan represents it explicitly as LOD 0.
- Compute skinning, cached skinned vertices, mesh shaders, indirect draws,
  instancing, GPU culling, crowd palette sharing, or bindless descriptors.
- Directional shadow rendering. M6 owns the first shadow-depth pass; this plan
  records caster eligibility and leaves execution absent until that pass exists.
- A public renderer, pass, vertex-factory, or primitive registration API.
- Generalizing every StaticMesh preparation type before the second family
  proves which fields and helpers are genuinely shared.
- Editor import, inspector, Content Browser, or animation-preview UX; Skeletal
  S4 owns those workflows.
- Unrelated fixed state, descriptor arrays, instancing, or cache work from the
  active RHI Graphics State and Bindings Plan.

## Design Decisions and Invariants

### Ownership and lifetime

- `DSkeletalMesh` owns complete-or-null `FSkeletalMeshRenderData` built from its
  immutable payload. Geometry and influence resources follow the established
  StaticMesh init, failure retention, recreate-context, fenced release,
  invalidation, and shutdown protocol.
- `DSkeletalMeshComponent` reads assets and the latest atomic pose only on the
  game thread. Proxy creation copies material proxy references and a complete
  palette/bound snapshot and borrows render data only while render state and the
  asset recreate fence keep it alive.
- `FSkeletalMeshSceneProxy` owns no `DObject`, component, actor, Skeleton,
  AnimationClip, or asset pointer. The render thread consumes detached render
  data, counted material proxies, immutable palette values, revision, and bounds.
- Replacement, reimport, unregister, and shutdown remove every proxy before
  borrowed render data retires. Failed replacement retains the last complete
  renderable state where the existing asset transaction permits it.

### Geometry and vertex factory

- Initial render data materializes positions, tangent basis, UVs, colors, four
  joint indices, four normalized weights, uint32 indices, sections, and material
  slots without changing the authored/DDC/cooked payload schema.
- A dedicated `FSkeletalMeshVertexFactory` owns the structural declaration and
  stream bindings. It is a second concrete vertex factory, not a mode bit on
  `FLocalVertexFactory`.
- Joint indices remain exact integer palette indices. Weight packing may narrow
  only after Stage 0 records an error bound and CPU/GPU goldens; four float32
  weights are the baseline.
- Prepared records carry explicit LOD index zero; no draw aliases a StaticMesh
  LOD or chooses geometry implicitly.

### Palette transport and skinning semantics

- Vertex-shader linear-blend skinning is the only production deformation path.
  Stage 0 records exact multiplication and memory layout against S2 CPU goldens.
- Position uses the weighted affine result. Normal and tangent use weighted
  linear transforms followed by deterministic normalization; tangent handedness
  is preserved. Non-finite, out-of-range, zero-sum, or incompatible data rejects
  before publication or draw.
- Palettes bind as read-only storage-buffer ranges, not uniform buffers. The RHI
  publishes a maximum range and frame-local dynamic storage allocator with the
  recorded/immediate ownership, alignment, retention, and frame-reuse policy of
  existing transient data. Vulkan transitions it to `GraphicsShaderRead` before
  the scene pass.
- One view captures one shared immutable palette pointer and revision per
  primitive. Every section uses it; later FIFO updates affect later views only.
- Identical publications within one view may share an upload by stable proxy
  identity plus revision. No sharing across views or frames is assumed.
  Counters report requested, uploaded, reused, rejected, matrix, and byte totals.
- Asset, device-range, per-view-byte-budget, or allocator overflow rejects the
  primitive with an owned diagnostic; it never truncates or substitutes bind
  pose.

### Animated bounds and scene mutation

- Render-data construction computes one conservative bind-space influence bound
  per palette entry from all vertices with positive weight for that entry.
- Pose evaluation transforms all non-empty influence-bound corners by their
  palette matrices and publishes the finite union as
  `FSkeletalPosePalette::LocalBounds`. Every skinned vertex is a convex weighted
  combination of points contained by the transformed contributing bounds.
- Reference pose, seek, tick, stop, and rebind publish matrices and local bounds
  atomically. Invalid math leaves the previous complete candidate unchanged.
- One skeletal dynamic-data update carries primitive id, palette candidate, and
  bound through FIFO render commands. It updates the proxy and recomputes
  SceneInfo world bounds before later visibility preparation. Revisions are
  diagnostic identity, not stale-command cancellation.
- Without a compatible complete palette, proxy creation rejects the primitive;
  the renderer never invents identity matrices.

### Scene, material, pass, and preparation reuse

- SkeletalMesh extends the private primitive-kind switch and authoritative typed
  collection. Visibility classifies it once using the StaticMesh authored
  visibility, frustum, fitted-view, fallback, and sequential-view policy.
- `FSkeletalMeshRenderer` is a private feature owner composed explicitly by
  `FSceneRenderer`. It owns shaders, pipelines, retry slots, preparation,
  execution, invalidation, and release; there is no second frame renderer.
- Skeletal sections resolve the same material snapshots and Opaque/Masked/
  Translucent meaning, raster/depth/blend state, fallback, view mode, and
  distance-first translucent policy as StaticMesh.
- After both families exist, extract only byte-equivalent value-only material/
  pass resolution, surface sorting facts, and conservation helpers. Primitive
  payloads, vertex factories, palette state, resource slots, and execution stay
  concrete-family owned.
- Preparation captures all visibility, bound, material, pass, transform,
  palette, vertex-factory, section, and sort facts before Scene Color. Resource
  creation/upload completes before the pass; execution only binds and draws.
- Opaque and Masked work is state-grouped within the family. Translucent work is
  globally ordered across geometry families by the existing distance metric and
  complete stable ties.

### Failure and coordination boundaries

- Geometry, vertex factory, shader, PSO, palette, and binding failures reject
  the smallest complete primitive or draw and preserve other features. Retry,
  invalidation, and last-known-good slots follow existing owner policy.
- The RHI Graphics State and Bindings Plan owns general descriptor-array, draw,
  PSO identity, and cache changes. This plan adds only storage range capability
  and transient storage allocation selected by skinning, and consumes the
  established reflected scalar binding model.
- The active RHI plan lands first. Until its completion handoff, this plan is
  limited to Stage 0 documentation, fixture, measurement, and CPU-golden work;
  it does not modify shared RHI, Vulkan, RenderCore vertex-input, shader-binding,
  or graphics-pipeline sources.
- This plan records shadow-caster eligibility but does not depend on the future
  shadow pipeline.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this plan |
| --- | --- | --- |
| Assets | Validated geometry, four influences, sections, palette mapping, materials, bounds, DDC/cook/runtime load | No render data, vertex factory, RHI resources, or fenced replacement |
| Pose | Detached finite matrices, compatibility, time, revision | No conservative animated bound or render-scene update |
| Component | Registered/ticked component, reference pose, deliberate no-proxy lifecycle | No materials, proxy, palette synchronization, or recreation |
| Scene | Stable identity, Proxy/Info pair, typed collections, FIFO mutation, visibility | No skeletal kind, collection, accessor, or palette/bound update |
| Preparation | Per-view culling, two-level StaticMesh records, phase ownership, counters | No second family, shared surface facts, global translucency, or skeletal counters |
| Materials | Versioned PBR Opaque/Masked/Translucent behavior and effective state | Skeletal sections cannot resolve or execute material proxies |
| RHI | Multiple streams, scalar storage binding, transitions, counted commands | No published storage range or frame-local dynamic storage allocator |
| Renderer | Explicit private feature owners with isolated retry/invalidation/release | No skeletal owner, shaders, pipelines, preparation, or execution |
| Validation | Skeletal fixtures, CPU pose goldens, runtime-only playback, StaticMesh Vulkan baselines | No CPU/GPU skinning, animated-bound visibility, lifecycle, or images |

## Frozen Stage 0 Contract

### Numeric and fixture contract

- Matrix composition uses column vectors. CPU `FMatrix` is indexed
  `[column][row]`; fixture JSON and shader-facing matrices are written as rows.
  A palette entry is exactly `inverse(mesh bind) * joint component * inverse
  bind`. Position uses the weighted affine result. Normal and tangent use the
  weighted upper 3x3 followed by normalization, and tangent handedness is
  preserved. CPU/GPU comparisons use absolute tolerance `1e-5`.
- `Engine/Tests/Data/AssetImport/Skeletal/ExpectedContract.json` owns the
  independent rendering golden. Its `rendering.samples` cover reference,
  interpolated `0.5`, key `1.0`, exact-duration looping `2.0 -> 0.0`, and
  non-looping clamp `2.0`. Each sample contains palette matrices, skinned
  positions/normals/tangents, exact bounds, and conservative bounds for both
  glTF mesh/skin associations. The generated deformation leaves the bind
  geometry bound and the glTF/data-URI/external/GLB fixture graph remains
  byte-deterministic.
- The fixture is LOD 0 only: 6 vertices, 6 uint32 render indices, 2 triangles,
  2 sections, 2 material slots, and 4 palette matrices. One palette upload is
  256 bytes. The asset maximum is 65,535 matrices or 4,194,240 bytes. The
  per-view palette upload budget is 64 MiB. Every allocation uses the device's
  `minStorageBufferOffsetAlignment`, and admission requires the exact range to
  be no greater than the published `maxStorageBufferRange`. Vulkan's portable
  minimum range (128 MiB) exceeds the asset maximum but the runtime still
  validates the reported device value.

### Renderer and behavior boundary

- The value-only extraction candidates are material pass/effective-state
  resolution, material sampler description/key construction, material and
  pipeline identity text, lexicographic value comparison, common surface sort
  facts, distance-first translucent comparison, and counter-conservation
  helpers. Existing anchors include `GetMaterialSamplerKey`,
  `MakeMaterialSamplerDesc`, `GetIdentityText`, `CompareArray`, and the
  material/pass portions of `MakeStaticMeshDrawSortKey`.
- StaticMesh-owned fields remain concrete: `FLocalVertexFactory`, selected LOD
  resources and LOD histograms, StaticMesh section pointers, geometry sort
  fields, resource retry slots, shader/PSO owners, and draw execution. Skeletal
  equivalents remain concrete too: its vertex factory, LOD 0 render data,
  palette/revision/bounds, upload state, shader/PSO owners, and execution.
- Missing or incompatible pose rejects proxy creation. Reference pose is an
  explicitly evaluated complete palette, never an identity substitution.
  Caster eligibility is captured as a prepared fact but no shadow command is
  emitted. Opaque/Masked state grouping remains family-local; Translucent uses
  distance descending across both families, then the complete state/material/
  vertex-factory/geometry/primitive/LOD/section key. Preparation and execution
  counters conserve every visible candidate, section, triangle, palette byte,
  resource attempt, and draw outcome.
- Frozen unchanged baselines are the fixture's four StaticProjection mesh
  instances/two materials/no diagnostics; existing StaticMesh authored
  visibility and fallback behavior; Opaque, Masked, then Translucent pass
  order; Lit/Unlit and Solid/Wireframe effective state; and current main,
  auxiliary, offscreen, fixed-aspect, and sequential-view behavior.

### Ownership and FIFO sequences

- Replacement/reimport: game thread validates a complete asset candidate,
  removes proxies, enqueues fenced render-resource retirement, exchanges the
  candidate only when the established asset transaction permits it, recreates
  proxies, and lets the RHI owner release resources after the fence. Failure
  keeps the prior complete candidate.
- Pose/transform/material: game thread captures detached counted/value state
  and enqueues FIFO commands. The render thread applies one palette+revision+
  local-bound update and recomputes world bounds atomically; transform and
  material updates ordered before/after it observe their FIFO position. No
  command reads a component, actor, asset, Skeleton, or AnimationClip.
- Remove/re-add: removal retires the exact primitive identity before any later
  add publishes a new identity; queued updates address only their captured
  identity. Invalidation clears family-owned retry/resources after prepared
  views retire. Shutdown removes scene membership, drains render commands,
  crosses the resource fence, releases RHI resources exactly once, then
  destroys reflected owners.

### RHI coordination baseline

The RHI Graphics State and Bindings plan completed at `c50a7b2a`; current entry
baseline `bf0613f8` also validates only ranges consumed by a draw. Skeletal work
reuses the final structural vertex input, typed draw arguments, reflected
scalar storage binding, pre-draw completeness validation, retained binding
snapshot, canonical PSO identity, and bounded Vulkan caches. It adds only the
consumer-proven published storage range/alignment capabilities and frame-local
dynamic storage allocation; it does not add binding sets, descriptor-array
alternatives, raw handles, or a second PSO/cache contract.

## Implementation Stages

### Stage 0: Freeze the rendering contract and entry baselines

Dependencies: completed Skeletal S1-S2, Rendering M1-M3, and the active RHI
binding contract.

- [x] Record exact S2 matrix layout and CPU-skinned position, normal, tangent,
  and bound goldens at reference, key, interpolated, loop, and clamp times.
- [x] Select a repository-authored glTF/GLB fixture with at least two joints,
  multiple influences, deformation outside bind bounds, multiple sections and
  materials, and deterministic framing; extend the fixture generator.
- [x] Measure and freeze vertex/index/section/palette counts, palette bytes per
  primitive/view, upload budget, storage alignment, and Vulkan range limits.
- [x] Inventory exact StaticMesh helpers eligible for value-only extraction and
  the family-owned fields that must remain concrete.
- [x] Freeze single-LOD, reference-pose, missing-pose, caster-eligibility,
  cross-family translucency, and conservation semantics.
- [x] Record ownership/FIFO sequences for replacement, reimport, material, pose,
  proxy retirement, invalidation, and shutdown.
- [x] Reconcile minimal storage allocation/limit work with the active RHI plan.
- [x] Record the completed RHI plan baseline and confirm no skeletal task
  duplicates its final vertex-input, draw, binding, validation, or cache APIs.
- [x] Capture unchanged StaticMesh material/pass/visibility baselines.

#### Acceptance Gate

- The fixture proves deformation, material sections, moving bounds, glTF/GLB
  equivalence, and runtime-only playback.
- Independent CPU goldens define exact finite shader and bounds results.
- Numeric budgets reject unsupported work before incomplete commands record.
- Every mutation sequence names game, render, RHI, and fence ownership without a
  render-thread object read.
- Shared extraction is bounded and needs no public renderer framework.
- Stage 1 remains closed until the RHI plan's final handoff is stable.

### Stage 1: Build skeletal render data and lifecycle

Dependencies: Stage 0 contracts and fixture goldens; the RHI Graphics State and
Bindings Plan is completed and its final handoff has been re-inspected.

- [x] Add deterministic geometry, influence, index, section, material-slot, and
  influence-bound render-data types from the existing payload.
- [x] Validate per-palette bounds and their reference-pose containment.
- [x] Implement `FSkeletalMeshVertexFactory` with exact joint/weight semantics,
  structural identity, stream readiness, and no StaticMesh cast.
- [x] Add complete-or-null initialization, diagnostics, retry, release, and
  invalidation for all skeletal render resources.
- [x] Add asset render-data construction, lazy init, recreate context,
  replacement handoff, release fence, destruction, and shutdown behavior.
- [x] Preserve the lifecycle through import exchange, authored/DDC/cooked load,
  runtime-only load, reimport, and failed replacement.
- [x] Test conversion, formats, ranges, materials, bounds, malformed candidates,
  partial RHI failure, retry, replacement, and exact releases.

#### Acceptance Gate

- Accepted payloads produce deterministic render data equal to CPU source facts.
- Vertex-factory readiness rejects missing or mismatched streams before PSO work.
- Replacement cannot expose retired data; resources release exactly once after
  the owning fence.
- glTF, GLB, DDC, cooked, failure, invalidation, and shutdown semantics agree
  without changing StaticMesh.

### Stage 2: Publish animated bounds and skeletal scene state

Dependencies: Stage 1 render data and the M1 scene contract.

- [x] Extend binding/evaluation with influence bounds and one finite conservative
  `LocalBounds` in every complete pose.
- [x] Validate bounds against exact CPU-skinned vertices, mirrored/nonuniform
  transforms, and empty palette entries.
- [x] Add component material defaults/overrides, name lookup, fallback,
  revisioned updates, and failure-atomic edits.
- [x] Add the skeletal proxy, primitive kind, typed accessor/collection, and
  centralized classification without RTTI or component retention.
- [x] Add one FIFO dynamic update for palette, revision, and bound; update proxy
  state and SceneInfo world bounds atomically.
- [x] Cover registration, pose controls, mesh/clip/material replacement,
  visibility, transform, unregister, and remove/re-add ordering.
- [x] Extend scene/visibility conservation counters and focused tests.

#### Acceptance Gate

- Each live skeletal primitive has one Proxy/Info pair and typed membership; the
  rendering thread reads no reflected owner.
- Matrices and bounds change together and never mix revisions.
- Bounds contain CPU-skinned fixture vertices and prevent false culling.
- FIFO retirement prevents stale identity mutation while later ordered revisions
  remain valid.
- Material updates preserve complete bindings and fallback diagnostics.

### Stage 3: Add palette transport and prove GPU skinning

Dependencies: Stage 2 snapshots and Stage 0 budgets.

- [x] Publish RHI storage range/alignment capabilities and early rejection.
- [x] Add a frame-local dynamic storage range API with recorded/immediate parity,
  retention, alignment, bounded chunks/bytes, deterministic failure, frame
  reuse, and Vulkan lowering.
- [x] Validate range usage/transitions through the reflected scalar binding path
  without a binding-set alternative.
- [x] Add skeletal base-pass vertex shaders and palette parameter metadata while
  preserving PBR fragment policy.
- [x] Skin position, normal, and tangent using Stage 0 semantics and reject
  palette/interface mismatch before draw.
- [x] Compare Vulkan results with CPU goldens for reference/key/interpolated/
  loop/clamp, nonuniform, multiple-influence, and glTF/GLB-equivalent poses.
- [x] Cover allocation/range/transition/binding/PSO failure, retry, validation,
  inline, recorded, and RHI-thread execution.

#### Acceptance Gate

- GPU deformation matches CPU goldens within recorded tolerances.
- Each palette is one exact read-only storage range with no uniform assumption,
  truncation, native handle, or object pointer.
- Allocation, alignment, retention, transitions, and replay are coherent across
  command paths.
- Failed palette work creates no partial binding/draw and Vulkan validation is
  clean.
- General RHI behavior remains owned by the active RHI plan.

### Stage 4: Integrate visibility, materials, and scene passes

Dependencies: Stage 3 GPU path and M2-M3 preparation contracts.

- [x] Add prepared skeletal primitive/draw records, pass buckets, complete value
  sort keys, and exactly-once prepare/resources/execute phases.
- [x] Capture only authoritative visible inputs: transform, bound, render data,
  LOD 0, palette/revision, material, pass, section, and sort facts.
- [x] Extract bounded shared value helpers only where both mesh families match.
- [x] Implement `FSkeletalMeshRenderer` resource slots, pipelines, palette
  upload/reuse, execution, invalidation, retry, release, and explicit composition.
- [x] Preserve pass order and state grouping, and establish global cross-family
  distance-first translucency with stable ties.
- [x] Add skeletal/combined visibility, section/triangle, state, palette byte,
  resource, and execution counters with conservation assertions.
- [x] Record base-pass and future caster eligibility without a shadow pass.

#### Acceptance Gate

- Both families share classification, visibility, surface meaning, pass order,
  fallback, and orchestration without unsafe payload sharing or scene scans.
- Preparation completes resource and palette work before Scene Color; execution
  only consumes complete records.
- Opaque/Masked grouping and global translucency are deterministic.
- Counters explain every submission and palette byte across success and failure.
- StaticMesh baselines remain exact except a recorded combined-translucency case.

### Stage 5: Qualify lifecycle, views, and failure recovery

Dependencies: Stage 4 production pass integration.

- [x] Exercise asset/material/pose/transform/visibility replacement and
  component/actor/world retirement with queued render work.
- [x] Validate geometry/palette/shader/PSO failure, retry, shader reload, device
  invalidation, resource reload, shutdown, and exact release evidence.
- [x] Validate sequential main/auxiliary views with different cameras,
  dimensions, settings, and revisions without shared prepared lifetimes.
- [x] Cover present/offscreen, fixed aspect, perspective/orthographic,
  Lit/Unlit, Solid/Wireframe, all surface modes, post-process, assistance, and
  unchanged thumbnail/preview consumers.
- [x] Run focused skeletal, scene, material, RHI, renderer, viewport, and Vulkan
  targets using repository guidance.
- [x] Compare frozen animated images for glTF/GLB, authored/DDC, cooked runtime,
  debug editor, and the plan-gated shipping game profile.
- [x] Measure palette bytes/reuse, draws, triangles, groups, and transitions to
  inform rather than activate deferred compute skinning.

#### Acceptance Gate

- Identical inputs reproduce poses, bounds, ordering, counters, uploads, and
  pixels; glTF/GLB and authored/cooked paths agree.
- Sequential views consume coherent snapshots without stale bounds, palette
  overwrite, or cross-view state.
- Lifecycle and failure preserve allowed complete state, recover through the
  owner, and release resources exactly once.
- Existing StaticMesh, viewport, assistance, import, and runtime playback tests
  pass with clean Vulkan validation.
- Budgets hold; overflow rejects locally and diagnostically.

### Stage 6: Enable the production primitive and close M4/S3

Dependencies: Stages 0-5 and their handoffs.

- [x] Enable proxy creation by default and remove the S2 no-proxy comment and
  temporary migration seams.
- [x] Remove duplicate helpers, bind-pose-only bounds, identity-palette
  fallbacks, RTTI scans, incomplete shapes, and diagnostic bypasses.
- [x] Publish lasting GPU skinning, bounds, scene, material/pass, palette budget,
  counter, failure, and viewport rules in Runtime documentation.
- [x] Update both roadmaps with completion evidence and opened M6/S4 gates.
- [x] Run document validation, focused native targets, the required full `all`
  build, and editor smoke using repository guidance.
- [x] Record final baselines, commits, symbols, fixture hashes/tolerances,
  budgets, counters, validation, limits, and verified editor executable.

#### Acceptance Gate

- Rendering M4 and Skeletal S3 exit gates pass without a parallel renderer or
  RTTI scan.
- Palette and bounds updates are deterministic, coherent, bounded, safe, and
  detached from reflected objects.
- All surface modes pass CPU/GPU, Vulkan image, multi-view, runtime-only,
  failure, reload, and shutdown evidence.
- Lasting behavior is documented and M6 can consume explicit skeletal caster
  facts.
- Documentation, focused tests, full build, and editor smoke pass with a verified
  executable.

### Final Completion Evidence

- Stages 0-6 and their completion evidence are compressed into the single
  plan-closing commit containing this completed plan; its commit body retains
  one exact provenance line for every stage.
- The authoritative rendering golden
  `ExpectedContract.json` has SHA-256
  `1c4f8f007ee029cadc246d63205470d3fd1af9bf2f00837a9fec9a580633e109`.
  The equivalent source fixtures hash to
  `0d833590ea4e6aec80a7e114dd20c234280f0a17ba6dc7bddb6aeb5780625f6e`
  (`Contract.gltf`),
  `80535e9424b0d1dd4f3b99f616e7082108727d38f01a7790e163c53eb76034ce`
  (`ContractExternal.gltf`), and
  `03a7aca83a834c1f43ed5da25c034fdc959c0874847ff9f51b8bff0338936845`
  (`Contract.glb`). CPU/GPU numeric comparisons retain absolute tolerance
  `1e-5`.
- `FSkeletalMeshRenderData`, `FSkeletalMeshVertexFactory`,
  `FSkeletalMeshSceneProxy`, `FPreparedSkeletalMeshView`,
  `FSkeletalMeshRenderer`, `FRHIStorageBufferRange`, and
  `AllocateDynamicStorageBuffer` are the lasting implementation anchors.
- One fixture palette is 4 matrices/256 bytes; one asset admits at most 65,535
  matrices/4,194,240 bytes; one view admits at most 64 MiB of unique palette
  bytes. Vulkan additionally caps a frame slot at 16 allocator chunks and the
  device-published storage range/alignment limits.
- Final focused qualification covered `SkeletalAssetTests`,
  `SkeletalSceneLifecycleTests`, `MaterialTests`,
  `RendererResourceReloadVulkanTests`, `ViewportTests`, `ThumbnailTests`,
  `StaticMeshRenderPreparationTests`, `RendererSceneContractTests`,
  `EditorRenderingTests`, and
  `SkeletalMeshRenderResourcesVulkanTests`, including the Shipping Game
  skeletal target. The final full Debug Editor build and editor smoke passed on
  2026-08-11.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Render data | Payload/stream equality, formats, sections, materials, bounds, malformed/failure candidates | Authored, DDC, cooked, reimport, replacement, and shutdown own complete resources identically. |
| Pose/bounds | Reference/key/interpolated/loop/clamp, exact skinned vertices, per-bone boxes, transforms | Matrices and bounds publish atomically and visibility never false-culls the fixture. |
| Scene | Add/update/remove/re-add, replacement, typed membership, FIFO dynamic updates | No object read, retired update, RTTI scan, or duplicate membership. |
| Shader | Declaration identity, joints, weights, matrix convention, position/normal/tangent goldens | A second vertex factory renders equivalent glTF/GLB deformation. |
| Palette RHI | Range/alignment, budget, transition, binding, command replay, failure/retry | Exact frame-local storage ranges survive through draw without unsafe overwrite. |
| Materials/passes | Overrides/fallback, all surfaces, cull/depth/blend, view modes | Both families share authored meaning and deterministic translucency. |
| Preparation | Immutable revision, safe indices, LOD 0, phase enforcement, sequential views | No prepared borrow or palette escapes its view lifetime. |
| Counters | Visibility, pass/triangle/state, palette upload/reuse/reject/bytes, failures | Primitive, draw, triangle, and byte conservation explains each view. |
| Failure | Geometry/palette/shader/PSO, retry, reload, release fences | Failure is family-local and resources retire exactly once. |
| Output | View variants, composition, animation hashes | Animated pixels match goldens without changing unrelated composition. |
| Qualification | Focused targets, doc validators, full build, editor smoke, shipping gate | M4/S3 close with verified editor and M6/S4 handoff. |

## Definition of Done

- Skeletal assets own deterministic render data and fenced resource lifecycle.
- Components publish detached material, palette, and conservative-bound state.
- SkeletalMesh is a typed primitive classified once and prepared before passes.
- A dedicated vertex factory/shader matches CPU-skinned geometry goldens.
- Palettes use capability-admitted frame-local storage ranges with exact
  transitions, retention, budgets, diagnostics, and failure semantics.
- Skeletal sections reuse all surface policies, ordering, and composition.
- Counters conserve visibility, geometry, state, palette bytes, and draw work.
- glTF/GLB, DDC/cooked runtime, multi-view, editor/game, reload, and shutdown
  evidence passes with clean Vulkan validation.
- Lasting contracts and roadmap completion are published after full build/smoke.

## Deferred Follow-ups

- Skeletal shadow-depth participation in Rendering M6.
- Inspector, preview, playback controls, and workflow qualification in S4.
- Multiple skeletal LODs after the asset/source contract defines them.
- Compute/cached skinning, palette sharing, streaming/compression, crowds,
  instancing, and GPU submission after counters prove a bottleneck.
- Morphs, cloth, retargeting, blending, root motion, events, IK, control rigs,
  and gameplay animation through separate activated plans.
- Public registration only after a named external module requires it.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Skeletal Mesh and Animation Roadmap](../Roadmaps/SkeletalMeshAndAnimation.md)
- [Skeletal Asset and Import Foundation](Archive/2026-08/SkeletalAssetAndImportFoundation.md)
- [Skeletal Runtime Pose and Playback](Archive/2026-08/SkeletalRuntimePoseAndPlayback.md)
- [Per-View Visibility and LOD](Archive/2026-08/PerViewVisibilityAndLOD.md)
- [Material Render Pass Policies](Archive/2026-08/MaterialRenderPassPolicies.md)
- [RHI Graphics State and Bindings](RHIGraphicsStateAndBindings.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`
- `Engine/Source/Runtime/Engine/Public/Animation/SkeletalAnimation.h`
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/Animation/SkeletalAnimation.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAnimationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalSceneLifecycleTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Data/AssetImport/Skeletal/`
