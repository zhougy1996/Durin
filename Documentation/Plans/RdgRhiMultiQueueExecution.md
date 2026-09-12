# RDG and RHI Multi-Queue Execution Plan

Summary: Introduce explicit GPU queue submission, completion, and resource retirement contracts across RDG, RHI, and Vulkan, then enable asynchronous compute, split barriers, and safe transient aliasing.

Last reviewed: 2026-09-12

Status: Active
Completed:

## Current Status

Stage 0's ownership audit is recorded below; its performance measurement is
deferred by explicit operator instruction. Stage 1's single-queue correctness
gates passed in `24ba95e5e`. Stage 2 now includes execution-local physical
transition preparation and recorded RHI wait/signal submission scopes with
Vulkan lowering. Production RDG recording still uses graphics;
explicit RHI scopes can route to a provisioned independent compute queue with
native timeline waits. Shared RHI ownership-transfer commands now retain paired
resource barriers and route them to graphics or diagnostic compute contexts.
Descriptor batch leases are retained by recording and in-flight payloads.
Transfer ranges likewise retain allocation leases through both CPU access and
payload lifetime; queue tickets only locate producers for bounded pressure.
Frame reuse and mapped uniform producers now wait all frame queue prefixes.
The UE-informed submission refactor separates context sealing from native
submission and gives each payload its own completion identity.
The coordinator now seals participating contexts into one batch and validates
explicit dependencies plus queue reservation order before native submission.
The preflight now checks each physical timeline's complete pending prefix,
including reservations owned outside the batch, before submitting any queue.
This passed 85 RHI command/completion tests, 95 Vulkan integration tests, and
the default-profile `all` build. Logs use prefixes `20260912-161903-912771-31192`,
`20260912-161958-030568-26628`, and `20260912-162016-244967-22584`, respectively.
The native regression rejects a missing middle reservation before either queue
accepts work; the omitted recording remains pending and batch-owned recordings
are canceled. This does not change the remaining production-routing gates.
Native command-buffer replacement now restores pipeline, vertex/index and
push-constant bindings before subsequent work.
Nested diagnostic labels likewise close and reopen across internal native
submissions without changing public command-list flush admission.
Logical submission receipts and ownership releases now use the recording
payload's exact ticket when other contexts reserve later work on the same queue.
The interleaved-context regression and all 95 Vulkan integration cases passed
in `20260912-161503-859640-25188-VulkanRHIIntegrationTests.log`; ownership
readback covers same-family and dedicated queues with both barrier lowerings.
Cloud-scene and resource-reload Vulkan cases also passed (log prefixes
`20260912-161539-903609-7200` and `20260912-161548-292378-28592`).
GPU timing results now resolve through that payload's physical queue completion;
the query manager no longer polls a graphics-only submission token.
No production async compute,
split-barrier or transient-aliasing acceptance gate is complete.

Resource handoffs now carry the latest prior range use on every logical queue,
with cross-queue producer-to-consumer execution edges. The existing range
traversal supplies these endpoints without a second resource analysis. Initial
transitions have no graph producer; final transitions retain the used range's
queue frontier. Equal-access queue changes now emit logical ownership handoffs,
including the first move from graphics and the final return of async ranges.
Handoffs and transition captures retain their source queue. Physical paired
lowering now creates release/acquire objects during preparation, removes the
corresponding ordinary barriers, and records producer-tail releases and
consumer-prologue acquires. Initially graphics-owned ranges use an explicit
release preamble. Allocators default to rejecting async reuse through
`SupportsAsyncCompute()`. Renderer now opts in by retaining execution-local
allocation retirement proofs: only successful completion of the graph's explicit
terminal join permits pool reuse or ordinary pressure eviction. Independent
graphs recorded before completion receive different physical allocations.
Unpublished and unsuccessful recordings remain quarantined until pool release.
Pressure eviction snapshots the selected entries so completion advancing during
compaction cannot invalidate retained-byte accounting. Delayed native completion
and unpublished-proof pressure coverage now pass. Native submission-failure
coverage and production eligibility remain outstanding.
Pool integration validation passed 166 RenderContractTests, 54 Renderer scene
contract tests, two Renderer resource reload Vulkan tests, 97 Vulkan integration
tests, and the default-profile `all` build. The resource pool fixture checks
pending graph isolation and reuse after the terminal join on both compute
topologies. Logs use prefixes `20260912-171713-793785-10304`,
`20260912-171604-570484-28888`, `20260912-171630-515157-27984`,
`20260912-171524-615687-32880`, and `20260912-171641-908639-22852`.
Native logs contain no VUID errors, failed cases, or skipped cases.
The pool fixture additionally throws during an async pass callback, then records
and completes later graphs. The failed graph's allocation remains unavailable
even after the partial command prefix has drained; subsequent successful graphs
reuse their own completed allocation. Both compute topologies passed in
`20260912-171914-312619-35796-RendererResourceReloadVulkanTests.log` (2/2 tests,
no VUID errors or skips). This covers recording failure, not native submission
failure or memory-pressure recovery.
Native delayed-pool validation now gates compute with a host-signaled timeline
semaphore. The graph's submitted terminal join times out while compute is held;
another graph selects different storage. After release and successful completion,
the old allocation is reused. Shared-family and dedicated-family configurations
both passed in inline and threaded replay modes. The production pool now rejects
allocation when outstanding uses prevent sufficient eviction under its 640 MiB
structural ceiling. A CPU fixture covers mixed texture/buffer quarantine,
eviction of eligible entries, exact retained-byte accounting, and pool-release
recovery without allocating VRAM.

- RendererSceneContractTests: 55/55 passed, log
  `20260912-172721-670184-35124-RendererSceneContractTests.log`.
- RendererResourceReloadVulkanTests: 2/2 passed with all four topology/replay
  combinations, log `20260912-172800-384867-32772-RendererResourceReloadVulkanTests.log`.
- VulkanRHIIntegrationTests: 97/97 passed with `--output full`, log
  `20260912-173050-520076-28320-VulkanRHIIntegrationTests.log`. Two preceding compact
  runs ended with access violation `0xc0000005` at different lifecycle positions
  (logs `20260912-172847-067008-26520` and `20260912-172940-337485-30012`).
  The failure is not localized; the full-output pass does not establish that the
  intermittent lifecycle crash is fixed. Keep this stability gap open.
- The successful native logs contain no VUID errors, failed cases, or skips.
  The default-profile `all` build passed, log `20260912-173138-001701-1560-cmake.log`.

Physical lowering validation passed 166 RenderContractTests, 97 Vulkan
integration tests, 54 RendererSceneContractTests and the default-profile `all`
build. Logs use prefixes `20260912-165633-415673-19372`,
`20260912-165741-544178-4920`, `20260912-165905-317760-10396`, and
`20260912-165920-070746-19392`. Native fixtures verify graph receipts on
compute/graphics/compute/graphics, pending state after CPU-only replay, and
exact texture bytes after the final join on both topologies and replay modes.
That fixture provides barrier/ownership coverage. Actual shader dispatch is now
covered by `PublicComputePipelineWritesBufferAndImageInlineAndThreaded`: three
RDG compute passes write a storage buffer and image, followed by graphics
sampling and exact byte comparisons. It exercises default, shared-family and
distinct-family queues in inline and threaded modes. All 97 Vulkan integration
tests passed in `20260912-170405-940301-26104-VulkanRHIIntegrationTests.log`,
without VUID, failure or skip diagnostics. Native single-queue fallback with
enabled RDG policy is included. Allocator reuse and production renderer
eligibility remain to be fully qualified.
Queue-only transition validation passed 166 RenderContractTests, 97 Vulkan
integration tests, 54 RendererSceneContractTests and the default-profile `all`
build. The corresponding log prefixes are `20260912-165057-079070-19020`,
`20260912-165131-865382-36896`, `20260912-165218-331114-19032`, and
`20260912-165229-592812-1088`. The new fixture checks equal-access ownership
changes at both graph boundaries and their absence with async policy disabled.
Producer-endpoint validation passed 165 RenderContractTests, 97 Vulkan
integration tests, 54 RendererSceneContractTests and the default-profile `all`
build. Logs have prefixes `20260912-164455-370805-15500`,
`20260912-164538-716658-28840`, `20260912-164624-192229-19132`, and
`20260912-164631-243073-35444`. Coverage includes the latest reader on both
logical queues and final texture endpoints after subresource culling.

