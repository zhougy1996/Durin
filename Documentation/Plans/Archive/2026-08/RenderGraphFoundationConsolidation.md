# Render Graph Foundation Consolidation Plan

Summary: Consolidate Render Graph resource, pass-isolation, dependency, preparation, and inspection contracts before evidence-gated aliasing or queue scheduling.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

Stages 0 through 3 are complete. The compiler now
normalizes partial ranges, constructs resource versions and a minimal hazard
frontier, culls from value/effect reachability, validates pass domains and
unique imported identity, resolves only retained logical backing, and publishes
exact pointer-free resource/use/transition diagnostics. Pass callbacks receive
capability-bounded views and undeclared lookup terminates at the authoring
boundary.

The scene consolidation is complete. Scene Color, depth, GBuffer, ambient
occlusion, Contact Visibility, volumetric-cloud shadow visibility, cloud
spatial/composite, isolated deferred, GBuffer debug, and final output have
logical/imported graph identities and retained backing publication. GBuffer,
isolated-deferred, and Scene Color typed outcomes carry status and logical
selection only; the ambient-occlusion, Contact Visibility, cloud-shadow, and
cloud-spatial outcomes likewise carry no cross-pass physical pointers. Their
consumers resolve declared graph handles rather than forwarding physical
textures through tokens. Attachment and managed multi-render-pass bodies
publish their RHI-validated exit access, and Contact Visibility no longer builds
or executes a child graph. The directional-shadow depth array is now a unique
imported graph resource; its result is status-only and its graphics consumers
declare the array read. Post process and editor assistance are separate
parent-graph passes whose final color and depth edges are explicit, and the
post-process result no longer forwards a physical input pointer. Contact
Visibility, volumetric-cloud shadow visibility, and volumetric-cloud spatial
rendering now resolve shader/pipeline availability before graph construction,
author only the selected compute or fragment target and truthful pass domain,
and leave migrated texture transitions to the parent graph. Scene opaque,
cloud spatial, cloud temporal/composite, and sorted translucency boundaries are
separate graph passes; the former manual depth and color boundary transitions
are graph edges. Cloud density/weather inputs are unique imported resources
whose route-specific reads and physical fallback aliases are explicit. GBuffer
and directional-shadow depth layouts publish their actual sampled exit access,
GBuffer debug no longer duplicates the depth transition authority, and the
disabled directional-shadow fallback is a depth-array resource compatible with
comparison sampling rather than a color texture.

Backing preparation now derives its target-family request from the compiler's
retained logical resource list, including cloud extent from the retained
descriptor, and publishes only requested handles through the atomic backing
candidate. Directional shadow, cloud density/weather, default white/shadow,
and environment textures share one per-frame physical import identity, so
fallback aliases cannot escape graph hazard tracking. Stage 2 is complete:
every deferred scene callback now names its bounded execution inputs instead of
capturing the complete authoring scope, and the retained-backing resolver names
only its requirements, graph-resource table, and atomic resolution result. Retained
opaque now publishes depth directly for shader reads, sorted translucency owns
the final depth-attachment exit, and the scene executor issues no manual
texture transition.

The consolidated scene budget is frozen at 12 declared passes, 28 dependencies,
and 20 texture transitions; the obsolete zero-buffer-transition gate was
removed because the current scene graph declares no cross-pass buffers. The
Vulkan cloud fixture records six disabled, invalid-input, compute, fragment,
offscreen, present, and resized parent graphs: all schedule 11 passes, use
22--25 dependencies, and lower either 1 or 17 texture transitions. Every
capture remains below the existing 5 ms compile and 250 ms complete callback
schedule observation ceilings.

