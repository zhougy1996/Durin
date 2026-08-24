# Render Graph Architecture Roadmap

Summary: Establish a declarative Render Dependency Graph as the renderer's single frame-scheduling and resource-dependency authority, then add transient-memory and queue optimizations only from measured evidence.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Milestones 1 through 4A are complete. One immutable `FSceneRenderPlan`
separates preparation from execution. `FRenderGraphSceneFrameExecutor` owns the
sole graph's lifecycle boundary, `FSceneFrameGraphComposer` wires typed feature
contributors, and `RenderCore` compiles exact resource
versions, minimal value/hazard dependencies, retained lifetimes, culling,
preparation requests, transitions, and pointer-free diagnostics. RHI/Vulkan
remain the only physical execution-state authority.

Every directional-shadow-through-output inter-pass resource now has one graph
identity and pass-scoped declaration. Compute, fragment, disabled, and
factor-one topology is selected before compile; retained logical requests drive
one atomic target publication before recording; no feature callback compiles a
child graph or owns a migrated outer transition. Representative cloud frames
schedule 11 passes with 22--25 dependencies and 1 or 17 texture transitions
under frozen ceilings of 12, 28, and 20 respectively. Debug graph compilation
and complete callback recording remain below their 5 ms and 250 ms observation
ceilings.

The completed boundary passed the full `Win64-Debug-DurinEditor` build, all 79
routine native-test targets, focused RenderCore/RHI/Renderer contracts, the
renderer Vulkan integration set, and Directional Shadow, GBuffer, HDR display,
and Volumetric Cloud qualifications. Adapter-specific timing fixtures report
the selected Vulkan device and apply frozen RTX 3090 thresholds only on the
matching named lane; the available GTX 1060 run remains truthful observation
evidence while preserving functional, memory, telemetry, and relative-route
gates.

Milestones 5 through 7 are explicitly dispositioned rather than activated.
Current captures do not establish material physical-aliasing savings, the
qualified Vulkan device exposes one shared graphics/compute/transfer family
rather than an independent overlap opportunity, and graph compile/record cost
does not cross its frozen ceilings. Aliasing, async compute, and advanced graph
compilation therefore require new measured entry evidence and independent
plans; none is implied by this roadmap's completion.

## Outcome

Provide one renderer-owned frame graph in which:

1. Renderer code declares passes, typed resources, access intent, and required
   external effects; it does not hand-author inter-pass restoration barriers.
2. A backend-neutral graph compiler validates dependencies and lifetimes,
   determines deterministic execution order, culls only provably unused work,
   and emits transitions through existing RHI commands.
3. Imported, persistent/history, and frame-transient resources retain distinct
   ownership and failure policies while participating in one dependency model.
4. Renderer features keep their preparation, fallback, shader, pipeline, and
   draw/dispatch policy; RDG owns orchestration rather than feature semantics.
5. Logical transient lifetimes become observable before any physical memory
   aliasing, async compute, or multi-queue scheduling is attempted.
6. The fixed manual scheduler and migrated pass-local transition code are
   retired after the graph path reaches production parity; the repository does
   not retain two authoritative frame architectures.

## Scope

- Add graph-local texture and buffer identities with generation-safe handles
  that cannot escape their graph lifetime.
- Register imported resources with explicit initial and required final access;
  preserve caller ownership of viewport, offscreen, asset, default, and shared
  resources.
- Represent persistent resources and view-state history without transferring
  their lifetime to the graph or transient allocator.
- Describe transient resources independently from their physical RHI backing
  and integrate allocation with the existing complete-or-null recovery model.
- Declare graphics, compute, copy, and explicit external-effect passes with
  precise read, write, read/write, attachment, load/store, and access intent.
- Compile resource uses into deterministic dependencies, logical lifetimes,
  validation diagnostics, and RHI transition batches.
- Preserve the existing single-queue rendering-thread command model while the
  required migration is underway.
- Migrate the fixed Renderer schedule in bounded vertical slices, including
  directional shadow, GBuffer, screen-space visibility and lighting, Scene
  Color, volumetric cloud, translucency, post process, editor assistance, and
  final output.
- Add graph inspection, pass/resource naming, dependency and lifetime dumps,
  barrier observation, timing correlation, and deterministic test fixtures.
