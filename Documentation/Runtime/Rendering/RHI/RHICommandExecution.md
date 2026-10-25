# RHI Command Execution

Summary: Define recorded GPU command ownership, submission, synchronization, and RHI-thread execution.

Modules: RHI

Durin's renderer records backend-neutral RHI commands into owned command-list
storage and replays them through one executor. The normal runtime uses one
dedicated RHI thread; setting `DURIN_RHI_EXECUTION=inline` keeps the same
executor contract but replays on the rendering thread for focused diagnostics.
`Immediate` identifies the primary timeline and coordination surface, not a
direct backend-call path.

Backends share `IsInRHIExecutionThread()` and `CheckRHIExecutionThread()` from
`RHIThread.h` for execution affinity. When `GRHIThread` exists, only that exact
owner is accepted. Without an RHI thread, execution belongs to the rendering
thread. The registered game thread is accepted only when no rendering thread
exists, so initialization, shutdown, and synchronous single-threaded operation
use the same fallback. A live rendering thread never grants game-thread backend
execution permission. Unknown, worker, and IO threads remain rejected.

`RHIInit`, `RHIInitWithBackendForTests`, and `RHIExit` require the registered game
thread and no live rendering thread. Initialization precedes rendering-thread
launch; shutdown follows rendering-thread stop and join. These entry preconditions
are checked in every build configuration. Actual ordering comes from the managed
thread lifecycle, command queues, fences, and joins. Affinity checks do not acquire
locks or temporary execution permissions; there is no rendering-loop ownership
scope. Thread publication/removal must be synchronized with callers, as with the
rest of the RHI lifecycle. `IsInRHIThread()` retains its thread-role meaning;
`IsExecutingRHICommands()` describes replay scope.

Metal checks lifecycle, command-context mutation and uniform allocation through
the same execution-affinity API, retaining its uniform command-replay checks.
Thread-safe creation factories, pipeline compiler tasks, synchronized snapshots,
and driver completion callbacks retain their own concurrency contracts.

Platform presentation targets are prepared on the game thread. In inline mode,
Mona dispatches viewport creation and resize to the rendering thread and waits
for completion before publishing their result. Vulkan also dispatches native
viewport creation when its game-thread entrypoint is used directly; rendering
execution can create or resize inline without another dispatch. Metal's inline
viewport mutations use the same execution-affinity check. These runtime paths
do not grant game-thread access while a rendering thread is running.

## Results and diagnostics

Detailed resource, transfer, transition, indirect-command, and shader-binding
validation belongs to `Backend/RHIValidation.h`. RHI recording and backend
execution own the checks and diagnostics; ordinary consumers do not receive
operation-specific validation errors. Attachment selection may query
`IsTextureViewDescValid` without consuming diagnostic details. Pipeline admission
may report `InvalidDescription`.
`ToString` formats at presentation boundaries; semantic tests assert codes and
context, never classify by text. Formatting lives in `RHIErrorStrings.cpp`.

`RHICreateTexture`/`RHICreateBuffer` return a complete resource or null;
backends log recoverable creation failures locally. Thread work
callbacks return `void`; the queue does not catch execution exceptions. An
exception escaping the RHI worker terminates the process; inline execution
propagates it to the caller. Explicit external failure notification stops
admission, releases queued work, and wakes waiters. `FailedSerial` records that
terminal state without retaining exception text. Synchronous submission returns
admission and completion status without an error payload.
Backend `Init` owns partial-state cleanup before propagating its original
exception to the initialization operation's handler. The RHI owner
then destroys the failed backend and releases its module; it does not call
runtime `Shutdown` on a partially initialized backend. Device/invariant failures
stay terminal.
Vulkan startup propagates original enumeration and native-creation exceptions
to that initialization boundary. Direct native resource factories translate
recoverable failures into null resources; ordered synchronous factories
retain the narrow creation-exception boundary described below. Unpublished
buffer, image, and view candidates use scope ownership for failure cleanup.
Vulkan startup and swapchain configuration may use local human-readable reasons;
success follows result state and native retry policy follows `vk::Result`.

## Command-List Roles

`FRHICommandListBase` owns the common recording surface. Both concrete list
types always use the same typed command representation:

- `FRHICommandListImmediate` is the executor-owned primary timeline. It adds
  flush, frame, synchronous-result, and regular-list insertion operations.