Focused RenderCore, Renderer scene-contract, and Vulkan transition tests pass.
The `fast-all` contract, feature, and infrastructure profile also passes after
its editor asset fixtures were made to own AssetForge registration and wait for
scheduled static-mesh recovery before inspecting render data.
The Directional Shadow Vulkan qualification preserves Contact Visibility
compute/fragment output parity and route telemetry after graph ownership moved
to the parent scene frame. Its frozen capture now records the sole parent-owned
shader-read-to-depth-attachment boundary after retained opaque began publishing
sample-ready depth.
The Volumetric Cloud scene Vulkan fixture likewise preserves compute and forced
fragment cloud-shadow routes, output composition, and route telemetry after its
visibility targets and request-bounded SceneDepth edge moved into the parent
graph.
The same fixture preserves spatial compute/fragment routing and composition
after spatial rendering became a truthful parent-graph pass and the opaque,
cloud, and translucency boundaries stopped issuing manual texture transitions.
It also preserves offscreen/present composition and direct spatial-route
recovery after the fragment, compute, and composite targets plus the selected
Scene Color output became graph-owned.
The consolidated renderer Vulkan integration set passes after failure injection
exposed and fixed a retained-preparation propagation defect: every requested
target family is now validated in a local candidate and published to the frame
only after the complete set succeeds, so an injected GBuffer allocation failure
aborts before recording and returns `RendererResourcesUnavailable` without
publishing telemetry.
The first full native-test sweep also exposed two independently reproducible
cooked-package fixture defects. The Terrain heightmap and Texture 2D cooked
consumers now own AssetForge provider registration for their complete import,
cook, manager-restart, and source-free load lifecycle instead of relying on
another target's process state; both isolated failures pass after the repair.
The GBuffer Vulkan qualification, including full- and half-resolution GTAO,
passes after its adapter contract was corrected. The fixture had reported a
hard-coded RTX 3090 identity and applied that adapter's absolute nanosecond
thresholds while Vulkan was actually running on a GTX 1060. It now queries the
selected physical device, publishes truthful adapter/API/driver evidence,
preserves the frozen RTX 3090 + Vulkan 1.4.325 gates only on that named lane,
and treats other adapters as observations while still enforcing output,
route-relative timing, memory, telemetry, and stability contracts.
The HDR display qualification had the same hard-coded adapter defect. It now
queries and reports the selected Vulkan device, keeps copy/FXAA correctness and
route ordering active on every adapter, and applies its unchanged RTX 3090
absolute timing thresholds only on the matching named lane.

Final validation passed on `Win64-Debug-DurinEditor`: the complete `all` build,
all 79 routine native-test targets, the focused RenderCore/RHI/Renderer and
renderer Vulkan integration sets, and the four renderer Vulkan qualification
targets. Changed and repository-wide documentation, plan, and roadmap
validators also pass. No compatibility scheduler, nested scene graph, hidden
inter-pass physical path, migrated manual transition authority, physical
aliasing, or queue scheduling path remains active.

## Goal

Make the production scene frame one enforceable parent Render Graph whose
declared passes, physical and logical resources, value dependencies, access
states, retained lifetimes, preparation requests, and diagnostics completely
explain every inter-pass edge before any graph pass records commands.

## Scope

- Separate graph-created logical texture and buffer descriptions from their
  physical RHI backing and resolve retained backing as one pre-execution
  transaction.
- Enforce one graph identity for each imported physical resource and expose
  only the resources declared by the executing pass.
- Validate graphics, compute, and copy pass domains against their declared
  attachment and access intents.
- Replace dense all-pairs dependency construction with version-aware value
  dependencies and a minimal execution-hazard frontier, including correct
  discard and partial-overlap semantics.
- Migrate the scene-frame schedule from token-only wrapper passes to declared
  texture, buffer, and typed logical result edges.
- Replace nested feature graphs with feature authoring functions that
  contribute passes to the parent scene graph.
- Make culling determine the retained transient preparation request and expose
  exact resource, transition, lifetime, culling, and allocation diagnostics.
- Preserve current rendering output, failure and fallback policy, telemetry,
  temporal transactions, recovery, and supported runtime modes.