- Measure graph compile cost, retained and peak transient bytes, physical
  allocation reuse, barrier count, culled work, GPU timing, and PSO demand so
  conditional follow-ups are selected from repository evidence.

## Non-Goals

- Changing scene preparation, visibility, material, lighting, cloud, temporal,
  or editor-assistance algorithms merely to adopt RDG.
- A public plugin API, runtime-polymorphic feature registry, generic mutable
  blackboard, or cross-module pass injection mechanism.
- Moving renderer feature policy, fallback decisions, shader compilation, PSO
  creation, or resource-recovery generations into the graph compiler.
- Replacing RHI access vocabulary, Vulkan resource-state tracking, render-pass
  lowering, command recording, or submission ownership.
- Physical transient memory aliasing in the foundation or first production
  pilot.
- Async compute, multiple queues, queue-family ownership transfer, timeline
  scheduling, or CPU-parallel command recording in the required migration.
- Automatic render-pass merging, pass reordering for performance, bindless
  resources, GPU-driven visibility, or centralized PSO caching as prerequisites.
- Keeping the fixed scheduler as a permanent fallback once production graph
  migration and qualification are complete.

## Program Decisions and Invariants

- **RenderCore owns graph mechanics; Renderer owns graph construction.** The
  backend-neutral builder, resource model, validation, and compiler belong
  above RHI and below feature policy. Renderer translates its prepared frame
  into passes. RHI and Vulkan consume compiled commands and never inspect RDG
  feature semantics. A separate runtime module requires later dependency or
  build-cost evidence; it is not created solely for naming symmetry.
- **One graph is one bounded render-thread transaction.** Graph-local handles,
  pass data, compiled dependencies, and transient leases are invalid after
  execution ends. Recorded RHI commands retain the concrete references they
  need through replay.
- **Compile before execute.** Invalid handles, missing producers, illegal
  access combinations, dependency cycles, overlapping writes, incompatible
  descriptions, or unsatisfied external states fail before the first graph
  pass records commands. Graph compilation never partially advances RHI state.
- **RHI remains the state authority.** RDG tracks declared logical access to
  synthesize `FRHIBufferTransition` and `FRHITextureTransition` commands. RHI
  validates expected-before access and Vulkan owns native stages, masks,
  layouts, and tracked state. No parallel backend-layout cache is introduced.
- **External state is explicit.** Every imported resource declares an initial
  access contract and, when it crosses the frame boundary, a required final
  access. Window output ends in `Present`; offscreen output retains its
  documented shader-read contract. Unknown external state is an error, not a
  guessed transition.
- **Ownership and graph participation are separate.** Imported and persistent
  resources stay owned by their caller, asset, view state, or feature owner.
  The graph may retain an RHI reference and schedule uses, but it cannot evict,
  replace, or extend the logical owner lifetime.
- **History is never transient.** Temporal Begin/Commit/Abort and view identity
  remain Renderer/view-state transactions. A graph may read a previous history
  and produce a candidate next history, but only successful frame finalization
  may publish it.
- **Logical transience precedes physical aliasing.** Foundation work computes
  first/last uses and obtains complete physical resources through the existing
  transient provider. Placed-resource reuse or overlapping physical storage is
  a separate milestone with Vulkan placement, GPU-completion, budget, and
  failure evidence.
- **Pass declarations are narrow and typed.** A pass callback receives only
  its declared resources and immutable typed feature data. It does not receive
  the whole frame plan or use a mutable global blackboard to discover hidden
  dependencies.
- **Side effects are explicit.** Present, readback, timestamp/capture, external
  publication, and other non-resource-visible effects must be declared. Pass
  culling never infers that such work is disposable from an unused texture
  result.
- **Fallback is selected before execution.** Optional compute/fragment routes,
  missing optional features, and complete fallbacks are resolved during graph
  construction from prepared data and resource readiness. Compiler culling or
  allocation failure does not silently select a rendering policy.
- **Determinism precedes scheduling freedom.** Equal graph inputs produce the
  same pass order, resource identities, diagnostics, and transition plan. The
  initial compiler preserves declared stable order between otherwise
  independent passes; performance reordering requires an explicit later plan.
