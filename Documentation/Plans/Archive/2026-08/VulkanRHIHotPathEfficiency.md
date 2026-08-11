# VulkanRHI Hot-Path Efficiency Plan

Summary: Remove avoidable descriptor-validation scans, barrier-lowering allocations, and descriptor-statistics rescans while preserving the completed RHI/Vulkan correctness contracts.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

The completed RHI and Vulkan correctness work established canonical graphics
keys, exact descriptor state validation, checked mapped-memory synchronization,
and complete per-surface swapchain selection. A follow-up source audit found
three smaller CPU-side costs in already-correct hot paths:

- draw preparation performs completeness lookup with a nested
  layout-element-by-resource search, then walks the same bindings again for
  Vulkan-specific validation;
- each buffer or texture transition batch creates both synchronization2 and
  legacy barrier vectors and lowers every transition into both forms even
  though one immutable device capability selects only one path; and
- descriptor cache insertion, eviction, clear, and pipeline removal recompute
  occupancy by traversing every pipeline state, cache entry, and retained
  descriptor value.

The correctness-hardening baseline passes 59 `RHICommandListTests` cases and
54 `VulkanRHIIntegrationTests` cases. This plan first records deterministic
work counters and allocation-sensitive fixtures, then optimizes only work that
the evidence shows is redundant. Swapchain maintenance and the maintenance-
unavailable queue-idle fallback remain explicitly outside scope.

Completion evidence:

| Hot path | Before | Completed evidence |
| --- | --- | --- |
| Binding completeness | A 64-element complete array performed 2,080 snapshot comparisons, followed by another Vulkan layout/resource search and a dynamic-offset pass. | The shared ordered cursor performs 64 validation visits. A 514-draw, three-element Vulkan cache-hit/eviction fixture reports exactly 1,542 visits. |
| Barrier lowering | Every input constructed one synchronization2 and one legacy record and reserved both vectors. | Forced legacy and synchronization2 fixtures each construct 130 buffer and two image records for 130/two inputs, with zero inactive-path records. Operation-local single-path vectors retain zero context capacity after small or 64-element burst batches. |
| Descriptor occupancy | Insertion and other mutations called a full pipeline/cache/value refresh. | The hit/insertion/eviction fixture reports 514 local occupancy mutations and zero verification traversal visits; the bounded result remains exactly 512 entries/1,536 values across statistics reset. Clear, pipeline deletion, and context reset subtract their exact owned counts; test-only cold verification independently recomputes totals. |

Qualification completed on the `windows-msvc-x64` Agent Build Profile:
`RHICommandListTests` 60/60, `VulkanRHIIntegrationTests` 54/54, changed-document
validation, incremental full `all` build, and validation-enabled hidden
DurinEditor PIE lifecycle smoke through normal shutdown.

## Goal

Make descriptor validation, resource-transition lowering, and descriptor-cache
statistics proportional to the work changed by the current command, without
changing public RHI behavior, Vulkan synchronization semantics, cache policy,
diagnostics, or resource lifetime.

## Scope

- A single ordered validation walk for complete graphics binding snapshots.
- Reuse of the same ordered binding/resource correspondence for Vulkan view,
  state, and dynamic-offset validation before descriptor-cache lookup.
- Construction of only the barrier representation selected by immutable
  `bSupportsSynchronization2`.
- A measured, bounded strategy for avoiding repeated barrier-vector heap
  allocation without retaining unbounded one-frame spikes.
- Incremental descriptor entry/value occupancy accounting across insertion,
  eviction, clear, pipeline deletion, frame reset, and context reset.
- Deterministic work counters or focused benchmarks that prove the redundant
  scans/lowerings were removed without timing-only acceptance criteria.
- Focused RHI/Vulkan tests, lasting-contract updates where implementation
  ownership changes, documentation validation, and proportional runtime
  qualification.

## Non-Goals

- Adding swapchain maintenance, present-wait, present-id, or changing the
  queue-idle fallback used when swapchain maintenance is unavailable.
- Caching dynamic surface capabilities or changing swapchain negotiation.
- Changing descriptor cache capacities, LRU policy, hashing, equality, pool
  rotation, completion-token retirement, or frame-generation ownership.
- Coalescing barriers, synthesizing transitions, adding a render graph, or
  changing portable `ERHIAccess` semantics.
