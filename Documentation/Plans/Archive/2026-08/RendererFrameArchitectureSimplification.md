# Renderer Frame Architecture Simplification Plan

Summary: Turn the recently extracted scene-frame stages into real ownership boundaries, centralize frame-transient resolution, and simplify fixed Renderer data flow without changing rendering behavior.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

The completed Renderer frame-preparation refactor established valuable
foundations: receiver and shadow logical draws are distinct from most fallible
execution resources, the production view follows one fixed schedule, temporal
state is committed transactionally, GBuffer execution publishes a typed
result, telemetry is published only after successful output finalization, and
a shared transient-target pool owns the physical frame textures.

A post-refactor architecture review found that the intended boundaries are not
yet represented by the implementation structure. `FFixedSceneFrameExecutor`,
`FSceneFramePreparation`, and `FSceneFrameFinalization` are empty forwarding
types whose operations delegate back into private `FSceneRenderer` methods.
`FSceneRenderer` therefore remains the actual frame scheduler and exposes a
large friend/private orchestration surface. The preparation implementation
also resolves GPU resources, performs dynamic uploads, and records the
directional-shadow pass before returning.

Frame-transient acquisition remains distributed across feature renderers.
Pass helpers call `EnsureTargets_RenderThread` during execution and feature
renderer state stores mutable `CurrentTargets` values. This conflicts with the
lasting contract that frame setup owns complete typed leases and execution
receives already-resolved targets. Most pass helpers also communicate by
mutating the frame plan, deferred parameters, and shared counters rather than
returning typed results. Prepared and resolved geometry retain numerous
telemetry fields, and resolved draw readiness uses pointer-keyed hash
containers on the per-frame hot path.

Stages 0 through 4 are complete. The current schedule, failure and transaction
policy, target inventory, selected frame types, option ownership, structural/
performance baseline, and required tests are frozen below. The unchanged
baseline passes all 28 `RendererSceneContractTests` and the GBuffer
qualification. The first volumetric-cloud qualification sample missed its
existing relative timing gate under contention; an unchanged exact rerun
passed, so the recorded cloud timings are diagnostic and no threshold was
changed.

`FFixedSceneFrameExecutor` is now the real non-copyable/non-movable scheduler
and directly owns explicit references to the 18 Renderer services it uses.
`FSceneRenderer` retains composition, lifetime/invalidation, view-state
registry, and the narrow frame entry, but no longer declares or implements a
second private pass API. The empty preparation/finalization types, two friends,
three files, and their empty-type assertions are removed; preparation remains a
focused executor implementation file and finalization remains adjacent to the
fixed sequence rather than creating another facade. Additional pass-file
splitting was deliberately not performed because it would fragment the only
ordering authority without narrowing dependencies in this stage. The updated
ownership passes 28 Renderer scene contract tests, 77 editor rendering tests,
and the GBuffer qualification.

Logical preparation now returns a typed success-or-failure value containing
one complete `FSceneRenderPlan`; the executor publishes that plan as `const`
for every later boundary. `FResolvedSceneFrame` separately owns receiver and
shadow resources, the lighting uniform, cloud sampler bindings, and a resolved
Skeletal palette table. Logical palette entries contain only immutable poses;
upload attempt/range/accounting state is shared through the resolved table.
Directional-shadow resource preparation and command recording are explicit
post-preparation steps, and cloud sampler creation moved to resolution. The
stable combined-translucency family/index handle remains the only cross-family
logical dispatch identity. Compile-time tests reject resolved, telemetry, and
upload-range fields in logical types. Validation passed 28 Renderer scene
contract tests, 77 editor rendering tests, the GBuffer qualification, and the
volumetric-cloud qualification; both GPU qualification suites required an
unchanged rerun of their known relative timing gates, with no threshold
changes.

The executor now derives one immutable `FSceneFrameRequirements` after logical
and execution-resource resolution, then resolves Scene Color/depth and every
requested feature bundle into `FResolvedSceneFrame::Targets` before the first
consuming pass. Execution contains no target acquisition, and feature renderers
retain no `CurrentTargets` variants. Target factories return owning optional
bundle values, so multi-view submissions cannot overwrite another frame's
feature-local target view. The transient pool uses 12 bounded typed groups with
per-group lookup rather than string identities and a global scan. Bundle
acquisition validates descriptions up front and rolls back newly created
successful siblings on failure while preserving same-generation retry
suppression and applying the retention budget. Validation passed 28 Renderer
scene contract tests, 77 editor rendering tests, 7 target-pool/recovery Vulkan
tests, the volumetric-cloud Vulkan test, and unchanged GBuffer and volumetric-
cloud qualification gates. The final GBuffer run first missed the existing
half/full GTAO relative timing gate by about 0.08%; the unchanged exact rerun
passed, so no threshold was adjusted.

The fixed sequence now stores directional-shadow, GBuffer, GTAO, contact,
cloud-shadow, isolated-deferred, Scene Color/cloud, and post-process outcomes
in one `FSceneFrameOutcome`. Complete predicates require both a complete status
and the feature's mandatory outputs. Optional producers no longer mutate the
shared deferred parameter block: one `BuildDeferredParameters` boundary forms
the binding set from predecessor outcomes and documented fallbacks, while
isolated and production deferred paths receive separate copies. Cloud-shadow
visibility is an explicit cloud-composite input rather than executor member
state, and Scene Color/post process return typed values instead of rewriting a
bare texture reference. Timing/capture observers remain immutable and
telemetry is reduced independently of pass correctness. Validation passed 29
Renderer scene contract tests, 77 editor rendering tests, and the unchanged
volumetric-cloud and GBuffer qualification gates. The GBuffer half/full GTAO
relative timing gate and the cloud High/Reference gate each needed two
unchanged quiet-window reruns before passing; no threshold or rendering policy
changed. Stage 5 is ready to simplify geometry result storage and observation
plumbing.

