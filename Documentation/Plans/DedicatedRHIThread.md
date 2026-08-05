# Dedicated RHI Thread Plan

Summary: Move RHI command replay and backend submission onto one independently owned thread with explicit lifecycle, synchronization, and shutdown behavior.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

Durin has a game thread and a rendering thread, but no RHI thread. The completed
[Recorded RHI Command List](RecordedRHICommandList.md) predecessor replaced
direct command-list forwarding with always-recorded immediate and regular
lists, private immutable batches, ordered inline replay, monotonic serials,
executor fences, owned upload payloads, retained resources, and declared
synchronous operations. Vulkan context mutation, command-buffer finalization,
queue submission, and present still execute inline on the rendering thread.

The implemented producer/consumer contract is now owned by
[RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md). This plan
must move that consumer without changing batch representation or payload
ownership. The inline baseline exposes cumulative commands, payload bytes,
batches, submission groups, replay nanoseconds, waits, rejections, and pending
batches. Three 60-tick predecessor samples observed 14,257-14,259 commands,
555,145-555,305 payload bytes, 122-124 batches, 129-131 submission groups, and
69.6-72.7 ms cumulative inline replay: approximately 238 commands, 9.25 KB of
payload, 2.2 groups, and 1.18 ms replay per tick.

Stage 0 completed on 2026-08-05 against baseline commit
`0812adfeba0c536e3787ff1d74dea8fcbfb81315`. The API and mutable-object
inventory, queue/admission contract, dynamic-uniform page design, and
startup/shutdown order are fixed below. The first threaded queue is bounded to
8 queued or active entries, 16 batches, and 32 MiB of command payload. The
steady-state upload path reserves one 4 MiB persistently mapped page per frame
slot through at most one synchronous frame-acquire operation, then performs all
per-draw suballocation on the rendering thread.

Stage 1 completed on 2026-08-05. Core now exposes the `RHIThread` role, global
owner, affinity helpers, and diagnostic name. RHI owns an isolated
`FRHIThread` lifecycle with `Stopped`/`Running`/`Draining` admission, move-only
FIFO work, exact serial waits, synchronous-result coordination, the selected
entry/batch/byte bounds, producer backpressure, failure propagation, rejected
ownership preservation, and deterministic drain/join. Nine isolated queue
tests pass, alongside all 24 predecessor `RHICommandListTests` and five focused
`FRunnableThreadTests`. The executor remains inline and Vulkan/runtime startup
remain unchanged as required by the stage boundary.

Stage 2 completed on 2026-08-05. Runtime startup now creates the RHI thread,
initializes and shuts down the backend through synchronous queue work, and keeps
inline execution as the pre-Stage-5 default; `DURIN_RHI_EXECUTION=threaded`
explicitly opts into the dedicated thread. Command-list submission, ordered
begin/end-frame work, submit/present, deferred deletion, synchronous immediate
operations, exact flushes, and executor fences share one serial timeline.
Fake-context parity, asynchronous dispatch, flush-flag, additional-list, and
thread-affinity tests pass, and a Debug Editor Vulkan smoke run completed ten
ticks and normal shutdown in threaded mode.

The Stage 2 preflight corrections give command-list admission a
typed rejection result, restores rejected batches to the executor, updates
submitted telemetry only after acceptance, and makes the legacy `Submit`
entry point terminate in every build instead of returning a false-success
serial. Queue payload accounting includes command-owned buffer, texture, push
constant, shader-parameter, and explicitly declared callable allocations with
checked aggregation. `RHIInit` and `FEngineLoop::Init` report failure, unwind
thread/backend/application state in order, and do not start rendering, Mona,
or the Engine after failure. Unset and invalid execution-mode configuration now
select inline, with an explicit diagnostic for invalid values. Executor submit
is the single serial-wait owner for flush flags, and the duplicate
deferred-resource flush declaration is removed. The Stage 3 entry gate is clear.

Stage 3 completed on 2026-08-05. Threaded Vulkan now enforces RHI affinity for
context replay, command-pool/buffer mutation, queue submission, present, frame
state, deferred deletion, and resource construction. Buffer, texture, sampler,
shader, vertex-declaration, PSO, and viewport creation marshal through a
context-free synchronous executor operation; native command-buffer integration
uses a scoped RHI-thread callback instead of returning a retainable handle.
Viewport resize is ordered and synchronous after earlier rendering work,
back-buffer acquisition is recorded, and the back-buffer wrapper remains stable
across swapchain recreation.

The dynamic-uniform path preallocates one persistently mapped 4 MiB page for
each frame slot on the RHI thread. `FRHIBeginFrameArgs` carries immutable frame
identity inside the ordered submission group; Vulkan selects its RHI-owned slot,
waits that slot's GPU fence, and only then returns the page lease for rendering-
thread reset and suballocation. This is the frame's one acquire round trip; only
page overflow synchronously reserves another RHI-owned page.
The focused test proves a prepared allocation adds zero synchronous operations
and a deliberate oversized allocation adds exactly one. A 60-tick threaded
Editor sample reported 146 waits, including 76 separately counted low-frequency
synchronous operations, leaving 70 frame/lifecycle serial waits across startup,
60 frames, and shutdown; it reported zero backpressure events and zero
rejections. The equivalent inline sample reported 71 waits, including one
synchronous operation, likewise leaving 70 frame/lifecycle waits, zero
backpressure events, and zero rejections.

