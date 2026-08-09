# Per-View Visibility and LOD Plan

Summary: Establish a renderer-owned per-view preparation boundary with conservative primitive visibility, deterministic StaticMesh LOD selection, reusable prepared draw data, stable state ordering, and explainable counters.

Last reviewed: 2026-08-09

Status: Archived
Completed: 2026-08-09

## Current Status

This plan completed M3 for the
[Rendering Capability Expansion Roadmap](../../../Roadmaps/RenderingCapabilityExpansion.md).
All stages are complete. Stage 6 started from
`8027ccf99bc0e8241e5ca564cd9656129041c856`. M1 supplies
detached primitive identity, transform, bounds, visibility, and typed scene
membership; M2 supplies complete StaticMesh material/pass classification and
effective graphics state.

The renderer now owns one command-local `FPreparedSceneView` after output fitting,
classifies authoritative live primitives once, prepares visible selected-LOD
StaticMesh buckets and demanded resources before Scene Color, and executes only
prepared scene data inside that pass. Authored-hidden primitives retain identity and
SceneInfo state but do not enter family preparation; finite outside bounds are
culled in normal mode, while invalid bounds/views stay visible through named
fallbacks. StaticMesh preparation now selects and validates requested/actual
LODs and stores primitive facts once for indexed section draws. StaticMesh
render data and DMSH schema 4 now carry a validated deterministic `ScreenSize`
threshold beside every LOD; Stage 4 consumes that policy without changing its
ownership.

Production views now default to authored visibility, conservative frustum
culling, and deterministic projected-size LOD selection. The immutable
culling-disabled and forced-LOD-0 comparison settings remain qualified without
process-global policy. Visibility classification is discarded after typed
family preparation and before the first scene pass; execution consumes only
complete selected-LOD prepared draws. The lasting contract is recorded in
[Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md), and the
roadmap records M3 complete with its M4/M6 preparation dependency open.

## Goal

Make one immutable `FSceneView` produce one deterministic, command-local
prepared scene result before the first scene render pass. That result must:

- classify authoritative scene primitives once into visible and rejected work;
- apply explicit authored visibility and conservative CPU frustum culling;
- select a valid StaticMesh LOD from stable screen-size metadata;
- prepare each visible StaticMesh primitive once and each accepted section once;
- expose complete pass/state/sort facts without later scene or material scans;
- execute Opaque, Masked, and Translucent work in deterministic order;
- report enough submitted, rejected, LOD, section, triangle, state, and draw
  counts to explain every view invocation; and
- remain private, render-thread-owned, multi-view-safe, and directly reusable
  by the selected M4 primitive family and M6 shadow preparation without
  imposing a public renderer registry or render graph.

The architectural outcome is a clear split:

```text
immutable FSceneView + renderer-owned FScene
                    |
                    v
        FSceneRenderer prepares one view
          - generic primitive visibility
          - typed visible family lists
          - family-specific prepared work
          - demand and counters
                    |
                    v
       resources ensured outside render passes
                    |
                    v
        pass executors consume prepared work
          - no scene scan
          - no LOD selection
          - no material re-resolution
          - no hidden resource upload
```

## Scope

- A Renderer-private command-local prepared-scene-view owner created by
  `FSceneRenderer` after output fitting and before any scene render pass begins.
- One generic primitive visibility phase over authoritative
  `FPrimitiveSceneInfo` state, with explicitly typed visible-family outputs.
- Finite perspective and orthographic frustum construction and conservative
  world-AABB classification using Durin's actual matrix, clip-depth, and
  handedness conventions.
- Authored visibility rejection before frustum work and conservative handling
  of invalid bounds or unsupported projection inputs.
- A deterministic projected screen-size metric based on primitive bounds and
  the fitted content viewport, independent of output black bars.
- Stable StaticMesh per-LOD selection metadata in render data and its required
  authored/derived/cooked payload boundary, including versioning and
  compatibility disposition.
- A two-level StaticMesh prepared representation: one record per accepted
  primitive and indexed draw records for its accepted sections.
- Explicit selected LOD resources, selected vertex factory, transform parity,
  material binding, pass, pipeline key, sort key, and triangle count in prepared
  work.
- Stable Opaque/Masked state grouping and preserved Translucent back-to-front
  ordering with complete tie-breaks.
- Per-view counters and focused diagnostic reasons for hidden, outside-frustum,
  invalid-bounds fallback, invalid-view fallback, unavailable LOD, malformed
  section, pass membership, state groups, triangles, and executed draws.
- A culling/LOD comparison mode carried by immutable view policy or an
  equivalent command snapshot, usable by focused tests and diagnostics without
  mutating process-global policy during an enqueued view.
- Main, auxiliary, present, offscreen, fixed-aspect, thumbnail, Material
  Preview, Lit/Unlit, and Solid/Wireframe compatibility.
- Lasting Runtime Rendering documentation and roadmap status updates when M3
  completes.

## Non-Goals

- Occlusion queries, HZB, portals, spatial trees, GPU culling, meshlets,
  indirect submission, instance-level visibility, or asynchronous compute.
- Generating simplified mesh geometry. M3 selects existing LOD resources; a
  mesh-reduction pipeline requires its own asset/build plan.
- Temporal LOD hysteresis, dithered transitions, TAA integration, or persistent
  view history. Those require a stable temporal view identity and a selected
  temporal consumer.
- Per-section or per-triangle frustum culling. Primitive world bounds are the
  M3 culling granularity.
- Shadow-caster visibility, shadow LOD policy, depth-only command execution, or
  shadow resource allocation. M6 may reuse the preparation contracts with its
  own light view and pass participation.
- A public or runtime-polymorphic renderer/pass registration API.
- A universal public mesh draw command shared speculatively by future vertex
  factories. M3 defines a private common visibility seam and a StaticMesh
  prepared family; M4 proves which draw facts genuinely generalize.
- A render graph, persistent prepared-view cache, target-size-keyed visibility
  cache, or cross-view reuse of results.
- Point/spot light selection, light-volume culling, or M5 GPU light payloads.
- Editor UI for hand-authoring thresholds unless existing asset surfaces
  already provide an ownership-correct extension seam. Stable data and
  deterministic defaults are required; polished authoring UX is not.

## Design Decisions and Invariants

### FSceneRenderer owns the preparation boundary

`FSceneRenderer::RenderView_RenderThread` owns the complete lifetime of one
prepared view. It first fits the submitted view to the output, converts the
public `IScene` boundary to the Renderer-private `FScene` once, prepares scene
and editor demand, ensures demanded resources, and only then opens a render
pass. Private feature methods accept `FScene` or prepared family data rather
than repeatedly `dynamic_cast`-ing `IScene`.

The prepared result does not enter `FScene`, survive the render command, or
become a cache member. Main and auxiliary views rendered sequentially construct
independent results even when they share scene state and target dimensions.

