# Dedicated RHI Thread Plan

Summary: Move RHI command replay and backend submission onto one independently owned thread with explicit lifecycle, synchronization, and shutdown behavior.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

Durin has a game thread and a rendering thread, but no RHI thread. The rendering
thread executes queued lambdas against the process-wide
`FRHICommandListImmediate`, whose methods currently call the Vulkan command
context immediately. `EImmediateFlushType::DispatchToRHIThread`,
`FlushRHIThread`, and the corresponding submit flags are names without a
separate consumer or wait boundary. Vulkan command-buffer recording and
`FVulkanCommandListContext::Finalize` therefore run on the rendering thread,
and `Finalize` submits directly to the queue.

This plan depends on completed acceptance gates in
[Recorded RHI Command List](RecordedRHICommandList.md). It does not begin
backend migration until immutable command batches, inline replay, serial
fences, payload ownership, and synchronous-result classification are stable.
No implementation stage has started.

## Goal

The rendering thread records backend-neutral command batches and transfers
them to one dedicated RHI thread. The RHI thread is the sole owner of command
context replay, Vulkan command-pool/buffer mutation, queue submission,
frame-end/present execution, and ordered backend deletion. Rendering can keep
recording after dispatch, while explicit flushes and synchronous RHI operations
wait for a precisely identified completion serial.

Startup, frame pacing, error handling, and shutdown must remain deterministic;
normal operation must not add device-idle waits or confuse CPU thread
separation with GPU asynchronous compute.

## Scope

- An `RHIThread` engine role, affinity helpers, diagnostics, and profiling name.
- RHI-thread startup, admission, queueing, draining, stop, and backend shutdown.
- Transfer of immutable command batches from the rendering thread to a single
  RHI-thread consumer.
- Submission/completion serials, condition-variable waits, bounded
  backpressure, and executor fences.
- Threaded semantics for immediate flush types and RHI submit flags.
- RHI-thread ownership of `IRHICommandContext`, Vulkan command buffers, queue
  submission, present, end-frame work, and deferred backend deletion.
- Explicit marshaling or producer-safe contracts for synchronous resource
  creation, buffer locks, dynamic uniform allocation, queries, readback, and
  GPU-idle calls.
- Runtime initialization/shutdown integration, stress tests, validation-layer
  coverage, and performance/latency instrumentation.
- A retained inline executor mode for focused tests and recovery diagnostics.

## Non-Goals

- GPU async compute, multiple GPU submission queues, or queue-family ownership
  transfer.
- Parallel replay, multiple RHI consumers, or worker-thread command-list
  submission.
- Removing the rendering thread or changing game-to-render command ownership.
- A render graph or automatic scheduling of GPU dependencies.
- General asynchronous resource creation or futures in renderer-facing APIs.
- Guaranteed CPU/GPU performance improvement without workload measurements.
- Moving platform window-system calls that are contractually main-thread-only
  onto the RHI thread.

## Design Decisions and Invariants

### Thread topology and ownership

- The production path has one rendering-thread producer and one RHI-thread
  consumer. Other threads submit renderer work through the existing render
  command pipe; they do not enqueue command batches directly. The game thread
  may issue lifecycle control operations only before render admission opens or
  after the rendering thread has drained.
- `EThreadRole::RHIThread`, `GRHIThread`, `IsInRHIThread`, and
  `CheckRHIThread` identify the owner. Backend context and queue-mutating entry
  points assert this affinity in Debug.
- Only the RHI thread calls `IRHICommandContext`, records Vulkan command
  buffers, finalizes/submits payloads, performs ordered present/end-frame work,
  or drains backend deletion tied to completed submissions.
- Public viewport creation/resize retains its required main-thread caller
  affinity, and read-only capability queries remain explicit exceptions.
  Backend/WSI mutation is marshaled as an ordered synchronous RHI operation
  where the platform permits it. Any unavoidable main-thread backend mutation
  first drains the RHI thread and holds an exclusive ownership phase documented
  in Stage 0; it may not overlap command recording, queue submission, or frame
  state mutation.
- CPU RHI-thread separation is not GPU async compute. The first threaded path
  continues to use the same immediate GPU queue and same-queue ordering as the
  completed inline implementation.

### Startup and shutdown

- `RHIInit` loads/creates the selected backend on the game thread, starts the
  RHI thread, and runs backend `Init` as a synchronous RHI-thread operation.
  It returns only after initialization succeeds, before render-command
  admission opens.
- Admission states are `Stopped`, `Running`, and `Draining`. Every queue entry
  is either accepted with a serial or rejected synchronously with its state;
  work is never silently abandoned.
- Runtime shutdown first closes and drains render-command admission. The final
  render batch and deletion audit are dispatched, the RHI queue drains, backend
  `Shutdown` runs on the RHI thread, the thread stops, and only then is the
  backend object/module released by `RHIExit`.