- `FRHICommandList` is a movable finite recorder. `FinishRecording()` validates
  and changes it from recording to finished without executing or submitting
  anything. A finished list can be admitted exactly once.

`FRHICommandBatch` is the RHI module's private immutable transfer boundary.
Renderer-facing code never receives or submits one directly. Queueing a
finished regular list seals any earlier immediate segment, inserts the regular
batch at that point, and lets later immediate recording continue in a new
segment. Lists supplied to the compatibility additional-list submit boundary
follow the current immediate segment in argument order.

`FRHICommandListImmediate::DispatchToRHIThread()` explicitly dispatches the
pending ordered prefix and returns its executor serial. It requests neither a
completion wait nor native GPU submission. Threaded queue admission may apply
backpressure; inline executors replay on the caller. `IsRHIThreadEnabled()`
queries the owning executor mode without collecting statistics.
`CanDispatchToRHIThread()` checks for open render passes, diagnostic/timing
scopes, buffer locks, and GPU submissions before an opportunistic dispatch. Queueing a
regular list alone does not dispatch it.

There is no bypass path. Normal rendering, immediate lists, and regular lists
all record before executor replay.

## Ownership Through Replay

Each recorded command owns the data needed after its caller returns. Raw byte
payloads, attachment values, parameter arrays, and upload rows are copied into
command storage. The executor retains this storage through replay and ordered
events. `RHISetReplayStorageOwner` additionally gives GPU contexts an owning
storage lease before replay begins. Vulkan attaches that lease to every
payload touched by the group, including intermediate upload/readback
submissions and work recorded without `SubmitToGPU`. Clearing the active
replay owner does not clear pending or in-flight payload leases. CPU-only
contexts need no GPU retention.

Backend pending shader bindings also retain their resource views independently
of command storage. A partial parameter update preserves unchanged bindings.
When rebuilding the pending owner snapshot, acquire the new references before
releasing the old snapshot; an unchanged explicit view may have no other owner.
Descriptor cache or in-flight submission ownership must not be required to keep
an undispatched pending binding alive.

Explicit GPU submission replay resolves `RHIGetQueueContext` from the default
context. Pipeline and non-pipeline commands use that selected context until
the matching end restores the default context. Pipeline switches change the
command domain without changing the selected physical queue. Each newly
selected context receives the group's storage owner before its first command;
normal completion and exception unwinding detach active owners from every
touched context. Their retained GPU leases remain independent. Single-context
backends keep the default resolver and validate their supported queue at begin.

The recorder never obtains an `IRHICommandContext` or native Vulkan command
buffer. Only executor replay resolves the active context and invokes it. This
keeps an immutable batch movable to another CPU thread without changing its
payload or resource-lifetime contract.

Recorded diagnostic regions and GPU timing begin/end commands follow the same
ownership and replay rules. Region names and timing queries are retained by the
command batch; unbalanced/crossed regions or timing intervals invalidate
admission before replay. Timing result reads are nonblocking observations, not
synchronous-result operations. See
[RHI Diagnostics and Conformance](RHIDiagnosticsAndConformance.md).

Regular recorder states are `Recording -> Finished -> Admitted`. Recording
after finish, admission before finish, double admission, an unbalanced render
pass, or an open buffer lock fails at the closest CPU boundary. Moving a list
transfers its exact state and storage and leaves the source inert.

## Submission And Completion

The executor groups pending immediate segments and admitted regular lists. In
threaded mode the bounded FIFO queue assigns each accepted group a monotonically
increasing nonzero serial while holding the queue lock; empty flushes and fences
capture that same queue-owned last-accepted serial. Inline mode preserves the
same serial and completion behavior locally. Empty work without an ordered
event does not manufacture a serial or backend submission.

The queue admits at most 8 queued-or-active entries and 3 frame-end boundaries,
with backpressure budgets of 16 batches and 32 MiB of owned payload. A single
indivisible submission above either budget waits for an empty queue and runs alone;
later work waits for its completion. Each submission carrying `EndFrame`
charges one frame until replay finishes or rejects the work. This is a CPU
queue latency limit, independent of Vulkan's two-slot GPU pacing. A producer
that would cross a bound waits for capacity and wakes
on completion, failure, or admission close. Admission is explicit
`Stopped -> Running -> Draining -> Stopped`; rejection never consumes the
producer's work. The RHI thread is the sole runtime owner of command-context
replay, Vulkan command-pool/buffer mutation, graphics and present queue
submission, frame state, and ordered backend deletion.