The current `DrawScene_RenderThread` contract, which discovers, classifies, and
executes work after the pass begins, is transitional. M3 splits it into
preparation, demand/resource readiness, and pass execution. No resource upload,
shader creation, pipeline creation, scene traversal, material fallback
resolution, or LOD choice may be hidden inside the final section draw loop.

### Generic visibility is centralized; draw preparation remains typed

One Renderer-private visibility phase walks `FScene::GetPrimitiveSceneInfos()`
and produces a `FSceneVisibilityResult`-equivalent value. It records one
classification per live primitive and owns typed visible lists for StaticMesh,
TextureCube preview, and later explicit primitive kinds. A single exhaustive
kind switch in this private classifier is preferable to repeated whole-scene
RTTI scans or one visibility implementation per feature renderer.

The result preserves strong primitive identity and authoritative SceneInfo
pointers. It does not copy proxies, own resources, or expose a public extension
registry. Adding M4 may add one explicit typed output and switch case; that is
an accepted private composition change until a concrete external module proves
the need for registration.

Family preparation consumes only its typed visible list. StaticMesh code does
not re-query the full scene to verify visibility, and execution does not
re-query either collection.

### Visibility is conservative under uncertainty

Visibility classification follows this order:

1. An explicitly hidden primitive is rejected without bounds or LOD work.
2. A finite valid world AABB is tested against a valid view frustum.
3. A fully outside AABB is rejected; intersecting or inside AABBs remain
   visible.
4. Invalid bounds, a degenerate/unsupported projection, or non-finite derived
   math cannot make a primitive disappear. The primitive remains visible,
   selects the conservative LOD fallback, and increments a named diagnostic.
5. Invalid family render resources may reject later family preparation, but
   are not misreported as frustum culls.

Plane-touching bounds are visible. Epsilon policy is finite, scale-aware, and
frozen by Stage 0 tests rather than inherited accidentally from GLM defaults.
Mirrored transforms use the existing eight-corner world-AABB contract and do
not alter visibility semantics.

### The screen-size metric is view-relative and projection-aware

LOD selection uses one normalized projected-size metric derived from the
primitive's authoritative world bounds and fitted `FSceneView`. It must be
stable for perspective and orthographic projections, independent of the
absolute render-target resolution for an unchanged framing, and based on the
content viewport rather than fixed-aspect black bars.

The metric uses the whole primitive bound, not the current LOD's local bound,
so selecting an LOD cannot change the input that selected it. Bounds crossing
the camera or near plane take the conservative maximum-detail result. Nonuniform
scale is covered conservatively by the world bound. Stage 0 freezes the exact
formula against Durin's projection matrices and clip convention before data
schema changes begin.

M3 does not add temporal hysteresis. Exact threshold equality has one documented
winner, and moving a camera across a threshold in either direction yields the
same selected index for the same metric.

### LOD policy is data, not a renderer distance ladder

Every renderable StaticMesh LOD receives a finite normalized transition
threshold or an equivalent validated value descriptor. Thresholds are ordered
from highest-detail LOD to lowest-detail LOD, have an exact equality rule, and
guarantee the final valid LOD as the fallback for sufficiently small projected
size. LOD 0 remains the conservative fallback for invalid view/size math.

The preferred Stage 0 decision is to store this policy beside per-LOD runtime
geometry and carry it through StaticMesh build/payload data. Builders that lack
authored thresholds generate one documented deterministic sequence; they do
not leave policy implicit in Renderer code. A payload schema or builder/DDC-key
change is required if the value reaches cooked/runtime data. Stage 0 must
record whether old payloads are deliberately rejected and rebuilt or decoded
with an explicit compatibility default; silent reinterpretation is forbidden.

Malformed threshold arrays fail publication or select a complete known fallback
before reaching scene preparation. The renderer does not clamp arbitrary bad
data differently per frame. A selected but unavailable LOD uses one frozen
fallback search rule over ready LODs and reports both the requested and actual
index; it never silently aliases vertex factories by an unchecked index.

### StaticMesh prepared work has primitive and draw levels

M3 replaces repeated per-section primitive data with two indexed levels:

- a prepared primitive record containing primitive id, SceneInfo/proxy/render-
  data borrows, selected/requested LOD, selected vertex factory, transform,
  parity, bounds/size facts, and aggregate geometry counts; and
- prepared draw records containing a prepared-primitive index, section index,
  resolved material/binding, pass, complete pipeline key, section geometry,
  sort facts, and deterministic tie-break fields.

Draw records use indices or other relocation-safe handles into the owning
prepared value; vector reallocation must not invalidate internal references.
All borrows are bounded by the owning render command and the existing
StaticMesh proxy/render-data fence protocol. Prepared data never retains a
component, asset, reflected object, or raw mutable material state.

This separation is intentionally family-private in M3. M4 may extract a shared
private mesh-pass record only after a second vertex factory demonstrates the
same fields and lifetime.

### Ordering keys are complete and deterministic

Opaque and Masked draws may be grouped to reduce state changes. Their key
contains pass, effective pipeline state, material/shader identity, vertex-
factory identity, selected geometry identity, and stable primitive/section
ties. Equality of grouping keys never leaves order dependent on pointer values,
unordered-container iteration, or attachment timing that lacks a stable id.

Translucent order remains primarily back-to-front by the M2 center metric.
State grouping cannot move a farther translucent draw behind a nearer one.
Equal distance uses stable primitive id, selected LOD, and section index before
any non-semantic implementation detail.

Changing opaque order can expose coplanar/equal-depth differences. Stage 5
must compare culling-disabled LOD-0 output against the M2 baseline and either
freeze the new deterministic result with rationale or refine the key; image
changes cannot be dismissed as visually equivalent without evidence.

### Counters are part of the prepared result

The prepared view owns a value-only `FViewRenderCounters`-equivalent record.
Counters are updated at the phase that owns the decision and obey conservation
identities, including:

- submitted primitives = hidden + frustum-culled + conservatively visible +
  ordinarily visible;
- visible StaticMesh candidates = prepared + resource/data rejected;
- prepared StaticMesh draws = Opaque + Masked + Translucent;
- selected LOD histograms sum to prepared StaticMesh primitives; and
- attempted draws = successful draws + execution failures/skips.

Invalid bounds and invalid-view fallbacks are visible subsets, not additional
submitted primitives. Triangle counts derive from accepted index ranges of the
selected LOD. State-group counts derive from the final sorted buckets.

Focused tests consume the counter value directly. Production diagnostics emit
or expose one complete result per render invocation without requiring persistent
view identity. M3 does not key semantic results by target size and does not
claim temporal continuity between sequential views.

### View policy is immutable and comparison is explicit

Production defaults enable authored visibility, frustum culling, and LOD
selection. The comparison path is represented in the submitted view settings
or in an equivalent immutable render-command snapshot with at least:

- normal production behavior;
- frustum culling disabled while authored visibility remains authoritative; and
- forced LOD 0 for baseline comparison.