- A failed backend initialization or RHI-thread launch unwinds accepted state
  in reverse order and does not start the rendering thread.

### Queue, completion, and backpressure

- The queue transports the immutable submission groups and synchronous
  operations defined by the recorded-command-list plan. Ownership transfers at
  successful enqueue and returns only through completion/destruction.
- Submission and completion serials are monotonic. Completion means CPU replay
  and ordered executor events have finished; GPU completion remains represented
  by the backend frame/fence mechanism and is not implied by an RHI-thread
  flush.
- Dispatch wakes the consumer and returns without waiting once ownership has
  transferred. Flush waits on the target completion serial using a condition
  variable; no path spin-waits on queue size or thread state.
- Outstanding batches/bytes have a measured bound. Hitting the bound blocks the
  rendering-thread producer until completion advances and records a
  backpressure diagnostic; it never drops rendering work.
- The consumer preserves FIFO order. Frame-end, present, deletion, and
  synchronous operations cannot overtake earlier recorded commands.

### Immediate-result and resource operations

- Stage 0 assigns every `FDynamicRHI` API one of four policies: recorded
  command, producer-safe CPU operation, synchronous RHI-thread operation, or
  main-thread platform exception. Unclassified backend calls are prohibited
  once threaded mode is enabled.
- Resource creation that mutates Vulkan device/backend state runs as a
  synchronous RHI-thread operation and returns a fully published RHI object.
  Any initial upload is a later ordered recorded command whose payload and
  destination lifetime are batch-owned.
- High-frequency dynamic uniform allocation must not introduce an RHI-thread
  round trip per draw in the default path. It uses producer-reserved per-frame
  upload storage with explicit page lifetime, or another Stage 0 design that
  proves equivalent ownership and latency before threaded mode becomes default.
- A writable buffer lock returns CPU-owned mapped/staging memory for one lock
  scope. `Unlock` publishes the written range as ordered work before the RHI
  thread can consume or recycle it. Read locks/readback and GPU-idle operations
  are explicit blocking calls that drain required earlier work first.
- RHI resources referenced by queued batches or synchronous operations cannot
  enter deferred deletion until those owners release them. Deletion occurs on
  the RHI thread at an ordered safe point.

### Frame and flush behavior

- `DispatchToRHIThread` seals and enqueues without waiting.
  `FlushRHIThread` waits for the serial containing all work accepted before the
  call. `FlushRHIThreadFlushResources` additionally runs the ordered deletion
  drain after preceding batch references are released.
- `FFrameSync::Threads` drains both the rendering thread and the RHI serial
  produced by its fence command. An ordinary end-of-frame dispatch may remain
  pipelined and waits only for configured frames-in-flight/backpressure.
- Present and `RHIEndFrame` execute on the RHI thread in recorded order.
  CPU flush completion does not call `RHIBlockUntilGPUIdle` unless the caller
  explicitly requested the GPU-idle contract.
- The inline mode implements the same serial, fence, ordering, and failure
  semantics; only the consumer location differs.

## Current Foundations and Gaps

| Area | Existing foundation | Plan gap |
| --- | --- | --- |
| CPU threads | `FRunnableThread`, thread roles, the rendering thread, and command admission/drain already exist. | Add a distinct RHI role, owner, lifecycle, and affinity checks. |
| Producer | Rendering thread already serializes renderer lambdas. | Seal batches and transfer them instead of executing the backend immediately. |
| Executor | The predecessor plan supplies immutable batches, serials, fences, and inline replay. | Add a live consumer queue, waits, backpressure, and threaded mode. |
| Vulkan context | One immediate context owns command pools, payloads, and queue submission. | Constrain all mutation and replay to the RHI thread. |
| Frame lifecycle | `RHIInit`, render admission, render drain, `RHIExit`, and deferred deletion have a documented order. | Insert RHI startup/drain/shutdown without weakening existing audits. |
| Synchronous APIs | Resource creation, lock/readback, dynamic uniforms, and WSI behavior already work immediately. | Assign thread-safe, marshaled, or main-thread exception policies and validate latency. |
| Tests | Threading, RenderCore, Engine, and Vulkan tests cover pieces of the lifecycle. | Add thread-affinity, queue-order, flush, stress, failure-unwind, and repeated-shutdown coverage. |

## Implementation Stages

### Stage 0: Thread-affinity and API policy

- [ ] Verify every recorded command and synchronous-operation classification
  produced by the predecessor plan against current RHI/Vulkan call sites.
- [ ] Assign every `FDynamicRHI` and backend-context API a required calling
  thread and one of the recorded, producer-safe, synchronous, or main-thread
  exception policies.