## Non-Goals

- Physical memory aliasing, placed or sparse resources, alias barriers, or a
  replacement transient allocator.
- Queue-family ownership, timeline synchronization, asynchronous compute,
  multi-queue submission, or CPU-parallel command recording.
- Feature shader, lighting, shadow, cloud, post-process, visibility, or
  material algorithm changes.
- A public pass-injection API, runtime-polymorphic feature registry, mutable
  string blackboard, or renderer plugin surface.
- Automatic pass merging, performance reordering, persistent graph reuse,
  centralized PSO caching, or an unrelated compiler-optimization bundle.
- Replacing RHI or Vulkan as the physical resource-state authority.

## Design Decisions and Invariants

- **One scene submission has one parent graph.** Renderer features may author
  multiple passes through typed helper functions, but no pass callback creates,
  compiles, or executes a nested Render Graph.
- **Logical identity precedes backing.** A graph-created texture or buffer owns
  a logical description during compilation. After culling and lifetime
  analysis, one resolver receives only the retained requests and publishes a
  complete backing table before the first transition or callback.
- **Preparation is complete or records nothing.** A missing required backing
  aborts that graph before command recording. A renderer fallback that changes
  graph topology is selected by building a deterministic replacement graph
  before recording; callbacks do not change route because allocation failed.
- **Imported physical identity is unique.** One physical RHI texture or buffer
  is imported once per graph. All of its ranges use the same handle; duplicate
  registration is a compile error rather than a second state authority.
- **Pass access is capability-bounded.** The resource view supplied to a pass
  resolves only handles declared by that pass. An undeclared lookup is an
  unrecoverable authoring-contract failure, not a nullable optional resource.
- **Pass domains are truthful.** Graphics passes own graphics and attachment
  access, compute passes own compute access, and copy passes own transfer
  access. Route selection occurs before pass declaration.
- **Value reachability and execution hazards are distinct.** Culling follows
  produced-value and explicit-effect dependencies. Stable execution ordering
  additionally respects the minimal required RAW, WAR, and WAW hazard frontier
  among retained passes. A discard write starts a new value version and does
  not retain an overwritten producer.
- **Ranges are normalized by the compiler.** Partially overlapping buffer
  intervals and texture aspect/mip/layer ranges are partitioned into exact
  tracked regions; valid overlap is not rejected merely because declarations
  use different range extents.
- **RHI remains state authority.** The graph lowers declared access to existing
  RHI transitions; RHI and Vulkan validate and commit physical execution state.
- **Observation remains passive.** Captures own stable, pointer-free resource,
  use, transition, lifetime, allocation, and pass records. Capture or timing
  state cannot influence graph construction, preparation, execution, or frame
  commit.

## Current Foundations and Gaps

| Area | Reusable foundation | Gap owned by this plan |
| --- | --- | --- |
| Graph API | Generation-safe texture, buffer, token, and pass handles | Graph-created resources require physical pointers before compile; pass lookup is graph-wide |
| Compilation | Stable topological order, exact-range validation, transition lowering, structural budgets | All pass pairs are compared; value reachability and execution hazards share one dense edge set; partial overlap is rejected |
| Production schedule | One scene-frame executor and explicit output root | Parent callbacks exchange hidden physical resources and contact visibility executes a nested graph |
| Transient targets | Budgeted complete target groups and recovery generations | Retained graph lifetimes do not select preparation requests or publish graph backing |
| Fallback | Existing compute, fragment, and factor-one feature policy | Some route and availability decisions occur after the parent graph is compiled |
| Diagnostics | Owning pointer-free capture, deterministic dump, CPU and structural statistics | Capture reports transition counts rather than exact logical resource, access, range, and backing records |
| Validation | RenderCore structural tests, Renderer contracts, Vulkan transition tests, image and performance fixtures | No contract covers undeclared pass access, duplicate physical identity, domain mismatch, dead-version culling, parent feature authoring, or retained-only preparation |