No console callback or test mutates a Boolean that an already-enqueued view
reads later. Debug comparison state is not part of shader/PSO identity.

### Failure stays local and execution consumes only complete items

Failure to construct a frustum selects conservative visibility for that view;
it does not abort rendering. A malformed primitive, LOD, section, material
binding, shader, or pipeline rejects only the smallest complete unit already
owned by its established contract. No partial prepared draw reaches execution.

Preparation assertions cover duplicate classification, typed-kind mismatch,
non-finite sort/size facts, invalid LOD indices, mismatched LOD resources and
vertex factories, incomplete material bindings, pass/bucket mismatch, unstable
sort ties, and counter conservation. Device/shader invalidation discards no
persistent prepared cache because none exists.

## Current Foundations and Gaps

| Area | Existing foundation | M3 gap or redesign |
| --- | --- | --- |
| Scene ownership | `FPrimitiveSceneInfo` owns identity, kind, visibility, transform, local/world bounds, and typed membership. | Visibility facts are not consumed by scene drawing; private render functions still accept/cast `IScene` in multiple places. |
| View snapshot | `FSceneView` carries finite matrices, camera location, settings, and fitted content viewport fields. | No frozen frustum/size contract or immutable culling/LOD comparison policy exists. |
| StaticMesh resources | Parallel per-LOD geometry and vertex factories initialize, validate, retry, and release together. | Rendering hard-codes index 0; LOD transition metadata is absent. |
| M2 preparation | Each LOD-0 section resolves material/pass/pipeline/sort facts once into three view-local buckets. | Preparation is hidden inside draw execution after the scene pass begins and repeats primitive facts per section. |
| Pass execution | Opaque, Masked, and Translucent behavior and deterministic translucent ties are qualified. | Opaque/Masked have no complete state-group ordering; execution still owns discovery and LOD choice. |
| Multi-view | Main and auxiliary views carry independent immutable matrices/settings and render sequentially. | There is no explicit prepared-view lifetime proving visibility and LOD results cannot leak between views. |
| Diagnostics | Resource creation has structured diagnostics and tests inspect selected M2 preparation facts. | No conservation counters explain submitted, hidden, culled, selected LOD, triangles, groups, or draws. |
| Test data | StaticMesh payload/resource tests can construct multiple LOD arrays and current camera paths cover perspective/fixed aspect. | Existing multi-LOD cases mostly validate resource rejection; representative distinct-geometry thresholds, orthographic cameras, and boundary goldens are missing. |

## Implementation Stages

### Frozen Stage 0 Contract

The initial five-file working set is
`RenderCore/Public/SceneView.h`, `Renderer/Private/Renderers/SceneRenderer.cpp`,
`Renderer/Private/Renderers/StaticMeshRenderer.h`,
`Renderer/Private/Renderers/StaticMeshRenderer.cpp`, and
`Engine/Public/StaticMesh/StaticMeshResources.h`. Targeted expansion is limited
to the camera/editor projection builders, StaticMesh payload/DDC seams, the
Renderer-private pure preparation math seam, and its CPU fixtures.

Current execution fits the view after size-keyed scene resources are ensured,
then opens Scene Color. `RenderScene_RenderThread` casts `IScene` once for
SkyBox lookup; `FStaticMeshRenderer::DrawScene_RenderThread` casts it again,
prepares all M2 LOD-0 section items on the stack, ensures shader/pipeline slots
from the draw path, and executes them before the pass closes. TextureCube
preview receives `IScene` independently. Main game/editor, auxiliary camera
preview, Material Preview, StaticMesh Preview, rendered thumbnails, present,
offscreen, and fixed-aspect outputs all converge on this sequence.

The selected private types are:

- `FPreparedSceneView`, owned as a local value by
  `FSceneRenderer::RenderView_RenderThread` from post-fit preparation until the
  final scene consumer returns;
- `FSceneVisibilityResult`, with authoritative
  `StaticMeshSceneInfos` and `TextureCubePreviewSceneInfos` typed lists of
  `const FPrimitiveSceneInfo*`;
- `FPreparedStaticMeshPrimitive`, `FPreparedStaticMeshDraw`, and
  `FPreparedStaticMeshView`, where draw records index the owning primitive
  vector; and
- `FViewRenderCounters`, stored by value in `FPreparedSceneView`.

Durin uses column vectors and a right-handed world/view basis whose camera
faces view-space `+X`; view `+Y` maps to clip `+X`, and view `+Z` maps to clip
`-Y`. Perspective clip `w` is view `X`. Depth is ordinary Vulkan/D3D-style
`[0, 1]`: near maps to zero and far maps to one. Orthographic goldens use the
same axes and depth direction with constant positive clip `w`. Frustum planes
are inward-facing `row3 + row0`, `row3 - row0`, `row3 + row1`,
`row3 - row1`, `row2`, and `row3 - row2`, then normalized. A world AABB is
outside only when its positive radius endpoint is below a plane by more than
`1e-9 * max(1, max(abs(bounds coordinates)))`; equality and epsilon-scale
contact are visible.

Projected screen size projects all eight authoritative world-AABB corners.
If `dx` and `dy` are their finite NDC spans and `W`/`H` are the fitted content
viewport dimensions, the normalized diameter is
`clamp(max(dx * W, dy * H) / (2 * min(W, H)), 0, 1)`. This is resolution
independent for unchanged framing and excludes output black bars. Any corner
at or behind the near plane/camera, invalid bounds, unsupported projection,
zero content dimension, or non-finite math selects size `1` and the named
conservative fallback; later visibility still rejects an ordinarily valid box
that is fully outside.

Each runtime LOD will own one finite `ScreenSize` threshold beside its geometry,
and each payload LOD will encode the same value. Thresholds are strictly
descending in `[0, 1]`, the final threshold is exactly zero, and the first
threshold met by `size >= ScreenSize` wins, so equality selects the
higher-detail LOD. Missing authored values use exact defaults
`2^-(LODIndex + 1)` except the final LOD, which is zero; one LOD therefore uses
`[0]`. Invalid size or policy selects LOD 0. If the requested LOD is not ready,
selection searches toward lower detail first, then back toward higher detail;
no ready LOD rejects that primitive.

Stage 3 will bump DMSH from schema 3 to 4 and the StaticMesh builder from 2 to
3. The existing derived-data key already includes both values, so its key
schema remains 1. Schema/builder mismatches remain explicitly incompatible:
source-backed assets and DDC entries rebuild, cooked/runtime-only content must
be recooked, and no old payload is silently reinterpreted. Importers,
procedural meshes, single-LOD meshes, previews, thumbnails, and test builders
all receive the deterministic defaults before publication. Authored threshold
UX remains deferred.

`FSceneViewSettings` will carry independent immutable
`EViewVisibilityMode::{Normal, FrustumCullingDisabled}` and
`EViewLODMode::{Automatic, ForceLOD0}` values. The M2 baseline combines
culling-disabled with forced LOD 0 while still honoring authored visibility.
No process-global state participates.

