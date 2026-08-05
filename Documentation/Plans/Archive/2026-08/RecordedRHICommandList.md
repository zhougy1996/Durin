# Recorded RHI Command List Plan

Summary: Replace the immediate RHI forwarding facade with UE-style immediate and regular recorders that seal owned command batches for inline replay and later RHI-thread transfer.

Last reviewed: 2026-08-05

Status: Archived
Completed: 2026-08-05

## Current Status

Stages 0 through 5 completed on 2026-08-05. The RHI now has typed arena
storage, movable finite `FRHICommandList` recorders, explicit
finish/admission states, ordered immediate segments, move-only submission
groups, inline replay, monotonic serials, and serial fences. The complete
graphics command surface records owned typed commands; executor replay alone
resolves its active context. Buffer and texture uploads now own source bytes and
retain destinations, command-list locks own balanced CPU staging scopes, and
resource initialization records backend work without exposing a context from
the recorder. Dynamic uniform allocation, texture readback, back-buffer
acquisition, and GPU-idle waits use declared executor synchronous operations;
CPU-only uniform suballocation does not split an active recorded render pass.
The transitional `FRHICommandListBase::GetContext` and
`RHIGetVkCommandBuffer(RHICmdList)` escape hatches have been removed. The one
remaining native command-buffer API is explicitly classified for Vulkan-only
backend integrations outside the portable command surface, currently the
compute integration test.

Every immediate flush type and submit flag now follows the Stage 0 mapping.
Submission groups replay before ordered GPU-submit and frame-end events, release
their retained batches before deferred deletion, and publish completion only
after the requested CPU-side work finishes. Flush waits use executor fences;
ordinary end-of-frame dispatch does not wait for device idle. Executor snapshots
expose cumulative command, payload, batch, submission, replay-duration, wait,
and rejection counters plus the current pending-batch count. Render-thread
shutdown verifies zero queued render commands, pending batches, incomplete
serials, and deferred RHI deletes.

Final validation passed 24 `RHICommandListTests`, 34 `RenderContractTests`, 59
`MaterialTests`, 47 `ViewportTests`, Vulkan upload/sampling and readback,
SkyBox graphics, and renderer-resource reload integration tests plus a full
`all` build. Three independent 60-tick hidden-window Vulkan sessions completed
normal startup, rendering, viewport present, render flush, module teardown,
render-thread drain, and `RHIExit` without validation errors, late work, or
rejected submissions.

Those sessions recorded 14,257-14,259 commands, 555,145-555,305 payload bytes,
122-124 batches, 129-131 submission groups, and 69.6-72.7 ms cumulative inline
replay. For this workload that is approximately 238 commands, 9.25 KB of owned
payload, 2.2 submission groups, and 1.18 ms of inline replay per tick. The
lasting behavior is now owned by
[RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md). This
completed plan is the required predecessor of
[Dedicated RHI Thread](DedicatedRHIThread.md) and an upstream dependency of the
compute-pipeline roadmap. The dedicated-thread plan has now consumed the same
immutable groups without changing their representation: threaded replay is the
normal runtime path, while explicit inline mode remains available for focused
diagnostics.

## Goal

Rendering code records backend-neutral RHI operations through a UE-style
command-list hierarchy: `FRHICommandListBase` owns the common recording
surface, regular `FRHICommandList` instances provide movable finite recording
units, and the executor-owned `FRHICommandListImmediate` owns the primary
timeline plus flush, synchronous-operation, and frame-boundary control.
Immediate does not mean direct backend execution. Both list kinds use the same
owned command representation and seal their storage into immutable batches for
deterministic executor replay against an `IRHICommandContext`.

The recording path does not obtain or call a backend context, retain borrowed
payload memory, or allow referenced RHI resources to die before replay
completion. Regular lists can be inserted at a defined point in the immediate
timeline without changing their command representation.

The same batch and executor contract must work in synchronous inline mode and
be transferable without redesign to the dedicated RHI thread plan.

## Scope

- Command storage, payload alignment, destruction, reset, and reuse.
- A regular `FRHICommandList` recording type, its finish/seal lifecycle, and
  ordered insertion into the immediate command timeline.