Stage 5 is complete. StaticMesh/SplineMesh,
SkeletalMesh, and Terrain draws receive stable contiguous `ResolvedIndex`
values after their final sort; Terrain batches receive the same treatment.
Each resolved draw record now co-locates its optional material binding and
readiness bit, Skeletal palette ranges align with prepared primitive indices,
and no submission-local resolved geometry type contains a pointer-keyed draw,
primitive, or batch hash container. The frozen allocation proxy therefore
dropped from eight pointer-keyed containers to zero. Focused StaticMesh,
SkeletalMesh, Terrain contract/Vulkan tests and the unchanged GBuffer and
Terrain qualifications pass. Resource preparation, GBuffer, and execution
measurements now live in feature-local `Observations` values; resolved
correctness views expose only resources and index-aligned records, and
compile-time tests reject direct execution-counter fields. The common index and
conservation helpers preserve explicit family dispatch without a virtual
registry. The measured allocation proxy fell from eight pointer-keyed hash
containers to zero; target-byte gates and command/image qualifications remain
unchanged. The executor/preparation implementation is currently 1,554/624
lines and the qualified test executable is 2,527,232 bytes; Stage 0 did not
freeze an executable-size value, so these are recorded for final comparison
rather than treated as a new threshold. The Stage 5 acceptance gate is met.

Stage 6 is complete. Public `FSceneViewRenderOptions` now contains only the
production environment override and supported GBuffer, deferred, and GTAO
debug modes. Qualification-only GBuffer/deferred/GTAO enablement and forced
contact/cloud fragment routes live in one Renderer-private
`FRendererQualificationPolicy`. Tests install it with a lexical render-thread
scope, and the fixed executor snapshots the immutable value once per
submission. The cloud preparation callback and its mutable qualification
output were removed; qualification policy never enters or rewrites the logical
plan. Compile-time contracts reject the former public switches, a nesting test
proves scoped restoration, and production, editor debug, geometry, cloud,
directional-shadow, and GBuffer qualification coverage pass through the same
fixed scheduler. Stage 7 is ready for aggregate validation and handoff.

Stage 7 is complete. The full `Win64-Debug-DurinEditor` `all` build passed,
and the complete routine native-test matrix passed in both inline and threaded
RHI execution modes. The Renderer qualification set passed inline; the current
code also passed an exact normal-threaded GBuffer qualification before the
documentation-only finalization, while directional-shadow, HDR mapping, and
volumetric-cloud qualification passed in the later threaded aggregate. Terrain
qualification passed in both modes. Later exact GBuffer samples, collected
while `nvidia-smi` reported 6–18% competing desktop/application GPU activity,
missed only the existing cross-batch GTAO/contact timing ratios; these samples
are retained as non-authoritative contention diagnostics under the documented
quiet-GPU rule, and no implementation or threshold changed.

Both process smokes passed: the hidden Editor initialized, rendered 120 ticks,
and shut down normally; the visible 600-tick Renderer contact smoke exercised
main/auxiliary views, Auto/Compute/Fragment and contribution-debug routes,
shader reload, resource retry, resize/restore, and normal shutdown. Focused and
full-matrix coverage also exercised invalid output/environment, optional target
failure, retry/invalidation, multi-view/view-state transactions, reload,
resource release, and restart paths.

The final structural audit finds zero empty frame-stage facades, feature
`CurrentTargets`, execution-time target ensures, pointer-keyed resolved geometry
hashes, public qualification switches, or mutable cloud preparation callbacks.
The hot-path proxies changed from 11 target ensures to zero, eight pointer-keyed
containers to zero, and 127 direct telemetry-counter touches to 65. Final
executor/preparation sizes are 1,593/626 lines; the explicit typed contracts
increase those two files relative to the 1,514/491 Stage 0 snapshot, while the
three forwarding files and duplicate `FSceneRenderer` pass surface remain
deleted. `GBufferQualificationTests.exe` is 2,528,256 bytes, 1,024 bytes
(0.04%) above the Stage 5 diagnostic snapshot; Stage 0 had no executable-size
baseline. Representative final medians remained at or below the frozen values
(GBuffer 79,440 ns, isolated deferred 250,848 ns, combined 330,272 ns, hybrid
397,744 ns), and active/retained ceilings remain 69,984,000/268,435,456 bytes.
The lasting frame-preparation and resource-recovery contracts now describe the
implemented owners without depending on this plan.

## Goal

Make one concrete fixed-frame executor the sole scene-view scheduler; publish
logical preparation as immutable input to separately resolved frame resources;
acquire every requested frame-transient target through one frame-resolution
boundary; express pass dependencies through typed inputs and results; and
remove redundant state, pointer-keyed lookup, and observation plumbing from
the execution hot path.

The resulting design must be smaller than the current forwarding architecture,
must remain understandable as a fixed ordered sequence, and must provide a
direct future handoff to a bounded Render Graph without introducing a graph,
registry, or runtime-polymorphic feature system in this plan.

## Scope

- Renderer-private ownership and implementation of fixed scene-frame
  preparation, resource resolution, execution, output finalization, and
  temporal/telemetry commit.
- `FSceneRenderer` composition, friend surface, and frame-entry delegation.
- The logical, resolved, target, pass-result, and observation values currently
  aggregated under `FSceneRenderPlan` and family-specific prepared/resolved
  geometry types.
- Directional-shadow logical preparation, resource resolution, target
  acquisition, and execution placement in the fixed schedule.
- Scene Color/depth, GBuffer, GTAO, contact-shadow, cloud-shadow, deferred/debug,
  cloud, and post-process frame-transient target acquisition.
- StaticMesh/SplineMesh, SkeletalMesh, and Terrain resolved-draw indexing,
  material bindings, palette uploads, execution measurements, and combined
  translucency dispatch.
- Typed results and exact inputs for GBuffer, GTAO, contact visibility,
  cloud-shadow visibility, isolated deferred/debug, Scene Color, clouds,
  post process, editor assistance, and final output.