Counter conservation is frozen as:

- `Submitted = Hidden + FrustumCulled + Visible`, where invalid-bounds and
  invalid-view fallbacks are named subsets of `Visible`;
- `VisibleStaticMeshCandidates = PreparedStaticMeshPrimitives +
  RejectedStaticMeshPrimitives`;
- `PreparedStaticMeshDraws = Opaque + Masked + Translucent` and selected-LOD
  histogram entries sum to `PreparedStaticMeshPrimitives`;
- selected section and triangle totals sum accepted selected-LOD geometry;
  state-group counts derive only from final buckets; and
- `AttemptedDraws = SuccessfulDraws + ExecutionRejectedDraws`.

The bounded enabling refactor is the new Renderer-private pure CPU math/LOD
seam used by Stage 0 goldens and later preparation. Projection-builder
deduplication is recorded for a later bounded change only if the goldens expose
drift; Stage 1 already owns the monolithic StaticMesh preparation/resource/draw
split, so no separate broad renderer refactor is added.

### Stage 0: Freeze the view-preparation and LOD contracts

Dependencies: completed M1 and M2 plans and their lasting Runtime Rendering
contracts.

- [x] Record the implementation baseline, initial working set, current render-
  pass/resource ordering, current `IScene` casts, M2 prepared-item lifetime,
  and all viewport consumers without expanding into unrelated renderer systems.
- [x] Freeze Durin's view/clip conventions with perspective and orthographic
  matrix goldens, including near/far mapping, plane orientation, camera-facing
  direction, reversed or ordinary depth, fixed-aspect content dimensions, and
  plane-touch epsilon.
- [x] Select exact Renderer-private names and ownership for the command-local
  prepared scene view, generic visibility result, typed visible lists,
  StaticMesh prepared primitive/draw records, and counters.
- [x] Freeze the normalized projected-size formula, near-plane/camera-crossing
  behavior, exact threshold equality rule, LOD fallback search, and deterministic
  default thresholds.
- [x] Trace StaticMesh authored, imported, derived, DDC, cooked, and runtime-only
  paths and decide the smallest ownership-correct threshold schema change.
- [x] Record explicit compatibility policy for existing authored assets, DDC
  entries, cooked payloads, procedural meshes, thumbnails, and test fixtures.
- [x] Define comparison modes and counter conservation equations before
  production behavior changes.
- [x] Add pure CPU golden fixtures for frustum classification and LOD selection,
  including a representative mesh whose LODs have visibly different triangle
  counts and bounds-consistent geometry.

#### Acceptance Gate

- Actual engine projection conventions, not recalled API conventions, determine
  the frustum and projected-size formulas.
- Every unresolved ownership, schema, equality, fallback, and comparison choice
  has one selected answer recorded in the plan before implementation continues.
- The prepared-view design has one owner and lifetime, and no stage depends on
  a persistent view identity, render graph, public registry, or reflected object
  read.
- Representative multi-LOD assets/cameras satisfy the roadmap M3 entry gate.
- CPU goldens fail against the current no-culling/fixed-LOD implementation for
  the intended reasons and do not require RHI initialization.

#### Stage 0 Handoff

Baseline is `2e35b5032403f4a2bec557ca67089efb27912d1d`; the initial
five-file working set, selected names, render ordering, projection/size
formulas, LOD schema, compatibility, comparison, counter, and bounded-refactor
decisions are recorded in `Frozen Stage 0 Contract`. The reusable symbols are
`FViewFrustum`, `TryBuildViewFrustum`, `ClassifyWorldBounds`,
`ComputeProjectedScreenSize`, `MakeDefaultStaticMeshLODScreenSizes`,
`ValidateStaticMeshLODScreenSizes`, `SelectStaticMeshLOD`, and
`ResolveAvailableStaticMeshLOD` in `ViewPreparationMath`.

`FRendererSceneViewTests` now supplies pure CPU perspective and orthographic
matrix/depth goldens, all-six-plane inside/intersect/outside classification,
invalid-view/bounds fallback, fitted-viewport size invariance, near-plane
maximum detail, threshold equality/defaults, availability fallback, and a
three-LOD bounds-consistent distinct-geometry fixture with 4/2/1 triangles.
These fixtures require no RHI initialization; the current production
no-culling/fixed-LOD path still cannot satisfy the outside rejection and
selected triangle outcomes, as intended before Stages 1-4.

No Stage 0 design question remains open. Validation on 2026-08-09 passed all 6
`FRendererSceneViewTests`, all 38 `EditorRenderingTests`, changed-document
validation, and all-plan validation. Stage 1 should retain culling-disabled,
forced-LOD-0 output while moving this seam and M2 preparation before Scene
Color.

### Stage 1: Move preparation ahead of render passes

Dependencies: Stage 0 decisions and fixtures.

- [x] Convert `IScene` to Renderer-private `FScene` once at the renderer module
  boundary and pass typed private references through scene preparation and
  execution.
- [x] Introduce the command-local prepared-scene-view owner and make
  `FSceneRenderer` create it after `FitViewToOutput` and before Scene Color pass
  construction.
- [x] Split StaticMesh discovery/preparation, demanded resource readiness, and
  execution into distinct render-thread functions.
- [x] Move StaticMesh resource/shader/pipeline readiness outside active render
  passes; preserve complete-or-null creation slots and last-known-good retry
  behavior.
- [x] Re-express current M2 LOD-0 buckets through the new boundary with culling
  disabled and no visible output change.
- [x] Delete the old monolithic entrypoint and redundant private `IScene`
  discovery only after all callers use prepared data.
- [x] Add ordering assertions proving no preparation or resource creation occurs
  while the owning scene render pass is active.

#### Acceptance Gate

- One render invocation owns one prepared result from post-fit view through
  final scene-pass execution and destroys it before returning.
- Scene pass execution performs no scene traversal, LOD selection, material
  re-resolution, or resource creation.
- Main, auxiliary, present, offscreen, fixed-aspect, thumbnail, and preview
  paths match the culling-disabled LOD-0 M2 baseline.
- Resource failure/retry and device/shader invalidation remain feature-local and
  validation-clean.

#### Stage 1 Handoff

Baseline is `335b65d04229aee78dd92f41595ca192db70626c`. The working set expanded from
`SceneRenderer`, `StaticMeshRenderer`, and `StaticMeshRenderPreparation` to the
direct module-boundary, command-list state, TextureCube thumbnail, and Vulkan
preparation-test dependencies. `FPreparedSceneView` is a command-local owner of
the fitted view, directional light, sky data, current StaticMesh buckets,
TextureCube preview proxies, resource-readiness result, and initial
`FViewRenderCounters` value.