- [ ] Identify all current uses of `GetContext`, native Vulkan command-buffer
  access, queue submission, present, frame state, dynamic allocators, and
  deferred deletion outside the proposed RHI thread.
- [ ] Define queue entry ownership, admission state, monotonic serial behavior,
  bounded batch/byte capacity, wakeup, blocking, and failure-unwind contracts.
- [ ] Select the steady-state dynamic-uniform/upload-page design and set a
  measurable bound for synchronous RHI round trips per representative frame.
- [ ] Define startup/shutdown ordering relative to `RHIInit`, render-command
  admission, rendering-thread drain, module shutdown, and `RHIExit`.

#### Acceptance Gate

- Every backend entry point and mutable backend object has one thread owner or
  documented platform exception, and every immediate result has a deadlock-safe
  wait and lifetime contract.
- The design does not require a per-draw RHI-thread round trip and does not call
  user/render code while holding the queue mutex.

### Stage 1: RHI thread lifecycle and isolated queue tests

Dependencies: Stage 0 and the completed Recorded RHI Command List plan.

- [ ] Add the RHI thread role, global owner, affinity helpers, thread name, and
  profiler registration.
- [ ] Implement start, `Running`, `Draining`, stop, join, and failed-launch
  cleanup with condition-variable wakeups.
- [ ] Implement the bounded FIFO queue for immutable submission groups and
  synchronous operations, including accepted/rejected ownership transfer.
- [ ] Implement serial completion publication, fence wait, producer
  backpressure, and wakeup of all waiters during drain/failure.
- [ ] Exercise lifecycle and queue behavior against a fake executor/context
  without starting Vulkan or the rendering thread.

#### Acceptance Gate

- Deterministic tests cover start/stop, FIFO execution, dispatch without wait,
  flush to an exact serial, bounded backpressure, rejection after close,
  synchronous operation results, failure propagation, and destruction of every
  accepted/rejected payload.
- Thread-affinity observations prove replay occurs on the RHI thread and the
  producer never executes a queued command as an overflow shortcut.

### Stage 2: Runtime startup and threaded batch replay

Dependencies: Stage 1.

- [ ] Start the RHI thread during `RHIInit`, run backend initialization through
  its synchronous-operation path, and open render-command admission only after
  successful completion.
- [ ] Connect `FRHICommandListExecutor` dispatch/flush to the RHI queue while
  retaining the inline mode behind the same interface.
- [ ] Make the RHI thread resolve the active context and replay immutable
  batches; assert that recording paths cannot obtain the context.
- [ ] Implement the specified threaded behavior for every immediate flush type,
  submit flag, additional list, and executor fence.
- [ ] Add runtime configuration and diagnostics that report inline/threaded
  mode, pending batches/bytes, submitted/completed serials, wait duration, and
  backpressure events.

#### Acceptance Gate

- A normal graphics frame is recorded on the rendering thread and replayed on
  the RHI thread with identical fake-context order and Vulkan-visible output.
- Dispatch permits subsequent render recording before replay completes; flush
  waits for the correct serial without a GPU-idle wait or deadlock.

### Stage 3: Vulkan ownership and synchronous RHI operations

Dependencies: Stage 2.

- [ ] Move Vulkan command-pool/buffer recording, payload finalization, queue
  submission, present, end-frame mutation, and ordered deferred deletion under
  RHI-thread affinity assertions.
- [ ] Marshal buffer/texture/sampler/shader/PSO creation that mutates backend
  state and publish returned objects only after the synchronous operation
  completes.
- [ ] Implement the selected producer-safe dynamic uniform/upload allocation
  scheme with frame/page lifetime retained through RHI/GPU consumption.
- [ ] Implement buffer lock/unlock ownership transfer and blocking readback/GPU-
  idle operations without exposing mutable context state to the rendering
  thread.
- [ ] Preserve main-thread caller affinity for viewport create/resize while
  marshaling backend mutation or enforcing the documented exclusive phase;
  ensure acquire, command recording, present, and frame-owned state execute on
  the RHI thread.
- [ ] Remove or reject every undeclared direct backend call from game/render
  paths when threaded mode is active.

#### Acceptance Gate

- Debug affinity checks remain clean across resource creation, uploads, dynamic
  uniforms, render passes, present, readback, frame end, and deletion.
- Resource-returning calls publish valid objects, upload/readback bytes remain
  deterministic, and representative frames stay within the Stage 0 synchronous-
  round-trip budget.

### Stage 4: Frame pacing, flush integration, and shutdown drain

Dependencies: Stage 3.

- [ ] Integrate RHI completion serials into `FFrameSync::Threads`, render-command
  fences, end-of-frame dispatch, and any frames-in-flight throttle.
- [ ] Ensure `EndFrame`, present, GPU submission, completion publication, and
  resource deletion preserve their declared order without whole-device idle.