- Replacing VMA, changing mapped-memory policy, or introducing a general frame
  allocator, arena, small-vector framework, or public container API.
- Weakening binding completeness, exact texture-view state validation,
  dynamic-offset alignment, diagnostics, or failure behavior.
- GPU performance tuning, shader changes, queue topology changes, or unrelated
  Vulkan object-cache work.

## Design Decisions and Invariants

### Measure work, not wall-clock noise

- Acceptance uses deterministic counts such as resource comparisons,
  validation visits, native barrier records built, occupancy traversal visits,
  and relevant allocation/capacity events. Wall-clock data may support a
  decision but is not a pass/fail gate.
- Counters are test-only or folded into an existing private diagnostic seam;
  no new public RHI statistics contract is created solely for this work.
- Baseline fixtures cover empty, scalar, array, sparse update, maximum admitted
  binding, small transition, and burst transition shapes before production
  algorithms change.

### Ordered binding validation

- Pipeline binding layouts retain canonical set order and slot order; pending
  shader resources retain canonical set/binding/array-element order.
- Completeness is proven by one merge-style walk over the expected flattened
  elements and actual resource records. Missing, duplicate, mismatched, null,
  and trailing unexpected records retain deterministic owned diagnostics.
- Vulkan-specific resource-kind, view-usage, exact tracked-state, and dynamic-
  offset checks consume the same correspondence before cache reuse. They do not
  create a second layout scan or another resource-state authority.
- Dynamic offsets preserve the descriptor-set binding order required by Vulkan
  and remain excluded from immutable descriptor identity.

### Barrier lowering and scratch ownership

- The immutable synchronization2 capability is selected before lowering a
  batch. A synchronization2 device builds only `vk::*MemoryBarrier2`; a legacy
  device builds only legacy barriers and stage aggregates.
- Complete state validation still precedes native barrier recording and tracker
  commit. A rejected batch cannot partially mutate state or scratch-visible
  authority.
- Any retained scratch storage belongs to one command context on the RHI thread,
  is cleared between operations, and has an explicit capacity-retention bound.
  Oversized bursts may use temporary storage or release excess capacity rather
  than permanently growing every context.
- The implementation must not rely on nested transition replay being
  impossible unless that non-reentrancy is already an enforced context
  contract; otherwise operation-local storage remains the safe choice.

### Incremental descriptor occupancy

- `DescriptorSnapshots.Occupancy` equals the exact total number of live cache
  entries owned by the context. `DescriptorValueOccupancy` equals the sum of
  retained resource records in those entries.
- Each mutation updates counters in the same ownership operation that changes
  the cache. Failed candidate creation changes neither cache contents nor
  occupancy.
- Clear, pipeline deletion, frame-generation reset, eviction, and context reset
  subtract exact owned counts before destruction. Debug/test verification may
  recompute totals at cold boundaries, but production hot paths do not rescan
  all pipeline states.
- Public reset semantics remain unchanged: accumulated counters reset while
  current occupancy stays exact.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| Binding identity | Canonically sorted layouts and pending resource snapshots | Completeness uses repeated linear searches and Vulkan repeats the layout walk |
| Descriptor correctness | Exact view usage/state validation before cache reuse | Correct validation correspondence is not reused as one linear pass |
| Barrier correctness | One portable mapping feeds sync2 and legacy Vulkan paths | Both native representations and two vectors are built for every batch |
| Context ownership | Transition replay is serialized on the RHI thread | Operation-local vectors repeatedly allocate; no measured bounded reuse policy exists |
| Cache accounting | Bounded descriptor LRU and exact exposed occupancy | Every mutation may traverse all pipeline/cache/value state to refresh totals |
| Tests | RHI binding arrays, transitions, descriptor cache and conformance coverage | No retained work-count fixtures prove linear validation, single-path lowering, or O(1) occupancy updates |

## Implementation Stages

### Stage 0: Freeze baselines and work-count evidence

Outcome: correctness invariants and deterministic redundant-work measurements
are executable before optimization.

- [x] Record expected flattened binding order and diagnostic precedence for
  scalar, array, missing, null, mismatched, duplicate, and trailing records.
- [x] Add work-count fixtures that expose the current nested completeness
  search and repeated Vulkan binding traversal at representative binding sizes.
- [x] Add sync2 and legacy lowering fixtures that count native barrier records
  constructed for buffer and texture batches without requiring two GPUs.