- Production options, renderer debug options, and qualification-only override
  ownership at the Renderer boundary.
- Structural, focused, aggregate, Vulkan/image, failure/recovery, multi-view,
  performance, full-build, runtime-smoke, and documentation validation.

## Non-Goals

- Introducing `FRDGBuilder`, graph compilation, automatic barrier generation,
  pass culling, physical transient aliasing, pass merging, or graph debugging.
- Changing the fixed pass order, render-pass layouts, explicit transition
  authority, output access states, or temporal commit/abort semantics.
- Changing visible shading, GBuffer encoding, material evaluation, shadow
  fitting/filtering, GTAO, cloud sampling/reconstruction, post process, editor
  assistance, or presentation policy.
- Adding asynchronous compute, multiple GPU queues, parallel command recording,
  or task-parallel scene preparation.
- Adding GPU-driven visibility, indirect drawing, meshlets, bindless resources,
  a persistent mesh draw-command cache, clustered lighting, or local-light
  shadows.
- Replacing feature-owned persistent shader, PSO, sampler, material, geometry,
  terrain, or history caches with one central cache.
- Creating a public pass interface, service locator, mutable blackboard,
  callback scheduler, or runtime-polymorphic feature registry.
- Changing `FScene`, SceneInfo/SceneProxy publication, material or asset
  residency contracts, viewport submission, or public view-statistic meanings.
- Optimizing a pass algorithm merely because its orchestration is touched.
  Algorithmic changes require independent evidence and plans.

## Design Decisions and Invariants

### One executor owns the fixed schedule

- `FSceneRenderer` owns persistent Renderer services and exposes the existing
  scene-view entry. It does not implement feature pass orchestration through a
  second private API.
- One per-submission `FFixedSceneFrameExecutor` owns the temporal transaction,
  fixed sequence, pass results, telemetry publication, and final commit. It is
  a real stateful implementation boundary rather than an empty forwarding tag.
- Preparation and output finalization remain named functions/stages inside the
  fixed pipeline; they do not require empty public classes or extra friend
  relationships.
- The executor may be implemented across focused translation units, but file
  separation must not create additional schedulers or duplicate pass policy.
- Feature shader/pipeline/render bodies remain in their existing feature
  renderers. The executor coordinates them but does not absorb their backend
  implementation.

### Prepared, resolved, and outcome state are distinct

- `FSceneRenderPlan` remains the command-local logical plan. After its builder
  returns successfully, execution observes it as `const`.
- The plan owns the fitted view, imported environment facts, selected lights,
  receiver and shadow logical draws, stable combined translucency order, and
  optional cloud logical inputs. Borrowed render-thread scene/resource facts
  remain bounded by the existing render-command lifetime.
- Per-frame-created RHI allocations, dynamic uniform/storage ranges, PSO and
  material-binding resolution, readiness, pass outputs, and execution counters
  do not live in logical prepared values.
- `FResolvedSceneFrame` owns lighting uploads, receiver/shadow execution
  resources, resolved palette ranges, and complete typed transient-target
  bundles. It may record resolution-local diagnostics but no command-recording
  results.
- `FSceneFrameOutcome` owns typed pass results and execution observations.
  Correctness and later pass policy read typed results, never telemetry.
- The temporal view-state submission and telemetry publication are transaction
  guards beside the plan, not logical plan fields. Every failure path aborts
  pending temporal state and publishes no partial public statistics.

### Frame requirements precede target resolution

- The executor derives one immutable `FSceneFrameRequirements` from the fitted
  view, render options, and prepared optional features before acquiring
  transient targets.
- Requirements distinguish required production targets from optional debug,
  qualification, and compute/fragment fallback candidates. Disabled features
  allocate no targets.
- One `ResolveFrameTargets_RenderThread` operation acquires every requested
  bundle and returns complete typed values. Pass execution performs no target
  lookup, creation, extent selection, or mutation of feature-owned
  `CurrentTargets` state.
- Imported output, asset/default/environment resources, persistent/history
  resources, and frame-transient targets remain separate lifetime classes.
- Feature renderers retain persistent shader, pipeline, sampler, geometry,
  material, and history caches. They do not retain the current frame's
  transient attachment set.
- The transient pool preserves complete texture-description keys,
  device-generation-aware retry, bounded retention, deterministic release, and
  RHI-reference lifetime. A failed bundle acquisition must enforce its budget
  and must not publish or unintentionally retain a partially created bundle.

### Typed pass data flow stays explicit

- Every fallible pass boundary returns a feature-specific result containing
  status, output resources, route/fallback reason, completeness, and local
  execution measurements required by later reduction.
- The fixed schedule constructs deferred-lighting inputs once from GBuffer,
  lighting, shadow, GTAO, contact, cloud-shadow, and documented fallback
  results. Feature helpers do not mutate a shared deferred parameter object.
- Pass signatures take exact prepared/resolved partitions, target bundles, and
  predecessor results. A generic frame context may own these values, but pass
  APIs do not accept an untyped mutable blackboard.
- Qualification branches consume the same preparation, resolved resources,
  targets, and typed results as production. They do not run a second scene
  scheduler.
- Manual RHI transitions and render-pass load/store contracts remain the sole
  resource-state authority until a later Render Graph plan replaces them with
  equivalent generated barriers.

### Geometry identity is index-based and contiguous

- Prepared primitive, draw, batch, and combined-translucency identities use
  stable family/bucket/index handles rather than object addresses.
- Resolved draw records are contiguous arrays aligned with their prepared
  buckets or are addressed by stable prepared indices. One resolved record
  owns readiness/status and the exact binding/range needed by execution.
- Pointer-keyed `unordered_set`/`unordered_map` tables are removed from
  submission-local resolved geometry unless a benchmark proves a remaining
  non-draw lookup requires hashing.