Stage 3.5 completed on 2026-08-05. `FRHICommandListExecutor` now owns an
acquire-published `FrameNumber` that starts at zero for the executor lifetime,
survives backend initialization and inline/threaded mode transitions, and
advances only after a replayed backend `RHIEndFrame` returns successfully.
Ordered BeginFrame replay constructs the backend-only `FRHIBeginFrameArgs`
from that number; command-list callers can no longer supply frame identity or
fall through an overload that discards it.

Vulkan derives both `FVulkanDevice::CurrentFrameIndex` and rendering-thread
dynamic-uniform page selection from the executor number after the existing
exact BeginFrame serial completes. No Vulkan frame-slot path reads
`GRenderFrameCounterRenderThread`, and the focused slot test observes the same
uniform backing page across the executor-derived 0 -> 1 -> 0 cycle without a
new synchronous operation. Present retains its ordered viewport/present/vsync
payload without `FRHIPresentArgs`; `GVulkanRHIDeletionFrameNumber` remains the
independent Vulkan deletion-aging counter. Focused command-list,
initialization, and Vulkan integration tests, the complete native-test
aggregate, and the full Debug Editor `all` build pass. Default-inline and
explicit-threaded 60-tick Editor runs exit normally with zero backpressure and
zero rejection. The Stage 4 entry gate is clear.

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

### RHI frame identity and backend frame slots

- `FRHICommandListExecutor` owns a `uint64 FrameNumber` initialized to zero for
  the executor lifetime. Inline/threaded mode changes do not derive or reset it
  from game- or rendering-thread counters.
- Ordered BeginFrame replay reads the current executor `FrameNumber` on the RHI
  execution owner. A successful ordered `RHIEndFrame` increments it exactly
  once after the backend call returns and before that submission serial is
  published complete. A failed or rejected EndFrame does not advance it.
- `FRHIBeginFrameArgs` is retained only as the executor-to-backend replay
  payload and carries the executor-owned `FrameNumber`; render/game callers do
  not construct or supply it. Silent overloads that discard frame identity are
  not part of the production contract.
- Vulkan derives `FVulkanDevice::CurrentFrameIndex` from the executor frame
  number. After the synchronous BeginFrame serial completes, the rendering
  thread may read the published frame number/slot and lease the matching
  dynamic-uniform page without another RHI round trip.
- Present remains ordered by command-list FIFO and consumes the current
  acquired back buffer and active backend frame state. Durin does not add
  `FRHIPresentArgs` or a separate present frame counter in this plan.
- `GVulkanRHIDeletionFrameNumber` remains Vulkan-owned and advances in Vulkan
  `RHIEndFrame` for deferred-deletion aging. It is deliberately separate from
  the executor frame number and does not select frame slots.

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

## Stage 0 Contract

### API execution and caller policy

The public caller policy and the backend execution owner are separate. A
rendering- or game-thread caller may invoke a marshaled API, but only the RHI
thread executes device, context, queue, frame, descriptor-pool, command-pool,
or deferred-deletion mutation. Once RHI admission is running, an API not in the
following table is rejected rather than implicitly executed on its caller.