The render sequence is now fit view, traverse typed `FScene`, prepare LOD-0
buckets, resolve demanded base/shader/pipeline/sampler resources, construct and
begin Scene Color, then execute prepared sky, StaticMesh, and TextureCube work.
`FRendererModule` performs the only `IScene` to `FScene` conversion.
`FStaticMeshRenderer::DrawScene_RenderThread` and
`FTextureCubeThumbnailRenderer::DrawScene_RenderThread` were removed;
`PrepareStaticMeshView_RenderThread`, `PrepareResources_RenderThread`,
`Execute_RenderThread`, and `DrawPrepared_RenderThread` expose the new phases.
Execution performs cache lookup only and skips an item whose complete resource
payload is unavailable. Creation slots retain their existing failure,
last-known-good, retry, and generation behavior because all `Resolve` calls stay
in pre-pass readiness.

The comparison mode remains culling-disabled and selects `LODResources[0]` with
the same opaque/masked/translucent membership and translucent ordering. Main,
auxiliary, present, offscreen, fitted-aspect, thumbnail, and preview callers all
continue through the single `RenderView_RenderThread` sequence. There are no
open Stage 1 design questions. Validation on 2026-08-09 compiled the Renderer
and preparation fixtures, passed all 38 `EditorRenderingTests`, changed-doc
validation, and all-plan validation. Stage 2 should
replace the all-primitive StaticMesh discovery with the centralized conservative
visibility result while retaining this owner lifetime and resource boundary.

### Stage 2: Implement centralized conservative primitive visibility

Dependencies: Stage 1 prepared-view lifetime and Stage 0 math goldens.

- [x] Implement finite frustum construction and world-AABB classification in a
  pure/testable Renderer-private math seam.
- [x] Walk authoritative primitive SceneInfos once, apply authored visibility
  first, and produce exactly one classification plus typed visible membership
  per live primitive.
- [x] Treat plane contact and intersection as visible; implement the selected
  scale-aware epsilon and conservative invalid-bounds/view fallbacks.
- [x] Route StaticMesh and TextureCube preview preparation from typed visible
  lists without another full-scene scan or RTTI discovery.
- [x] Add immutable normal/culling-disabled comparison policy and prove an
  already-enqueued view cannot observe later debug-policy mutation.
- [x] Add classification assertions and submitted/hidden/frustum/fallback
  counter conservation.
- [x] Cover perspective, orthographic, mirrored/nonuniform transforms, all six
  planes, near-plane crossing, invalid bounds, invalid matrices, camera motion,
  and sequential views.

#### Acceptance Gate

- Every live primitive appears in exactly one visibility outcome and at most one
  matching typed visible list.
- Explicitly hidden and fully outside primitives issue no family base-pass draw.
- Invalid math remains visible through a named conservative fallback rather
  than disappearing or aborting the view.
- Culling-disabled output and counts match the Stage 1 baseline; normal mode
  removes only CPU-golden outside primitives.
- Main and auxiliary views with different cameras produce independent results
  from identical scene state.

#### Stage 2 Handoff

Baseline is `676e521e43fa3ee1177287e7466b5ae786776440`. The working set expanded from
the Stage 1 prepared owner, Scene renderer, StaticMesh preparation, and Stage 0
math seam to the direct `FSceneViewSettings`, `IScene`/`FScene`, primitive
component visibility synchronization, Renderer scene-contract tests, Vulkan
preparation fixture, and the one `IScene` test double.

`TryBuildViewFrustum(const FSceneView&, FViewFrustum&)` validates the immutable
projection snapshot before reusing the frozen inward-facing six-plane
extraction. `ClassifyWorldBounds` retains the Stage 0 scale-aware epsilon:
inside and intersecting including plane contact are visible, outside is culled,
and invalid bounds remain visible. `PrepareSceneVisibility` walks only
`FScene::GetPrimitiveSceneInfos()`, emits one `FPrimitiveVisibilityRecord` per
live primitive, and fills matching `StaticMeshSceneInfos` and
`TextureCubePreviewSceneInfos` typed lists. Family preparation consumes those
lists and performs no full-scene or RTTI discovery.

Authored hidden state now remains on the authoritative `FPrimitiveSceneInfo`:
component registration publishes its initial visibility and later owner
visibility changes enqueue `UpdatePrimitiveVisibility` without destroying the
proxy or stable id. Hidden classification precedes bounds work. An invalid view
marks every authored-visible primitive as `VisibleInvalidViewFallback`; an
invalid bound marks only that primitive as `VisibleInvalidBoundsFallback`.
`EViewVisibilityMode::{Normal, FrustumCullingDisabled}` is copied inside
`FSceneViewSettings`, so an enqueued view cannot observe later caller mutation;
the disabled mode still honors authored visibility and otherwise reproduces
Stage 1 membership.

`FViewRenderCounters` now records submitted, hidden, frustum-culled, visible,
invalid-bounds fallback, invalid-view fallback, and prepared-section counts.
The classifier asserts unique primitive ids and
`Submitted = Hidden + FrustumCulled + Visible`; fallback counts are visible
subsets. There are no open Stage 2 design questions. Validation on 2026-08-09
passed `RendererSceneContractTests` 4/4, `EditorRenderingTests` 38/38,
`WorldTests` 62/62, and `StaticMeshRenderPreparationVulkanTests` 1/1. Stage 2
also passed the focused new-classifier assertion side-effect audit, changed-doc
validation, all-plan validation, and the full `all` build. Stage 3 should add
validated StaticMesh LOD policy data without changing this
visibility ownership, typed-family routing, or immutable comparison seam.

### Stage 3: Publish explicit StaticMesh LOD policy data

Dependencies: Stage 0 schema decision and compatibility fixtures.

- [x] Add the selected finite transition descriptor to the ownership-correct
  StaticMesh render/build/payload representation.
- [x] Validate count, range, monotonic ordering, exact equality behavior, and
  guaranteed lowest-detail fallback transactionally before publication.
- [x] Generate documented deterministic defaults for importers, procedural
  meshes, and legacy-compatible paths that lack authored thresholds.
- [x] Update payload encoding/decoding, builder/schema versions, DDC keys, cook,
  runtime-only loading, and migration/rebuild behavior exactly as selected in
  Stage 0.
- [x] Preserve multi-LOD material slots, bounds, resource initialization,
  replacement, release fences, and render-data retry behavior.
- [x] Add distinct-geometry two- and three-LOD fixtures plus malformed count,
  NaN, unordered, equality, old-payload, DDC, cook, and runtime-only cases.
- [x] Document the lasting LOD data contract outside the active plan once it is
  implemented.

#### Acceptance Gate

- Every renderable LOD set has one validated deterministic transition policy;
  Renderer code contains no hidden distance/index ladder.
- Identical source/settings produce byte- and key-stable derived/cooked outputs.
- The selected old-data policy is explicit and tested; corrupt or ambiguous
  threshold data cannot publish partial render data.
- Existing single-LOD content resolves to LOD 0 without requiring editor edits.
- Resource initialization and release remain complete across every LOD and
  selected vertex factory.