- **Migration has one authority per resource edge.** A migrated dependency is
  graph-declared and graph-transitioned. Manual barriers may remain around
  unmigrated boundaries during a bounded pilot, but the same edge is never
  controlled by both paths.
- **No permanent dual renderer.** Qualification may compare fixed and graph
  recording in isolated tests or captures. Production chooses one path per
  migrated slice, and the fixed orchestration is deleted after complete parity.
- **Observability cannot control correctness.** Graph dumps, counters, timings,
  captures, and barrier traces observe immutable compilation/execution output;
  rendering policy and pass success never read telemetry.
- **PSO ownership stays evidence-gated.** RDG records centralized request and
  creation evidence required by the open PSO investigation, but a graph does
  not inherently require a shared PSO cache.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this roadmap |
| --- | --- | --- |
| Frame preparation | Immutable `FSceneRenderPlan`, typed feature partitions, resolved geometry, and typed pass outcomes | Translate prepared values into graph passes without exposing the whole plan to callbacks |
| Scheduling | One lifecycle executor, one stable-order scene composer, and feature-owned typed graph contributions | Continue replacing closed feature-internal scheduling with typed graph authoring patterns |
| Access transitions | Backend-neutral `ERHIAccess`, exact buffer ranges and texture subresources, recorded transition commands, and authoritative Vulkan validation | Derive transition batches from declared uses and diagnose graph/resource context before RHI replay |
| Render passes | Existing attachment load/store and initial/final access contracts | Represent attachment semantics in pass declarations without weakening legacy render-pass validation |
| Transient textures | Description-keyed, budgeted, complete-or-null `FRendererTransientTargetPool` leases with recovery and invalidation | Separate logical graph resources from physical leases, compute lifetimes, and move acquisition to graph execution preparation |
| Imported/persistent resources | Counted RHI references, renderer resource slots, generation invalidation, and view-state history | Register ownership-neutral graph views with exact frame boundary access and prohibit accidental transient treatment |
| Compute integration | Synchronous graphics/compute access intents and explicit handoff paths | Express graphics/compute dependencies once while retaining one queue and the current fallback policy |
| Failure handling | Complete-or-null publication, last-known-good persistent resources, retry generations, and frame commit/abort | Fail compilation/allocation before execution and reconcile temporary resources on every early exit |
| Observation | Feature telemetry, GPU pass timings, immutable capture observers, and Vulkan validation | Add deterministic graph, dependency, lifetime, culling, allocation, and barrier diagnostics |
| Testing | Renderer/RHI/Vulkan unit, integration, image, multi-view, resize, recovery, and runtime fixtures | Add graph structural tests and fixed-versus-graph command/output equivalence gates during migration |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate | State |
| --- | --- | --- | --- | --- | --- |
| 1. Render Graph foundation and barrier equivalence | Landed frame-preparation refactor; RHI transition/state contracts | Graph-local resource handles, pass declarations, dependency compilation, deterministic scheduling, imported initial/final states, compiled transition batches, diagnostics, and synthetic graphics/compute/copy fixtures | Current fixed pass/resource/access inventory and performance baseline remain reproducible | Invalid graphs fail before recording; compiled transitions pass RHI/Vulkan validation; synthetic graphs prove read/write ordering, cycles, discard, ranges/subresources, final states, and deterministic dumps with bounded compile cost | Completed |
| 2. Renderer production pilot | Milestone 1 | One closed production vertical slice with imported, persistent, and transient resources, at least one graphics/compute or attachment/sampling handoff, optional fallback, and fixed-path equivalence evidence | The selected slice has typed inputs/results, stable image/readback fixtures, exact manual transition inventory, and no unrelated feature redesign | Production uses the graph for the slice; output, failure, telemetry, barriers, draw/dispatch identity, runtime modes, and GPU/CPU gates match baseline; migrated edges contain no manual transition authority | Completed |
| 3. Transient lifetime integration, culling, and complete frame migration | Milestone 2 | Logical transient descriptions and first/last-use analysis, graph-owned lease preparation through the existing pool, explicit side-effect roots, safe pass culling, migration of the complete fixed schedule, and retirement of duplicate orchestration | Pilot diagnostics can explain every dependency, allocation, external effect, and barrier; pool recovery and budgets remain authoritative | Directional shadow through final output executes under one graph; unused optional branches cull deterministically; temporal/output transactions, resize, multi-view, recovery, validation, images, and performance pass; fixed production scheduling and migrated manual barriers are removed | Completed |
| 4. Graph hardening and feature-authoring contract | Milestone 3 plus at least one new post-migration rendering feature | Stable typed pass/resource authoring patterns, graph inspection/capture tools, compile-time and runtime budgets, documentation, and regression gates that make RDG the required route for new inter-pass resources | Full migration exposes real authoring repetition and diagnostics needs; one new feature can exercise the contract without compatibility scaffolding | The feature lands without bypass scheduling or hidden transitions; graph dumps and failure diagnostics are actionable; aggregate compile/execute overhead stays within frozen budgets | Completed |
| 4A. Foundation consolidation | Milestone 4; post-migration architecture review | Logical resource descriptions with retained backing, pass-scoped access, minimal value/hazard dependencies, truthful pass domains, one parent scene graph, and exact capture diagnostics | The completed migration provides parity evidence and the review inventories hidden physical edges, nested scheduling, preparation gaps, and dense dependency construction | Every scene inter-pass edge and retained allocation is graph-declared; no nested graph or undeclared access remains; culling, fallback, transitions, captures, and budgets pass focused and production qualification | Completed |
| 5. Physical transient allocation and aliasing | Milestone 4A; Vulkan allocation/placement and GPU-completion prerequisites | Measured allocator design for compatible non-overlapping resources, alias barriers, retained/peak budgets, deterministic fallback, and capture diagnostics | Graph lifetime telemetry shows material peak or retained-memory savings on target content and RHI/Vulkan can express safe placement and alias transitions | Validation, stress, resize/multi-view, failure injection, capture, and target hardware measurements prove safety and material memory benefit without regression | Evidence-gated |
| 6. Queue-aware scheduling and asynchronous compute | Milestone 4A; compute workloads, queue-family/timeline RHI contracts, and profiling | Queue-qualified access, cross-queue dependencies, ownership transfer, timeline synchronization, overlap policy, and synchronous fallback | A measured workload has independent graphics/compute work, supported hardware queues, and expected overlap benefit greater than scheduling/synchronization cost | Validation and timing on supported and fallback devices prove deterministic correctness, no starvation/deadlock, bounded submission overhead, and material frame-time improvement | Evidence-gated |
| 7. Advanced graph compilation | Milestone 4A; independent evidence per optimization | Selected pass merging, scheduling reordering, parallel recording, persistent graph reuse, or other compiler optimizations | Profiling identifies graph compile, command recording, bandwidth, or render-pass overhead that one bounded optimization can address | The selected optimization has its own plan, preserves graph semantics/diagnostics, and demonstrates target improvement without coupling unrelated techniques | Optional and evidence-gated |