RDG now accepts building-only compute-pass async eligibility and a separate
opt-in scheduling policy. Logical batches preserve FIFO per queue and join both
terminal prefixes at the epilogue; independent branches no longer gain global
consecutive-pass edges when the policy is enabled. Physical RDG recording uses
independent queues when topology and allocator safety permit. The earlier 164 RenderContractTests passed
with deterministic/culling/policy/invalid-declaration coverage in
`20260912-163721-089031-3084-RenderContractTests.log`.
Integration validation also passed 97 Vulkan tests, 54 Renderer scene contract
tests and the default-profile `all` build (log prefixes
`20260912-163828-124302-23496`, `20260912-163915-826307-31116`, and
`20260912-163931-989195-21804`).

RHI executor replay now resolves explicit submission contexts by physical queue,
routes both pipeline and operation commands within the scope, and propagates
the recording-storage lease to every selected context. Inline and threaded
contract coverage verifies routing, default-context restoration and independent
storage retention. Vulkan now resolves provisioned physical contexts and seals
multi-queue scopes into coordinator-owned pending payloads. Submission drains
these together with recording contexts; cancellation and shutdown release them
before command pools are destroyed. Diagnostic/timing intervals must close at
physical queue changes. Provisioned independent Vulkan queues now advertise
that capability; RDG additionally checks graph policy and allocator reuse safety.
Renderer pool integration and production pass eligibility remain outstanding.
Native scope routing passed 97 Vulkan integration tests, including public
graphics/compute/graphics ownership round trips on same-family and dedicated
queues in both inline and threaded replay. The receipts remain pending after
CPU-only replay, then exact readback completes the joined queue chain. Queued
storage retention, retirement and cancellation also passed. Final native log:
`20260912-163332-704069-25824-VulkanRHIIntegrationTests.log`. Cloud-scene and
resource-reload tests passed with prefixes `20260912-163248-211934-25872` and
`20260912-163300-976384-25960`; the final `all` build passed with prefix
`20260912-163354-779196-25660`. These tests do not yet qualify RDG async
dispatch, cross-graph pool reuse or performance.
Validation passed: 86 `RHICommandListTests`, 12 `RHIThreadTests`, 162
`RenderContractTests`, 95 `VulkanRHIIntegrationTests`, and the default-profile
`all` build. Reproducible logs are under `Build/.agent-state/logs/` with prefixes
`20260912-162432-600165-33160`, `20260912-162604-794085-28088`,
`20260912-162617-814259-24548`, `20260912-162528-535593-10368`, and
`20260912-162638-208789-25288`, respectively.

On 2026-09-11 the operator authorized continuing implementation and deferring
performance measurements because an exclusive quiet GPU lane is unavailable.
This explicitly relaxes the Stage 0 baseline-before-Stage-1 ordering only.
Implementation may proceed; concurrent-machine timings remain diagnostic and
the final performance gate remains outstanding. Use the recorded pre-change
revision for the eventual baseline, with the same instrumentation applied to
both revisions. Do not claim frozen budgets or complete the performance gate
without authoritative measurements.

Fresh correctness evidence at `3f541aa0a98c468be7226881be6c83a399db9d8d`, using
the default `Win64-Debug-DurinEditor` profile:

- `RenderContractTests`: 160/160 passed (3530 ms test total).
- `RendererSceneContractTests`: 54/54 passed (1993 ms test total).
- `VulkanRHIIntegrationTests
  FVulkanResourceTransitionTests.RenderGraphTransitionsReplayThroughVulkanStateTracking`:
  1/1 passed (400 ms test total), with Khronos validation loaded.

These totals are correctness-run durations, not compile/prepare/submit or GPU
performance measurements. Logs are in `Build/.agent-state/logs/`, with run
prefixes `20260911-163841`, `20260911-164005`, and `20260911-163755`, respectively.
No shared API changed, so an `all` build is not claimed for this audit.

The logical barrier foundation landed in commit `7d5812720`:
`FRDGBufferTransition` and `FRDGTextureTransition` carry resource IDs and
transition descriptions together; `FRDGBarrierBatch` is shared by passes and
the final epilogue. `RecordBarrierBatch` resolves physical backings without
mutating the compiled plan. This foundation passed the default-profile `all`
build, 160 RenderContractTests, 54 RendererSceneContractTests, and the Vulkan
graph-transition replay case. Those results do not qualify the future work
in this plan.

The current executor's command-batch serial tracks CPU replay completion.
Vulkan separately tracks submitted GPU work through completion tokens and
submission fences. The existing graph and backend path still rely on a
single-queue execution model. Extending this requires coordinated scheduling,
submission, synchronization, and lifetime changes.

## Goal

Support explicit multi-queue execution while preserving deterministic RDG
compilation, correct resource lifetimes, and the existing single-queue path.
Keep logical scheduling independent of physical resource preparation and
backend synchronization objects. Establish completion and retirement semantics
before enabling asynchronous execution.

## Scope and Selected Decisions

- Cover RenderCore, RHI, VulkanRHI, Renderer allocation/reuse, and their native
  tests. Migrate consumers across every project in `Durin.dworkspace` whenever
  shared APIs change.
- Separate pass domain, pipeline stages, and physical queue identity. A compute
  pass may execute on a graphics queue. Async eligibility and scheduling policy
  must not imply hardware queue availability.
- Preserve the current barrier records as logical data. Extend them or add a
  separate resource-handoff record for queue synchronization; never restore
  parallel transition/resource arrays or place mutable physical pointers in
  the compiled plan.
- Represent a GPU completion point with device generation, queue identity, and
  a queue-local monotonically increasing value. CPU command-batch completion,
  logical graph positions, and GPU completion points must be distinct types.
- Represent retirement prerequisites as a set of queue completion points,
  retaining the maximum value only within the same queue and device generation.
  A larger value on another queue is not evidence of completion.
- Allow an explicit join to produce an aggregate completion point after waiting
  for all inputs. Do not impose a global join on every resource or submission.
  Any later removal of redundant prerequisites requires proven dependency
  coverage, not a numeric comparison across queues.
- Submission batches own all referenced resources until ownership transfers
  safely to backend in-flight work. Commands recorded but not yet submitted
  also retain their resources. Include views, descriptors, backing allocations,
  upload storage, and command allocators in the ownership audit.
- Resource destruction or CPU overwrite requires actual completion of relevant
  GPU uses. Planned transient aliasing instead requires a proven execution
  order and the necessary backend barriers; it does not require a CPU wait.
- Queue waits/signals, resource access/layout transitions, ownership transfers,
  and split-barrier begin/end are separate concepts with an explicit protocol.
  Backends may lower a logical handoff differently according to capabilities.
- Keep synchronous fallback available when independent compute queues or split
  barriers are unavailable. Supported logical behavior must not depend on a
  particular device's queue topology.
- Treat graph extraction as publication of a retained resource reference, not
  proof that its GPU contents are complete. Cross-graph consumers must inherit
  or explicitly consume the required readiness dependencies.

Multi-adapter scheduling and additional graphics backends are outside this
plan. Queue identities must accommodate multiple queues of the same class,
but initial production scheduling targets graphics and compute. A dedicated
copy queue remains disabled until its ownership and transfer cases are covered.
UE provides architectural references, not source or API compatibility targets.