| API surface | Caller and policy | Execution and lifetime rule |
| --- | --- | --- |
| `Init`, `Shutdown` | Game-thread lifecycle, synchronous RHI operation | Backend construction remains on the game thread. `Init` and `Shutdown` execute on the RHI thread and return only after success/failure is published. |
| `RHIBeginFrame` | Rendering thread, synchronous frame-acquire operation | The executor supplies its current `FrameNumber`; the RHI thread derives the backend frame slot, waits that slot's fence, resets frame/descriptor state, begins the context, and publishes the matching upload-page lease after earlier work. It is the default frame's single allowed synchronous RHI round trip. |
| `RHIEndFrame_RenderThread`; compatibility `RHIEndFrame` | Rendering-thread ordered coordination | Seal and enqueue `EndFrame`; context finalization, submit/present ordering, page retirement, and deletion execute on the RHI thread. The executor increments `FrameNumber` once only after successful backend EndFrame and before publishing serial completion. Direct compatibility calls use the same executor event and never call the context inline. |
| `RHICreateViewport`, `RHIResizeViewport` | Main-thread platform entry point; synchronous or render-marshaled synchronous RHI mutation | Native window identity is captured on the main thread. Surface/swapchain/device mutation executes on the RHI thread. Resize may retain its main-to-render notification, but that render command must synchronously marshal the backend phase. No main-thread exception may overlap RHI mutation. |
| `RHICreateGraphicsPipelineState`, `RHIGetGraphicsPipelineState`, `RHICreateVertexDeclaration`, `RHICreateTexture`, `RHICreateSampler`, `RHICreateShader`, `RHICreateBuffer` | Rendering thread, synchronous RHI operation | Device allocation and mutable cache lookup execute on the RHI thread; the completed ref-counted object is published to the waiter. Texture/buffer initial bytes are copied and admitted later as recorded initialization/upload commands. |
| `RHIIsTextureFormatSupported` | Game or rendering thread, producer-safe query | Reads immutable capabilities published after `Init`; it allocates nothing and cannot expose mutable backend state. |
| `RHIUpdateTextureReference` | Rendering thread, producer-safe CPU publication | Updates only the stable RHI reference under its existing render-thread ownership. A backend descriptor/bindless mutation must be represented by ordered recorded work or a synchronous RHI operation before publication. |
| `RHIGetViewportBackBuffer` / `RHIAcquireBackBuffer` | Rendering thread, producer-safe stable-wrapper lookup plus recorded acquire | Return the viewport's stable back-buffer wrapper immediately and record an acquire command at that timeline position. Swapchain acquisition and frame-owned image selection execute during RHI-thread replay; the command and later batches retain the wrapper. |
| `RHIAllocateDynamicUniformBuffer` | Rendering thread, producer-safe allocation from an acquired page | Copies into the current frame lease and returns a retained buffer range without an RHI round trip. Page exhaustion explicitly requests another page synchronously and increments an overflow diagnostic. |
| `RHILockBuffer`, `RHIUnlockBuffer` | Rendering thread, producer-safe lock scope plus recorded unlock | Write lock returns CPU-owned staging bytes. Unlock records the copied range and transfers ownership to the batch. Read locks are unsupported here and must use an explicit synchronous readback contract. |
| `RHIUpdateTexture2D` and command-list graphics/upload methods | Rendering thread, recorded command | Existing immutable batch copying, resource retention, and replay rules remain unchanged. |
| `RHIReadTexture2D`, `RHIBlockUntilGPUIdle` | Rendering thread or explicit test/lifecycle caller, synchronous RHI operation | Seal and enqueue all earlier work, enqueue the operation, wait for its exact result serial, and return copied output/status. GPU idle is performed only by the explicit idle API. |
| `RHIGetDefaultContext`, `RHIGetCommandContext` | RHI thread only | Executor replay resolves the context. Recording, renderer, game, and main-thread platform code cannot obtain it. |
| Vulkan instance/device/physical-device/loader access | Initialization-only exclusive phase, or explicit synchronous backend integration | Opaque handles do not grant mutation authority. Runtime integrations marshal device use to the RHI thread. |
| `RHIGetVkCommandBufferForBackendIntegration` | RHI thread only | Replace direct test access with an executor synchronous integration operation; no caller may retain the native command buffer after the operation returns. |

Every `IRHICommandContext` method is RHI-thread-only: begin/submit/end frame;
render-pass and viewport begin/end; viewport/scissor, pipeline, buffer, shader,
push-constant, and draw commands; buffer/texture initialization and transfer;
readback; dynamic-uniform backend allocation; back-buffer acquire; and GPU-idle.
`RHISetShaderUniformBuffer` is a Vulkan context-internal helper with the same
owner. Context methods do not call renderer/user code.

### Mutable backend ownership

| Object or state | Owner after threaded admission opens | Exception or publication rule |
| --- | --- | --- |
| `FRHICommandListImmediate` recording state and pending producer batches | Rendering thread | Finished immutable batches transfer exactly once at queue admission. |
| Executor queue, serial allocator, admission state, and waiter registry | Queue mutex for short metadata operations | Replay, destruction, diagnostics callbacks, and user/backend operations never run while the mutex is held. |
| Executor `FrameNumber` | RHI replay owner; acquire-only snapshot after exact BeginFrame completion | BeginFrame reads the current value; successful EndFrame increments it before serial completion. Engine/render counters never mutate or replace it. |
| `IRHICommandContext`, Vulkan command pools/buffers/payloads, graphics/present queues | RHI thread | Native handles are borrowed only within an RHI-thread operation. |
| `FVulkanDevice` frame state, global descriptor pools, pipeline cache, dynamic-uniform page creation, and deferred deletion queue | RHI thread | Immutable capability snapshots may be read producer-side after successful init. |
| Vulkan resource allocation and backend object destruction | RHI thread | Recorded batches and synchronous-operation captures retain refs; final destruction occurs only in an ordered RHI deletion drain. |
| `FVulkanViewport`, swapchain, acquire/present resources, and back-buffer selection | RHI thread | Main thread owns window-system notification/caller affinity but not backend mutation. Published size/back-buffer observations require synchronized snapshots or returned refs. |
| Dynamic-uniform page write cursor and mapped bytes for the current lease | Rendering thread | The RHI thread creates/retires pages; ownership transfers to the producer after frame acquire and back only with the submitted frame serial. |