- Shared Skeletal poses remain logical preparation. Upload-attempt state,
  storage ranges, and upload accounting move to a resolved palette table shared
  by receiver and shadow execution.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain keep feature-specific
  preparation and draw implementations. Common iteration helpers may remove
  repeated fixed-family dispatch, but no virtual geometry registry is added.

### Telemetry and qualification remain observational

- Feature preparation, resolution, and execution return or retain local
  measurements beside their authoritative outputs. One final reducer maps
  them into the stable public `FSceneViewStatistics` and counter snapshot.
- Large counter aggregates are not threaded through logical selection,
  target resolution, or pass policy. No success/fallback branch reads a
  telemetry field.
- Production submission options contain production behavior and supported
  user/editor diagnostics. Development qualification overrides move to one
  explicit Renderer-private policy supplied by tests or qualification setup.
- Global qualification sinks do not mutate a prepared frame. Timing and capture
  observers receive immutable named pass results/resources.
- Public statistic meanings and successful-frame publication timing remain
  unchanged unless a separately reviewed contract change is approved.

### Migration is incremental and behavior-preserving

- Each implementation stage leaves one compiling scheduler and removes the
  superseded path in the same stage. No compatibility forwarding layer remains
  after its consumers migrate.
- Pass ordering, resource access, fallbacks, output selection, and failure
  results are frozen before structural edits and compared after every stage.
- Structural improvements are validated independently from storage/performance
  improvements so regressions can be localized.
- Plan status, checklist evidence, and any changed decision are updated in the
  same commit as the corresponding implementation stage.

## Stage 0 Frozen Baseline and Boundary Record

### Fixed schedule and resource-state authority

| Order | Route/stage | Frozen behavior |
| --- | --- | --- |
| 1 | Submission validation | Increment the bounded submission serial; validate output extent; resolve required post-process, environment/sky, and Scene Color/depth resources. |
| 2 | Temporal begin | Fit the view to output; reject an interleaved view-state submission; begin registered state or record missing/duplicate discontinuity under an abort-on-exit guard. |
| 3 | Logical/resource preparation | Select environment, visibility, lights, receiver/shadow draws, shared Skeletal poses, combined translucency, and cloud inputs; current baseline also resolves geometry/uploads and records directional shadow. |
| 4 | GBuffer | When production, debug, or qualification requires it, render StaticMesh/SplineMesh, SkeletalMesh, and Terrain into four GBuffer attachments plus Scene depth and publish typed completeness/rendered-geometry state. |
| 5 | Deferred inputs | Run requested GTAO raw/filter/resolve, contact visibility, and volumetric-cloud shadow visibility; apply documented white/default fallbacks for unavailable optional producers. |
| 6 | Isolated diagnostics | Optionally render isolated deferred/GBuffer/GTAO debug outputs over the same GBuffer and prepared geometry. |
| 7a | Special forward | For non-solid or non-lit views, open one Scene Color pass, render optional environment, opaque/masked families, then globally sorted translucency, and finalize family execution. |
| 7b | Hybrid production | Bootstrap Scene Color/environment; render deferred lighting; render retained unlit opaque/masked geometry; transition depth to graphics sampling; render/reconstruct/composite clouds; restore Scene Color attachment access; render globally sorted translucency. |
| 8 | Output transaction | Select Scene Color or a complete debug output; prepare editor assistance; render post process; optionally render assistance with Scene depth; finish output in `Present` or offscreen `ShaderReadOnly`. |
| 9 | Commit | Reduce geometry observations; on complete output success commit view state then telemetry/statistics. Every earlier return leaves the view-state guard to abort and publishes no statistics. |

Manual `BeginRenderPass`/`EndRenderPass`, explicit texture transitions, and
`RenderTargetLayouts` remain the frozen resource-state authority. Stage 1 only
moves ownership of these operations. Stages 2 through 4 may narrow their inputs
but may not reorder or synthesize them.

### Failure, fallback, and transaction matrix

| Condition | Frozen result/policy |
| --- | --- |
| Null or zero-extent output | Return `InvalidOutput`; no temporal begin and no telemetry publication. |
| Required post-process, Scene Color/depth, lighting upload, or production deferred resource unavailable | Return `RendererResourcesUnavailable`; abort active view state and publish no statistics. |
| Required environment override invalid, or selected required environment cannot render | Return `RequiredEnvironmentUnavailable`; abort active view state and publish no statistics. |
| Missing/foreign view state | Render without persistent history, record `MissingState`, and commit no registry state. |
| Duplicate/interleaved view-state submission | Render without that state, record `DuplicateSubmission`, and leave the active owner unchanged. |
| Optional compute route unavailable | Select the documented fragment route when complete; otherwise publish the feature fallback. |
| Optional GTAO/contact/cloud-shadow/cloud input unavailable | Preserve production through white/factor-one/disabled feature fallback; incomplete optional debug output is not selected. |
| Directional shadow unavailable | Bind the documented complete array/sampler fallback and continue. |
| Successful final output | Commit temporal state first, then publish the immutable counter snapshot and optional public statistics exactly once. |
| Any failure before final output | RAII aborts pending temporal candidates; `FSceneTelemetryPublication` remains uncommitted. |

### Frame-transient target inventory