Ordinary dispatches can replay a ready ordered prefix before later PSO dependencies
complete. Replay pauses only at recorded closed scopes, never inside a GPU
submission, render pass, timing/diagnostic interval, buffer lock, or split
transition. Immediate GPU-submission ends preserve such boundaries in storage.
The partial work keeps its queue serial, capacity charge, and storage owners;
later entries cannot overtake it, and its fence remains pending. Ready adjacent
scopes replay together without another queue round trip. Dependency failure or
cancellation discards the remaining suffix and publishes the group's failed or
canceled outcome; it cannot undo the already executed prefix. Frame/resource
event submissions retain an aggregate dependency gate and execute their events
only after all dependencies succeed. Drain preserves readiness waits, while
explicit external failure cancels queued continuations and wakes waiters.

Ordered events execute in this sequence:

1. replay every batch;
2. optionally submit backend commands;
3. optionally end the frame and, only after successful backend `RHIEndFrame`,
   advance the executor-owned `FrameNumber` once;
4. release the executor's command-storage ownership, preserving backend leases;
5. optionally drain deferred RHI deletion;
6. publish the completed serial.

An executor fence targets one exact accepted serial. CPU completion means replay
and the ordered executor events above have finished; it does not imply GPU idle.
Vulkan submission tokens and their pooled fences represent queue completion.

`FRHIGPUSyncPoint` is a noncopyable one-shot metadata object; owning
`FRHIGPUSyncPointRef` copies retain the same identity from recording through
completion. It uses RHI intrusive reference counting with immediate metadata
destruction, not GPU-resource deferred deletion. `IsComplete()` is a thread-safe
metadata read and is true only after successful native completion and required
result processing. `GetState()` distinguishes Pending, Submitted, Complete,
Canceled, Failed and DeviceLost; an empty reference reports Invalid. Observers
survive backend teardown without holding a native fence or backend pointer.

Physical queue coordinates, reservation mutation and single-assignment binding
are confined to `Backend/RHICompletionBackend.h`. A recording creates no native
reservation. Replay associates the same logical object with its producing
payload; multiple logical objects can share one authoritative reservation.
Queue capabilities still publish physical identity separately from command
roles; roles may share one queue. Foreign/replacement-device backend queries
return Invalid even when the old object's retained metadata says Complete.

`FRHIGPUQueueTimeline::CanSubmitBatch` preflights an ordered pending prefix
without changing signal states. Every earlier pending reservation must appear
in that prefix; submitted and canceled positions may be omitted. Duplicate,
foreign, terminal, and out-of-order signals reject admission. Native acceptance
still precedes each `MarkSubmitted` call.

`FRHIRetirementPrerequisites` retains exact deduplicated logical uses, including
unassociated recordings. Adding a use may prune already retireable entries.
Cancellation remains distinct from successful content production. An associated
canceled signal becomes retireable only after its earlier queue prefix retires;
an unassociated canceled recording is retireable after executable owners detach.
Failure and device loss never authorize ordinary retirement. Success dependencies
retain every producer and never replace failed/canceled output with a later value.

`RHIGetCompletionStatus` reads published metadata without dispatch or GPU waits.
`RHIWaitForCompletion` waits an exact submitted sync point with a GPU timeout and
returns separate pending, timeout, cancellation, failure and device-loss
outcomes. CPU dispatch latency is outside that GPU timeout. The wait does not
submit pending GPU work or infer completion from another device generation.
`WaitForRHIGPUSyncPoint` first observes terminal metadata and is safe after
backend shutdown. Pending recordings return Pending without dispatching replay
or blocking the backend thread on its own work; submitted signals route through
the active backend. Observer destruction never cancels executable work.

The executor `FrameNumber` starts at zero and advances only after a successful
replayed `RHIEndFrame`; callers do not supply it. Ordered `BeginFrame` passes the
current number to the backend. Present uses the ordered active-frame state and
has no independent frame-counter argument. Vulkan frame pacing, binding-version
reuse, descriptor-pool reuse, and native retirement use exact GPU
completion tokens rather than deriving safety from that frame number.