- An immutable submitted-batch type with monotonic submission/completion
  serials.
- Recording and replay for the current graphics, render-pass, viewport,
  binding, draw, push-constant, and shader-parameter commands.
- Owned upload payloads and explicit treatment of APIs that return data
  synchronously.
- Resource and viewport lifetime retention through replay completion.
- Ordered frame-end, GPU-submit, flush, and deferred-delete executor events.
- Inline replay, executor fences, diagnostics, and focused native/Vulkan
  validation.
- Removal of recording-time backend-context escape hatches used by current
  Vulkan upload, readback, lock, and storage-texture initialization paths.

## Non-Goals

- Starting or scheduling a dedicated RHI thread.
- Parallel command-list recording or submission from arbitrary worker threads.
- Queueing an unfinished regular list and waiting on an asynchronous recording
  prerequisite; the first implementation accepts only explicitly finished
  lists.
- GPU asynchronous compute, multiple GPU queues, or queue-family ownership
  transfer.
- A render graph, automatic resource-state inference, or the compute-specific
  transition vocabulary.
- Making resource creation generally asynchronous.
- A stable serialized command ABI across process runs or engine versions.
- Optimizing command packing before ownership and replay behavior is measured.

## Design Decisions and Invariants

### Recording and batch ownership

- Follow the established UE-style role split without copying UE implementation
  details: `FRHICommandListBase` contains the common recording surface,
  `FRHICommandList` is a movable finite recorder, and the process-wide
  `FRHICommandListImmediate` is the executor-owned primary recorder and the only
  list that exposes global flush, frame, present, and synchronous-result
  coordination.
- `FRHICommandBatch` is a Durin transfer abstraction, not an Unreal Engine API
  type. UE keeps finalized work in move-only command lists and executor submit
  state; Durin explicitly detaches immutable storage so the dedicated-RHI-thread
  plan receives a narrow ownership boundary. It remains private to the RHI
  module: renderer-facing code records and finishes command lists but never
  receives or submits a batch directly. Immediate and regular lists must
  produce the same batch representation.
- A command-list call records; it does not call `IRHICommandContext` or cache a
  context pointer.
- Submitting the immediate list detaches its current storage and immediately
  leaves it ready to continue recording. `FRHICommandList::FinishRecording()`
  returns `void`, validates the completed recording, changes the list from
  recording to finished, and does not queue, submit, or expose its internal
  storage. `FRHICommandListImmediate::QueueCommandList(FRHICommandList&&)`
  accepts only a finished list, inserts it at the current primary-timeline
  position, and internally detaches an immutable `FRHICommandBatch`; the moved
  source list can no longer be recorded or admitted. A batch is accepted exactly
  once and is destroyed only after replay or explicit rejected-submission
  cleanup.
- Command representation is private to the RHI module. Stage 0 may select
  typed arena nodes or an equivalent type-erased layout, but each command must
  have explicit execute and destroy behavior without relying on a borrowed
  lambda capture.
- Raw bytes, strings, spans, attachment arrays, and parameter arrays are copied
  into batch-owned storage. A caller may mutate or destroy its source storage
  immediately after the record call returns.
- Every referenced `FRHIResource` that can be released before execution is
  retained by a counted reference owned by the command or batch. Raw pointers
  are permitted only for objects whose stronger lifetime contract is stated and
  verified at the submission boundary.

### Replay and ordering

- Only executor replay resolves and calls an `IRHICommandContext`.
  `SwitchPipeline` records pipeline selection; replay resolves the matching
  context and rejects unsupported pipelines before executing dependent
  commands.
- Commands replay in record order. `QueueCommandList(std::move(CmdList))` places
  a finished regular list at that point in the primary timeline: commands
  recorded before the insertion replay before the regular batch, and commands
  recorded afterward replay after it. This may be represented by sealed
  immediate segments or equivalent ordered submission-group entries; it must
  not flatten all regular lists after the final immediate segment.