Milestones 1 through 4A preserve the completed migration, parity, and
consolidation evidence. Milestones 5 through 7 remain recorded so their
prerequisites are preserved, and are explicitly dispositioned from current
memory, queue, and compile-cost evidence rather than treated as implementation
work.

## Child Plan Boundaries

| Child plan | State | Owns | Must not own |
| --- | --- | --- | --- |
| [Renderer Frame Preparation Refactor](../../../Plans/Archive/2026-08/RendererFramePreparationRefactor.md) | Completed foundation | Immutable preparation, typed results, transient-provider handoff, fixed schedule, and migration baseline | An RDG builder/compiler or automatic barriers |
| [Render Graph Foundation and Barrier Equivalence](../../../Plans/Archive/2026-08/RenderGraphFoundationAndBarrierEquivalence.md) | Completed | Milestone 1 graph mechanics, structural tests, transition oracle, diagnostics, and compile-cost baseline | Production-wide migration, physical aliasing, async compute, or a public registry |
| [Render Graph Renderer Pilot](../../../Plans/Archive/2026-08/RenderGraphRendererPilot.md) | Completed | One selected production subgraph, boundary adapters, exact parity qualification, and removal of its manual barriers | Whole-frame rewrite or feature algorithm changes |
| [Render Graph Frame Migration](../../../Plans/Archive/2026-08/RenderGraphFrameMigration.md) | Completed | Logical transient integration, side effects and culling, bounded feature migrations, fixed-scheduler retirement, and lasting authoring contract | Physical aliasing, queue expansion, or unrelated renderer modernization |
| [Render Graph Hardening and Authoring Contract](../../../Plans/Archive/2026-08/RenderGraphHardeningAndAuthoringContract.md) | Completed | Structural/CPU budgets, owning graph capture, scene inspection wiring, and the mandatory inter-pass authoring contract | Physical aliasing, queue scheduling, or observer-controlled correctness |
| [Render Graph Foundation Consolidation](../../../Plans/Archive/2026-08/RenderGraphFoundationConsolidation.md) | Completed | Logical descriptions and retained backing, pass isolation, minimal dependency semantics, one parent scene graph, and exact diagnostics | Physical aliasing, queue scheduling, feature algorithm changes, or unrelated compiler optimizations |
| Render Graph Transient Aliasing | Evidence-gated | Compatible placement classes, physical reuse, alias transitions, GPU retirement, budgets, and memory evidence | Logical lifetime correctness or ordinary pool recovery |
| Render Graph Async Compute | Evidence-gated | Queue capabilities, ownership transfer, timeline scheduling, workload policy, and fallback | Treating every compute shader as asynchronously profitable |
| Render Graph Compiler Optimization: `<Technique>` | Optional per measured bottleneck | One bounded merge/reorder/reuse/parallel-recording optimization | A general optimization bundle without independent evidence |
| [PSO Cache for Render-Graph Expansion](../../../Investigations/PSOCacheForRenderGraphExpansion.md) | Open investigation | PSO request identity, duplication, creation cost, working-set, and invalidation evidence at the RDG boundary | Assuming RDG requires a cache or selecting cache policy without measurements |