## Flush And Synchronous Operations

Immediate flush behavior is explicit:

- `WaitForOutstandingTasksOnly` does not seal or submit RHI work.
- `DispatchToRHIThread` seals and dispatches through the configured executor.
  Inline mode completes before it returns but does not count as a caller wait.
- `FlushRHIThread` dispatches and waits for the resulting executor fence.
- `FlushRHIThreadFlushResources` also performs ordered deferred deletion after
  preceding batch references have been released.

Buffer and texture uploads are recorded and own their source bytes.
`WriteBuffer` leaves the written range in `TransferWrite` for an explicit
caller transition or a graph-managed next barrier. Texture updates follow the
same contract for their affected mip/slice. Neither restores canonical access.
Texture uploads use immutable packed payloads; graph recording retains the same
allocation without another CPU copy. Updates are recorded directly on the
command list, with no DynamicRHI forwarding layer. Write-only buffer locks allocate CPU temporary storage and transfer it
into `WriteBuffer` at unlock without copying the bytes; they do not provide
native mappings. Uniform resources use `UpdateUniformBuffer`; ordinary buffers
use ordered `WriteBuffer` commands. Operations
that must return a completed result—texture readback,
back-buffer acquisition, GPU-idle waits, resource creation, viewport resize,
and backend-dependent allocation—use declared synchronous executor operations.
They first complete required earlier recorded work and execute backend mutation
on the RHI thread. The normal `ExecuteSynchronousOperation` surface is terminal:
a waiter always targets the exact accepted serial, and operation failure or
admission rejection never falls back to producer-thread backend work.

Command recording canonicalizes raw buffer and texture shader parameters and
implicit render-pass attachments into immutable views. That path uses
`RHIGetOrCreateBufferView` and `RHIGetOrCreateTextureView`: a backend may reuse
an exact resource-plus-description identity, while explicit `RHICreate*View`
calls remain creation-only factories. Vulkan retains a thread-safe bounded
working set of these automatic views, refreshes entries on use, and removes
entries after 120 unused frames or capacity pressure. A cache miss remains one
fallible synchronous native creation; a hit performs no RHI-thread round trip.
Graph raster callbacks lower their frozen attachment declarations with
`MakeRDGNativeAttachmentBinding`, retaining the view owners until `BeginRenderPass`
records its own references. Native color/depth binding validates the full declared
layout. Forward, bootstrap, deferred, retained and sorted-translucency pipeline
layouts use the same graph-boundary policy; they leave attachments in attachment
access and rely on RDG for subsequent sampled transitions.

Texture identity includes the native backing image and its backing-set
generation. Stable swapchain back-buffer wrappers therefore reuse one view per
image while swapchain recreation cannot expose a stale view.
Recorded commands retain their returned view references independently of cache
eviction, and backend shutdown clears the cache before device teardown so view
destruction continues through ordinary ordered and GPU-safe retirement.

### Nullable Resource Creation

Runtime resource factories return complete references or null. Vulkan translates
recoverable native errors locally, logging native status without transporting a
failure classification through RHI or RenderCore. Cache capacity exceptions remain
internal to construction and request admission. Device loss and invariant errors
propagate as terminal exceptions.

Allocation failures retain their native result and counters. Resource factories
own the creation failure log; allocation helpers do not capture heap-budget
snapshots or repeat that log. Heap budgets remain available through explicit
memory statistics queries.

Buffer, texture, texture-view, shader, graphics-pipeline, compute-pipeline, and
sampler factories must publish complete resources. A non-null wrapper with an
invalid native handle or allocation is forbidden. Vertex declarations copy CPU
descriptions directly. Context-dependent creation uses ordinary synchronous RHI
execution; the executor does not wrap creation exceptions.

Pipeline creation returns its native result explicitly. The failure branch
releases the earlier layout and any returned pipeline before error translation;
no generic creation guard is involved. Constructors do not
promise rollback for terminal exceptions from naming or container allocation.
A later preparation may create a fresh candidate after a recoverable failure.
Vulkan vertex declarations only copy CPU metadata and are created directly,
without native-failure translation or RHI-thread replay.

