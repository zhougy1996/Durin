# Recorded RHI Command List Plan

Summary: Replace the immediate RHI forwarding facade with owned command batches that can be replayed inline today and transferred to a dedicated RHI thread later.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

`FRHICommandListBase` currently stores a graphics context pointer and forwards
every graphics, viewport, shader-parameter, and draw call directly to
`IRHICommandContext`. `FRHICommandListExecutor::Submit` ignores
`AdditionalCmdLists`; its only observable work is deferred RHI deletion and an
optional `RHIEndFrame`. The immediate-flush names therefore do not describe a
real record, dispatch, replay, or completion boundary.

Several APIs also expose data whose lifetime is currently valid only because
execution is immediate. `PushConstants` accepts a raw byte pointer,
`SetShaderParameters` accepts a span, buffer writes route through a returned
lock pointer, and dynamic uniform allocation returns a range synchronously.
Vulkan upload, readback, storage-texture initialization, and buffer-lock paths
can obtain the native command buffer through `FRHICommandListBase::GetContext`.

No implementation stage has started. This plan is the required predecessor of
[Dedicated RHI Thread](DedicatedRHIThread.md) and an upstream dependency of the
compute-pipeline roadmap. Its first shipped executor remains inline so command
ownership and replay correctness can be validated without concurrency masking
errors.

## Goal

Rendering code records backend-neutral RHI operations into an owned command
stream. Submission seals that stream into an immutable batch, and the executor
replays the batch against an `IRHICommandContext` in a deterministic order.
The recording path does not obtain or call a backend context, retain borrowed
payload memory, or allow referenced RHI resources to die before replay
completion.

The same batch and executor contract must work in synchronous inline mode and
be transferable without redesign to the dedicated RHI thread plan.

## Scope

- Command storage, payload alignment, destruction, reset, and reuse.
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
- GPU asynchronous compute, multiple GPU queues, or queue-family ownership
  transfer.
- A render graph, automatic resource-state inference, or the compute-specific
  transition vocabulary.
- Making resource creation generally asynchronous.
- A stable serialized command ABI across process runs or engine versions.
- Optimizing command packing before ownership and replay behavior is measured.

## Design Decisions and Invariants

### Recording and batch ownership

- A command-list call records; it does not call `IRHICommandContext` or cache a
  context pointer.
- Submission detaches the current storage into one immutable
  `FRHICommandBatch` and immediately leaves the recorder ready for a new batch.
  A batch is accepted exactly once and is destroyed only after replay or
  explicit rejected-submission cleanup.
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
- Commands replay in record order. One submit replays the sealed immediate
  batch first and then each explicitly supplied additional list in argument
  order. Empty batches do not manufacture backend work.
- Render-pass balance, pipeline selection, lock pairing, double submission, and
  record-after-seal errors fail at the closest CPU boundary with command and
  submission context in the diagnostic.
- Inline mode is a real executor mode, not a direct-call shortcut. All runtime
  rendering passes through the same seal and replay code that threaded mode
  will consume.

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
| Backend context | `IRHICommandContext` defines the graphics replay surface. | Make it executor-only and add context operations needed by uploads/readback. |
| Executor | One process-wide immediate list and submit entry point exist. | Implement sealing, ordering, inline replay, serials, fences, and additional lists. |
| Payloads | Current callers provide ordinary RHI descriptors, spans, and data pointers. | Define deep-copy and retained-resource rules per command. |
| Uploads | Buffer and texture upload paths already encode Vulkan work. | Stop extracting a native command buffer during recording and own upload bytes until replay. |
| Lifecycle | Render commands and deferred RHI deletion already have ordered shutdown expectations. | Tie deletion and frame events to completed command batches. |
| Tests | RenderCore and Vulkan RHI tests exercise resources and real draw paths. | Add fake-context replay tests plus inline-executor integration coverage. |

## Implementation Stages

### Stage 0: Command and ownership contract

- [ ] Inventory every `FRHICommandListBase`, `FRHICommandListImmediate`, and
  command-list-taking `FDynamicRHI` entry point.
- [ ] Classify each entry point as recorded, synchronous executor operation,
  producer-safe query/reservation, or prohibited backend escape hatch.
- [ ] Record for every command its copied fields, retained objects, replay
  context, valid pipeline/render-pass state, and failure condition.
- [ ] Select the internal command-node/arena representation and state the
  alignment, destructor, move, reset, and out-of-memory behavior.
- [ ] Define submission-group order, serial allocation, fence targeting, and
  the exact mapping of existing flush types and submit flags.
- [ ] Add a test context/executor seam that can observe replayed operations and
  calling thread without creating Vulkan objects.

#### Acceptance Gate

- Every current command-list or command-list-taking RHI API has one ownership
  and execution classification; no raw payload, native-context access, or
  immediate-result method is left to implicit behavior.
- The selected batch contract can be moved to another thread without changing
  public renderer call sites or retaining recording-thread stack memory.

### Stage 1: Owned command batches and inline executor

Dependencies: Stage 0.

- [ ] Implement command storage with typed execute/destroy behavior and
  batch-owned auxiliary payload memory.
- [ ] Implement seal/detach/reset so a recorder can begin a new batch without
  mutating an already submitted batch.
- [ ] Add immutable submission groups, monotonic submission/completion serials,
  and a serial-targeted fence in inline mode.
- [ ] Make empty, rejected, moved, replayed, destroyed, and reused batch states
  explicit and assertion-covered.