## Implementation Stages

### Stage 0: Freeze Queue, Completion, and Ownership Contracts

Dependencies: none beyond the existing logical barrier foundation.

Outcome: a reviewed ownership and submission contract with an exact migration
inventory and named validation gates, before implementation changes behavior.

- [x] Trace RDG preparation/recording, RHI replay, Vulkan submission, resource
  release, descriptor recycling, upload storage, and Renderer allocator reuse.
  Identify which lifetime decisions currently depend on implicit queue order.
- [x] Specify queue capability discovery and physical queue identity, including
  graphics/compute sharing one underlying queue and distinct queue families.
- [x] Specify GPU completion point creation, publication, polling, ownership,
  device-generation validation, and shutdown behavior. Determine how points
  reserved before submission distinguish pending, submitted, failed, and
  canceled work without advancing a completion watermark falsely.
- [x] Specify transaction boundaries for submission rejection and partial graph
  submission: already-submitted work stays alive until completion; unsubmitted
  work releases references only when it can no longer execute. Device loss
  follows an explicit teardown path rather than normal completion.
- [x] Select where retirement metadata lives: resource/allocation state,
  in-flight batch ownership, or a documented combination. Avoid competing
  authorities and per-resource hardware fences.
- [x] Define cross-graph extraction/import readiness, including external work,
  and distinguish resource object reuse from backing-memory aliasing.
- [ ] Record concrete API signatures, backend capability requirements, test
  selections, and a representative performance baseline in this plan before
  beginning Stage 1. Freeze acceptable regression thresholds from measurements.

Completion: each lifetime-sensitive consumer has an identified owner and
completion authority; unresolved protocol decisions are closed and recorded.

### Stage 0 Contract Decisions and Migration Inventory

The following are selected implementation contracts, not descriptions of
already implemented multi-queue behavior. All source paths in the table are
relative to `Engine/Source/Runtime/`.

| Consumer and current evidence | Migration owner and completion authority |
| --- | --- |
| `RenderCore/Private/RDG.cpp`, `FRDGBuilder::Record`: validates all allocated backings before callbacks, resolves logical barriers once, publishes retained extraction references after recording | RDG owns immutable batch/dependency/handoff records and a separate prepared-resource table. Prepare every retained backing and transition object before graph commands can submit. Preserve this all-or-nothing preparation boundary. |
| `RHI/Private/RHICommandList.cpp`: typed commands retain resources and copied bytes; executor calls `Group.ReleaseBatches()` after replay/events | RHI batches retain recorded resources before submission. Transfer a deduplicated resource-use bundle to the active backend payload before releasing replay storage, including commands replayed without `SubmitToGPU`. CPU serial completion remains a CPU-only authority. |
| `VulkanRHI/Private/VulkanContext.cpp`, `GetPayload`/`Finalize`; `VulkanQueue.cpp`, `SubmitPayloads` | Context owns unsubmitted payloads. Queue submission transfers payload ownership to a per-physical-queue tracker only after native acceptance. Preallocate the tracker entry and all ownership storage before the native call so an allocation exception cannot orphan accepted GPU work. |
| `VulkanRHI/Private/VulkanCompletion.cpp`: one contiguous scalar watermark and fence deque | Each physical queue owns an independent timeline and in-flight deque. Only observed completion of that queue advances its successful GPU progress. Reservation/cancellation state is separate from GPU progress. |
| `VulkanRHI/Private/VulkanDevice.cpp`, `FDeferredDeletionQueue::EnqueueResource`: snapshots device-wide `GetLastReservedToken()` | Replace the implicit last-global-token authority with retained payload ownership and exact use prerequisites. Move native dependent handles/allocations into deletion records before destroying their wrapper; records also retain prerequisites for external native work. Never create a hardware fence per resource. |
| `VulkanRHI/Private/VulkanView.cpp`, `VulkanPendingState.cpp`, `VulkanDescriptorSets.cpp`: cached views/descriptors and frame pool maxima | Retain each consumed view and its backing, shader/pipeline dependencies, descriptor snapshot and pool lease in the payload use bundle. Pool reset requires all using queues to finish and no unsubmitted leases; cache eviction alone grants no reset permission. |
| `VulkanRHI/Private/VulkanTransferArena.cpp`; `VulkanTexture.cpp` readback | A transfer range belongs to its recording payload until submitted or irrevocably canceled. Reclaim only its completed/canceled uses; readback requires successful completion, then invalidate and copy. Capacity waits target the required producer, never another queue's numeric maximum. |
| `VulkanRHI/Private/VulkanBuffer.cpp`: dynamic uniform/storage producer states and chunk maxima | Each mapped chunk has a use set and a pending producer lease. CPU overwrite requires every use to retire; stamp all consuming payloads, including intermediate submissions, rather than only the final graphics/frame token. |
| `VulkanRHI/Private/VulkanCommandBuffer.cpp`, `VulkanSubmission.cpp`, `VulkanGPUTiming.cpp` | Payload retains command allocator/buffer, synchronization objects, transition objects and timing-query storage through completion. Frame pacing carries all frame terminal points. Queue-qualified timestamp results report queue-local intervals; cross-queue numeric timestamps are not assumed comparable. |
| `Renderer/Private/Renderers/RendererRDGAllocator.cpp`, `PlanCandidate`: selects a compatible pool entry without a GPU readiness check | Pool entries retain resource objects plus readiness/use metadata. Before subsequent GPU reuse, import the earlier uses into the new graph's first-use dependencies, or select completed storage. Eviction releases a reference; backend retirement remains the only native destruction authority. |
| `RenderCore/Private/RenderResource.cpp` and rendering shutdown | Rendering-thread deferred C++ cleanup protects non-owning render-command pointers. Keep this CPU contract distinct; backend payload retention protects later GPU use. Close producers before draining either authority. |
| `VulkanRHI/Private/VulkanViewport.cpp` | Keep WSI presentation completion and semaphore ownership separate. A graphics submission fence does not alone prove presentation has released its semaphore or image. |

The workspace migration search covered `Engine/Source`, `Engine/Tests`,
`Sandbox/Source`, `Sandbox/Tests`, `RoadWeaver/Source`, and `RoadWeaver/Tests`,
as declared by `Durin.dworkspace` and its three project files. The search for
`FRHICommandListFence`, `GetCompletedSerial`, `GetLastSubmittedSerial`,
`FRDGExecutionContext`, `QueueTextureExtraction`, `QueueBufferExtraction`,
`FRDGAllocator`, `RHISubmitCommands`, and `RHICollectCompletedResources` found
25 consuming files, all in Engine. Repeat the cross-project symbol search for
each actual API migration; this snapshot does not exempt later consumers.

#### Queue Identity and Backend Requirements

Use `FRHIQueueId { uint32 Index; }` as a device-local physical queue identity,
independent of `ERDGPassType` and pipeline-stage masks. Capabilities publish an
immutable `FRHIQueueInfo` array containing identity, supported command classes,
queue-family ownership domain and timestamp support. Graphics and compute
roles resolving to the same native queue must publish the same ID and tracker.
Multiple queues in one family receive distinct IDs. Native family indices are
capability data, never graph-stable identity.

Stage 1 maps all roles to graphics. Stage 3 prefers a dedicated compute family,
then another compute-capable queue in the graphics family, then the graphics
queue itself. Diagnostic policy can force each supported topology. Dedicated
copy scheduling remains disabled. Vulkan creates one command pool/context per
selected physical queue, serializes host access on the RHI thread, and uses
per-queue timeline semaphores for dependency fan-out when supported. Devices
without the selected synchronization capability retain single-queue execution.
Fence polling remains sufficient for the Stage 1 path.