#### Stage 3 Handoff

Baseline is `645d4429863162a6a28915a1d6092ea20250ef18`. The working set is the
StaticMesh runtime/resource and derived-data headers and implementations, the
payload/DDC/cooked-runtime and DAST v4 material fixtures, the existing Vulkan
lifecycle fixture, and the lasting StaticMesh/Material rendering contracts. No
Renderer visibility or prepared-view file changed.

`FStaticMeshLODResources::ScreenSize` and
`FStaticMeshPayloadLOD::ScreenSize` own the finite normalized threshold beside
their geometry. `GenerateDefaultStaticMeshLODScreenSizes` produces exact
`2^-(LODIndex + 1)` values with the final LOD forced to positive zero;
single-LOD importer, procedural debug, preview, thumbnail, and ordinary test
paths therefore publish `[0]`. `ValidateStaticMeshLODScreenSizes`, payload
validation, `DStaticMesh::CommitRenderDataCandidate`, and resource
initialization reject empty, non-finite, negative-zero, out-of-range,
non-descending, or nonzero-final policies before publication or upload. The
Stage 0 first-match `size >= ScreenSize` equality and unavailable-resource
goldens remain unchanged for Stage 4.

DMSH is schema 4 and the StaticMesh builder is version 3. The derived-data key
schema remains 1 and continues to encode both version values. Schema 3 and
older payloads are incompatible: source-backed assets and stale DDC entries
rebuild, while cooked/runtime-only content must be recooked; no compatibility
decoder synthesizes thresholds. Encoding, decoding, runtime reconstruction,
cook descriptors, and DDC restore copy the same threshold transactionally.
Two- and three-LOD fixtures use distinct vertex/index geometry and policies
`[0.5, 0]` and `[0.5, 0.25, 0]` while retaining material-slot, bounds, section,
vertex-factory, replacement, release, and retry contracts.

There are no open Stage 3 design questions. Validation on 2026-08-09 passed
the owned payload/DDC/cook/runtime suites 20/20, Stage 0 view/LOD goldens 6/6,
`SceneImportVulkanTests` 1/1, `EditorRenderingTests` 38/38, focused assertion
review with no newly introduced assertion, `StaticMeshTests` 52/52, and the
full `all` build. The material-package field checks and legacy-map fixture use
the production DAST v4 decoder and canonical re-encoder rather than assuming
the obsolete inline-string wire layout. Stage 4 should consume only the
validated published policy and retain LOD 0 as the invalid-size fallback.

### Stage 4: Select LODs and build two-level StaticMesh prepared work

Dependencies: Stage 2 visible StaticMesh list and Stage 3 policy data.

- [x] Implement the frozen projected-size helper for perspective and
  orthographic views using authoritative world bounds and fitted content
  viewport facts.
- [x] Select requested and actual LOD deterministically, including equality,
  near-plane, invalid-size, unavailable-resource, and single-LOD fallbacks.
- [x] Validate matching `LODResources` and `LODVertexFactories` before accepting
  a primitive; never reuse LOD-0 readiness as proof for another index.
- [x] Introduce prepared StaticMesh primitive records and relocation-safe
  indexed draw records, removing repeated transform/proxy/LOD facts from every
  section item.
- [x] Resolve each accepted selected-LOD section once into its M2 material,
  pass, graphics-state, geometry, and sort facts.
- [x] Bind and draw the selected vertex factory/index buffer/section rather than
  any implicit LOD-0 resource.
- [x] Add selected/requested LOD histograms, selected triangle/section counts,
  resource-fallback counts, and conservation assertions.
- [x] Validate camera motion around and exactly on thresholds at multiple output
  dimensions/aspects, perspective/orthographic projection, nonuniform scale,
  main/auxiliary sequencing, and resource failure/retry.

#### Acceptance Gate

- The same scene and fitted framing select the same LOD independent of absolute
  target dimensions, while camera/projection changes follow frozen goldens.
- Exact threshold equality, near-plane crossing, invalid math, and unavailable
  LOD behavior are deterministic and diagnostic.
- Every prepared draw references the selected primitive/LOD through a valid
  relocation-safe index and complete resource/material state.
- Distinct LOD fixtures produce the expected triangle counts and Vulkan pixels;
  forced LOD-0 mode matches the M2 baseline.
- No draw loop reads `LODResources[0]` or chooses a vertex factory implicitly.

#### Stage 4 Handoff

Baseline is `07be037cd183844f68c0efe01dbc847ee39d9885`. The working set is the
Renderer-private view math and StaticMesh preparation/execution boundary,
`FSceneViewSettings`, prepared-scene counters, the rendered-thumbnail preview
view, focused renderer/Vulkan fixtures, and the lasting StaticMesh rendering
contract.

Stage 0 `ComputeProjectedScreenSize`, `SelectStaticMeshLOD`, and
`ResolveAvailableStaticMeshLOD` are now used by production preparation.
`EViewLODMode::{Automatic, ForceLOD0}` is immutable per view. Automatic
selection measures authoritative world bounds in the fitted content viewport;
invalid/near-crossing math requests LOD 0. Readiness is checked independently
for every paired `LODResources`/`LODVertexFactories` index, with unavailable
requested LODs searching lower detail before higher detail and no ready LOD
rejecting the primitive.

`FPreparedStaticMeshPrimitive` owns primitive identity, requested/selected LOD,
transform, and selected LOD/VF borrows once. `FPreparedStaticMeshDraw` carries a
relocation-safe `PrimitiveIndex`, selected section geometry, material/pass/state,
and finite sort facts. Resource preparation and execution resolve that index
and bind the selected VF/index buffer/section; Renderer draw loops contain no
implicit index-zero LOD access.

Prepared counters conserve visible candidates against prepared plus rejected
primitives, requested/selected histograms against prepared primitives, and
selected sections against final pass buckets; selected triangles derive from
accepted selected sections. Fixtures cover perspective/orthographic views,
threshold equality and camera crossings, absolute-size invariance, nonuniform
scale, sequential views, single/three-LOD meshes, unavailable-resource fallback,
complete rejection, and retry. Vulkan output selects simplified geometry with
hash `36ff62c3dd2df3cd3cf45db46e9e4198`; forced LOD 0 retains the prior M2 hash
`bdd34099da4b080de210ad2d9af122a9`.

There are no open Stage 4 design questions. Focused validation on 2026-08-09
passed `EditorRenderingTests` 38/38, `RendererSceneContractTests` 4/4,
`StaticMeshRenderPreparationVulkanTests` 1/1, `SceneImportVulkanTests` 1/1,
and `StaticMeshTests` 52/52. Changed-document validation, all-plan validation,
and the full `all` build passed. The new conservation assertions have no side
effects; the full assertion presubmit remains blocked by stale allowlist line
entries across the rebased baseline, including unrelated Core, DObject, Launch,
and Editor files. Stage 5 should consume the two-level prepared buckets to
stabilize opaque/masked grouping and complete execution diagnostics without
reopening LOD selection.