- [x] Add descriptor occupancy mutation fixtures for insertion, cache hit,
  eviction, clear, pipeline deletion, frame reset, failed creation, and context
  reset.
- [x] Measure small and burst transition capacity behavior and choose a frozen
  scratch-retention bound or operation-local single-path strategy.
- [x] Confirm the pre-change 59/59 RHI and 54/54 Vulkan baselines.

#### Acceptance Gate

- Every optimization has a deterministic pre-change work metric and unchanged
  semantic oracle.
- The selected barrier storage strategy has explicit ownership, reentrancy, and
  retained-capacity rules.
- No public API, cache-policy change, swapchain work, or timing-only gate is
  required.

### Stage 1: Make binding validation one ordered pass

Outcome: draw preparation proves completeness and Vulkan resource compatibility
with one ordered correspondence rather than nested lookup scans.

- [x] Introduce one private flattening/ordered-walk helper or equivalent cursor
  logic shared by portable completeness and Vulkan draw preparation.
- [x] Preserve exact set, binding, array-element, type, null-resource, and stage
  validation plus deterministic diagnostic precedence.
- [x] Fold Vulkan buffer-view, texture-view, exact texture-state, sampler-kind,
  and dynamic-offset checks into the ordered resource walk before cache lookup.
- [x] Build dynamic offsets in Vulkan-required binding order without adding a
  second resource traversal where the ordered walk can emit them directly.
- [x] Retain descriptor hash/equality, resource ownership, cache hit behavior,
  and threaded/inline execution semantics.
- [x] Run focused RHI binding tests and Vulkan descriptor/conformance coverage.

#### Acceptance Gate

- Validation visits are linear in expected plus actual binding elements for all
  retained fixtures; no per-element search over the complete resource snapshot
  remains in draw preparation.
- All previous rejection diagnostics and descriptor correctness tests remain
  valid, including dual-use sampled/storage exact-state checks.
- Descriptor cache identity and dynamic-offset ordering are unchanged.

### Stage 2: Lower only the active barrier representation

Outcome: each transition batch validates once and constructs only the Vulkan
barriers that the active synchronization path consumes.

- [x] Select sync2 versus legacy lowering before barrier construction for both
  buffer and texture transitions.
- [x] Share portable validation/mapping logic without materializing the unused
  native barrier representation.
- [x] Apply the Stage 0 storage strategy and enforce its retained-capacity bound
  for small normal batches and oversized bursts.
- [x] Preserve all-before-record validation, legacy stage aggregation, queue-
  family ignored fields, image layouts, exact ranges, and post-record tracker
  commit ordering.
- [x] Cover sync2 and legacy buffer/texture mappings, rejection atomicity,
  repeated small batches, burst batches, and inline/threaded replay.
- [x] Run focused RHI transition and Vulkan integration targets.

#### Acceptance Gate

- Each transition constructs exactly one native barrier record per input for
  the selected path and zero records for the inactive path.
- Repeated ordinary batches meet the chosen allocation/capacity evidence while
  oversized bursts do not create unbounded retained context memory.
- Native barrier fields and tracker results remain identical to the semantic
  pre-change oracle on both synchronization paths.

### Stage 3: Maintain descriptor occupancy incrementally

Outcome: descriptor cache mutations update exact occupancy locally and no hot
mutation rescans all pipeline descriptor states.

- [x] Add context-owned entry/value totals with checked add/subtract helpers.
- [x] Update totals atomically with successful insertion, LRU eviction, clear,
  pipeline deletion, frame reset, and context reset ownership changes.
- [x] Keep failed descriptor allocation/update candidates and cache hits neutral
  to occupancy while preserving existing accumulated counters.
- [x] Remove production calls to full `RefreshDescriptorCacheOccupancy` from
  hot mutation paths; retain a test/debug recomputation assertion only if it
  provides useful invariant evidence.
- [x] Cover zero/nonzero pipelines, mixed entry sizes, repeated reset, eviction,
  failure recovery, and statistics reset semantics.
- [x] Run focused Vulkan descriptor-cache and diagnostic-statistics coverage.

#### Acceptance Gate

- Exposed entry/value occupancy equals an independent recomputation after every
  tested mutation.
- Production mutation work is O(1) plus the entries actually inserted or
  removed; it does not traverse unrelated pipelines or cache entries.