`vulkaninfo` on 2026-09-11 reported NVIDIA GeForce GTX 1060 6GB, Vulkan 1.4.312:
family 0 has 16 graphics/compute/transfer queues, family 2 has eight
compute/transfer queues, and family 1 has two transfer queues. Both same-family
and distinct-family compute integration are therefore candidates on this host;
enumeration is not execution evidence. The default backend creates only
family 0, queue 0; Stage 3 diagnostic provisioning can also create family 0,
queue 1 or family 2, queue 0. Startup also loaded `VK_LAYER_OBS_HOOK`; that observation
alone neither proves nor disproves an active capture workload.

Queue semaphore waits/signals establish execution dependencies. Resource
barriers separately establish access/layout visibility. Exclusive resources
crossing families require paired release/acquire barriers with identical
family indices and exact ranges, ordered by the semaphore dependency. Shared-
family handoffs omit ownership transfer but retain memory synchronization.
Concurrent readers on different exclusive families must be serialized or use
an explicitly concurrent-sharing allocation; do not give two queues exclusive
ownership simultaneously.

#### Completion, Failure, and Ownership Protocol

`FRHIGPUCompletionPoint { uint64 DeviceGeneration; FRHIQueueId Queue;
uint64 Value; }` is a non-owning identifier. Values are nonzero and increase only
within one physical queue. `FRHICommandBatchSerial` and `FRDGSubmissionId` are
separate wrappers without implicit conversion to this type. Device generations
are allocated monotonically by RHI across backend replacement, not reset by a
Vulkan device constructor.

An owning `FRHIGPUSubmissionTicket` retains the state of a reserved point.
Its states are `Pending`, `Submitted`, `Complete`, `Canceled`, `Failed`, and
`DeviceLost`. Polling a foreign/expired identifier yields `Invalid`; it must
never yield `Complete`. Metadata needed by a live ticket survives deque
compaction without retaining the hardware fence after completion. This avoids
an unbounded device-wide history of dead reservations.

Reservations are ordered per queue. A later reservation cannot submit while an
earlier one is still pending; it may proceed after that earlier reservation is
irrevocably canceled. A canceled reservation is never signaled as GPU work.
The query for that ticket stays `Canceled` even if a later signal has a larger
value. Explicit terminal reservation state lets retirement traverse canceled
holes without calling them successful completion. Submission rejection before
ownership transfer leaves the caller's finished batch intact and pending;
cancellation is allowed only after removal from every executable path.

`FRHIRetirementPrerequisites` retains owning tickets and keeps the maximum use
only within the same queue/generation, using the enforced reservation order.
Its retirement predicate accepts successful completion or irrevocable
cancellation, not unknown points, pending work, failure, or device loss.
Publication/readiness additionally tracks producer success: replacing a lower
canceled producer with a later completed point must not make its output valid.
Do not compress success dependencies as though they were retirement uses.
An explicit join submits a real wait-for-all batch and returns its signal;
cross-queue prerequisite elimination requires that recorded coverage proof.

The command batch owns references before backend replay; the unsubmitted
payload owns them after replay; the in-flight tracker owns them after native
acceptance. The resource/allocation use set coordinates future readiness and
recycling, but cannot shorten any of those ownership leases. Descriptor pools,
upload chunks and other shared storage retain every outstanding use. View
retention includes the exact backing generation, not just a mutable wrapper.

For a partial graph submission, accepted payloads stay in-flight. Stop new
admission, detach and cancel the unsubmitted suffix, and publish no successful
graph extraction. A known pre-submit rejection may release detached storage;
an ambiguous native failure must quarantine it. Device loss closes the device
generation and enters a separate teardown path: stop CPU producers/replay,
detach all work, destroy device-owned objects in dependency order, then release
quarantined host owners. Never advance completion to infinity to force cleanup.
Ordinary shutdown instead submits accepted pending work, waits every submitted
queue, then destroys contexts, pools, deferred handles and the device. Already
retained tickets remain terminal and cannot address a replacement device.

#### Concrete API Boundaries

The selected public signatures for implementation are:

```cpp
// FDynamicRHI: immutable capabilities and thread-safe state observation.
auto RHIGetQueueCapabilities() const -> const FRHIQueueCapabilities&;
auto RHIGetCompletionStatus(const FRHIGPUSubmissionTicket& Ticket) const
    -> ERHIGPUSubmissionState;
auto RHIWaitForCompletion(const FRHIGPUSubmissionTicket& Ticket,
    uint64 TimeoutNanoseconds) -> ERHIGPUWaitResult;

// FRHICommandListImmediate: recorded, balanced batch boundaries.
auto BeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc)
    -> FRHIGPUSubmissionTicket;
auto EndGPUSubmission() -> void;

// FRHIResource: counted readiness snapshot, separate from logical access.
auto GetGPUReadiness() const -> FRHIResourceReadiness;

// FRDGBuilder: explicit readiness for externally produced resources.
auto RegisterExternalTexture(FTextureRHIRef Texture, std::string Name,
    ERHIAccess Initial, ERHIAccess Final, FRHIResourceReadiness Readiness)
    -> FRDGTextureHandle;
auto RegisterExternalBuffer(FBufferRHIRef Buffer, std::string Name,
    ERHIAccess Initial, ERHIAccess Final, FRHIResourceReadiness Readiness)
    -> FRDGBufferHandle;
```

`FRHIGPUSubmissionDesc` owns queue identity, dependency tickets and resource-use
bundles; no borrowed spans survive recording. Begin reserves a ticket without
calling a native queue. End seals a balanced batch; ordinary executor dispatch
performs replay/submission. Admission rejects cycles, foreign generations,
illegal context commands and open render-pass/batch scopes. No CPU wait is
inserted between passes. Nonblocking status observes published backend state;
native polling runs on the RHI thread. A bounded wait reports timeout,
cancellation, failure or device loss distinctly and cannot block the RHI thread
waiting for its own pending replay. Allowed wait purposes remain readback,
bounded allocator pressure, frame pacing and shutdown.

Existing external-registration overloads inherit the resource's recorded
readiness. Extraction retains the resource and its producing tickets; it
publishes a reference after successful recording, not completed contents.
Failure of a producing ticket invalidates content readiness. External native
work must enter through an owned RHI submission ticket with an explicit
resource-use bundle; untracked foreign queue access is unsupported. Raw native
integration hooks must finish or import their work before returning resources
to ordinary RDG use.

Pool object reuse inherits all prior uses before a new first use. Graph-local
backing aliasing is a later, separate optimization: every terminal prior user
must reach the acquiring batch through the submission DAG, followed by an
aliasing handoff. Extracted/live external allocations are ineligible. Heap
destruction and mapped-memory CPU overwrite still require observed retirement.
Split barriers likewise add retained begin/end transition objects to handoff
records; Vulkan event-based lowering is restricted to legal same-queue scopes.
Cross-queue handoffs use semaphore plus release/acquire synchronization. Lack
of event support lowers to full barriers without changing the dependency DAG.

#### Validation Selections and Pending Performance Baseline

Registry discovery confirmed `RHICommandListTests`, `RHIThreadTests`,
`RHIResourceTransitionValidationTests`, `RHIResourceViewValidationTests`,
`RHITransferValidationTests`, `RenderContractTests`,
`RendererSceneContractTests`, and `VulkanRHIIntegrationTests` as existing
targets. Add CPU completion fixtures to the owning RHI target and independent
queue scheduling/lifetime fixtures to RenderContractTests; use the existing
Vulkan integration target for actual shared-family and distinct-family work.
The new cases must cover canceled holes, pending retention without submission,
out-of-order observations, stale generations, failure after native acceptance,
partial graph submission, extraction producer failure, fan-out readers and
descriptor/upload reuse under delayed compute completion. Run affected tests
and the mandatory shared-API `all` build after implementation changes.

As authorized above, measurement is deferred until an exclusive quiet lane is
available. Instrument and measure these representative workloads on both the
recorded unchanged single-queue revision and the implementation revision:

1. CPU RDG: the existing 128-pass same-buffer hazard chain, a graphics/compute
   fork/join graph, and a production frame graph. Record compile, prepare,
   recording and executor replay/submission separately; exclude authoring,
   shader creation, lazy capture and readback waits from those phase samples.
2. GPU production: the registered `GBufferQualificationTests` workload
   `FGBufferQualificationTests.StaticAndSplinePassMeetsFrozenRTX3090TimingAndMemoryGates`
   provides GBuffer, contact compute and full frame routes. Its RTX 3090 budgets
   are not GTX 1060 budgets. Preserve the original qualification and collect
   this plan's adapter-specific baseline separately.
3. GPU dependencies: a matched independent graphics/compute fork/join fixture
   with deterministic readback, forced single-queue output oracle and both
   physical topology overrides. Measure elapsed graph GPU cost and actual
   overlap using supported timestamp semantics, not summed queue-local times.

For the added measurement fixtures use 10 warm-up iterations followed by 100
samples in each of three consecutive runs; preserve any longer warm-up already
required by an existing qualification. Record median/p95, adapter/driver,
profile, validation configuration and source revision. Also record peak
transient/retained bytes, retirement backlog and count/duration/purpose of waits.
Freeze numerical regression budgets from the baseline revision's results, with
explicit justification for any noise allowance, before accepting performance.
Keep topology, resolution and warm-up identical for later full/split and
single/multi-queue comparisons. No numerical thresholds have been frozen yet;
the Stage 0 measurement checkbox intentionally remains open.

### Stage 1: Introduce GPU Completion and Retirement on One Queue

Dependencies: Stage 0.

Outcome: explicit GPU completion and resource retirement contracts operate
through the existing single-queue execution path.

- [x] Introduce typed queue identities, GPU completion points, and retirement
  prerequisite sets in RHI; keep CPU replay fences distinct.
- [x] Adapt Vulkan completion tracking to queue-qualified points and preserve
  submission fence and payload ownership through GPU completion.
- [x] Integrate pending-command retention, backend in-flight retention, resource
  deletion, descriptor/upload recycling, and allocation eviction with the
  selected ownership contract.
- [x] Provide nonblocking completion polling and bounded-purpose waits. Keep
  ordinary reclamation free of device-idle calls and per-resource GPU waits.
- [x] Test delayed and out-of-order completion observations, unsubmitted work,
  failed/canceled submissions, stale device generations, and shutdown.

Completion: early CPU replay completion cannot release GPU-used storage;
single-queue behavior remains equivalent, and affected tests plus the shared
Engine API `all` build pass.

Stage 1 uses `RHICompletion.h` for queue-qualified tickets and retirement
prefixes. The old independent Vulkan watermark was removed. Queue capabilities,
metadata observation and bounded exact waits are exposed through `FDynamicRHI`.
Vulkan tracker publication now has prepare/commit boundaries; native failure
quarantines payload and fence ownership until device teardown. Pending and
submitted command storage retains RHI references, and native deletion entries
carry owning retirement prerequisites. Existing descriptor/upload/frame pool
values remain explicitly local to the sole graphics queue; their multi-queue
use-set migration belongs to Stage 3.

Implementation refinement: this first retention boundary shares the complete
immutable command-storage vector with payloads, rather than introducing a
second per-command resource enumerator. This preserves views, copied uploads,
pipeline dependencies and opaque command-owned references across intermediate
submissions. It conservatively retains CPU payload bytes until GPU retirement.
Measure this cost at the deferred baseline gate before deciding whether a
deduplicated resource-only bundle is justified. CPU admission budget counters
still describe queued/active replay work, not total in-flight GPU storage.

Fresh Stage 1 validation on `Win64-Debug-DurinEditor`:

- `@domain=renderer,kind=contract`: all 10 registered targets passed, including
  eight RHI completion cases and pending backend-storage retention. Log:
  `Build/.agent-state/logs/20260911-170600-692797-36260-ctest.log`.
- `VulkanRHIIntegrationTests`: 74/74 passed, including real pending/submitted
  ticket observation, bounded completion wait and rejection of an old ticket
  after backend replacement. Log:
  `Build/.agent-state/logs/20260911-170859-097833-11376-VulkanRHIIntegrationTests.log`.
- Cross-project searches covered Engine, Sandbox and RoadWeaver source/test
  roots for the changed context, completion and queue capability symbols.
- Default-profile `all` build passed in 15.02 seconds. Log:
  `Build/.agent-state/logs/20260911-170925-957453-32116-cmake.log`.
- Native failure/cancellation state-machine tests are deterministic CPU
  fixtures; physical device-loss injection has not been performed.

### Stage 2: Compile and Submit an Explicit Execution Plan

Dependencies: Stage 1.

Outcome: RDG emits explicit submission batches and synchronization edges,
initially mapped to one physical queue.

- [x] Introduce execution-plan records for pass assignment, submission batches,
  dependencies, resource handoffs, and prologue/epilogue barrier locations.
- [x] Separate logical barrier planning, physical transition preparation, and
  submission. Preserve immutable plans and one resource-resolution boundary.
- [x] Lower graph submission dependencies to RHI wait/signal operations without
  inserting CPU waits between passes.
- [x] Extend captures with logical queue assignment, batch IDs, and dependency
  reasons. Keep dumps deterministic and exclude device addresses, native fence
  values, and measured timing from stable identity.
- [x] Preserve culling, pass callbacks, extraction publication, failure outcomes,
  budgets, and exact range/access/discard behavior on the single-queue mapping.

Completion: single-queue replay oracles and production graph captures remain
equivalent; preparation failure emits no graph work, and partial submission
retains all already-submitted resources correctly.

Initial Stage 2 checkpoint (`6cea0167a`): `FRDGExecutionPlan` owns typed graph-local batch IDs,
scheduled pass intervals, dependency causes, shared-queue FIFO edges and exact
logical barrier locations. Each retained pass and the graph epilogue are
explicit batches; an empty graph has none. Recording traverses this immutable
plan after preparation, and captures/dumps preserve its owning pointer-free
data. The existing backend command stream is unchanged at this checkpoint;
these records alone are not an implementation of RHI queue waits/signals.

That checkpoint identified the need to handle recorded-but-not-replayed graph
dependencies without a synchronous RHI-thread round trip per pass. Native
queue points currently reserve during backend replay, so a recorded submission
receipt must distinguish an unresolved CPU recording from a published native
ticket. Preserve ownership across intermediate submissions caused by uploads
or synchronous callbacks, and cancel abandoned recordings without claiming
completion of any already-submitted prefix.
Batch lowering must also preserve diagnostic-region and GPU-timing scopes
that surround graph execution; native command-buffer finalization requires
balanced diagnostic regions, even when a logical region spans several batches.

Checkpoint validation on `Win64-Debug-DurinEditor`:

- `RenderContractTests`: 162/162 passed, including compact IDs after culling,
  plan equality during/after recording, empty graphs and exact handoff lookup.
  Log: `Build/.agent-state/logs/20260911-171744-394510-24316-RenderContractTests.log`.
- `RendererSceneContractTests`: 54/54 passed. Log:
  `Build/.agent-state/logs/20260911-172012-525870-37556-RendererSceneContractTests.log`.