### Stage 5: Stabilize state ordering and complete diagnostics

Dependencies: Stage 4 complete prepared buckets.

- [x] Define complete value-based Opaque and Masked grouping/sort keys from
  effective pass, pipeline, material/shader, vertex-factory, geometry, and
  stable identity facts.
- [x] Sort Opaque and Masked deterministically without changing pass order;
  count state groups and measured transitions.
- [x] Preserve Translucent distance-first ordering while extending complete
  ties with selected LOD and section facts.
- [x] Complete submitted, visibility, fallback, LOD, pass, section, triangle,
  state-group, attempted, successful, and rejected draw counters.
- [x] Expose/emit one counter snapshot per view invocation through the selected
  development/profiling seam without introducing persistent target-size or
  temporal caches.
- [x] Add assertions for all conservation equations and diagnostics for invalid
  classification, incomplete commands, non-finite keys, bucket mismatch, and
  execution outside the owning prepared lifetime.
- [x] Compare deterministic grouping against the M2 image baseline, with focused
  coplanar/equal-depth, multiple-material, remove/re-add, camera-motion, and
  auxiliary-view cases.

#### Acceptance Gate

- Repeated identical inputs produce identical draw order and counters without
  pointer-address or unordered-container ties.
- Opaque/Masked grouping reduces or preserves state-group counts on the selected
  representative scene and never reorders passes.
- Translucent output retains the M2 back-to-front contract.
- Counters reconcile for normal, hidden, culled, fallback, malformed-resource,
  and failed-pipeline views and identify the phase that rejected work.
- Any accepted image-baseline change has a recorded deterministic cause and
  lasting contract; otherwise output remains unchanged.

#### Stage 5 Handoff

Baseline is `65827d3e1eef0db0408a3f89e68225fa873ef95e`. The working set is the
Renderer-private StaticMesh prepared-key/resource/execution boundary,
command-local view counters and snapshot seam, focused renderer/Vulkan fixtures,
the Engine test target's Vulkan failure-injection linkage, and the lasting
StaticMesh rendering contract.

`FStaticMeshDrawSortKey` is value-only: it orders effective pass/pipeline and
material/shader identity, validated material uniform bytes, local vertex-factory
declaration facts, section geometry, then primitive id, selected LOD, and section
index. Opaque and Masked sort by the complete key while retaining their fixed
pass order. Translucent still sorts by descending per-view distance first and
uses the complete key only for exact-distance ties. No pointer address or
unordered-container order participates. The representative alternating-material
fixture reduced Opaque state groups from 8 input groups to 2 final groups;
identical repeats and remove/re-add produced identical keys, order, groups, and
transition counts.

The counter schema now includes pass section/triangle totals, input/final state
groups, pipeline/material/vertex-factory/geometry transitions, resource
preparation attempted/successful/rejected draws, and execution
attempted/successful/rejected draws in addition to visibility, fallback, and LOD
facts. `SetViewRenderCounterSink`/`EmitViewRenderCounterSnapshot` expose one
immutable value at the end of every `RenderView` invocation without retaining a
view or temporal cache. `EPreparedStaticMeshPhase` enforces prepare-resources-
execute ownership exactly once; bucket/pass, sort-key/pass, complete command,
finite key, classification, and conservation assertions diagnose boundary
violations. A controlled Vulkan sampler-creation failure reconciled as 1 resource
attempt, 0 resource successes, 1 resource rejection, then 1 attempted and 1
rejected execution draw.

There are no open Stage 5 design questions. Focused validation on 2026-08-09
passed `StaticMeshRenderPreparationVulkanTests` 1/1,
`RendererSceneContractTests` 5/5, `SceneImportVulkanTests` 1/1,
`EditorRenderingTests` 38/38, `RendererResourceReloadVulkanTests` 1/1,
`MaterialTests` 78/78, and `StaticMeshTests` 52/52. Automatic simplified LOD
output remains `36ff62c3dd2df3cd3cf45db46e9e4198`; forced LOD 0 remains
`bdd34099da4b080de210ad2d9af122a9`, so deterministic grouping caused no image
baseline change. Changed-document/all-plan validation and the full `all` build
passed. The assertion audit still stops on stale allowlist line entries across
the rebased baseline; it reported no new Stage 5 assertion-side-effect finding.
Stage 6 should enable and qualify the production defaults, remove migration
seams that are no longer required, and close M3.

### Stage 6: Enable production defaults and qualify M3

Dependencies: Stages 0-5 and all recorded handoffs.

- [x] Make authored visibility, frustum culling, and deterministic LOD selection
  the production defaults while retaining the explicit comparison seam for
  tests and diagnosis.
- [x] Remove obsolete LOD-0 assumptions, duplicate visibility checks, legacy
  prepared-section shapes, redundant scene casts, and temporary migration code
  not required by the selected compatibility policy.
- [x] Update Runtime Rendering documentation with lasting preparation,
  visibility, LOD, ordering, lifetime, fallback, and counter contracts.
- [x] Update this plan and the Rendering Capability Expansion Roadmap with M3
  completion evidence and the now-open M4/M6 gates.
- [x] Run focused CPU math, Scene contract, StaticMesh payload/DDC/cook/runtime,
  material/pass, viewport/editor, preparation, resource-failure, and Vulkan
  suites using repository guidance.
- [x] Validate perspective/orthographic, main/auxiliary, present/offscreen,
  fixed-aspect, thumbnails/previews, Lit/Unlit, Solid/Wireframe, camera motion,
  mutation, reload, device invalidation, and shutdown.
- [x] Run changed/all-plan documentation validation and the required full
  `all` build; perform the editor smoke required for this user-visible rendering
  milestone.
- [x] Record the final compact handoff with exact baseline, working set, symbols,
  decisions, remaining limitations, counters, test/build outcome, and verified
  editor executable.

#### Acceptance Gate

- The roadmap M3 exit gate is satisfied: invisible primitives issue no base-
  pass draw, LOD thresholds are deterministic, pass ordering remains correct,
  and counters explain submitted/culled/selected work in every viewport path.
- Normal production behavior passes focused CPU and Vulkan image evidence;
  culling-disabled forced-LOD-0 comparison remains available and qualified.
- Preparation completes before the first scene pass and execution contains no
  scene discovery, material resolution, resource creation, visibility test, or
  LOD selection.
- Main and auxiliary views share no prepared result or semantic cache.
- Lasting rules live in Runtime Rendering documentation, the full build/editor
  smoke pass, and M4 can consume the private visibility/preparation seam without
  copying the frame renderer.

#### Stage 6 Handoff

Baseline is `8027ccf99bc0e8241e5ca564cd9656129041c856`; the completion cohort is
Stages 0 through 5 recorded above plus this Stage 6 implementation commit. The
final working set is the Renderer-private prepared-view orchestration, the
production-default view contract and Scene test, the StaticMesh Runtime
contract, M3 roadmap state, and two post-rebase native-test repairs required to
qualify the full repository suite.