- For compatibility, regular lists passed only through the existing
  `Submit(AdditionalCmdLists, ...)` boundary are appended after the current
  immediate segment and replay in argument order. A list queued into the
  immediate timeline and a list supplied as an additional submit argument are
  mutually exclusive admission paths. Empty batches do not manufacture backend
  work.
- Render-pass balance, pipeline selection, lock pairing, double submission, and
  record-after-seal errors fail at the closest CPU boundary with command and
  submission context in the diagnostic.
- Inline mode is a real executor mode, not a direct-call shortcut. All runtime
  rendering passes through the same seal and replay code that threaded mode
  will consume.
- The first version has no bypass mode: both `FRHICommandListImmediate` and
  regular `FRHICommandList` always record the same typed representation. A
  future bypass optimization must preserve identical ordering, ownership,
  validation, and completion semantics and is outside this plan.

### Immediate-result operations

- `WriteBuffer`, `UpdateUniformBuffer`, texture updates, push constants, and
  shader parameters copy their source payload before returning and replay as
  ordered commands.
- Buffer `Lock`/`Unlock`, dynamic uniform allocation, texture readback, GPU-idle
  waits, and any backend creation call that must return a completed result use
  an explicit synchronous executor operation. The operation first seals and
  completes all commands recorded before it; in this plan it then runs inline.
  The dedicated-thread plan can marshal the same operation and wait on its
  completion serial.
- A returned mapped pointer belongs to one declared lock scope. Submission is
  rejected while the scope is incomplete, and `Unlock` transfers any staged
  upload ownership into an ordered command before the memory can be reused.
- Backend resource allocation may remain synchronous, but initialization work
  that needs a command buffer is recorded through a backend-neutral context
  operation. `FDynamicRHI` code must not recover a native command buffer from a
  recording command list.

### Submission, completion, and deletion

- The executor assigns a monotonic nonzero serial to each accepted submission
  group and publishes a monotonic completed serial only after every command and
  ordered executor event in that group has run.
- A fence targets a serial. Waiting uses the executor completion mechanism and
  never infers completion from an empty producer-side command list.
- `DispatchToRHIThread` seals and dispatches through the configured executor;
  inline mode completes before returning. `FlushRHIThread` additionally waits
  for the target serial. `WaitForOutstandingTasksOnly` does not submit RHI work.
- `SubmitToGPU`, `EndFrame`, and `DeleteResources` are ordered after the
  commands they accompany. Deferred RHI objects are not deleted until earlier
  batches have released their retained references.
- Failure to accept a batch leaves its ownership recoverable and reports the
  admission state; it must not leak command payloads or silently drop work.

## Current Foundations and Gaps

| Area | Existing foundation | Plan gap |
| --- | --- | --- |
| Public facade | `FRHICommandListBase` already centralizes common renderer calls. | Replace direct forwarding with owned recording and remove stored contexts. |
| List roles | The executor owns one `FRHICommandListImmediate`, and `Submit` already names additional lists. | Add constructible regular lists, shared storage, finish/seal states, and ordered immediate-timeline insertion. |
| Backend context | `IRHICommandContext` defines the graphics replay surface. | Make it executor-only and add context operations needed by uploads/readback. |
| Executor | One process-wide immediate list and submit entry point exist. | Implement sealing, ordering, inline replay, serials, fences, and additional lists. |
| Payloads | Current callers provide ordinary RHI descriptors, spans, and data pointers. | Define deep-copy and retained-resource rules per command. |
| Uploads | Buffer and texture upload paths already encode Vulkan work. | Stop extracting a native command buffer during recording and own upload bytes until replay. |
| Lifecycle | Render commands and deferred RHI deletion already have ordered shutdown expectations. | Tie deletion and frame events to completed command batches. |
| Tests | RenderCore and Vulkan RHI tests exercise resources and real draw paths. | Add fake-context replay tests plus inline-executor integration coverage. |

## Stage 0 Contract

### Public roles and recorder states

`FRHICommandListBase` is non-copyable and owns the common recording state and
commands. It is not directly constructible outside the hierarchy.
`FRHICommandList` is a public, movable finite recorder. Its state machine is:

`Recording -> Finished -> Admitted`, with `Recording -> Empty` available only
through an explicit reset before finish. `FinishRecording()` returns `void` and
validates pipeline, render-pass, and lock balance before changing state. It
does not detach storage or contact the executor. Recording and a second finish
are rejected in `Finished`; queue/submit requires `Finished`, atomically
detaches the private batch, and leaves the source `Admitted`. Reset and reuse
of `Finished` or `Admitted` lists are deliberately unsupported in the first
version; callers construct another list. Move construction/assignment transfer
the exact state and storage and leave the source `MovedFrom`, for which only
destruction and move-assignment are legal.

`FRHICommandListImmediate` is process-wide, executor-owned, non-movable, and
permanently recording while the executor accepts work. It adds coordination,
synchronous-result, and regular-list insertion APIs. Sealing detaches the
current segment and immediately installs empty recording storage. Queueing a
finished regular list first seals any nonempty immediate segment, then detaches
and appends the regular batch, so a later immediate command becomes a later
segment. Renderer-facing code never sees `FRHICommandBatch`.

### API execution classification

| API surface | Class | Ownership and execution rule |
| --- | --- | --- |
| `SwitchPipeline`; render-pass, viewport, PSO, vertex/index binding, viewport/scissor, push-constant, shader-parameter, and draw calls | Common recorded command | Store owned values/references in either list; only replay resolves a context. |
| `FRHICommandList::FinishRecording` | Regular-list state transition | Validate and seal in place; no detach, queue, replay, serial, or backend call. |
| `FRHICommandListImmediate::QueueCommandList` | Immediate coordination | Consume one finished rvalue once at the current primary-timeline position. |
| `ImmediateFlush` | Immediate coordination | Apply the flush mapping below to current timeline entries and ordered submit events. |
| `WriteBuffer`, `UpdateUniformBuffer`, `FDynamicRHI::RHIUpdateTexture2D` | Recorded upload | Copy source bytes at the call, retain destination, replay through a context operation. |
| `LockBuffer` / `UnlockBuffer` | Immediate synchronous lock scope | Flush earlier work before lock; return executor-owned staging/native mapping; unlock records the ordered transfer and closes the scope. Submission with an open scope is rejected. |
| `AllocateDynamicUniformBuffer` | Immediate synchronous executor operation | Flush earlier work, allocate through the backend, copy input before returning, and return the completed range. A future producer-side reservation is not implied. |
| `RHIReadTexture2D`, `RHIBlockUntilGPUIdle` | Synchronous executor operation | Flush and complete prior commands, run on the executor/backend, then return the completed result. |
| `RHICreateTexture`, `RHICreateBuffer` | Synchronous creation plus recorded initialization | Resource identity/allocation returns synchronously. Descriptors are consumed during the call; initial bytes are copied into a subsequent recorded upload/initialization command. No native command buffer is exposed. |
| `RHIEndFrame_RenderThread` | Immediate coordination | Admit the current timeline with ordered `EndFrame` and requested cleanup events. |
| `RHIGetDefaultContext` | Executor-only backend query | Production backend adapter may call it during replay; command lists and renderer code may not. |
| `FRHICommandListBase::GetContext`, Vulkan `RHIGetVkCommandBuffer(RHICmdList)`, and command-list-based context casts | Prohibited escape hatch | Remove them after all upload/readback/viewport initialization users have context operations or synchronous executor routes. |

The inventory of command-list-taking `FDynamicRHI` entry points is complete at
Stage 0: `RHIEndFrame_RenderThread`, `RHICreateTexture`, `RHICreateBuffer`,
`RHIAllocateDynamicUniformBuffer`, `RHILockBuffer`, `RHIUnlockBuffer`,
`RHIUpdateTexture2D`, and `RHIReadTexture2D`. Backend-neutral creation/query
methods without a command-list parameter remain synchronous device APIs unless
a later plan explicitly reclassifies them.

### Recorded command ownership matrix