- Vulkan graph-transition replay: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-172118-221077-31720-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-172141-392832-33572-RendererResourceReloadVulkanTests.log`.
- Shared Engine API `all` build passed in 11.62 seconds. Log:
  `Build/.agent-state/logs/20260911-172204-072530-29888-cmake.log`.

These results qualify the initial logical-recording checkpoint only.

The subsequent implementation resolves every physical barrier before graph
recording and lowers batch dependencies to owning RHI submission receipts.
Receipts receive native queue tickets during replay, without a synchronous
RHI-thread round trip per pass. Single-queue Vulkan validates dependencies and
coalesces logical signals onto payload tickets; FIFO execution plus resource
barriers supplies same-queue synchronization. Existing native submission
boundaries, diagnostic scopes and GPU-timing scopes remain intact.
Recorded boundary commands share a cancellation lease across CPU replay
segments and early native retirement. Preparation failure emits no graph
submission; a callback failure after native submission retains the submitted
prefix and leaves extraction unpublished. Runtime receipts remain outside
capture identity and do not assert graph success.

Stage 2 correctness validation on `Win64-Debug-DurinEditor`:

- The 10 `@domain=renderer,kind=contract` targets passed, including 82
  RHICommandListTests and existing graph/capture equivalence oracles. Log:
  `Build/.agent-state/logs/20260911-173857-693166-19132-ctest.log`.
- `VulkanRHIIntegrationTests`: 77/77 passed, including same-queue coalescing,
  an open scope spanning native retirement, graph receipt publication,
  preparation rejection and submitted-prefix failure retention. Log:
  `Build/.agent-state/logs/20260911-173841-005978-37068-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-173934-959668-34912-RendererResourceReloadVulkanTests.log`.
- Shared API consumers were searched across Engine, Sandbox and RoadWeaver;
  the required `all` build passed in 22.87 seconds. Log:
  `Build/.agent-state/logs/20260911-174012-415340-28356-cmake.log`.

Timing baselines remain deferred by operator instruction. Stage 2 does not
qualify independent-queue execution, which belongs to Stage 3.

### Stage 3: Enable Async Compute with Multi-Queue Lifetime Safety

Dependencies: Stage 2. Automatic production async scheduling remains disabled
until all gates in this stage pass together. Explicit diagnostic queue
provisioning and graph policy may exercise validated paths while completing
these gates; allocators must independently opt into multi-queue reuse.

Outcome: eligible compute work executes on a separate queue with correct
dependencies, fallback behavior, and resource retirement.

- [x] Add explicit async eligibility and deterministic queue-assignment policy,
  including graphics-queue fallback and a diagnostic override for comparisons.
- [x] Implement Vulkan cross-queue waits/signals and required resource ownership
  transfers for both shared-family and distinct-family configurations.
- [ ] Track every using queue in retirement prerequisites. Cover concurrent
  reads, write/read handoffs, compute-only final uses, and graph boundaries.
- [x] Integrate Renderer resource pools and reuse: require completed uses or
  explicitly scheduled dependencies before reuse; never assume frame number,
  pass index, or graphics completion covers outstanding compute work.
- [ ] Test fork/join, fan-in/fan-out, multiple readers, empty batches, culling,
  external resources, extraction/readiness, submission failure, and delayed
  compute completion. Validate that the submission graph is acyclic.
- [x] Run Vulkan integration on an independent compute queue, inspect validation
  output, and compare rendered/read-back results with single-queue execution.
  Record unavailable queue-family coverage as outstanding, not passing.

Completion: cross-queue correctness and reclamation tests pass, single-queue
fallback passes, shared API builds pass, and enabled production wiring has an
identified integration test.

Stage 3 infrastructure checkpoint: physical Vulkan queues now own distinct
completion trackers, using one device-owned generation and explicit queue IDs
and native queue indices. Submission validates payload queue ownership;
completion waits route by the ticket's queue identity and reject foreign
timeline authority even when numeric coordinates match. Device polling,
teardown and conservative native-deletion prerequisites enumerate physical
queues, while graphics-only pool and arena compatibility consumers remain
explicitly identified for migration. Initialization-failure cleanup handles a
queue created before the GPU timing manager. This checkpoint still creates
only the graphics queue and does not satisfy async execution acceptance.

`VulkanRHIIntegrationTests`: 78/78 passed, including initialization-failure
injection, foreign queue authority rejection and the prior submission/lifetime
cases. Log:
`Build/.agent-state/logs/20260911-174822-319269-26236-VulkanRHIIntegrationTests.log`.
The `all` build passed in 3.22 seconds. Log:
`Build/.agent-state/logs/20260911-174930-144387-24768-cmake.log`.

Native synchronization checkpoint: candidate evaluation gates timeline
semaphores on the feature plus Vulkan 1.2 or its Vulkan 1.1 extension. The
diagnostic `DURIN_VULKAN_COMPUTE_QUEUE` override selects `same-family`,
`dedicated`, `auto`, or `disabled` (the current default). Automatic selection
prefers a compute-only family, then the second queue in the graphics family;
unavailable requested topologies fall back to graphics. Production async
capability remains disabled pending the remaining lifetime/ownership gates.
Each physical queue owns a timeline semaphore. Native submission validates
every producer ticket before deduplicating same-queue waits, includes binary
WSI semaphore values correctly, and signals the queue-local ticket value.

The native test holds compute behind a host-signaled timeline gate, submits a
graphics consumer through the production queue submission path, proves that
the consumer and retirement prerequisites remain blocked, then releases the
gate and observes both queue completions. This ran successfully on GTX 1060
with `(family,index)` pairs graphics `(0,0)` / compute `(0,1)` and graphics
`(0,0)` / compute `(2,0)`. These empty-payload tests prove native queue ordering,
not resource ownership transfers, shader output equivalence or production RDG
async execution; those remain outstanding.

- `VulkanRHIIntegrationTests`: 82/82 passed, no skipped tests or validation VUID
  errors in `Build/.agent-state/logs/20260911-175730-938659-18580-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-175814-938207-36356-RendererResourceReloadVulkanTests.log`.
- `all` build passed in 1.07 seconds. Log:
  `Build/.agent-state/logs/20260911-175848-274536-36600-cmake.log`.

Ownership-state checkpoint: Vulkan now tracks exclusive byte/subresource
ownership independently of access/layout state. Buffer ranges and texture
aspect/mip/layer sets can be released under an identity; only an exact matching
source/destination/range acquire restores access. Pending ranges reject normal
validation even for discard. Disjoint ranges and out-of-order independent
acquires remain valid. Multi-range operations validate before mutation and
restore metadata on allocation failure. Ordinary transitions check the whole
batch before native recording and claim ownership after it succeeds. This
supplies the state machine for the next native paired-barrier implementation;
it does not yet emit ownership transfers or enable RDG async scheduling.

- `VulkanRHIIntegrationTests`: 88/88 passed, including six ownership tests for
  exact pairing, pending discard rejection, independent intervals, texture
  subresource sets, invalid ranges and transactional rejection. Log:
  `Build/.agent-state/logs/20260911-181200-679494-8088-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-181240-372191-37424-RendererResourceReloadVulkanTests.log`.
- `all` build passed in 1.01 seconds. Log:
  `Build/.agent-state/logs/20260911-181243-003460-24528-cmake.log`.

Native ownership checkpoint: `FVulkanQueueTransfer` retains all participating
resources, captures source scopes/layouts once, emits paired release/acquire
barriers, and commits prepared ownership/access state only after recording.
Context payloads own the pair through completion; acquire adds the accepted
producer ticket as a native timeline wait. Distinct-family image pairs use
identical old/new layouts and queue indices; shared-family pairs change layout
once and omit family ownership indices. Both synchronization2 and legacy paths
use the saved portable access mappings. This is a backend protocol checkpoint;
shared RHI command admission, production RDG routing, and resource-pool use-set
migration remain outstanding, so Stage 3 is not complete.

The native integration writes a buffer and one texture mip on graphics,
transfers selected ranges to compute, copies both into mapped readback storage,
and verifies exact bytes after compute completion. It also checks that
unselected ranges stay unchanged and payloads retain/release the pair at the
correct observed completion boundaries. All four combinations of same-family
and dedicated-family queues with synchronization2 and legacy ownership
barriers passed on the GTX 1060. These transfer-command tests do not establish
production asynchronous shader scheduling or final performance acceptance.

- `VulkanRHIIntegrationTests`: 92/92 passed, no skips or validation VUID errors.
  Log: `Build/.agent-state/logs/20260911-182309-004085-31124-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-182345-179695-33248-RendererResourceReloadVulkanTests.log`.
- `all` build passed in 0.94 seconds. Log:
  `Build/.agent-state/logs/20260911-182427-644350-32928-cmake.log`.

Shared RHI ownership checkpoint: metadata-only `RHICreateQueueTransfer` creates
an owning release/acquire handle. Typed commands retain that handle through
recording, reject render-pass placement and mismatched active submission queues,
and route Vulkan replay to device-owned physical queue contexts. GPU submission
drains pending graphics and compute contexts. Acquire still requires explicit
producer submission; production GPU scope routing, RDG async assignment and
resource-pool use-set migration remain outstanding.

- Ten renderer contract targets passed, including command order and discarded
  recording ownership tests. Log:
  `Build/.agent-state/logs/20260911-183700-615210-24948-ctest.log`.
- `VulkanRHIIntegrationTests`: 94/94 passed, including public RHI texture
  ownership round trips through same-family and dedicated-family compute.
  No skips or validation VUID errors. Log:
  `Build/.agent-state/logs/20260911-183823-424953-35636-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`: 1/1 passed. Log:
  `Build/.agent-state/logs/20260911-184134-478986-31560-RendererResourceReloadVulkanTests.log`.
- Shared API consumer search covered all workspace source/test roots; `all`
  build passed in 16.04 seconds. Log:
  `Build/.agent-state/logs/20260911-184143-298600-35688-cmake.log`.

Descriptor retirement checkpoint: every native graphics/compute descriptor
bind records the physical queue's ticket, including snapshot cache hits. Pool
batch reuse requires all recorded queue prefixes; bounded pressure selects by
CPU retirement order and waits queue-qualified tickets. Frame retirement no
longer assigns graphics completion to the pool, and both provisioned contexts
clear their descriptor snapshots at frame start. Mapped allocation and frame
reuse migrations remain outstanding before production async compute.

- `VulkanRHIIntegrationTests`: 94/94 passed. The same-family and dedicated-family
  native signal-gate fixtures now allocate descriptor pools, complete independent
  graphics work, verify compute still prevents batch reuse, then verify reuse
  after both queue prefixes complete. Log:
  `Build/.agent-state/logs/20260911-184835-225889-28804-VulkanRHIIntegrationTests.log`.
- `RendererResourceReloadVulkanTests`, `VolumetricCloudSceneVulkanTests`, and
  `VolumetricCloudVulkanTests`: each passed 1/1. Logs:
  `Build/.agent-state/logs/20260911-184942-617920-18448-RendererResourceReloadVulkanTests.log`,
  `Build/.agent-state/logs/20260911-185152-720150-26348-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260911-185208-563874-36940-VolumetricCloudVulkanTests.log`.
  The scene test exposed stale transition totals from before logical barrier
  recording preserved all three shadow layers. Its totals now include those
  three entry barriers, with separate assertions covering each layer.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260911-185231-275819-34652-cmake.log`.