| Semantic group | Complete target set | Route/extent | Retained ceiling | Baseline acquisition owner |
| --- | --- | --- | --- | --- |
| Scene | `RGBA16_FLOAT` Scene Color + `D32` Scene depth | Output extent; required | 96 MiB | `FPostProcessRenderer::EnsureSceneTargets_RenderThread` |
| GBuffer | `RGBA8_UNORM` material/normals/surface + `R11G11B10_FLOAT` emissive | Output extent; conditional required/debug | 128 MiB | `FGBufferRenderer::EnsureTargets_RenderThread` |
| GTAO | Two `R8_UNORM` native targets; half-resolution adds selector and full-resolution resolve | Quality-selected extent; optional producer | 32 MiB | `FGroundTruthAmbientOcclusionRenderer::EnsureTargets_RenderThread` |
| Contact fragment | One render-targetable `R8_UNORM` visibility | Output extent; fragment candidate | 16 MiB | Contact renderer feature state |
| Contact compute | One storage/sample `R8_UNORM` visibility plus views | Output extent; compute candidate | 16 MiB | Contact renderer feature state |
| Cloud-shadow fragment | One render-targetable/readback `R8_UNORM` visibility | Output extent; fragment candidate | 16 MiB | Cloud-shadow renderer feature state |
| Cloud-shadow compute | One storage/sample/readback `R8_UNORM` visibility plus views | Output extent; compute candidate | 16 MiB | Cloud-shadow renderer feature state |
| Isolated deferred | One `RGBA16_FLOAT` color | Output extent; qualification/debug only | 64 MiB | Deferred renderer feature state |
| GBuffer debug | One `RGBA16_FLOAT` color | Output extent; debug only | Current extent at 16 bytes/pixel | GBuffer-debug feature state |
| Cloud fragment | One render-targetable/readback `RGBA16_FLOAT` cloud | Quality-selected spatial extent | 64 MiB family share | Cloud renderer feature state |
| Cloud compute | One storage/sample/readback `RGBA16_FLOAT` cloud plus views | Quality-selected spatial extent | 64 MiB family share | Cloud renderer feature state |
| Cloud composite | One render-targetable/readback `RGBA16_FLOAT` color | Output extent | 64 MiB family share | Cloud renderer feature state |

The cloud family shares a 192 MiB aggregate ceiling. Directional-shadow depth
and committed cloud history remain persistent feature/view-state resources,
not frame-transient pool entries. The baseline contains 11 executor-side target
ensure calls and mutable current-target state in the feature owners.

### Selected frame ownership

| Value | Selected owner and contents | Mutation rule |
| --- | --- | --- |
| `FSceneRenderPlan` | Logical fitted view, imported environment, selected lights, receiver/shadow logical draws, shared poses, combined translucency order, optional cloud inputs | Builder-only; `const` after successful return. |
| `FResolvedSceneFrame` | Lighting upload, resolved receiver/shadow geometry and bindings, resolved palettes, and typed frame target bundles | Resource-resolution-only; execution consumes it as `const`. |
| `FSceneFrameRequirements` | Production/debug/qualification pass demand and compute/fragment target candidates derived from plan and immutable options | Complete immutable value before target acquisition. |
| `FSceneFrameOutcome` | Typed pass status, outputs, routes/fallbacks, execution measurements, and selected final output | Written by the fixed executor in schedule order; later policy reads typed predecessors only. |
| `FSceneFrameTransaction` | View-state Begin/Commit/Abort guard, telemetry publication guard, and submission serial | Beside the plan; commit only after final output success. |

`FFixedSceneFrameExecutor` is selected as the one per-submission stateful
scheduler. `FSceneRenderer` remains the persistent composition/lifecycle owner.
No general service locator, pass registry, graph node, or mutable blackboard is
selected.

### Render option ownership

| Current option | Frozen classification | Selected destination |
| --- | --- | --- |
| `Environment` | Production per-submission content override | Remains in production submission options. |
| `GBufferDebugMode` | Supported renderer/editor diagnostic output | Move to explicit debug options while retaining the public/editor capability. |
| `DeferredDirectionalDebugMode` | Supported renderer diagnostic plus isolated qualification route | Keep the diagnostic selector in debug options; move isolated-route enablement to private qualification policy. |
| `GroundTruthAmbientOcclusionDebugMode` | Supported renderer diagnostic plus qualification input | Keep diagnostic selection in debug options; qualification-only production bypass moves private. |
| `bEnableGBufferQualification` | Development-only route enablement | Renderer-private qualification policy. |
| `bEnableDeferredDirectionalQualification` | Development-only route enablement | Renderer-private qualification policy. |
| `bEnableGroundTruthAmbientOcclusionQualification` | Development-only route enablement | Renderer-private qualification policy. |
| `bForceFragmentContactVisibility` | Development-only route forcing | Renderer-private qualification policy; production route preference remains in view settings. |
| Cloud force-fragment preparation sink | Development-only mutable global seam | Remove; express as a bounded field of the same private qualification policy. |

### Structural and performance baseline

- `FixedSceneFrameExecutor.cpp` is 1,514 lines but implements its work as 11
  private `FSceneRenderer` frame/pass methods; `SceneFramePreparation.cpp` is
  491 lines and includes logical preparation, resolution, uploads, and shadow
  execution.
- Preparation plus execution directly touches `Telemetry.Counters` 127 times.
  Resolved StaticMesh/SkeletalMesh/Terrain data contains eight pointer-keyed
  draw/primitive/batch hash containers.
- The current fixed executor performs 11 target-ensure calls after entry; the
  transient provider is reached from nine feature implementation files.
- Baseline `RendererSceneContractTests`: 28/28 passed in 10 ms, covering view
  transaction/discontinuity behavior, immutable snapshot publication, failed
  telemetry commit, sequential-view isolation, visibility, and lighting.
- Baseline GBuffer qualification on RTX 3090, Win64 Debug, validation enabled:
  1,920x1,080, 30 warm-up and 120 measured frames; GBuffer median/p95
  79,616/80,512 ns, isolated deferred 251,408/252,512 ns, combined
  331,008/332,928 ns, active route 107,827,200 bytes. Hybrid production median/
  p95 was 402,720/414,112 ns with 69,984,000 active bytes and a 268,435,456-byte
  retained ceiling.
- Baseline cloud qualification uses the existing 30 warm-up/120 measured frame
  matrix. One diagnostic sample reported 1,920x1,080 compute median/p95
  760,416/1,213,184 ns, fragment 799,840/1,322,976 ns, and 125,265,624 retained
  target bytes; its 4K relative High/Reference timing sample was noisy and
  failed, while the immediate unchanged exact rerun passed. These timings are
  comparison evidence only and do not authorize threshold changes.