| Command | Copied fields and retained objects | Replay state and failure |
| --- | --- | --- |
| Switch pipeline | `ERHIPipeline` | Resolve context at replay; reject unsupported pipeline. `None` clears selection. |
| Begin/end render pass | Deep-copy `FRHIRenderPassInfo`, attachment values, and `FName`; retain every attachment texture | Begin requires graphics pipeline and no active pass; end requires active pass. |
| Begin/end drawing viewport | Retain viewport and optional render-target texture; copy present/vsync booleans | Graphics context required; begin/end must balance for the same viewport. |
| Set graphics PSO | Retain PSO | Graphics pipeline and active render pass required. |
| Bind vertex/index buffer | Retain buffer; copy stream, offset | Graphics pipeline required; bounds/backend validation occurs at replay. |
| Set viewport/scissor | Copy all scalar values | Graphics pipeline required; reject invalid dimensions/ranges at record time where backend-independent. |
| Push constants | Copy flags, offset, size, and exactly `size` bytes into batch storage | Graphics pipeline required; null data with nonzero size and invalid ranges fail at record time. |
| Set shader parameters | Retain shader; deep-copy parameter array and scalar fields; retain every non-null parameter resource | Graphics pipeline required; binding type/range validation remains visible with command index. |
| Draw indexed | Copy count, start, and vertex offset | Graphics pipeline, active pass, PSO, and required bindings must be active. |
| Write/update buffer | Retain destination; copy offset, size, and bytes | Open write lock conflicts and destination range errors fail before admission/replay. |
| Update texture 2D | Retain texture; copy mip/slice/region/pitch and the addressed source rows into tightly owned bytes | Validate region/pitch/source and usage before record; replay performs layout/copy transitions through context. |

Resource references use `TRefCountPtr` (or an equivalent intrusive owning
wrapper) inside typed nodes. A raw pointer may cross replay only when the type
is not an `FRHIResource` and the stronger owner is itself retained; viewport
commands therefore retain the viewport rather than relying on swapchain or
frame lifetime. All failures include list state and command index; after a
serial exists they also include that serial.

### Internal storage and immutable batches

The selected representation is a private typed arena. Each node begins with a
small header containing `Next`, `Execute`, and `Destroy` function pointers,
followed by one concrete command object. Allocation rounds each node to its
requested alignment; node types with alignment greater than the arena block
alignment are a compile-time error. Arena blocks are allocated with at least
`alignof(std::max_align_t)`, never relocated, and linked in allocation order.
Variable-size bytes and arrays are copied into the same arena with their own
alignment and referenced by offsets/spans that become immutable on detach.

Stage 1 adds UE-style `EnqueueLambda` to the common recording surface. The
callable object is stored and destroyed as a typed arena node, enabling storage
and ordering tests without exposing `FRHICommandBatch` or inventing a fake
graphics command. Captures are owned by the callable; a reference capture is
legal only when its lifetime is explicitly guaranteed through the targeted
completion fence. Typed RHI commands remain the default for the graphics and
upload surfaces because their resource and payload ownership can be enforced
by construction.

Arena allocation failure is fail-fast and leaves no partially linked command.
Destruction walks nodes in record order, calls each `Destroy` exactly once,
then frees all blocks. Reset performs the same destruction before returning to
empty recording state. Moving a recorder or batch transfers block ownership,
head/tail, counters, and validation state in O(1), and empties the source.
`FRHICommandBatch` is move-only and has private `Mutable`, `Submitted`, and
`Consumed` states; only a detached `Mutable` batch may enter a submission
group, admission changes it to `Submitted`, replay changes it to `Consumed`,
and rejection destroys or returns the still-mutable owner without leaking.

### Submission groups, serials, and flush mapping

The immediate timeline is an ordered vector of sealed immediate segments and
detached regular batches. `QueueCommandList` seals a preceding nonempty segment
before adding the regular batch. `Submit(AdditionalCmdLists, Flags)` seals the
current segment, then validates and consumes each pointed-to finished regular
list in argument order. The compatibility parameter becomes a span/vector of
`FRHICommandList*`, not base lists; all entries are prevalidated before any are
detached. A list previously queued is already `Admitted` and is rejected.