- Cache capacity, LRU order, resource lifetime, completion retirement, and
  public statistics reset behavior are unchanged.

### Stage 4: Qualify and publish the optimization boundaries

Outcome: measured hot-path reductions are regression-qualified and documented
without broadening rendering behavior.

- [x] Update lasting graphics-binding, transition, and Vulkan cache ownership
  documents only where the optimized implementation changes a durable rule.
- [x] Record before/after deterministic work counts and capacity evidence in
  this plan; do not claim unsupported wall-clock gains.
- [x] Run documentation validation plus the smallest affected RHI and Vulkan
  native targets under repository guidance.
- [x] Run a full `all` build because the implementation crosses shared RHI and
  VulkanRHI runtime/test boundaries.
- [x] Run a validation-enabled hidden Editor smoke covering ordinary rendering,
  resize, and shutdown; detached viewport WSI remains covered by the retained
  Vulkan integration suite.
- [x] Review code, tests, measurements, documentation, and exclusions before
  marking the plan Completed.

#### Acceptance Gate

- Focused tests, documentation validation, the full build, and runtime smoke
  pass from the required profiles.
- Deterministic evidence proves linear binding validation, inactive barrier
  lowering removal, bounded allocation behavior, and incremental occupancy.
- No swapchain maintenance, queue-idle fallback, surface negotiation, cache
  policy, public API, or resource-state semantic change entered the work.

## Validation Matrix

| Contract | Focused coverage | Required outcome |
| --- | --- | --- |
| Binding completeness | RHI scalar/array/missing/mismatch tables | One ordered pass retains exact acceptance and diagnostics |
| Vulkan descriptor validation | Inline/threaded sampled, storage, buffers, samplers, dynamic offsets | Same correspondence validates type, view, state, and offset before cache reuse |
| Descriptor identity | Cache hit/miss and collision fixtures | Hash/equality, ownership, and dynamic-offset exclusion remain unchanged |
| Sync2 barriers | Buffer/texture mapping and transition integration | One Barrier2 record per transition; no legacy record construction |
| Legacy barriers | Injected/pure legacy lowering fixtures | One legacy record per transition with exact aggregate stages; no Barrier2 construction |
| Barrier storage | Repeated small and oversized batches | Ordinary reuse/one-path allocation meets bound; burst capacity is not retained without limit |
| Occupancy insertion/removal | Entry/value mutation tables | Incremental totals equal independent recomputation after each operation |
| Failure/reset behavior | Descriptor failure, eviction, frame/context/statistics reset | Failed work is neutral; occupancy and accumulated counters retain their contracts |
| Final qualification | Focused targets, full build, validation smoke | No rendering, validation, lifetime, or shutdown regression |

## Definition of Done

- All Stage 0-4 checklist items and acceptance gates are complete with recorded
  deterministic before/after evidence.
- Binding completeness and Vulkan compatibility validation use one ordered
  correspondence with linear work and unchanged diagnostics.
- Buffer and texture transitions construct only the active synchronization
  representation and meet the frozen bounded storage policy.
- Descriptor entry/value occupancy is maintained exactly by local mutations;
  production hot paths do not perform a global refresh traversal.
- Descriptor cache identity/policy, resource state/lifetime, mapped memory,
  swapchain behavior, queue topology, and public RHI semantics are unchanged.
- Focused tests, documentation validation, full build, and validation-enabled
  Editor smoke pass before the plan is marked Completed.

## Deferred Follow-ups

- Replacing the maintenance-unavailable queue-idle fallback requires a separate
  swapchain maintenance/present-wait plan with measured multi-window evidence.
- Barrier coalescing or render-graph synthesis requires a separate correctness
  and scheduling design; this plan only removes duplicate native lowering.
- A reusable small-vector or frame allocator requires multiple proven consumers
  and is not justified by this isolated optimization alone.
- Further descriptor cache capacity or eviction tuning requires workload traces
  and remains separate from eliminating accounting rescans.

## Related Documentation

- [Graphics State and Bindings](../../../Runtime/Rendering/GraphicsStateAndBindings.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Resource Views and Transfers](../../../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Vulkan Memory and GPU Completion](../../../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
- [RHI Diagnostics and Conformance](../../../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [RHI and Vulkan Correctness Hardening Plan](RHIAndVulkanCorrectnessHardening.md)
- [Native Tests](../../../Development/Build/NativeTests.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