- [ ] Verify command destructors release retained resources and payloads once
  on success and once on rejected-submission cleanup.

#### Acceptance Gate

- Unit tests prove that batches preserve order, survive source-memory mutation,
  retain resources until completion, reject double submission, release all
  owned state, and replay through the inline executor.

### Stage 2: Record and replay the graphics command surface

Dependencies: Stage 1.

- [ ] Convert pipeline switch, render-pass, viewport, graphics PSO, vertex/index
  binding, viewport/scissor, shader-parameter, push-constant, and draw calls to
  typed recorded commands.
- [ ] Deep-copy render-pass and shader-parameter data and retain every resource,
  shader, PSO, texture, buffer, and viewport required during replay.
- [ ] Resolve contexts only inside replay and remove recording-time dependence
  on `FRHICommandListBase::GetContext`.
- [ ] Preserve validation for active pipeline and render-pass ordering with
  diagnostics that identify the offending command.
- [ ] Implement and test the defined immediate-plus-additional-list replay
  order.

#### Acceptance Gate

- A fake context observes the same ordered graphics operations and values after
  caller-owned descriptors and arrays have gone out of scope.
- Existing graphics frames execute through seal and inline replay with no
  direct context call from the recording path.

### Stage 3: Uploads, readback, and synchronous-result boundaries

Dependencies: Stage 2.

- [ ] Record buffer writes, uniform updates, and texture updates with owned
  source bytes and retained destination resources.
- [ ] Replace Vulkan uses of `RHIGetVkCommandBuffer(RHICmdList)` and recording-
  time `GetContext()` in upload, readback, lock, and storage-texture
  initialization paths with replayable context operations or declared
  synchronous executor operations.
- [ ] Implement balanced lock-scope ownership and prevent submission or memory
  reuse before `Unlock` transfers staged work.
- [ ] Route dynamic uniform allocation, texture readback, GPU-idle waits, and
  other immediate results through the inline synchronous-operation contract.
- [ ] Ensure resource create descriptors and initial data remain valid through
  allocation and any deferred initialization command.

#### Acceptance Gate

- Buffer/texture uploads remain correct after source memory is overwritten
  immediately after recording; readback returns deterministic data; storage
  texture creation never obtains a native command buffer from the recorder.
- Lock/unlock, dynamic uniform allocation, and resource initialization pass
  lifetime and repeated-frame tests with no unclassified direct context access.

### Stage 4: Flush, frame, and deferred-deletion semantics

Dependencies: Stage 3.

- [ ] Implement the Stage 0 semantics for every `EImmediateFlushType` and
  `ERHISubmitFlags` value.
- [ ] Order GPU submit and `RHIEndFrame` after batch replay and publish
  completion only after their required CPU-side work finishes.
- [ ] Drain deferred RHI deletion only after all preceding retained command
  references are released.
- [ ] Integrate executor fences with render-thread flush and end-of-frame call
  sites without adding a device-idle wait to ordinary frames.
- [ ] Add counters or trace events for recorded command count, payload bytes,
  submitted batch count, replay duration, waits, and rejected submissions.

#### Acceptance Gate

- Tests distinguish dispatch from flush, prove serial/fence ordering across
  multiple batches and additional lists, and prove frame-end/deletion cannot
  overtake recorded resource use.
- Current render-resource shutdown completes with zero accepted commands,
  unreplayed batches, retained batch resources, and pending RHI deletions.

### Stage 5: Inline integration and contract documentation

Dependencies: Stage 4.

- [ ] Run focused native RHI/RenderCore tests for recording, lifetime, upload,
  readback, frame submission, and shutdown.
- [ ] Run Vulkan validation coverage for representative graphics, texture,
  material, viewport-present, and repeated-frame paths in inline mode.
- [ ] Exercise normal startup, rendering, render-command flush, module teardown,
  render-thread drain, and clean `RHIExit` through the runtime path.
- [ ] Measure command/payload volume and inline replay overhead to establish the
  backpressure and batching baseline used by the dedicated-thread plan.
- [ ] Move lasting record/replay, ownership, and flush contracts into the
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
| Recorder -> immutable batch | Sealing detaches storage; subsequent recording/reset cannot alter submitted commands. |
| Batch -> context replay | Fake-context tests verify pipeline, render-pass, binding, draw, and additional-list order. |
| Upload/create -> Vulkan command buffer | Upload, initialization, and readback paths use replay/synchronous executor operations, not recorder context escape hatches. |
| Submit -> frame/deletion | End-frame, GPU submit, completion serial, and deletion occur in their specified order. |
| Render thread -> runtime shutdown | Accepted render commands drain to zero batches and zero deferred RHI deletes before `RHIExit`. |

Build, test, and runtime commands follow the root [build and run](../Development/Build/BuildAndRun.md)
contract. A user-visible regression discovered during runtime validation requires
the full validation level specified there before this plan can complete.

## Definition of Done

- Every normal renderer command records into owned storage and replays only
  through the executor.
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

- Dedicated replay on an RHI thread is owned by
  [Dedicated RHI Thread](DedicatedRHIThread.md).
- Compute commands and transitions are owned by the
  [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md).
- Parallel recording, worker-produced lists, command sorting/merging, binary
  command compression, and asynchronous resource creation require separate
  evidence and plans.

## Related Documentation

- [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and run](../Development/Build/BuildAndRun.md)

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
- `Engine/Tests/Native/VulkanRHITests/`