Admission moves the timeline and ordered events into one immutable submission
group and assigns the next nonzero `uint64` serial. Empty work with no ordered
event returns the last submitted serial and creates neither a group nor backend
work. Serial exhaustion is fatal rather than wrapping. Inline execution replays
every entry, then runs events in this fixed order: `SubmitToGPU`, `EndFrame`,
batch destruction/reference release, and `DeleteResources`. The completed
serial is published only after all requested events finish. A fence snapshots
a submitted serial; waiting observes `CompletedSerial >= TargetSerial`.

| Flush/flag | Exact first-version behavior |
| --- | --- |
| `WaitForOutstandingTasksOnly` | Do not seal or submit RHI work. Wait only for producer prerequisites; it is a no-op while the first version has none. |
| `DispatchToRHIThread` | Seal/admit the current timeline and events. Inline mode replays to completion before return; threaded mode may return after admission. |
| `FlushRHIThread` | Dispatch, snapshot the resulting/latest serial, and wait for that serial. |
| `FlushRHIThreadFlushResources` | Behave as `FlushRHIThread` and add `DeleteResources` after batch references are released. |
| `SubmitToGPU` | Add one backend GPU-submit event after command replay; it does not imply CPU/GPU idle. |
| `DeleteResources` | Drain deferred deletes after replay and batch reference release. |
| `FlushRHIThread` submit flag | Force the same serial wait as flush type `FlushRHIThread`; it does not add a backend command. |
| `EndFrame` | Add one backend `RHIEndFrame` event after replay (and after `SubmitToGPU` when both are present). |

### Test executor seam

The executor depends on a private `IRHICommandListBackend` supplied at
construction. Production adapts `GDynamicRHI`; tests supply a fake with
`ResolveContext(ERHIPipeline)`, `SubmitToGPU`, `EndFrame`, and
`DeleteResources`. The fake context implements `IRHICommandContext` and logs
operation values, retained-resource destruction, serial/event order, and the
calling thread ID. This seam creates no Vulkan resource and is the only test
hook needed for Stage 1 storage/state tests and Stage 2 command replay tests.

## Implementation Stages

### Stage 0: Command and ownership contract

- [x] Inventory every `FRHICommandListBase`, `FRHICommandListImmediate`, and
  command-list-taking `FDynamicRHI` entry point.
- [x] Specify the `FRHICommandListBase` -> `FRHICommandList` ->
  `FRHICommandListImmediate` role hierarchy, including which operations are
  common recording calls and which are immediate-only coordination calls.
- [x] Classify each entry point as recorded, synchronous executor operation,
  producer-safe query/reservation, or prohibited backend escape hatch.
- [x] Record for every command its copied fields, retained objects, replay
  context, valid pipeline/render-pass state, and failure condition.
- [x] Select the internal command-node/arena representation and state the
  alignment, destructor, move, reset, and out-of-memory behavior.
- [x] Define regular-list construction, `FinishRecording`/seal, move, reset,
  single-admission, and destruction states. `FinishRecording()` must be a
  `void` state transition with no implicit queue or submit; the first-version
  queue API must consume a finished list by rvalue and extract its private batch
  internally. Define this insertion separately from append-at-submit
  compatibility.
- [x] Define submission-group order, serial allocation, fence targeting, and
  the exact mapping of existing flush types and submit flags.
- [x] Add a test context/executor seam that can observe replayed operations and
  calling thread without creating Vulkan objects.

#### Acceptance Gate

- Every current command-list or command-list-taking RHI API has one ownership
  and execution classification; no raw payload, native-context access, or
  immediate-result method is left to implicit behavior.
- Immediate and regular list roles, legal state transitions, and insertion
  ordering are explicit; `Immediate` denotes the primary submission authority,
  not a direct-context execution mode.
- The selected batch contract can be moved to another thread without changing
  public renderer call sites or retaining recording-thread stack memory.

### Stage 1: Owned command batches and inline executor

Dependencies: Stage 0.

- [x] Implement command storage with typed execute/destroy behavior and
  batch-owned auxiliary payload memory.
- [x] Add the regular `FRHICommandList` type and implement its finite
  record/finish/move/reset lifecycle using the same storage as the immediate
  list. `FinishRecording()` validates and seals the list without detaching its
  storage or performing submission work.