These readiness postconditions apply to all native buffer factories, including
structured and byte-address buffers. `CreateUniformBuffer` returns a dedicated
layout-defined resource with owned initial contents; its native backing is
resolved during consuming replay. It enforces input preconditions and allocates
pending contents and backend placement on demand. Uniform native allocation
failure during consuming replay follows terminal replay failure rather than a
recoverable native-factory result. See
[resource views](RHIResourceViewsAndTransfers.md#uniform-and-storage-buffers).

Graphics-pipeline initialization owns complete portable rasterizer,
multisample, depth/stencil, per-active-attachment blend/write-mask, structural
vertex-input, topology, shader, reflected-layout, and render-target state.
`BuildGraphicsPipelineStateKey` validates and canonicalizes every field before
fallible backend creation, so the public result is always a compatible complete
PSO or null. Non-indexed and indexed command records carry exact instance,
first-location, and base-vertex arguments. See
[Graphics State and Bindings](GraphicsStateAndBindings.md).

Single-command indirect operations retain their GPU-authored argument buffer
and copied byte offset through replay. `DrawIndirect`,
`DrawIndexedIndirect`, and `DispatchIndirect` return void and reject invalid
recording through enforced preconditions: a null or Storage buffer,
missing `DrawIndirect` usage, non-four-byte alignment,
an undersized record range, unsupported capability, wrong pipeline/render-pass
domain, missing PSO, or dependency-admission failure before recording. Invalid
usage is an invariant failure; callers select a supported rendering path from
published capabilities before recording. No argument bytes are mapped or copied
and no hidden CPU command loop exists.

Compute pipeline selection, reflected binding, and direct or indirect dispatch use the same
recorded ownership and executor rules without creating another queue. See
[Synchronous Compute Pipelines](SynchronousComputePipelines.md).

Executor admission, serial wait, replay-context, queue, device-loss, and
already-recorded command failures do not become a fallible result. Startup
instance, device, and allocator creation also stays outside this surface: it
aborts `RHIInit()`, rolls back the unpublished backend, and publishes one owned
initialization diagnostic. Frame-critical staging, upload, readback, submission,
presentation, and admitted native-backing allocation remain terminal unless their own
public contract explicitly permits failure.

At `BeginFrame`, Vulkan polls completion, waits the exact prerequisites of its
selected pacing slot, and prepares a completion-eligible descriptor pool batch.
The render thread dispatches this boundary asynchronously through
`DispatchToRHIThread`; it can prepare later frames while the RHI lane waits.
The `RHI.BeginFrame.Dispatch` scope measures dispatch, including bounded queue
pressure when applicable. Queue statistics expose current/peak queued frames
and frame-pressure wait count/duration separately from generic backpressure.
Uniform objects own pending initial bytes and a backend allocation; updates
copy payloads into commands and replace the allocation on ordered replay.
Storage retains a mutable CPU copy for partial updates and directly owns its
current backend allocation. Commands own only their patch bytes. Native pages are
materialized on the RHI thread.
There is no render-thread mapped producer, frame-slot reset, or synchronous
upload overflow allocation. The old dynamic allocation APIs and untyped uniform
update overload have been removed.

`CreateUniformBufferRange` is a convenience for recording-only consumers. It
creates a typed uniform resource and returns a logical range with counted
ownership, so preparation state can outlive the creating list. Callers needing
a separately typed resource use `CreateUniformBuffer`. Both require valid
input and allocate contents on demand without caller-managed upload quotas.
Shader recording retains logical resources, and backend bindings retain captured
allocations through GPU completion. Storage ranges must not enter explicit
native-buffer state transitions. See
[resource contracts](RHIResourceViewsAndTransfers.md#uniform-and-storage-buffers).

Ordinary end-of-frame dispatch is not a GPU-idle boundary. `SubmitToGPU`,
`EndFrame`, present-related context work, and `DeleteResources` remain ordered
relative to recorded commands without adding a device-wide wait.

Vulkan reserves a queue token while recording and publishes it only after
successful submission. CPU replay completion can release command-list storage,
but backend command-storage leases, submitted payloads, command buffers, fences, native dependencies, transfer
ranges, uniform pages, and descriptor pools remain retained until their exact
token completes. See [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md).

## Recorded GPU Submissions

`BeginGPUSubmission` copies a physical queue ID and owning dependency sync points
into the recording and returns a pending `FRHIGPUSyncPointRef`.
`EndGPUSubmission` closes the scope; scopes cannot nest or cross a render-pass
boundary. A regular recorder must close the scope before `FinishRecording`.
The immediate recorder may replay or submit a CPU segment inside a scope.
Both recorded boundary commands and the open scope share its cancellation
lease, so retiring an early native payload cannot cancel the later signal.
Discarding all executable owners cancels an unassociated signal.

Backend replay binds each signal once to backend reservation metadata and the
payload retains that same object. Callers never resolve a second handle. The
recording lease's final detach cannot cancel a bound signal; its native owner
now decides completion or failure. Canceling an unassociated recording never
cancels already-submitted native work. Sync points certify covered GPU work,
not the success of a graph callback.

Vulkan accepts provisioned physical queues. Dependencies must identify live or
completed work owned by this device. Same-queue waits lower to FIFO ordering
and resource barriers; consecutive logical batches on a single-queue device can
share one native reservation. Their boundaries do not finalize command buffers or
interrupt surrounding diagnostic and GPU-timing scopes. Existing explicit
submission, frame pacing and upload-pressure boundaries still submit payloads.
Cross-queue dependencies lower to native timeline semaphore waits and preserve
release/acquire ownership barriers; no per-pass CPU wait is introduced.

Explicit `ReleaseQueueOwnership` and `AcquireQueueOwnership` commands retain
shared transfer pairs through replay and route each side by its physical queue
identity. Acquire can record after release without an intermediate native submit;
the coordinator validates and orders their sealed payloads. Their lifetime and placement rules are defined in
[RHI resource transitions](RHIResourceTransitions.md). This explicit protocol
does not change GPU submission scope routing or enable production async compute.

## Runtime Drain And Diagnostics

`FFrameSync::EndFrame` preserves the two-slot render-command pacing fence and
does not wait for RHI completion. A `Threads` sync, including
`FlushRenderingCommands`, submits pending immediate work and waits the exact RHI
serial. Render commands trigger a shared `Tasks::FTaskEvent`; the GameThread-owned
fence stores its `Tasks::FTask` and, for RHI-inclusive fences, an independently
retained RHI receipt published before the event completes. Completion remains
visible whether it precedes or races a wait. Destroying or restarting the fence
does not invalidate queued event/receipt captures. RHI submission or execution
failure is terminal rather than a successful flush. Task-event admission remains
open until rendering and RHI have stopped; render-producing task owners must
close admission and join before render teardown. See [runtime shutdown](../../Application/RuntimeLifecycle.md).

Render shutdown closes render-command admission, drains every accepted command,
flushes the immediate timeline and deferred deletion, then verifies all of the
following before `RHIExit()`:

- both render command queues and the active-command count are zero;
- executor pending-batch count is zero;
- submitted and completed serials match;
- the render-resource registry, deferred C++ cleanup, and pending RHI deletion
  are empty.

`RHIExit()` performs the final resource/deletion flush, then installs a
zero-payload backend-shutdown marker and changes RHI admission to `Draining` in
one queue critical section. No public work can land after that marker. Blocked
producers wake and reject; the game thread waits the marker's exact serial,
switches the executor to inline diagnostics only after it completes, stops and
joins the RHI thread, audits zero outstanding entries/batches/bytes, and finally
releases the backend owner.

The final rendering-thread audit logs cumulative command count, payload bytes,
submitted batches, submission groups, replay time, wait count and duration,
synchronous-operation count, peak queued-or-active entries/batches/bytes,
backpressure, and rejected submissions. `FRHICommandListExecutor::GetStats()`
exposes the same CPU-side snapshot. These values diagnose executor stalls and
queue sizing; they are not GPU timing measurements.

## Related Documentation

- [RHI public API stability](RHIPublicAPIStability.md)
- [Runtime lifecycle](../../Application/RuntimeLifecycle.md)
- [Render resource lifecycle](../Pipeline/RenderResourceLifecycle.md)
- [Viewport rendering](../Pipeline/ViewportRendering.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md)
- [RHI diagnostics and conformance](RHIDiagnosticsAndConformance.md)
- [Dedicated RHI Thread plan](../../../Plans/Archive/2026-08/DedicatedRHIThread.md)
- [Build and run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