Child plans may refine type names and file placement while preserving the
program invariants. Create only the next plan whose entry gate is met; do not
activate the migration, aliasing, and queue plans together.

## Program Validation Matrix

| Concern | Required program evidence |
| --- | --- |
| Graph structure | Invalid handles, missing producers, illegal read/write combinations, duplicate producers, cycles, range/subresource overlap, and use-after-graph-lifetime fail deterministically before execution |
| Dependency semantics | Read-after-write, write-after-read, write-after-write, attachment load/store, discard, side-effect roots, optional branches, and stable independent-pass ordering compile to exact expected edges |
| RHI access | Compiled buffer/texture transitions use existing exact descriptors; expected-before/final access agrees with the RHI tracker in inline and threaded recording and both Vulkan synchronization paths |
| External resources | Window, offscreen, asset, default, environment, capture, and readback resources retain counted lifetime, initial access, final `Present`/shader-read/host/transfer contracts, and owner-controlled invalidation |
| Persistent/history | View identity, discontinuity, resize, device invalidation, Begin/Commit/Abort, successful candidate publication, and failed-frame rollback never enter transient ownership |
| Transient lifetime | Every logical resource has a valid producer/use interval; physical leases cover execution and recorded-command retention; allocation failure, eviction, retry, and shutdown expose no partial bundle or dangling handle |
| Pass behavior | Graphics, compute, copy, clear, attachment, sampling, read/write, fallback, qualification, debug, and external-effect passes preserve typed success and cannot access undeclared resources |
| Culling | Unused pure branches cull; present, history publication, readback, capture, timestamps, and declared external effects remain; culling cannot change fallback or temporal policy |
| Renderer parity | Fixed reference and migrated graph paths match images/readbacks, pass/draw/dispatch identity, feature results, telemetry, viewport/scissor, output state, and failure outcomes until the fixed path is retired |
| Recovery and isolation | Shader/manual/device generations, last-known-good persistent resources, transient creation failure, resize churn, main/preview/auxiliary views, and interleaved extents remain isolated and complete |
| Diagnostics | Named passes/resources, dependency causes, lifetimes, culling reasons, physical allocation identities, barrier plans, external states, and compile failures are deterministic and capture-safe |
| Performance | Graph build/compile/execute CPU median and p95, allocations, active/retained/peak bytes, barriers, command counts, GPU pass timings, and full-frame timings are compared to frozen gates on quiet lanes |
| Platform/runtime | Focused and aggregate tests, required builds, inline/threaded execution, Vulkan validation, window/offscreen output, resize/multi-view matrices, failure injection, rendered references, and Editor smoke follow repository guidance |