Direct-use inventory found outside the proposed owner consists of render-thread
`RHIBeginFrame`, resource/cache creation, dynamic uniform allocation,
back-buffer acquisition, resize, readback, and deferred-delete triggering;
main-thread viewport construction/resize entry points; lifecycle-thread backend
`Init`/`Shutdown`; and the Vulkan compute integration test's native command
buffer. Queue submission and present are already reached through the immediate
context, but execute inline today. All of these sites map to a policy above;
there is no remaining unclassified public `FDynamicRHI` or
`IRHICommandContext` entry point.

### Queue, serial, and failure contract

- One move-only queue entry is either an immutable submission group or a
  synchronous operation with an owned result state. The rendering thread is
  the only normal group producer; lifecycle control may enqueue before render
  admission opens or after it drains.
- Admission states are `Stopped`, `Running`, and `Draining`. `Start` changes
  `Stopped -> Running` only after the consumer is ready. Closing admission is
  one locked `Running -> Draining` transition. Only the already-installed
  internal backend-shutdown marker is legal in `Draining`; all public work is
  rejected synchronously. Join returns the owner to `Stopped`.
- A successful enqueue allocates the next nonzero serial and transfers entry
  ownership. Rejection allocates no serial and leaves ownership with the
  caller. Serial exhaustion, oversized entries, closed admission, consumer
  failure, and launch failure are explicit rejection reasons.
- Capacity counts queued plus actively replaying work: at most 8 entries, 16
  batches, and 32 MiB of recorded command payload. Admission waits on a
  condition variable until the whole entry fits. An entry that by itself
  exceeds a limit is rejected before ownership transfer; upload producers must
  split work rather than deadlock waiting for impossible capacity.
- Backpressure waits only on the rendering-thread producer, increments count
  and duration diagnostics, and wakes when capacity is released, admission
  closes, or execution fails. The consumer never executes work on the producer
  as an overflow shortcut.
- The consumer removes one FIFO entry under the mutex, releases the mutex,
  replays it, performs ordered submit/end-frame/delete events, destroys released
  payloads, then publishes its serial as completed under the mutex. Completion
  means CPU replay/events finished, not GPU completion.
- Flush first seals all work accepted before the call, captures its target
  serial, and waits on completion. A synchronous operation is ordered after the
  sealed group and publishes its value/error before its serial completes. A
  waiter invoked from the RHI thread is rejected as a self-deadlock; internal
  RHI-thread helpers execute directly only when their contract says so.
- The first execution failure records the failing serial and diagnostic, stops
  further replay, transitions to `Draining`, rejects queued-but-not-started
  entries with their owned result states intact, destroys every accepted
  payload exactly once, and wakes producers and serial/result waiters. Waiters
  return failure rather than waiting for a serial that can no longer complete.

### Dynamic-uniform and upload-page design

The Vulkan backend preallocates one 4 MiB persistently mapped uniform page for
each frame-in-flight slot during RHI initialization. At render-frame begin, one
synchronous frame-acquire operation waits the slot's existing GPU fence,
performs frame/context resets, resets the page cursor, and transfers a lease to
the rendering thread. All uniform allocations for that frame align and copy
directly into the leased mapped page. Returned `FRHIUniformBufferRange` values
retain the backing buffer through command-batch replay, while the frame slot's
GPU fence prevents page reuse until the GPU is finished.

If 4 MiB is insufficient, the producer synchronously leases another RHI-created
page instead of allocating a backend resource itself. The backend retains the
per-slot high-water mark and can provision that capacity at the next acquire.
Diagnostics report acquire round trips, overflow leases, bytes, and high-water
marks. The representative steady-state gate is at most one synchronous
frame-acquire operation and zero overflow operations per rendered frame; there
is no per-draw RHI-thread round trip. Resource creation, readback, viewport
mutation, and explicit GPU idle are demand-driven synchronous operations and
are reported separately from this steady-state budget. Ordinary back-buffer
acquisition is recorded work and therefore adds no producer round trip.

### Startup, frame, and shutdown order

1. On the game thread, `RHIInit` loads the backend module, creates the backend
   object, constructs the RHI owner, starts the named RHI thread, and waits for
   its ready signal while render admission remains closed.
2. `RHIInit` enqueues backend `Init` as a lifecycle synchronous operation. On
   success it publishes immutable capabilities, initializes the recorder's
   default pipeline, and only then permits `InitRenderingThread` and render
   admission. Thread launch or backend init failure closes admission, wakes all
   waiters, stops/joins the RHI owner, destroys the backend, unloads the module,
   and returns failure without starting rendering.
3. Each render frame synchronously acquires its frame/upload-page lease, records
   freely, and asynchronously enqueues ordered end-frame/submit/present/delete
   work. Ordinary pacing uses frame-slot fences and queue backpressure; it does
   not introduce device idle.
4. Shutdown first closes render-command admission, lets the rendering thread
   submit its final accepted command and deletion audit, flushes the exact RHI
   serial produced by that fence, then stops/joins the rendering thread.
5. The owner atomically installs the backend-shutdown marker and transitions
   RHI admission to `Draining`. The consumer releases all batch references,
   drains deferred deletion, verifies zero queued/active batches and waiters,
   runs backend `Shutdown` on the RHI thread, publishes the terminal result,
   and exits. The game thread joins it before deleting the backend object and
   releasing the backend module. Late render or RHI work is rejected and all
   blocked producers/waiters are woken.