- [ ] Close render admission first, submit the final accepted render batch,
  transition RHI admission to draining, and wait for zero queued/active work.
- [ ] Run the final render-resource and RHI deletion audits before backend
  shutdown; then execute backend `Shutdown` on the RHI thread, stop/join it, and
  complete `RHIExit`.
- [ ] Reject late render/RHI work synchronously and wake every blocked producer
  or fence waiter on shutdown and failure paths.

#### Acceptance Gate

- Thread-only flush drains both CPU stages but not the GPU; explicit GPU-idle
  remains distinct and testable.
- Repeated startup/shutdown, empty frames, queued resource release, present
  failure, and close-with-backpressure tests terminate with zero render
  commands, RHI batches, waiters, retained resources, and deferred deletions.

### Stage 5: Threaded validation and default enablement

Dependencies: Stage 4.

- [ ] Run focused Core/RHI/RenderCore tests for affinity, queue concurrency,
  serial fences, synchronous calls, resource lifetime, and shutdown.
- [ ] Run Vulkan validation for graphics, texture upload/readback, material,
  dynamic uniform, viewport present/resize, and repeated-frame workloads in
  both inline and threaded modes.
- [ ] Stress producer/consumer skew, maximum queue capacity, rapid flushes,
  resource churn, minimized/closed windows, and repeated clean process exit.
- [ ] Compare CPU frame time, RHI replay time, wait duration, queue depth,
  payload bytes, and synchronous round trips against the inline baseline.
- [ ] Make threaded mode the normal runtime path only after correctness,
  shutdown, and latency gates pass; retain inline mode for focused tests and
  diagnostics.
- [ ] Move lasting thread ownership, flush, frame, and shutdown rules into the
  runtime lifecycle/rendering documentation and update dependent plans.

#### Acceptance Gate

- Threaded and inline modes produce equivalent validated rendering and resource
  results; threaded mode has no undeclared backend calls from game/render
  threads and no recurring producer stall caused by per-draw synchronous work.
- The normal runtime starts and exits repeatedly with all documented lifecycle
  audits clean, and profiling exposes enough state to diagnose stalls without a
  debugger.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Rendering thread -> RHI queue | Ownership transfers once, FIFO order is stable, and producer can continue after dispatch. |
| RHI queue -> context replay | Every context call and Vulkan command-buffer mutation observes RHI-thread affinity. |
| Submission serial -> flush | Flush waits for the target CPU completion serial and does not imply GPU completion. |
| Immediate API -> returned result | Create, allocate, lock, and readback paths publish valid data with explicit wait/lifetime behavior. |
| Upload page -> frame lifetime | CPU upload storage survives replay and required GPU use before recycling. |
| End frame -> present/submit/delete | Ordered events cannot overtake earlier resource use and do not require ordinary device idle. |
| Render drain -> RHI drain -> exit | Shutdown rejects late work, wakes waiters, drains both queues, audits deletion, shuts down backend, then joins. |
| Inline -> threaded parity | Focused and runtime workloads produce equivalent output and validation-layer results. |

Build, test, and runtime commands follow the root [build and run](../Development/Build/BuildAndRun.md)
contract. Because this changes user-visible editor rendering and process
lifecycle, completion requires the full `all` build and runtime validation
defined there.

## Definition of Done

- Normal rendering records on the rendering thread and replays on one dedicated
  RHI thread.
- The RHI thread exclusively owns command-context replay, Vulkan command
  buffers, queue submission, frame-end/present work, and ordered deletion.
- Every RHI API has an enforced thread/marshaling policy; immediate-return APIs
  have explicit blocking and lifetime semantics.
- Dispatch, flush, CPU completion, GPU completion, and device-idle behavior are
  distinct and covered by tests.
- Startup failure and normal shutdown drain without lost work, stranded
  waiters, live batches, or pending deletion.
- Threaded mode passes validation and its synchronous-call/backpressure profile
  is acceptable against the recorded inline baseline.
- Lasting lifecycle and thread-ownership contracts are documented and this plan
  records completion evidence.

## Deferred Follow-ups

- GPU asynchronous compute remains the evidence-gated M5 milestone in the
  [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md).
- Multiple RHI consumers, parallel command-list replay, worker-produced lists,
  and asynchronous resource-creation futures require separate plans.
- More aggressive frame pipelining or adaptive batching starts only after
  traces identify a measurable bottleneck in this single-consumer design.

## Related Documentation

- [Recorded RHI Command List](RecordedRHICommandList.md)
- [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/RunnableThread.h`
- `Engine/Source/Runtime/Core/Private/Threading/RunnableThread.cpp`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Private/RHIGlobals.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanQueue.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/`
- `Engine/Tests/Native/VulkanRHITests/`