- Forward, editor-assistance, and sequential/multi-view behavior is covered by
  existing Renderer scene, editor rendering/grid, viewport, and Vulkan tests.
  No stable per-frame CPU allocation counter exists for those paths today, so
  the frozen allocation baseline is the explicit ensure/hash/current-target
  counts above; Stage 5 must demonstrate their removal without inventing a new
  timing threshold.
- New structural coverage is assigned to the stage that can make it pass:
  Stage 1 replaces empty-type assertions with one-executor ownership checks;
  Stage 2 asserts logical types exclude resolved/execution state; Stage 3
  verifies complete target resolution and injected failure; Stage 4 verifies
  typed-result conservation. Existing view-state and telemetry tests remain the
  failure-transaction baseline throughout.

## Current Foundations and Gaps

| Area | Existing foundation | Gap this plan closes |
| --- | --- | --- |
| Frame entry | `RenderView_RenderThread` constructs the per-submission `FFixedSceneFrameExecutor` | Stage 1 closed the duplicate private scheduler; later stages narrow frame data ownership. |
| Stage types | One real fixed executor owns the schedule; preparation remains a focused executor implementation file | Stage 1 removed empty preparation/finalization facades; logical preparation still performs resolution/execution work. |
| Logical geometry | Prepared and resolved family values are separate | Prepared palette state contains RHI upload state; resolved values mix resources, execution counters, and pointer identity. |
| Directional shadow | Logical receiver/caster preparation and resolved cascade values exist | Preparation also resolves and records the shadow pass before returning. |
| Pass results | GBuffer publishes typed completeness and rendered-geometry state; several feature renderers have local route results | Orchestration helpers often return `void` or raw textures and mutate shared deferred parameters, plan fields, and counters. |
| Transient resources | One shared pool owns physical textures and generation retry | Feature states retain `CurrentTargets` and execution-time `EnsureTargets` calls distribute frame ownership. |
| Transactions | View state and telemetry commit only after successful finalization | The guards and mutable telemetry are coupled to the all-purpose plan/executor implementation. |
| Qualification | Existing Vulkan/image, timing, capture, and route qualification is extensive | Production options and global mutable qualification seams are inconsistent and enlarge the main scheduler. |
| RDG readiness | Fixed order, explicit transitions, typed feature structures, and pooled targets already exist | Resource requirements, complete target bundles, and predecessor results are not yet explicit at every pass boundary. |

## Implementation Stages

### Stage 0: Freeze the current schedule and selected boundaries

- [x] Record the authoritative production, special-forward, debug, and
  qualification pass order, including every render-pass boundary and explicit
  texture transition affected by this plan.
- [x] Inventory every early return, required/optional resource failure,
  fallback, output access state, view-state Begin/Commit/Abort transition, and
  statistics publication point.
- [x] Inventory all frame-transient target descriptions, semantic groups,
  retained-byte budgets, compute/fragment candidates, and current acquisition
  sites.
- [x] Freeze the selected frame types and ownership map for
  `FSceneRenderPlan`, `FResolvedSceneFrame`, `FSceneFrameRequirements`, typed
  target bundles, `FSceneFrameOutcome`, and transaction guards.
- [x] Classify every `FSceneViewRenderOptions` field as production, supported
  debug/editor behavior, or qualification-only override; record the selected
  destination for each field.
- [x] Capture focused CPU/allocation and retained-target baselines for a
  representative forward, hybrid deferred, cloud, editor-overlay, and
  multi-view workload using the repository's established profiling path.
- [x] Add or identify structural tests that fail if preparation records a pass,
  pass execution acquires a target, correctness reads telemetry, or failed
  views commit temporal/statistics state.

#### Acceptance Gate

- The existing schedule, failure/fallback matrix, ownership map, target
  inventory, option classification, and performance baseline are explicit and
  reviewable; all unresolved ownership decisions are closed before code moves.

### Stage 1: Make the fixed executor the real scheduler

- [x] Move fixed-frame scheduling and pass-orchestration methods from
  `FSceneRenderer` to a per-submission `FFixedSceneFrameExecutor` implementation
  while preserving feature renderer ownership in `FSceneRenderer`.
- [x] Reduce `FSceneRenderer::RenderView_RenderThread` to construction and one
  executor call; remove the superseded private frame-orchestration method
  declarations from `SceneRenderer.h`.
- [x] Replace the empty preparation and finalization classes with named
  executor stages or free/private functions whose dependencies are explicit.
- [x] Reduce the Renderer friend surface to the one selected executor boundary;
  do not expose concrete feature renderers publicly.
- [x] Remove compile-time tests that assert the forwarding types are empty and
  replace them with tests for scheduler uniqueness, transaction ownership, and
  non-copyable per-submission execution state.
- [x] Split the large executor implementation into schedule, geometry/deferred,
  optional-feature, and output translation units only where it improves local
  readability; retain one class and one ordering authority.

#### Acceptance Gate

- `FSceneRenderer` owns composition/lifecycle but no feature pass schedule;
  `FFixedSceneFrameExecutor` is the only production scheduler; no empty stage
  facade or forwarding compatibility path remains; focused and aggregate
  rendering behavior matches the Stage 0 baseline.

### Stage 2: Publish immutable logical preparation

- [x] Make frame preparation return one complete `FSceneRenderPlan` candidate
  or a typed preparation failure instead of mutating an executor-owned
  all-purpose value.
- [x] Move lighting uniform allocation, material/pipeline/geometry resolution,
  Skeletal palette upload state, directional-shadow resource preparation, and
  directional-shadow command recording out of logical preparation.
- [x] Split logical Skeletal pose sharing from resolved palette ranges and
  upload measurements while preserving one upload per submission identity.
- [x] Keep environment and cloud imported inputs as complete optional logical
  values with their existing required/optional failure contracts.
- [x] Publish stable family/bucket/index handles for prepared draws and combined
  translucency; remove any preparation dependency on later object addresses.
- [x] Pass the successfully prepared plan as `const` to every later resolution
  and execution boundary.