### Stage 1 handoff

- Baseline commit: `0812adfeba0c536e3787ff1d74dea8fcbfb81315`.
- Initial working set: `RunnableThread.h/.cpp`, `CoreGlobals.h`, new RHI-thread
  owner/queue files under `Runtime/RHI`, `RHICommandList.h/.cpp`, and focused
  Core/RHI native tests. Do not integrate `Launch`, `RenderCore`, or Vulkan in
  Stage 1.
- Key existing symbols: `EThreadRole`, `FRunnableThread`,
  `FRHICommandListExecutor::FState`, `FSubmissionGroup`,
  `FRHICommandListFence`, `Submit`, `WaitForSerial`, and
  `FRHICommandListExecutorStats`.
- Selected decisions: one FIFO consumer; three admission states; exact serial
  waits; result-state synchronous entries; 8-entry/16-batch/32-MiB capacity;
  no callback under the queue mutex; inline mode retains identical semantics.
- Open questions: none for Stage 1. Vulkan page leasing and runtime integration
  remain deliberately deferred to Stages 2 and 3.
- Validation outcome: current symbols and external call sites were verified by
  targeted searches; documentation-only Stage 0 requires plan validation, not
  a build or runtime launch.

## Implementation Stages

### Stage 0: Thread-affinity and API policy

- [x] Verify every recorded command and synchronous-operation classification
  produced by the predecessor plan against current RHI/Vulkan call sites.
- [x] Assign every `FDynamicRHI` and backend-context API a required calling
  thread and one of the recorded, producer-safe, synchronous, or main-thread
  exception policies.
- [x] Identify all current uses of `GetContext`, native Vulkan command-buffer
  access, queue submission, present, frame state, dynamic allocators, and
  deferred deletion outside the proposed RHI thread.
- [x] Define queue entry ownership, admission state, monotonic serial behavior,
  bounded batch/byte capacity, wakeup, blocking, and failure-unwind contracts.
- [x] Select the steady-state dynamic-uniform/upload-page design and set a
  measurable bound for synchronous RHI round trips per representative frame.
- [x] Define startup/shutdown ordering relative to `RHIInit`, render-command
  admission, rendering-thread drain, module shutdown, and `RHIExit`.

#### Acceptance Gate

- Every backend entry point and mutable backend object has one thread owner or
  documented platform exception, and every immediate result has a deadlock-safe
  wait and lifetime contract.
- The design does not require a per-draw RHI-thread round trip and does not call
  user/render code while holding the queue mutex.

### Stage 1: RHI thread lifecycle and isolated queue tests

Dependencies: Stage 0 and the completed Recorded RHI Command List plan.

- [x] Add the RHI thread role, global owner, affinity helpers, thread name, and
  profiler registration.
- [x] Implement start, `Running`, `Draining`, stop, join, and failed-launch
  cleanup with condition-variable wakeups.
- [x] Implement the bounded FIFO queue for immutable submission groups and
  synchronous operations, including accepted/rejected ownership transfer.
- [x] Implement serial completion publication, fence wait, producer
  backpressure, and wakeup of all waiters during drain/failure.
- [x] Exercise lifecycle and queue behavior against a fake executor/context
  without starting Vulkan or the rendering thread.

#### Acceptance Gate

- Deterministic tests cover start/stop, FIFO execution, dispatch without wait,
  flush to an exact serial, bounded backpressure, rejection after close,
  synchronous operation results, failure propagation, and destruction of every
  accepted/rejected payload.
- Thread-affinity observations prove replay occurs on the RHI thread and the
  producer never executes a queued command as an overflow shortcut.

#### Stage 2 handoff

- Squashed landing baseline: `98f88ce894315e9bef59f785a76a6b51ceda60e0`
  (`dev`). Stages 0-2 share one plan commit after history consolidation, so use
  the recorded working sets and symbols rather than a per-stage commit boundary.
- Working set added by Stage 1: `CoreGlobals.h`, `RunnableThread.h/.cpp`,
  `RHIThread.h/.cpp`, `RHITests/CMakeLists.txt`, and `RHIThreadTests.cpp`.
  Stage 2 should initially add `RHICommandList.h/.cpp` and RHI-owned runtime
  configuration/diagnostics; expand to `RHIGlobals.cpp` only for the required
  startup connection and state the scope before touching Launch/RenderCore.
- Key symbols: `FRHIThread`, `FRHIThreadWork`, `FRHIThreadSubmission`,
  `FRHIThreadSynchronousResult`, `FRHIThreadStats`, `EThreadRole::RHIThread`,
  `GRHIThread`, `IsInRHIThread`, and `CheckRHIThread`.
- Decisions preserved: queue work is move-only; rejected work remains caller
  owned; accepted payloads are destroyed before completion publication;
  self-enqueue/self-wait are rejected; a consumer failure closes admission,
  destroys queued work, and wakes all producers/waiters.