- [x] Implement immediate seal/detach/reset so it can begin a new segment
  without mutating an already submitted batch.
- [x] Implement ordered submission-group entries so a finished regular list can
  be consumed by `QueueCommandList(FRHICommandList&&)`, converted internally to
  a private batch, and admitted once at an immediate-timeline insertion point;
  preserve the defined append order for `AdditionalCmdLists`.
- [x] Add immutable submission groups, monotonic submission/completion serials,
  and a serial-targeted fence in inline mode.
- [x] Make empty, rejected, moved, replayed, destroyed, and reused batch states
  explicit and assertion-covered.
- [x] Verify command destructors release retained resources and payloads once
  on success and once on rejected-submission cleanup.

#### Acceptance Gate

- Unit tests prove that immediate and regular batches preserve order, survive
  source-memory mutation, retain resources until completion, reject recording
  after finish, queue-before-finish, and double admission, release all owned
  state, and replay through the inline executor. `FinishRecording()` alone must
  not queue or execute work. An `A -> regular P -> B` insertion test must
  observe `A, P, B`, not `A, B, P`.

### Stage 2: Record and replay the graphics command surface

Dependencies: Stage 1.

- [x] Convert pipeline switch, render-pass, viewport, graphics PSO, vertex/index
  binding, viewport/scissor, shader-parameter, push-constant, and draw calls to
  typed recorded commands.
- [x] Deep-copy render-pass and shader-parameter data and retain every resource,
  shader, PSO, texture, buffer, and viewport required during replay.
- [x] Resolve graphics contexts only inside replay and remove the recorded
  graphics surface's dependence on `FRHICommandListBase::GetContext`; retain
  that transitional escape hatch only for the Stage 3 upload/readback users.
- [x] Preserve validation for active pipeline and render-pass ordering with
  diagnostics that identify the offending command.
- [x] Implement and test both immediate-timeline insertion and
  append-at-submit additional-list replay order.

#### Acceptance Gate

- A fake context observes the same ordered graphics operations and values after
  caller-owned descriptors and arrays have gone out of scope.
- Existing graphics frames execute through seal and inline replay with no
  direct context call from the recording path.

### Stage 3: Uploads, readback, and synchronous-result boundaries

Dependencies: Stage 2.

- [x] Record buffer writes, uniform updates, and texture updates with owned
  source bytes and retained destination resources.
- [x] Replace Vulkan uses of `RHIGetVkCommandBuffer(RHICmdList)` and recording-
  time `GetContext()` in upload, readback, lock, and storage-texture
  initialization paths with replayable context operations or declared
  synchronous executor operations.
- [x] Implement balanced lock-scope ownership and prevent submission or memory
  reuse before `Unlock` transfers staged work.
- [x] Route dynamic uniform allocation, texture readback, GPU-idle waits, and
  other immediate results through the inline synchronous-operation contract.
- [x] Ensure resource create descriptors and initial data remain valid through
  allocation and any deferred initialization command.

#### Acceptance Gate

- Buffer/texture uploads remain correct after source memory is overwritten
  immediately after recording; readback returns deterministic data; storage
  texture creation never obtains a native command buffer from the recorder.
- Lock/unlock, dynamic uniform allocation, and resource initialization pass
  lifetime and repeated-frame tests with no unclassified direct context access.

### Stage 4: Flush, frame, and deferred-deletion semantics

Dependencies: Stage 3.

- [x] Implement the Stage 0 semantics for every `EImmediateFlushType` and
  `ERHISubmitFlags` value.
- [x] Order GPU submit and `RHIEndFrame` after batch replay and publish
  completion only after their required CPU-side work finishes.
- [x] Drain deferred RHI deletion only after all preceding retained command
  references are released.
- [x] Integrate executor fences with render-thread flush and end-of-frame call
  sites without adding a device-idle wait to ordinary frames.
- [x] Add counters or trace events for recorded command count, payload bytes,
  submitted batch count, replay duration, waits, and rejected submissions.

#### Acceptance Gate