- [x] Add compile-time and focused behavior tests excluding readiness,
  execution phase, transient target, mutable upload, and execution-counter
  fields from logical prepared values.

#### Acceptance Gate

- Successful preparation performs logical selection only, returns a complete
  plan, records no render/dispatch pass, and publishes a value that remains
  `const` through resolution and execution; image and failure behavior remain
  unchanged.

### Stage 3: Centralize resolved resources and transient targets

- [x] Introduce `FResolvedSceneFrame` and resolve receiver, shadow, lighting,
  palette, material, pipeline, geometry, and terrain resources without
  mutating logical draws.
- [x] Derive `FSceneFrameRequirements` once from immutable plan/options and
  encode production-required, optional-debug, and route-fallback targets.
- [x] Resolve Scene Color/depth and every requested feature target into complete
  typed frame-owned bundles before the first consuming pass.
- [x] Change feature execution APIs to accept resolved targets by `const`
  reference/value and remove all execution-time `EnsureTargets_RenderThread`
  calls.
- [x] Remove feature-owned `CurrentTargets` and `CurrentComputeTargets` state;
  keep only persistent resource/cache state and explicitly committed history.
- [x] Replace string semantic groups in the transient pool with a bounded typed
  identity and a lookup structure whose allocation/search cost is measured
  against the Stage 0 baseline.
- [x] Make bundle acquisition internally transactional: failed multi-texture
  acquisition publishes no partial bundle, applies retention limits, and
  preserves generation retry diagnostics.
- [x] Verify active/retained byte accounting, extent changes, optional-target
  absence, multi-view isolation, feature release, manual/device invalidation,
  and shutdown.

#### Acceptance Gate

- One frame-resolution boundary owns every requested transient target and
  resolved execution value; feature passes perform no target lookup/creation
  and retain no current-frame target state; recovery, budgets, multi-view
  behavior, and rendered output match baseline.

### Stage 4: Express the fixed sequence through typed pass results

- [x] Define exact input and result values for GBuffer, GTAO, contact visibility,
  cloud-shadow visibility, isolated deferred/debug, Scene Color, cloud spatial
  and temporal/composite, post process, editor assistance, and final output.
- [x] Replace pass helpers that return `void` or a bare texture while mutating
  `FSceneRenderPlan`, shared deferred parameters, or counters with typed result
  returns.
- [x] Build production deferred-lighting inputs once from predecessor results
  and documented fallbacks; ensure missing optional results cannot form a
  partially valid binding set.
- [x] Make pass success, route selection, completeness, and output selection
  depend only on typed policy/input/result values.
- [x] Preserve explicit manual transitions and render-pass contracts while
  making each pass signature expose its exact predecessor resources.
- [x] Route timing and capture observers immutable pass results/resources after
  the pass has established a complete outcome.
- [x] Keep qualification branches explicit over the same typed values; remove
  duplicated setup and result reconstruction.

#### Acceptance Gate

- The top-level executor reads as one fixed typed data-flow sequence; every
  fallible pass returns an explicit outcome; shared mutable parameter/counter
  propagation no longer carries correctness; transition and image
  qualification match baseline.

### Stage 5: Simplify geometry resolution and observation hot paths

- [x] Replace pointer-keyed resolved draw readiness/material tables with
  contiguous records aligned to stable prepared indices for StaticMesh,
  SplineMesh, SkeletalMesh, and Terrain.
- [x] Consolidate readiness, material binding, palette/terrain ranges, and
  feature-local resolution status into one resolved record per logical draw or
  batch.
- [x] Remove duplicated opaque/masked/translucent lookup and finalize paths
  where a small compile-time family helper can preserve explicit dispatch
  without virtual registration.
- [x] Move preparation, resolution, and execution measurements out of logical
  and resolved correctness values into feature-local observation results.
- [x] Reduce feature observations once into the existing private counters and
  public `FSceneViewStatistics`; retain conservation checks beside the reducer.
- [x] Measure frame CPU time, dynamic allocations, lookup counts, command count,
  target bytes, and executable/code-size changes against Stage 0; investigate
  any material regression before proceeding.

#### Acceptance Gate

- Submission-local resolved geometry performs no pointer-keyed draw hashing;
  logical/resolved correctness values contain no execution counters; public
  statistics and draw conservation remain unchanged; representative frame
  preparation/execution cost does not regress materially and expected
  allocation reductions are demonstrated.

### Stage 6: Isolate qualification and debug policy

- [x] Split production submission behavior, supported editor/debug behavior,
  and qualification-only overrides according to the Stage 0 classification.
- [x] Move qualification-only route forcing and isolated-pass enablement behind
  one explicit Renderer-private qualification policy used by tests/tools.
- [x] Remove the mutable volumetric-cloud preparation sink and any other global
  callback that can alter prepared or resolved frame state.
- [x] Preserve immutable timing/capture observation seams and route them through
  typed pass results.
- [x] Update qualification fixtures to inject only feature-bounded policy and
  consume immutable outputs without constructing an alternate frame model.
- [x] Verify production callers cannot request development-only execution paths
  accidentally and supported editor debug output remains available.

#### Acceptance Gate

- Production options contain no qualification-only switches; qualification is
  explicit, private, and feature-bounded; no global observer mutates frame
  preparation; production, debug, and qualification coverage all pass through
  the same fixed scheduler.

### Stage 7: Integrate, qualify, document, and hand off

- [x] Run the focused Renderer scene/frame, view-state, geometry-family,
  shadow, GBuffer/deferred/GTAO, cloud, post-process, target-pool, resource-
  recovery, and transition tests selected through the repository testing
  workflow.
- [x] Run aggregate Renderer, RenderCore, RHI, Vulkan, viewport, and editor
  suites in the supported inline and threaded execution modes required by the
  affected contracts.
- [x] Re-run existing image/parity qualification for directional shadow,
  GBuffer/deferred lighting, GTAO, volumetric cloud, HDR/output mapping, editor
  grid/assistance, terrain, and representative scene import.
