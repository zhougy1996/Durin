# Per-View Visibility and LOD Plan

Summary: Establish a renderer-owned per-view preparation boundary with conservative primitive visibility, deterministic StaticMesh LOD selection, reusable prepared draw data, stable state ordering, and explainable counters.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

This plan is the active M3 child of the
[Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md).
Implementation has not started. M1 supplies detached primitive identity,
transform, bounds, visibility, and typed scene membership; M2 supplies complete
StaticMesh material/pass classification and effective graphics state.

The current renderer still prepares StaticMesh work from inside
`FStaticMeshRenderer::DrawScene_RenderThread`, after the Scene Color pass has
begun. It visits every typed StaticMesh SceneInfo, always selects
`LODResources[0]`, repeats primitive facts in every section item, and creates no
view-wide visibility result or performance explanation. `FPrimitiveSceneInfo`
already publishes the facts needed to replace this path, while current
StaticMesh payloads and render data can store multiple LODs but provide no
selection thresholds.

M3 therefore begins with an intentional preparation-boundary redesign. It does
not preserve the current monolithic draw entrypoint as the long-term interface.
The first implementation stage must retain a behavior-equivalent LOD-0,
culling-disabled comparison mode; production culling and LOD selection become
the default only after boundary, transition, multi-view, and image validation
pass.

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

### Stage 0: Freeze the view-preparation and LOD contracts

Dependencies: completed M1 and M2 plans and their lasting Runtime Rendering
contracts.

- [ ] Record the implementation baseline, initial working set, current render-
  pass/resource ordering, current `IScene` casts, M2 prepared-item lifetime,
  and all viewport consumers without expanding into unrelated renderer systems.
- [ ] Freeze Durin's view/clip conventions with perspective and orthographic
  matrix goldens, including near/far mapping, plane orientation, camera-facing
  direction, reversed or ordinary depth, fixed-aspect content dimensions, and
  plane-touch epsilon.
- [ ] Select exact Renderer-private names and ownership for the command-local
  prepared scene view, generic visibility result, typed visible lists,
  StaticMesh prepared primitive/draw records, and counters.
- [ ] Freeze the normalized projected-size formula, near-plane/camera-crossing
  behavior, exact threshold equality rule, LOD fallback search, and deterministic
  default thresholds.
- [ ] Trace StaticMesh authored, imported, derived, DDC, cooked, and runtime-only
  paths and decide the smallest ownership-correct threshold schema change.
- [ ] Record explicit compatibility policy for existing authored assets, DDC
  entries, cooked payloads, procedural meshes, thumbnails, and test fixtures.
- [ ] Define comparison modes and counter conservation equations before
  production behavior changes.
- [ ] Add pure CPU golden fixtures for frustum classification and LOD selection,
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

Record the baseline commit, five-file initial working set, selected type names,
projection/size formulas, LOD schema and compatibility decision, fixtures,
open questions, and validation outcome.

### Stage 1: Move preparation ahead of render passes

Dependencies: Stage 0 decisions and fixtures.

- [ ] Convert `IScene` to Renderer-private `FScene` once at the renderer module
  boundary and pass typed private references through scene preparation and
  execution.
- [ ] Introduce the command-local prepared-scene-view owner and make
  `FSceneRenderer` create it after `FitViewToOutput` and before Scene Color pass
  construction.
- [ ] Split StaticMesh discovery/preparation, demanded resource readiness, and
  execution into distinct render-thread functions.
- [ ] Move StaticMesh resource/shader/pipeline readiness outside active render
  passes; preserve complete-or-null creation slots and last-known-good retry
  behavior.
- [ ] Re-express current M2 LOD-0 buckets through the new boundary with culling
  disabled and no visible output change.
- [ ] Delete the old monolithic entrypoint and redundant private `IScene`
  discovery only after all callers use prepared data.
- [ ] Add ordering assertions proving no preparation or resource creation occurs
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

Record the baseline commit, working set, prepared-view owner, changed render
sequence, removed monolithic symbols, output comparison, resource validation,
open questions, and validation outcome.

### Stage 2: Implement centralized conservative primitive visibility

Dependencies: Stage 1 prepared-view lifetime and Stage 0 math goldens.

- [ ] Implement finite frustum construction and world-AABB classification in a
  pure/testable Renderer-private math seam.
- [ ] Walk authoritative primitive SceneInfos once, apply authored visibility
  first, and produce exactly one classification plus typed visible membership
  per live primitive.
- [ ] Treat plane contact and intersection as visible; implement the selected
  scale-aware epsilon and conservative invalid-bounds/view fallbacks.
- [ ] Route StaticMesh and TextureCube preview preparation from typed visible
  lists without another full-scene scan or RTTI discovery.
- [ ] Add immutable normal/culling-disabled comparison policy and prove an
  already-enqueued view cannot observe later debug-policy mutation.
- [ ] Add classification assertions and submitted/hidden/frustum/fallback
  counter conservation.
- [ ] Cover perspective, orthographic, mirrored/nonuniform transforms, all six
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

Record the baseline commit, frustum symbols and conventions, classifier and
typed outputs, epsilon/fallback decisions, comparison seam, counter identities,
open questions, and validation outcome.

### Stage 3: Publish explicit StaticMesh LOD policy data

Dependencies: Stage 0 schema decision and compatibility fixtures.