- Open question for Stage 2: choose the smallest private adapter that moves
  `FSubmissionGroup` into `FRHIThreadWork` without making batch representation
  public. Runtime startup and backend `Init` may be connected only after the
  threaded executor path passes fake-context parity tests.
- Validation: 9/9 `RHIThreadTests`, 24/24 `RHICommandListTests`, and 5/5
  focused `FRunnableThreadTests` passed with the Debug test profile. No Vulkan,
  rendering thread, full build, or runtime launch was required for this
  isolated stage.

### Stage 2: Runtime startup and threaded batch replay

Dependencies: Stage 1.

- [x] Start the RHI thread during `RHIInit`, run backend initialization through
  its synchronous-operation path, and open render-command admission only after
  successful completion.
- [x] Connect `FRHICommandListExecutor` dispatch/flush to the RHI queue while
  retaining the inline mode behind the same interface.
- [x] Make the RHI thread resolve the active context and replay immutable
  batches; assert that recording paths cannot obtain the context.
- [x] Implement the specified threaded behavior for every immediate flush type,
  submit flag, additional list, and executor fence.
- [x] Add runtime configuration and diagnostics that report inline/threaded
  mode, pending batches/bytes, submitted/completed serials, wait duration, and
  backpressure events.

#### Acceptance Gate

- A normal graphics frame is recorded on the rendering thread and replayed on
  the RHI thread with identical fake-context order and Vulkan-visible output.
- Dispatch permits subsequent render recording before replay completes; flush
  waits for the correct serial without a GPU-idle wait or deadlock.

#### Stage 3 handoff

- Squashed landing baseline: `98f88ce894315e9bef59f785a76a6b51ceda60e0`
  (`dev`). The Stage 0 contract, Stage 1 queue, Stage 2 integration, and its
  preflight corrections share the single plan commit after this baseline.
- Working set: `RHICommandList.h/.cpp`, `DynamicRHI.h/.cpp`,
  `RHIGlobals.h/.cpp`, `Launch.cpp`, `LaunchEngineLoop.h/.cpp`,
  `RenderingThread.cpp`, focused RHI command-list/initialization tests, and the
  minimal Vulkan frame/shutdown affinity adjustments in
  `VulkanDynamicRHI.cpp`, `VulkanViewport.cpp`, and `VulkanDevice.cpp`.
- Key symbols: `ERHICommandListExecutorMode`, `ERHIExecutionMode`,
  `ResolveRHIExecutionMode`, `TrySubmit`,
  `FRHICommandListSubmission`, `SetThreadedMode`,
  `ExecuteSynchronousOperation`, `ERHISubmitFlags::BeginFrame`, `RHIInit`,
  `RHIBeginFrame_RenderThread`, `RHIFlushDeferredResources`, and
  `DURIN_RHI_EXECUTION`.
- Decisions: queue serials are the threaded executor timeline; the private
  submission group stays private behind a shared callable payload; begin-frame
  waits once before publishing the next backend frame counter, then dispatches
  the new frame asynchronously; backbuffer acquire is recorded; backend init
  and shutdown run as synchronous queue entries; rejected submission batches
  return to executor ownership and never publish stats or a success serial;
  backend initialization failure rolls back on its owning thread; inline is the
  default until Stage 5 explicitly enables the threaded path; executor submit
  owns serial waits, while immediate flush only derives submit flags.
- Stage 3 entry: all reviewed preflight issues are resolved; no remaining gate.
- Open questions for Stage 3: replace the temporary synchronous dynamic-uniform
  allocation with the selected per-frame mapped page lease; marshal all
  backend-mutating resource creation; finish strict Vulkan affinity assertions
  for viewport/platform exceptions and native handles.
- Validation: 31/31 `RHICommandListTests`, 4/4
  `RHIInitializationTests`, and 9/9 `RHIThreadTests` pass. The correction also
  passes a full Debug Editor `all` build, a Release Editor `RHI` build, plus
  default-inline and explicit-threaded ten-tick Vulkan smoke runs with normal
  shutdown. Active-plan validation passes.

### Stage 3: Vulkan ownership and synchronous RHI operations

Dependencies: Stage 2.

- [x] Move Vulkan command-pool/buffer recording, payload finalization, queue
  submission, present, end-frame mutation, and ordered deferred deletion under
  RHI-thread affinity assertions.
- [x] Marshal buffer/texture/sampler/shader/PSO creation that mutates backend
  state and publish returned objects only after the synchronous operation
  completes.
- [x] Implement the selected producer-safe dynamic uniform/upload allocation
  scheme with frame/page lifetime retained through RHI/GPU consumption.
- [x] Implement buffer lock/unlock ownership transfer and blocking readback/GPU-
  idle operations without exposing mutable context state to the rendering
  thread.
- [x] Preserve main-thread caller affinity for viewport create/resize while
  marshaling backend mutation or enforcing the documented exclusive phase;
  ensure acquire, command recording, present, and frame-owned state execute on
  the RHI thread.
- [x] Remove or reject every undeclared direct backend call from game/render
  paths when threaded mode is active.