Frame and mapped-uniform checkpoint: frame retirement now captures queue
prefixes after submitting both provisioned contexts. Frame reuse waits their
conjunction before the render thread resets its mapped storage producer.
Uniform producer pages conservatively inherit the frame's queue prefixes;
bounded allocation pressure selects a producer by CPU retirement order and
waits queue-qualified tickets. This preserves frame ownership and existing
capacity bounds; it does not introduce waits between passes. Transfer arena,
GPU timing, resource readiness and Renderer pool migrations remain outstanding.

- `VulkanRHIIntegrationTests`: 94/94 passed. Same-family and dedicated-family
  signal-gate fixtures now verify uniform allocation selects another backing
  while compute is pending despite completed graphics work, then reuses the
  original backing and offset after completion. Log:
  `Build/.agent-state/logs/20260912-143224-952399-31196-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1; native logs
  contain no validation VUID errors or skipped tests. Logs:
  `Build/.agent-state/logs/20260912-143318-726172-30232-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-143345-166309-10888-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-143348-597656-18972-cmake.log`.

Submission-boundary refactor (operator requested on 2026-09-12): local UE 5.8
`VulkanContext.cpp`, `VulkanSubmission.h/.cpp`, `VulkanQueue.cpp`, and
`RenderGraphBuilder.cpp` informed the responsibility boundaries. Contexts now
seal and detach unique payloads; one RHI-thread coordinator handles native
submission for flush, frame end, presentation, readback and transfer pressure.
No new submission thread is introduced. Payloads retain timing query references
and their own reserved tickets. Completion preparation uses the payload ticket,
not the queue's newest reservation. Unsubmitted destruction cancels the ticket
and resets command buffers; native failure quarantine remains separate.

The previously uncommitted transfer migration is incorporated at this boundary:
ranges carry queue-qualified tickets and pressure requests producer submission
through the coordinator. Native pages have physical-queue affinity; only fully
free pages can be replaced for another queue under the existing capacity bound.
Retirement selection uses CPU order rather than cross-queue token comparisons.
This is not the final pool-ownership migration: descriptor/transfer leases,
query completion, dependency-ready pending submissions and RDG queue routing
still require follow-up before enabling production async compute.

- `VulkanRHIIntegrationTests`: 95/95 passed. The new native boundary test proves
  sealing alone does not submit, an earlier sealed payload submits with its own
  ticket after a later reservation, and discarded storage releases its owners
  while canceling its ticket. Log:
  `Build/.agent-state/logs/20260912-145522-918568-28800-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-145615-304966-37300-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-145619-862354-37224-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-145645-717022-28200-cmake.log`.

Descriptor ownership refactor: descriptor batches now expose allocation leases
retained by every binding payload, including cache hits. Frame retirement drops
the active allocator owner. Batch reset depends on lease expiration rather than
an allocator-owned ticket set; completion and unsubmitted discard release the
payload owners, while failure quarantine retains them. Bounded pressure asks
the submission coordinator to find the actual owning submissions and wait their
queue-qualified tickets. Compatibility token diagnostics are derived from that
same ownership lookup. The two-batch capacity and frame-local cache policy are
preserved. Transfer leases and queue-aware query completion remain pending.

- `VulkanRHIIntegrationTests`: 95/95 passed, including same-family and
  dedicated-family payload retention under delayed compute, coordinator pressure
  waits, and pool return after unsubmitted payload discard. Log:
  `Build/.agent-state/logs/20260912-150416-994101-24788-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-150511-509377-33056-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-150524-154187-18472-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-150527-141313-19336-cmake.log`.

Timing ownership refactor: sealed payloads retain ended intervals through their
physical queue's completion. The query manager now owns slots, conversion and
statistics without a separate pending-query list or graphics completion token.
Intervals capture the recording queue's timestamp width; unsubmitted payload
discard invalidates them and allows recording again. Native failure quarantine
retains query ownership until device shutdown. Production RDG queue routing and
the remaining Stage 3 acceptance gates are still outstanding.

- `VulkanRHIIntegrationTests`: 95/95 passed. Delayed compute fixtures prove
  completed independent graphics work does not resolve compute queries, then
  observe ready results after compute completion. Both family 0 and family 2
  reported timestamp support; unsubmitted discard and re-record admission also
  passed. No validation VUID errors or skipped tests. Log:
  `Build/.agent-state/logs/20260912-152600-436077-23428-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-152856-654940-9424-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-152911-888778-37276-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-152924-622537-31940-cmake.log`.

Transfer ownership refactor: CPU ranges and recording payloads share allocation
leases. Destroying a range before native submission preserves its interval while
the payload owns it. Normal and oversize recycling observe lease expiration;
the arena no longer independently interprets ticket completion as permission to
reuse. Pressure uses the exact payload ticket to locate a pending producer, then
asks the coordinator to wait the actual allocation owners. Existing capacity,
queue affinity and CPU readback ownership are preserved.

- `VulkanRHIIntegrationTests`: 95/95 passed. Same-family and dedicated-family
  delayed compute fixtures now retain transfer storage after the CPU range is
  destroyed and independent graphics work completes, then reuse it after compute
  completion. A bounded arena additionally rejects reuse after GPU completion
  while a CPU lease remains. Existing upload pressure, oversize, fragmentation,
  readback and failure cases passed. No validation VUID errors or skips. Log:
  `Build/.agent-state/logs/20260912-153645-460040-36344-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-153715-369359-16804-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-153719-958172-32192-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-153722-895042-33968-cmake.log`.

Submission dependency checkpoint: the coordinator now seals all participating
contexts and validates a deterministic batch dependency order before native
submission. Explicit waits and implicit queue-local reservation order both
participate in cycle detection. Missing pending producers and unsuccessful or
foreign dependencies reject admission. Acquire replay can record against a
pending release in the same eventual batch. Native acceptance and failure
quarantine remain queue-owned; no persistent submission thread or scheduler was
added. RDG async assignment and GPU scope routing remain outstanding.

- `VulkanRHIIntegrationTests`: 95/95 passed. Both compute topologies validate
  reversed batch admission, explicit cross-queue cycles, implicit same-queue
  cycles, and missing producer cancellation. Native buffer/texture ownership
  tests now seal the consumer before its pending producer and verify exact bytes
  using both synchronization2 and legacy barriers on both topologies. No VUID
  errors or skipped tests. Log:
  `Build/.agent-state/logs/20260912-154304-788493-28856-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-154331-679985-35872-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-154336-142666-34524-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-154338-826253-19236-cmake.log`.

Command-buffer state checkpoint: native command-buffer replacement now restores
current graphics/compute pipelines, vertex/index buffers and retained push
constants. Constant updates retain original stage masks and latest-write order;
incompatible layouts and deleted current pipelines clear saved constants. Bound
vertex/index buffers now have explicit context ownership. This addresses a
pre-existing submission/allocator-pressure boundary hazard before adding RDG
queue routing; diagnostic/timing scope boundaries still need separate handling.

- `VulkanRHIIntegrationTests`: 95/95 passed. The public compute storage-buffer
  and storage-image test now submits after setting constants/descriptors and
  before dispatch, preserving exact results in inline and threaded modes.
  No validation VUID errors or skipped tests. Log:
  `Build/.agent-state/logs/20260912-155435-671779-25320-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-155507-116069-25164-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-155511-898457-28336-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-155514-711056-33100-cmake.log`.

Diagnostic boundary checkpoint: contexts retain owned logical region names,
close native labels before sealing, and reopen them in nesting order on the
next native command buffer. Explicit public command-list flush still requires
closed diagnostic/timing scopes. The test exercises the internal backend
submission boundary directly; it does not claim cross-queue timing scope support.

- `VulkanRHIIntegrationTests`: 95/95 passed, including exact nested label
  close/reopen event ordering on a native submission boundary. No VUID errors or
  skipped tests. Log:
  `Build/.agent-state/logs/20260912-155916-002545-29992-VulkanRHIIntegrationTests.log`.
- Cloud scene and resource reload integration each passed 1/1. Logs:
  `Build/.agent-state/logs/20260912-155938-146917-32912-VolumetricCloudSceneVulkanTests.log`,
  `Build/.agent-state/logs/20260912-155942-752810-19244-RendererResourceReloadVulkanTests.log`.
- `all` build passed. Log:
  `Build/.agent-state/logs/20260912-155945-595021-4872-cmake.log`.

### Stage 4: Add Split-Barrier Scheduling and Transition Preparation

Dependencies: Stage 3.

Outcome: supported handoffs can begin after producers and complete before
consumers without changing their synchronization or lifetime guarantees.

- [ ] Introduce begin/end batch relationships and explicit transition-object
  ownership. Define legal placement around render passes and merged passes.
- [ ] Specify backend split-barrier support independently from queue support;
  preserve a full-barrier lowering when the optimization is unavailable.
- [ ] Validate pairing, exact resource/range agreement, no intervening illegal
  accesses, and transition-object retention through its final backend use.
- [ ] Centralize transition creation after required backings are prepared;
  add a creation queue only where batching or parallel creation needs it.
- [ ] Compare full and split lowering with CPU plan tests and Vulkan validation
  and output-equivalence tests on supported hardware.

Completion: split lowering preserves results and synchronization; forced full
barriers remain a tested fallback, and performance stays within the Stage 0
baseline or has an explicitly reviewed adjustment.

### Stage 5: Add Dependency-Aware Transient Aliasing and Qualify the Model

Dependencies: Stage 4.

Outcome: transient memory may be reused across non-overlapping GPU lifetimes
without conflating logical lifetime positions with observed GPU completion.

- [ ] Model allocation acquire/discard positions using submission dependencies
  and queue intervals. Prove ordering across all prior users before aliasing;
  a scalar first/last pass interval is insufficient across independent queues.
- [ ] Generate aliasing handoffs through the same execution-plan machinery and
  reject reuse when lifetimes can overlap on any participating queue.
- [ ] Keep backing heap destruction and CPU-side recycling governed by actual
  GPU completion even when graph-local aliasing was planned in advance.
- [ ] Test overlapping async lifetimes, safe ordered reuse, multiple terminal
  readers, extracted resources, and independent graphs sharing allocator pools.
- [ ] Measure CPU compile/prepare/submit cost, GPU overlap, peak transient memory,
  retirement backlog, and waits on the representative workloads selected in
  Stage 0. Adopt dependency compression or joins only with measured benefit.
- [ ] Run final affected native tests, multi-queue Vulkan integration gates,
  single-queue fallback gates, and an `all` build across the workspace targets.
- [ ] Update the owning runtime contracts and record reproducible completion
  evidence before completing the plan.

Completion: correctness gates and frozen performance budgets pass; diagnostics
explain queue assignment, handoffs, aliasing decisions, and retirement waits.

## Validation and Handoff

Use [agent build guidance](../Agents/BuildAndRun.md) and
[agent testing guidance](../Agents/Testing.md) for execution and target selection.
Use registry discovery for new target selections rather than inferring target
names. CPU fixtures must model independent queue progress; GPU integration
must exercise actual supported queue configurations. Unsupported hardware is
an explicit coverage gap for the relevant multi-queue gate.

Each stage must update this plan's status and evidence in its implementation
commit. Use the exact plan path and stage title for the repository's `Plan`
and `Stage` commit trailers. Do not carry a passing result across changes to
its relevant code, configuration, or environment.

## Owning Code and References

- [RDG public records](../../Engine/Source/Runtime/RenderCore/Public/RDG.h)
  and [compiler/recording](../../Engine/Source/Runtime/RenderCore/Private/RDG.cpp).
- [RHI command submission](../../Engine/Source/Runtime/RHI/Public/RHICommandList.h)
  and [implementation](../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp).
- [Vulkan completion](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.h)
  and [submission payloads](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.h).
- [Render Graph contract](../Runtime/Rendering/RenderGraph.md),
  [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md),
  and [render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md).
- [UE barrier batches](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBarrierBatchBegin)
  provide reference boundaries for transition creation and submission.
- [UE transient allocation fences](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHITransientAllocationFences)
  describe graphics/async-compute lifetime overlap and acquire/discard ordering;
  these are not interchangeable with polled GPU-completion objects.
- [UE GPU fences](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHIGPUFence)
  and [RHI resource deletion](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIResource?lang=en-US)
  document separate completion and object-lifetime interfaces. Backend deletion
  details must be verified against a specific UE version before copying a design.
- [D3D12 multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
  illustrates explicit cross-queue ordering and fence timelines.