- [ ] Add the selected finite transition descriptor to the ownership-correct
  StaticMesh render/build/payload representation.
- [ ] Validate count, range, monotonic ordering, exact equality behavior, and
  guaranteed lowest-detail fallback transactionally before publication.
- [ ] Generate documented deterministic defaults for importers, procedural
  meshes, and legacy-compatible paths that lack authored thresholds.
- [ ] Update payload encoding/decoding, builder/schema versions, DDC keys, cook,
  runtime-only loading, and migration/rebuild behavior exactly as selected in
  Stage 0.
- [ ] Preserve multi-LOD material slots, bounds, resource initialization,
  replacement, release fences, and render-data retry behavior.
- [ ] Add distinct-geometry two- and three-LOD fixtures plus malformed count,
  NaN, unordered, equality, old-payload, DDC, cook, and runtime-only cases.
- [ ] Document the lasting LOD data contract outside the active plan once it is
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

Record the baseline commit, data symbols, schema/builder/DDC versions,
compatibility policy, generated defaults, representative assets, lifecycle
validation, open questions, and validation outcome.

### Stage 4: Select LODs and build two-level StaticMesh prepared work

Dependencies: Stage 2 visible StaticMesh list and Stage 3 policy data.

- [ ] Implement the frozen projected-size helper for perspective and
  orthographic views using authoritative world bounds and fitted content
  viewport facts.
- [ ] Select requested and actual LOD deterministically, including equality,
  near-plane, invalid-size, unavailable-resource, and single-LOD fallbacks.
- [ ] Validate matching `LODResources` and `LODVertexFactories` before accepting
  a primitive; never reuse LOD-0 readiness as proof for another index.
- [ ] Introduce prepared StaticMesh primitive records and relocation-safe
  indexed draw records, removing repeated transform/proxy/LOD facts from every
  section item.
- [ ] Resolve each accepted selected-LOD section once into its M2 material,
  pass, graphics-state, geometry, and sort facts.
- [ ] Bind and draw the selected vertex factory/index buffer/section rather than
  any implicit LOD-0 resource.
- [ ] Add selected/requested LOD histograms, selected triangle/section counts,
  resource-fallback counts, and conservation assertions.
- [ ] Validate camera motion around and exactly on thresholds at multiple output
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

Record the baseline commit, projected-size and selection symbols, prepared
primitive/draw layout, fallback rules, selected-resource execution path,
counter results, image evidence, open questions, and validation outcome.

### Stage 5: Stabilize state ordering and complete diagnostics

Dependencies: Stage 4 complete prepared buckets.

- [ ] Define complete value-based Opaque and Masked grouping/sort keys from
  effective pass, pipeline, material/shader, vertex-factory, geometry, and
  stable identity facts.
- [ ] Sort Opaque and Masked deterministically without changing pass order;
  count state groups and measured transitions.
- [ ] Preserve Translucent distance-first ordering while extending complete
  ties with selected LOD and section facts.
- [ ] Complete submitted, visibility, fallback, LOD, pass, section, triangle,
  state-group, attempted, successful, and rejected draw counters.
- [ ] Expose/emit one counter snapshot per view invocation through the selected
  development/profiling seam without introducing persistent target-size or
  temporal caches.
- [ ] Add assertions for all conservation equations and diagnostics for invalid
  classification, incomplete commands, non-finite keys, bucket mismatch, and
  execution outside the owning prepared lifetime.
- [ ] Compare deterministic grouping against the M2 image baseline, with focused
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

Record the baseline commit, key definitions, before/after state counts, complete
counter schema and exposure seam, image disposition, open questions, and
validation outcome.

### Stage 6: Enable production defaults and qualify M3

Dependencies: Stages 0-5 and all recorded handoffs.

- [ ] Make authored visibility, frustum culling, and deterministic LOD selection
  the production defaults while retaining the explicit comparison seam for
  tests and diagnosis.
- [ ] Remove obsolete LOD-0 assumptions, duplicate visibility checks, legacy
  prepared-section shapes, redundant scene casts, and temporary migration code
  not required by the selected compatibility policy.
- [ ] Update Runtime Rendering documentation with lasting preparation,
  visibility, LOD, ordering, lifetime, fallback, and counter contracts.
- [ ] Update this plan and the Rendering Capability Expansion Roadmap with M3
  completion evidence and the now-open M4/M6 gates.
- [ ] Run focused CPU math, Scene contract, StaticMesh payload/DDC/cook/runtime,
  material/pass, viewport/editor, preparation, resource-failure, and Vulkan
  suites using repository guidance.
- [ ] Validate perspective/orthographic, main/auxiliary, present/offscreen,
  fixed-aspect, thumbnails/previews, Lit/Unlit, Solid/Wireframe, camera motion,
  mutation, reload, device invalidation, and shutdown.
- [ ] Run changed/all-plan documentation validation and the required full
  `all` build; perform the editor smoke required for this user-visible rendering
  milestone.
- [ ] Record the final compact handoff with exact baseline, working set, symbols,
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

Record the completion commit cohort, final working set, authoritative symbols
and Runtime contracts, counter and image evidence, compatibility disposition,
known deferred limits, validation outcome, and verified editor executable.

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

- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Renderer Scene Proxy and Info Contract Plan](RendererSceneProxyAndInfoContract.md)
- [Material Render Pass Policies Plan](MaterialRenderPassPolicies.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

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