## Risks and Control Gates

- **A second scheduler survives migration.** Every pilot names the exact edges
  moved to RDG and removes their manual barriers. Milestone 3 cannot exit while
  production retains an alternate fixed orchestration path.
- **The compiler duplicates RHI state.** RDG stores logical declarations and a
  compiled transition plan only; RHI remains responsible for validating and
  committing actual resource access during replay.
- **Generic graph APIs erase feature contracts.** Prepared feature values and
  pass results stay typed. No mandatory string lookup or mutable blackboard may
  replace compile-time resource/data relationships.
- **Callbacks hide undeclared access.** Migrated callbacks receive declared
  graph resources rather than arbitrary frame-wide target access. Debug
  validation and focused tests reject hidden transition or acquisition paths.
- **Partial migration creates double barriers or wrong expected-before state.**
  Each boundary has one documented owner, and the pilot uses compiled-versus-
  manual transition traces before deleting the manual oracle.
- **Culling removes required non-resource work.** Side effects and roots are
  explicit, and culling lands only after the pilot can explain dependency and
  effect reachability in diagnostics.
- **Transient allocation failure occurs after commands have recorded.** Graph
  compilation and complete physical lease preparation finish before pass
  execution; failures abort without partial RHI state or temporal publication.
- **Imported or history resources are accidentally recycled.** Resource class
  is immutable in the graph registry. Only graph-created transient identities
  may enter allocation reuse or later aliasing.
- **Compile cost exceeds the manual scheduler benefit.** Every milestone freezes
  graph size and CPU budgets, avoids per-frame string ownership in hot paths,
  and records build/compile median and p95 before expanding consumers.
- **Early abstraction anticipates unsupported workloads.** The required path is
  single queue and uses current access semantics. Aliasing, queue scheduling,
  pass merging, and reuse each wait for their own workload and platform data.
- **Aliasing saves nominal bytes but increases instability.** Milestone 5
  requires peak-memory evidence, compatibility classes, alias barriers,
  completion-safe reuse, graceful non-alias fallback, and target-device gains.
- **Async compute adds synchronization without overlap.** Milestone 6 requires
  timestamp evidence for independent work and validates both capable and
  synchronous fallback hardware before changing production policy.
- **RDG triggers speculative PSO architecture.** The graph instruments demand
  for the existing investigation; PSO ownership changes only if duplicate
  creation, critical-path latency, or working-set evidence crosses its gate.

## Completion Criteria

- Renderer production frames are constructed and executed through one RDG;
  the former fixed scheduling authority and migrated manual inter-pass
  transitions are removed.
- Graph declarations are the authoritative source of pass dependencies,
  resource access, logical lifetimes, external effects, and frame boundary
  states, while RHI/Vulkan remain the authoritative execution state model.
- Imported, persistent/history, and transient resources retain their specified
  owners, recovery behavior, temporal transactions, and shutdown guarantees.
- Required Renderer feature chains preserve image/readback output, typed
  outcomes, telemetry, failure/fallback semantics, multi-view isolation,
  runtime modes, and accepted CPU/GPU performance.
- New features that introduce inter-pass resources use the documented typed
  graph authoring contract without hidden scheduling or pass-local target
  ownership.
- Deterministic diagnostics expose passes, edges, lifetimes, culling,
  allocations, barriers, and external states sufficiently to diagnose a failed
  graph without backend capture as the only source of truth.
- Milestones 1 through 4 and the post-migration foundation consolidation
  complete through bounded validated child plans, and lasting contracts move
  to Runtime Rendering/RHI documentation.
- Physical aliasing, async/multi-queue scheduling, and advanced compilation are
  either completed from evidence or explicitly dispositioned with current
  measurements; their absence does not leave two graph architectures.

## Related Documentation

- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation and Render Graph Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Persistent View State](../../../Runtime/Rendering/PersistentViewState.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [PSO Cache for Render-Graph Expansion](../../../Investigations/PSOCacheForRenderGraphExpansion.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPlan.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.h`