## Implementation Stages

### Stage 0: Freeze the consolidation boundary

- [x] Inventory every scene-frame physical resource that crosses top-level
  callbacks, every surviving manual transition at such a boundary, every
  nested graph, and every route selected after parent compilation.
- [x] Record representative production graph captures, transient target
  requests, dependency counts, transition counts, compile/execute CPU cost,
  output fixtures, and failure outcomes as the unchanged baseline.
- [x] Freeze the logical resource description, retained-backing resolver,
  resource-version, range-partition, pass-resource-view, and fallback-retry
  contracts before changing the public RenderCore surface.

#### Acceptance Gate

- Every migrated scene edge has one named future graph declaration and one
  current owner; the selected contracts have no unresolved ownership,
  ordering, access, lifetime, or failure-policy decision.

### Stage 1: Harden RenderCore resource and compiler contracts

- [x] Add logical texture and buffer descriptions plus a retained-resource
  backing resolver that publishes one complete compiled-graph resource table.
- [x] Enforce unique imported physical identity, pass-scoped resource lookup,
  pass-domain/access compatibility, and exact imported boundary states.
- [x] Split compiler internals into declaration validation, normalized range
  and version construction, value reachability, retained scheduling,
  transition lowering, budget enforcement, and capture publication phases.
- [x] Replace all-pairs hazard edges with minimal value and execution frontiers;
  make discard, read/write, attachment load/store, disjoint ranges, and partial
  overlaps explicit in the version model.
- [x] Extend owning capture data with stable resource/use/transition identifiers,
  exact ranges and access states, logical lifetimes, preparation disposition,
  and backing-class diagnostics without exposing RHI pointers.
- [x] Add focused RenderCore and RHI/Vulkan contracts for every new invariant
  and preserve deterministic dumps and bounded compile cost.

#### Acceptance Gate

- Invalid identity, undeclared lookup, domain/access, range, producer, and
  preparation cases fail at their defined boundary; valid synthetic graphs
  produce minimal deterministic dependencies, exact transitions, and complete
  pointer-free captures in inline and recorded-command execution.

### Stage 2: Author the complete scene frame into one graph

- [x] Introduce typed Renderer graph-authoring inputs and results whose
  cross-pass physical values are graph handles rather than hidden RHI pointers
  in executor state.
- [x] Convert contact visibility and other closed feature schedulers into
  parent-graph authoring helpers and remove every nested graph compilation from
  scene-frame callbacks.
- [x] Declare imported, persistent, and transient edges for directional shadow,
  GBuffer/depth, ambient occlusion, contact visibility, cloud shadow, deferred
  lighting, Scene Color, post process, editor assistance, and final output.
- [x] Select compute, fragment, disabled, and factor-one topology before graph
  compile; preserve deterministic pre-recording fallback when retained backing
  preparation fails.
- [x] Derive transient target requests from retained logical resources, skip
  culled allocations, publish backing atomically, and preserve existing pool
  budgets, recovery generations, and recorded-command retention.
- [x] Remove migrated manual inter-pass transitions, token substitutes for
  physical edges, graph-wide callback captures, and obsolete zero-transition
  scene budgets; freeze new structural and observational budgets from evidence.

#### Acceptance Gate

- One parent graph capture explains every scene-frame inter-pass physical and
  logical edge, transition, lifetime, culling decision, preparation request,
  and external effect; no nested graph or hidden transition authority remains,
  and unused optional branches neither execute nor allocate targets.

### Stage 3: Qualify behavior, failure, diagnostics, and cost

- [x] Pass focused RenderCore, RHI, Vulkan, transient-pool, and Renderer
  contracts covering the consolidated authoring and execution boundaries.