- Tests distinguish dispatch from flush, prove serial/fence ordering across
  multiple batches and additional lists, and prove frame-end/deletion cannot
  overtake recorded resource use.
- Current render-resource shutdown completes with zero accepted commands,
  unreplayed batches, retained batch resources, and pending RHI deletions.

### Stage 5: Inline integration and contract documentation

Dependencies: Stage 4.

- [x] Run focused native RHI/RenderCore tests for recording, lifetime, upload,
  readback, frame submission, and shutdown.
- [x] Run Vulkan validation coverage for representative graphics, texture,
  material, viewport-present, and repeated-frame paths in inline mode.
- [x] Exercise normal startup, rendering, render-command flush, module teardown,
  render-thread drain, and clean `RHIExit` through the runtime path.
- [x] Measure command/payload volume and inline replay overhead to establish the
  backpressure and batching baseline used by the dedicated-thread plan.
- [x] Move lasting record/replay, ownership, and flush contracts into the
  owning runtime rendering documentation and update downstream plan links.

#### Acceptance Gate

- The inline executor is the only path used by normal rendering, produces
  validation-clean output, preserves existing upload/readback behavior, and
  shuts down repeatedly without leaked or late work.
- [Dedicated RHI Thread](DedicatedRHIThread.md) can consume immutable batches
  and executor fences without changing command payload ownership.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Caller memory -> recorded payload | Mutating/freeing source arrays and bytes after record does not change replayed values. |
| RHI resource -> batch lifetime | Releasing the caller reference before submit completion keeps the resource alive exactly through replay. |
| Recorder -> immutable batch | Finishing a regular list or sealing an immediate segment detaches storage; subsequent recording/reset cannot alter submitted commands. |
| Lists -> primary timeline | A finished regular list is accepted once and replays at its immediate insertion point; append-only additional lists retain argument order. |
| Batch -> context replay | Fake-context tests verify pipeline, render-pass, binding, draw, immediate-segment, and regular-list order. |
| Upload/create -> Vulkan command buffer | Upload, initialization, and readback paths use replay/synchronous executor operations, not recorder context escape hatches. |
| Submit -> frame/deletion | End-frame, GPU submit, completion serial, and deletion occur in their specified order. |
| Render thread -> runtime shutdown | Accepted render commands drain to zero batches and zero deferred RHI deletes before `RHIExit`. |

Build, test, and runtime commands follow the root [build and run](../../../Development/Build/BuildAndRun.md)
contract. A user-visible regression discovered during runtime validation requires
the full validation level specified there before this plan can complete.

## Definition of Done

- Every normal renderer command records into owned storage through either the
  immediate or a regular command list and replays only through the executor.
- The executor-owned immediate list controls the primary timeline and global
  coordination; movable regular lists finish explicitly, remain inert after
  finishing, and can then be consumed exactly once at a deterministic timeline
  position. No renderer-facing API exposes `FRHICommandBatch`.
- Submitted batches are immutable, serial-numbered, fenceable, and safe to move
  to another thread.
- Borrowed payload data and referenced RHI objects remain valid through replay
  without relying on caller stack or incidental frame lifetime.
- Existing additional-list, flush, frame-end, upload/readback, and deletion
  behavior has explicit tested semantics.
- No Vulkan path extracts a native command buffer or backend context from a
  recording command list.
- Inline graphics/runtime validation and repeated shutdown pass.
- Lasting contracts are documented and this plan records completion evidence.

## Deferred Follow-ups

- Compute commands and transitions are owned by the
  [Compute Shader Pipeline roadmap](../../../Roadmaps/ComputeShaderPipeline.md).
- Parallel recording, worker-produced lists, command sorting/merging, binary
  command compression, asynchronous queue-before-finish prerequisites, and
  asynchronous resource creation require separate evidence and plans.

## Related Documentation

- [Compute Shader Pipeline roadmap](../../../Roadmaps/ComputeShaderPipeline.md)
- [RHI command execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Runtime lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Viewport rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Build and run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Tests/Native/RenderCoreTests/`
- `Engine/Tests/Native/RHITests/`
- `Engine/Tests/Native/VulkanRHITests/`