#### Acceptance Gate

- Debug affinity checks remain clean across resource creation, uploads, dynamic
  uniforms, render passes, present, readback, frame end, and deletion.
- Resource-returning calls publish valid objects, upload/readback bytes remain
  deterministic, and representative frames stay within the Stage 0 synchronous-
  round-trip budget.

#### Stage 3.5 handoff

- Baseline: dev commit `49e202f31fed09e1bfbd5c7111e1c188118ceb3d`;
  the Stage 3 implementation and Stage 3.5 entry plan are part of the single
  squashed RHI commit containing this handoff.
- Working set: `RHICommandList.h/.cpp`, `DynamicRHI.h/.cpp`,
  `RHIDefinitions.h`, `RHIContext.h`, `RenderingThread.cpp`, Vulkan
  dynamic-RHI/device/descriptor/dynamic-uniform files, and focused RHI/Vulkan
  frame tests. Do not expand into present arguments or Stage 4 fence/shutdown
  work while establishing the frame-number contract.
- Key symbols: `FRHICommandListExecutor::ExecuteSynchronousOperation`,
  `ExecuteSynchronousContextOperation`, `SynchronousOperationCount`,
  `FRHIBeginFrameArgs`, `FVulkanDevice::CurrentFrameIndex`,
  `FVulkanDynamicUniformBufferAllocator::BeginFrameProducer`, `TryAllocate`,
  `ReservePage`, `FVulkanBackBuffer::UpdateSwapchain`,
  `RHIExecuteCommandBufferForBackendIntegration`, `FFrameSync::Sync`, and
  `RHIExit`.
- Decisions: producers can enqueue only context-free synchronous callbacks;
  callback-owned heap bytes participate in queue bounds; one preallocated
  mapped page per frame slot is rendering-thread suballocated only after an
  ordered BeginFrame group reads the executor-owned frame number, selects the
  backend-owned slot, and waits that slot's GPU fence; overflow allocation is
  the only additional dynamic-uniform round trip;
  viewport creation/resize mutation and all swapchain work are RHI-owned; the
  stable back-buffer wrapper records acquisition at its timeline position;
  inline mode keeps the same API while threaded affinity checks activate only
  when the RHI owner exists.
- Stage 3.5 entry: replace render-counter-driven frame identity before changing
  pacing or shutdown. Present remains implicitly associated through FIFO with
  the active executor frame; no `FRHIPresentArgs` is planned.
- Open questions for Stage 3.5: none. `GVulkanRHIDeletionFrameNumber` remains
  unchanged as the backend deletion-aging counter.
- Validation: 31/31 `RHICommandListTests`, 4/4
  `RHIInitializationTests`, 9/9 `RHIThreadTests`, and 2/2
  `VulkanRHIIntegrationTests` pass. A full Debug Editor `all` build succeeds.
  Default-inline and explicit-threaded 60-tick hidden-window Editor runs render
  and shut down normally with zero executor backpressure or rejection; the
  threaded test also covers resource publication, buffer lock/unlock, texture
  upload/readback, explicit GPU idle, uniform-page overflow, end-frame, and
  deferred deletion under Debug affinity checks.

### Stage 3.5: Executor-owned RHI frame numbering

Dependencies: Stage 3.

- [x] Add executor-owned `uint64 FrameNumber`, expose only an acquire-safe
  diagnostic/producer snapshot, and preserve it across inline/threaded mode
  transitions without deriving it from engine or rendering counters.
- [x] Make ordered BeginFrame replay construct `FRHIBeginFrameArgs` from the
  current executor frame number, and rename its payload field from
  `FrameCounter` to `FrameNumber`. Remove caller-supplied optional BeginFrame
  arguments and production fallbacks that silently discard frame identity.
- [x] Advance `FrameNumber` exactly once after each successful backend
  `RHIEndFrame` and before its submission serial completes; rejected or failed
  EndFrame work must not advance it.
- [x] Select `FVulkanDevice::CurrentFrameIndex` from the executor frame number
  on the RHI thread. After the exact BeginFrame serial completes, make the
  rendering-thread dynamic-uniform producer use the same published number/slot
  without an additional synchronous operation.
- [x] Remove Vulkan frame-slot and uniform-page selection reads of
  `GRenderFrameCounterRenderThread`; retain engine/render counters only for
  their higher-level tick and diagnostic roles.
- [x] Keep Present's existing ordered viewport/present/vsync payload and active
  backend state; do not introduce `FRHIPresentArgs`. Keep
  `GVulkanRHIDeletionFrameNumber` and its Vulkan-EndFrame increment unchanged.
- [x] Add inline/threaded tests for initial value, multiple and empty frames,
  successful EndFrame advancement, failed EndFrame non-advancement, identical
  backend/producer slots, mode transitions, and unchanged Present ordering.

#### Acceptance Gate

- Executor `FrameNumber` equals the number of successfully replayed
  `RHIEndFrame` calls in both modes, and its publication precedes the matching
  completion serial.