- [x] Exercise invalid output, missing required environment, optional target
  failure, shader/device invalidation, retry, resize, multi-view submission,
  duplicate/missing view state, failure abort, shutdown, and restart paths.
- [x] Compare Stage 5 CPU/allocation/retention measurements with the Stage 0
  baseline and record the final evidence without weakening existing thresholds.
- [x] Run the full supported build and test matrix and an Editor smoke covering
  startup, viewport rendering, debug modes, resize/movement, device/resource
  recovery, and shutdown according to the repository build/run workflow.
- [x] Update the lasting Renderer frame-preparation and resource-recovery
  contracts to describe the implemented owners, immutable/resolved boundary,
  typed pass flow, and qualification policy without retaining plan status.
- [x] Validate changed documentation and the complete active-plan lifecycle,
  record completion evidence, and move this plan to `Status: Completed` only
  after every required acceptance gate passes.

#### Acceptance Gate

- Focused, aggregate, Vulkan/image, failure/recovery, multi-view, performance,
  full-build/test, Editor smoke, and documentation validation pass; the lasting
  contracts match the code; no superseded scheduler, target cache, mutable
  qualification seam, or compatibility path remains.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Structural ownership | One real fixed executor; narrow `FSceneRenderer`; immutable logical plan; distinct resolved/outcome values; no empty forwarding stages. |
| Schedule and transitions | Frozen pass order, render-pass boundaries, load/store behavior, manual transitions, and final output access match baseline. |
| Logical preparation | Visibility, LOD, material/pipeline identity, shadow membership, cloud/environment selection, and combined translucency remain deterministic and preparation records no pass. |
| Resource resolution | Required/optional failures, material/pipeline/geometry/palette resolution, complete target bundles, retained budgets, generation retry, and shutdown are covered. |
| Geometry execution | StaticMesh, SplineMesh, SkeletalMesh, and Terrain opaque/masked/translucent, GBuffer, retained-forward, and shadow execution conserve prepared/resolved/results. |
| Typed pass flow | GBuffer, GTAO, contact/cloud shadow, deferred/debug, cloud, post process, editor assistance, and output expose complete typed results and fallbacks. |
| Temporal and multi-view | Begin/Commit/Abort, missing/duplicate state, history acceptance/rejection, failed-frame rollback, resize, and interleaved/multi-view isolation match baseline. |
| Observation | Timing/capture observers are immutable; failed views publish no statistics; successful public statistics retain their meanings and conservation checks. |
| Qualification policy | Production/debug/qualification ownership is explicit; tests inject bounded policy; no global callback mutates a frame. |
| Visual output | Existing Vulkan/image qualifications cover the affected shadow, deferred, GTAO, cloud, terrain, editor, HDR, and import routes without unexplained drift. |
| Performance | Representative CPU time, allocations, pointer/hash lookup removal, command counts, active/retained bytes, and final binary/code size are compared with the frozen baseline. |
| Integration | Supported full build/test matrix and Editor startup/viewport/resize/recovery/shutdown smoke pass. |
| Documentation | Changed-document and all-active-plan validation pass; lasting Runtime contracts describe only implemented behavior. |

## Definition of Done

- `FFixedSceneFrameExecutor` is the sole production scene-frame scheduler and
  contains real per-submission orchestration state.
- `FSceneRenderer` owns composition, lifecycle, invalidation, view-state
  registry, and the narrow public frame entry without a duplicate private pass
  API.
- Empty preparation/finalization forwarding classes and their structural tests
  are removed.
- `FSceneRenderPlan` contains logical preparation only and is `const` after
  successful publication.
- Dynamic uploads, resolved bindings, readiness, transient targets, pass
  outputs, and execution observations live in their selected resolved/outcome
  owners.
- Directional-shadow rendering occurs in the fixed execution sequence, not in
  logical preparation.
- All requested frame-transient targets are acquired once through one typed
  resolution boundary; feature renderers retain no `CurrentTargets` state and
  pass execution performs no target acquisition.
- Every fallible pass exposes a typed result, and no correctness/fallback branch
  reads telemetry or depends on shared mutable pass parameters.
- Resolved geometry uses stable index-based contiguous records rather than
  pointer-keyed draw hash tables.
- Qualification-only behavior is private and explicit; timing/capture seams are
  immutable observers.
- Rendering algorithms, fixed ordering, manual transition semantics, fallbacks,
  temporal transactions, output behavior, and public statistics remain
  compatible with the frozen baseline.
- Required focused, aggregate, Vulkan/image, recovery, multi-view, performance,
  full-build/test, Editor-smoke, and documentation validation pass.
- Lasting Renderer contracts are updated, the plan contains evidence-backed
  completion state, and no superseded compatibility path remains.

## Deferred Follow-ups

- A bounded Render Graph that consumes the resulting immutable plan, resolved
  target requirements, and typed pass inputs/results.
- Physical transient-memory aliasing and lifetime-interval allocation.
- Automatic barrier synthesis, pass culling/merging, graph visualization, and
  resource lifetime diagnostics.
- Asynchronous compute, multiple queues, and parallel command recording.
- Task-parallel visibility/preparation and persistent GPU-driven draw-command
  generation.
- Broader PSO-cache architecture, bindless resources, meshlets, clustered/local
  lighting, and local-light shadows.
- Feature algorithm or quality changes that require independent image and
  performance evidence.

## Related Documentation

- [Renderer Frame Preparation and Fixed Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Persistent View State](../../../Runtime/Rendering/PersistentViewState.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Volumetric Cloud Spatial Rendering](../../../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Renderer Frame Preparation Refactor Plan](RendererFramePreparationRefactor.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/FixedSceneFrameExecutor.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPlan.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderResults.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderTelemetry.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GroundTruthAmbientOcclusionRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudShadowRenderer.{h,cpp}`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.{h,cpp}`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