`FSceneViewSettings` retains `Normal` visibility and `Automatic` LOD as its
production defaults, with `FrustumCullingDisabled` and `ForceLOD0` as explicit
immutable comparison policies. `FSceneRenderer::RenderView_RenderThread`
creates `FSceneVisibilityResult` only inside the pre-pass preparation scope;
`FPreparedSceneView` no longer retains the unused SceneInfo classification
lists. `PrepareSceneVisibility`, `PrepareStaticMeshView_RenderThread`,
`FPreparedStaticMeshPrimitive`, `FPreparedStaticMeshDraw`, and
`EPreparedStaticMeshPhase` remain the authoritative classification,
selected-LOD, draw, and exactly-once resource/execution boundaries. The one
`IScene` to `FScene` conversion in `FRendererModule` is the intentional module
adapter boundary, not a repeated feature-renderer cast. Frozen exact-v2
material snapshots remain supported by the selected compatibility policy;
schema-3 StaticMesh payloads remain deliberately incompatible and require
rebuild/recook.

Counter conservation and image evidence remain unchanged from Stage 5:
automatic simplified LOD renders
`36ff62c3dd2df3cd3cf45db46e9e4198`, forced LOD 0 renders
`bdd34099da4b080de210ad2d9af122a9`, and the controlled Vulkan resource failure
attributes one attempted/rejected resource draw followed by one
attempted/rejected execution draw. Production-default assertions were added to
`RendererSceneContractTests`. The full native case aggregate and all direct
whole-executable lifecycles passed, covering the focused CPU math, Scene,
StaticMesh payload/DDC/cook/runtime, material/pass, viewport/editor,
thumbnail/preview, preparation, failure/reload, and Vulkan suites. A stale
handwritten DAST-v3 cook fixture now uses the production v4 writer and emits
publication diagnostics; two Core task tests now synchronize child admission
and asynchronous terminal-retention cleanup instead of racing their own
observations.

Changed-document validation, all-plan validation, the full `all` build, and an
eight-tick hidden-window editor smoke passed on 2026-08-09. The verified editor
is `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`. The
repository assertion audit remains blocked by stale allowlist line entries
across the rebased baseline; it found no new Stage 6 assertion-side-effect
category. Deferred limits remain the M4 second-family generalization proof, M6
shadow-specific visibility/LOD/depth work, temporal LOD policy, occlusion/GPU
submission, and authored threshold UX already listed below.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Projection/frustum | Known perspective/orthographic matrices; six planes; inside/intersect/outside; exact contact; near/camera crossing; invalid matrices | Main and auxiliary cameras cull only CPU-golden outside primitives. |
| Visibility ownership | Hidden/update/show, attach/replace/remove, typed kind, duplicate prevention, invalid bounds | Every live primitive has one outcome; no reflected object or stale SceneInfo read occurs. |
| LOD data | Count/range/order/equality, deterministic defaults, malformed values, schema/DDC/cook/runtime compatibility | Every published multi-LOD mesh has one stable policy across editor and runtime-only loads. |
| LOD selection | Perspective/orthographic, thresholds, camera motion, target dimensions, fixed aspect, scale/mirroring, unavailable resources | Requested/actual indices and triangle counts match goldens without LOD-0 aliasing. |
| Prepared lifetime | One view owner, relocation-safe indices, sequential views, proxy replacement/removal, scene release | No prepared borrow survives its render command or leaks across views. |
| Pass policy | Opaque/Masked grouping, Translucent distance and ties, material fallback, Lit/Unlit, Solid/Wireframe | M2 surface behavior and pass order remain authoritative for every selected LOD. |
| Resource/failure | Missing/malformed LOD, vertex-factory mismatch, shader/PSO failure, retry, invalidation, shutdown | Failure rejects the smallest complete unit and never executes partial work inside a pass. |
| Counters | Normal, hidden, culled, fallback, invalid resource, failed execution, empty scene | Conservation identities hold and one view result explains every submitted primitive and attempted draw. |
| Multi-view/composition | Main/auxiliary, present/offscreen, different cameras/dimensions/settings, fixed aspect, thumbnail/preview, post-process/assistance | Results remain view-local and fixed-aspect bars/editor-assistance composition are unchanged. |
| RHI/Vulkan | Selected LOD vertex/index binding, distinct geometry images, culling comparison, reload, validation layers | CPU selection and GPU output agree without resource work inside active render passes. |
| Qualification | Focused suites, documentation validators, full `all` build, editor smoke | M3 is ready for M4 and M6 and has a verified user-visible handoff. |

## Definition of Done

- One fitted immutable view produces one command-local prepared scene result
  before any scene render pass.
- One centralized primitive classifier applies authored visibility and
  conservative frustum culling and supplies authoritative typed visible lists.
- Every renderable StaticMesh LOD set owns validated deterministic transition
  policy data through derived/cooked/runtime publication.
- StaticMesh preparation selects requested/actual LODs, builds one prepared
  primitive plus complete indexed draw records, and executes the selected
  resources without implicit LOD 0.
- Opaque/Masked grouping and Translucent ordering are deterministic and retain
  M2 pass/material behavior.
- Per-view counters reconcile submitted, hidden, culled, fallback, selected LOD,
  pass, section, triangle, group, and draw work.
- Invalid math is conservative, malformed resources fail locally, and no
  reflected object or expired prepared borrow is read by rendering.
- Main, auxiliary, present, offscreen, fixed-aspect, thumbnail, preview,
  Lit/Unlit, Solid/Wireframe, reload, invalidation, and shutdown validation pass.
- Lasting contracts are moved to Runtime Rendering documentation, the roadmap
  records M3 completion, documentation validation passes, and the required full
  build/editor smoke produces a verified executable.

## Deferred Follow-ups

- M4 extraction of genuinely common prepared mesh-pass fields after the second
  production vertex factory proves them.
- M6 shadow-view visibility, shadow-specific LOD/caster policy, depth-only
  commands, bias, filtering, and sampling.
- Temporal LOD hysteresis or dithered transitions after stable view identity and
  a temporal consumer exist.
- Occlusion/HZB, spatial acceleration, instancing, indirect draws, GPU culling,
  meshlets, and GPU-driven submission after M3 counters identify a measured
  bottleneck.
- Editor-authored threshold UX, automated mesh reduction, and platform quality
  tiers through separately bounded asset/editor plans.
- Public renderer/pass registration only after a named external module requires
  it.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../../../Roadmaps/RenderingCapabilityExpansion.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Material System](../../../Runtime/Rendering/MaterialSystem.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Scene Proxy and Info Contract Plan](RendererSceneProxyAndInfoContract.md)
- [Material Render Pass Policies Plan](MaterialRenderPassPolicies.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