- [x] Pass scene image/readback, contact compute/fragment/factor-one, GBuffer,
  deferred lighting, volumetric cloud, editor assistance, present/offscreen,
  resize, multi-view, duplicate-submission, recovery, and shutdown fixtures.
- [x] Compare pass/draw/dispatch identity, output, temporal commit/abort,
  telemetry, transition plans, retained/active bytes, compile/execute CPU cost,
  and relevant GPU timings against the Stage 0 baseline.
- [x] Complete the required full build and documentation validators through the
  repository build, test, and documentation workflows.
- [x] Move lasting resource, pass, dependency, preparation, failure, capture,
  and authoring rules into Runtime Rendering documentation and update the
  roadmap with measured follow-up evidence.

#### Acceptance Gate

- All required validation passes without a compatibility scheduler or hidden
  resource path; production budgets are evidence-backed, diagnostics are
  actionable without backend capture, and aliasing or queue scheduling remains
  inactive unless its independent entry evidence is recorded.

## Validation Matrix

| Concern | Evidence |
| --- | --- |
| Handle, identity, pass access, and domain rules | Focused `RenderContractTests` compile and execution contracts |
| Versions, ranges, discard, dependencies, and culling | Synthetic texture/buffer/token graphs with deterministic dependency and lifetime assertions |
| Transition equivalence | RHI transition validation plus Vulkan state-tracking fixtures in inline and recorded modes |
| Retained-only preparation and recovery | Renderer transient-pool budget, failure-injection, resize, invalidation, and shutdown contracts |
| Single parent scene graph | Renderer capture tests proving no nested compile and complete declared physical/logical edges |
| Route and fallback parity | Contact/cloud compute, fragment, disabled, allocation-failure, and factor-one fixtures |
| Frame correctness | Existing scene image/readback, GBuffer, deferred, cloud, shadow, editor, present, and offscreen qualifications |
| Temporal and view isolation | Begin/Commit/Abort, duplicate submission, multi-view, history invalidation, and recovery fixtures |
| CPU/GPU and memory budgets | Stage 0 versus consolidated graph compile/execute, pass identity, GPU timing, active bytes, and retained bytes |
| Build and documentation | Required target build plus changed, all-plan, and all-roadmap documentation validation |

## Definition of Done

- Stages 0 through 3 and every acceptance gate pass.
- Production scene submission constructs, compiles, and executes exactly one
  parent graph; feature helpers contribute passes and never execute child
  graphs.
- Every physical resource crossing graph passes is represented by one graph
  handle, and every callback can resolve only its declared handles.
- Graph-created resources compile from logical descriptions; retained lifetime
  and culling results drive one complete backing publication before recording.
- Fallback topology and pass domain are fixed before compile, and allocation
  failure cannot silently change execution inside a callback.
- Dependency and culling tests prove linear rather than all-pairs growth for a
  same-range overwrite chain while preserving required execution hazards.
- Captures expose exact pointer-free resources, uses, transitions, lifetimes,
  culling, preparation, and allocation disposition with stable identities.
- Lasting Runtime contracts and the Render Graph roadmap reflect the completed
  boundary, and changes are validated and committed with this plan and stage
  provenance.

## Deferred Follow-ups

- Physical transient aliasing remains evidence-gated by measured logical peak
  and retained-memory savings plus Vulkan placement and retirement capability.
- Queue-aware scheduling and asynchronous compute remain evidence-gated by a
  truthful consolidated pass DAG, queue/timeline RHI support, and a measured
  overlap opportunity.
- Pass merging, performance reordering, parallel recording, persistent graph
  reuse, and other advanced compilation each require an independent measured
  bottleneck and bounded plan.
- PSO cache policy remains owned by the existing investigation rather than
  being inferred from this consolidation.

## Related Documentation

- [Render Graph Architecture Roadmap](../../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [Render Graph](../../../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation and Render Graph Execution](../../../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererTransientTargetPool.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererTransientTargetPool.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRendererProfiling.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRendererProfiling.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