- Vulkan backend frame state and rendering-thread uniform allocation use the
  same executor-derived slot after BeginFrame completion; no Vulkan frame-slot
  path reads a render-thread frame counter.
- Present remains correctly ordered without a present frame argument, Vulkan
  deletion aging remains independent, and the steady-state frame acquires no
  additional RHI wait.

#### Stage 4 handoff

- Baseline: dev commit `49e202f31fed09e1bfbd5c7111e1c188118ceb3d`;
  the Stage 3 and Stage 3.5 implementations plus validated plan state are the
  single squashed RHI commit containing this handoff.
- Working set: executor frame state and replay in `RHICommandList.h/.cpp`,
  backend BeginFrame contracts in `DynamicRHI.h/.cpp`, `RHIContext.h`, and
  `RHIDefinitions.h`, Vulkan dynamic-RHI/context frame handling, migrated
  direct test callers, and focused RHI/Vulkan frame tests.
- Key symbols: `FRHICommandListExecutor::GetFrameNumber`,
  `FState::FrameNumber`, `FRHIBeginFrameArgs::FrameNumber`,
  `FDynamicRHI::RHIBeginFrame_RenderThread`,
  `FVulkanDynamicRHI::RHIBeginFrame`, and
  `FVulkanDynamicUniformBufferAllocator::BeginFrameProducer`.
- Decisions: BeginFrame identity is executor-to-backend data only; successful
  EndFrame return publishes the next number before serial completion; global
  executor lifetime, not backend lifetime, owns continuity; Present remains
  implicit and Vulkan deletion aging remains independent.
- Stage 4 scope remains the reviewed queue-authoritative serial, lifetime-safe
  render fence, scoped viewport deletion, pacing integration, and terminal
  drain work. Do not reopen frame identity or introduce present arguments.
- Validation: focused `RHICommandListTests`, `RHIInitializationTests`, and
  `VulkanRHIIntegrationTests`, the complete native-test aggregate, and the
  full Debug Editor `all` build pass. Inline and threaded 60-tick hidden-window
  Editor runs exit with zero executor backpressure or rejection.

### Stage 4: Frame pacing, flush integration, and shutdown drain

Dependencies: Stage 3.5.

- [ ] Make empty flush and fence targets capture the queue-authoritative last
  accepted serial so lifecycle and synchronous producers cannot be missed and
  executor-side serial snapshots cannot regress under concurrency.
- [ ] Replace render-fence callbacks that capture the fence object's raw
  `this` with shared lifetime-safe state carrying render completion and, for an
  RHI-inclusive fence, the exact RHI serial. Rejection/failure must wake its
  waiter instead of leaving an incomplete fence.
- [ ] Integrate the exact serial payload into `FFrameSync::Threads`, final
  render drain, and explicit RHI-inclusive render-command fences. Preserve the
  existing two-slot render-thread end-frame pacing and Vulkan frame-slot GPU
  throttle; ordinary EndFrame pacing must not wait for full RHI or GPU idle.
- [ ] Ensure `EndFrame`, present, GPU submission, completion publication, and
  resource deletion preserve their declared order without whole-device idle.
  Viewport teardown may retire only its own swapchain resources and must not
  immediately clear unrelated device deferred-deletion entries.
- [ ] Close render admission first, submit the final accepted render batch,
  then atomically install the internal backend-shutdown marker while
  transitioning RHI admission to draining. No public work may be accepted
  after that marker.
- [ ] Run the final render-resource and RHI deletion audits before backend
  shutdown; then execute backend `Shutdown` on the RHI thread, stop/join it, and
  complete `RHIExit`.
- [ ] Reject late render/RHI work synchronously and wake every blocked producer
  or fence waiter on shutdown and failure paths.
- [ ] Keep the Stage 3.5 executor frame-number and implicit Present contract
  unchanged; do not introduce `FRHIPresentArgs` or return to render-counter-
  driven backend slots while integrating pacing.

#### Acceptance Gate

- Thread-only flush drains both CPU stages but not the GPU; explicit GPU-idle
  remains distinct and testable.
- Concurrent synchronous/lifecycle enqueue versus empty flush cannot miss an
  accepted serial; destroying or force-releasing a pending render fence cannot
  access freed state or strand a waiter.
- Repeated startup/shutdown, empty frames, queued resource release, present
  failure, multi-viewport teardown, and close-with-backpressure tests terminate
  with zero render commands, RHI batches, waiters, retained resources, and
  deferred deletions. Scoped viewport retirement never releases unrelated
  in-flight device resources.

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
| Successful RHI EndFrame -> executor frame number | `FrameNumber` advances once before serial completion; failure/rejection does not advance it. |
| Immediate API -> returned result | Create, allocate, lock, and readback paths publish valid data with explicit wait/lifetime behavior. |
| Upload page -> frame lifetime | Executor-derived backend and producer slots match; CPU upload storage survives replay and required GPU use before recycling. |
| End frame -> present/submit/delete | Present uses ordered active-frame state without a frame argument; events cannot overtake earlier resource use or require ordinary device idle. |
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
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
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
