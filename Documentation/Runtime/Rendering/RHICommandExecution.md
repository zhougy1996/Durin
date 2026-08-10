# RHI Command Execution

Summary: Define recorded GPU command ownership, submission, synchronization, and RHI-thread execution.

Modules: RHI

Durin's renderer records backend-neutral RHI commands into owned command-list
storage and replays them through one executor. The normal runtime uses one
dedicated RHI thread; setting `DURIN_RHI_EXECUTION=inline` keeps the same
executor contract but replays on the rendering thread for focused diagnostics.
`Immediate` identifies the primary timeline and coordination surface, not a
direct backend-call path.

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

There is no bypass path. Normal rendering, immediate lists, and regular lists
all record before executor replay.

## Ownership Through Replay

Each recorded command owns the data needed after its caller returns. Raw byte
payloads, attachment values, parameter arrays, and upload rows are copied into
command storage. Referenced RHI resources are retained until the containing
submission group has replayed its commands and ordered backend events.

The recorder never obtains an `IRHICommandContext` or native Vulkan command
buffer. Only executor replay resolves the active context and invokes it. This
keeps an immutable batch movable to another CPU thread without changing its
payload or resource-lifetime contract.

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

The queue admits at most 8 queued-or-active entries, 16 batches, and 32 MiB of
owned payload. A producer that would cross a bound waits for capacity and wakes
on completion, failure, or admission close. Admission is explicit
`Stopped -> Running -> Draining -> Stopped`; rejection never consumes the
producer's work. The RHI thread is the sole runtime owner of command-context
replay, Vulkan command-pool/buffer mutation, graphics and present queue
submission, frame state, and ordered backend deletion.

Ordered events execute in this sequence:

1. replay every batch;
2. optionally submit backend commands;
3. optionally end the frame and, only after successful backend `RHIEndFrame`,
   advance the executor-owned `FrameNumber` once;
4. release all batch-owned payloads and RHI references;
5. optionally drain deferred RHI deletion;
6. publish the completed serial.

An executor fence targets one exact accepted serial. CPU completion means replay
and the ordered executor events above have finished; it does not imply GPU idle.
Vulkan submission tokens and their pooled fences represent queue completion.

The executor `FrameNumber` starts at zero and advances only after a successful
replayed `RHIEndFrame`; callers do not supply it. Ordered `BeginFrame` passes the
current number to the backend. Present uses the ordered active-frame state and
has no independent frame-counter argument. Vulkan frame pacing, dynamic-uniform
producer selection, descriptor-pool reuse, and native retirement use exact GPU
completion tokens rather than deriving safety from that frame number.

## Flush And Synchronous Operations

Immediate flush behavior is explicit:

- `WaitForOutstandingTasksOnly` does not seal or submit RHI work.
- `DispatchToRHIThread` seals and dispatches through the configured executor.
  Inline mode completes before it returns but does not count as a caller wait.
- `FlushRHIThread` dispatches and waits for the resulting executor fence.
- `FlushRHIThreadFlushResources` also performs ordered deferred deletion after
  preceding batch references have been released.

Buffer and texture uploads are recorded and own their source bytes. Operations
that must return a completed result—buffer lock scopes, texture readback,
back-buffer acquisition, GPU-idle waits, resource creation, viewport resize,
and backend-dependent allocation—use declared synchronous executor operations.
They first complete required earlier recorded work and execute backend mutation
on the RHI thread. The normal `ExecuteSynchronousOperation` surface is terminal:
a waiter always targets the exact accepted serial, and operation failure or
admission rejection never falls back to producer-thread backend work.

### Fallible Resource Creation

`ExecuteFallibleSynchronousOperation` is the narrow exception for expected
runtime resource-creation failure. Its `FRHIFallibleOperationResult` owns a
success flag and diagnostic text. In threaded mode, the queued wrapper catches
the creation exception, records operation failure, and completes the queue entry
successfully so its serial and later admitted work remain valid. Inline mode
returns the same result directly. A Vulkan factory already executing on the RHI
thread catches at the same boundary without enqueueing and waiting on itself.

Nullable runtime factories translate only that operation-failure result into a
null RHI reference. Buffer, texture, shader, graphics-pipeline, sampler, and
vertex-declaration factories must otherwise publish a complete resource; a
non-null wrapper with an invalid native handle or allocation is forbidden.
Partially built native candidates clean up immediately on the RHI thread, and a
later caller-defined or Renderer-generation retry may create a fresh candidate.

Graphics-pipeline initialization owns complete portable rasterizer,
multisample, depth/stencil, per-active-attachment blend/write-mask, structural
vertex-input, topology, shader, reflected-layout, and render-target state.
`BuildGraphicsPipelineStateKey` validates and canonicalizes every field before
fallible backend creation, so the public result is always a compatible complete
PSO or null. Non-indexed and indexed command records carry exact instance,
first-location, and base-vertex arguments. See
[Graphics State and Bindings](GraphicsStateAndBindings.md).

Executor admission, serial wait, replay-context, queue, device-loss, and
already-recorded command failures do not become a fallible result. Startup
instance, device, and allocator creation also stays outside this surface: it
aborts `RHIInit()`, rolls back the unpublished backend, and publishes one owned
initialization diagnostic. Frame-critical staging, upload, readback, submission,
presentation, and dynamic-uniform overflow remain terminal unless their own
public contract explicitly permits failure.

At `BeginFrame`, Vulkan polls its contiguous completion watermark, waits only
the exact token required by the selected pacing slot, and selects a descriptor-
pool batch whose maximum use token is complete. The rendering-thread boundary
then selects one of two completion-eligible dynamic-uniform producer states.
Each has a preallocated 4 MiB mapped base page; ordinary aligned suballocation
needs no additional RHI round trip, while bounded page overflow synchronously
reserves an RHI-owned chunk.

Dynamic storage ranges use the same frame-slot lease principle but remain a
separate allocation class. `AllocateDynamicStorageBuffer` copies an exact
nonzero byte range into mapped frame-local storage and returns a retained
`FRHIStorageBufferRange`; admission requires the published storage alignment
and maximum range. Vulkan aligns offsets to
`MinStorageBufferOffsetAlignment`, caps each frame at 64 MiB and 16 chunks, and
reclaims a slot only after its frame fence completes. Exhaustion or an invalid
range returns an empty value without changing earlier allocations.

A returned range begins in `HostWrite`. Its owner must record an exact
`HostWrite -> GraphicsShaderRead` transition before a draw consumes it and bind
the same offset and size through reflected storage-buffer parameters. Command
records retain the underlying buffer through replay, so inline and dedicated
RHI-thread execution have identical lifetime and range semantics.

Ordinary end-of-frame dispatch is not a GPU-idle boundary. `SubmitToGPU`,
`EndFrame`, present-related context work, and `DeleteResources` remain ordered
relative to recorded commands without adding a device-wide wait.

Vulkan reserves a queue token while recording and publishes it only after
successful submission. CPU replay completion can release command-list storage,
but submitted payloads, command buffers, fences, native dependencies, transfer
ranges, uniform pages, and descriptor pools remain retained until their exact
token completes. See [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md).

## Runtime Drain And Diagnostics

`FFrameSync::EndFrame` preserves the two-slot render-command pacing fence and
does not wait for RHI completion. A `Threads` sync, including
`FlushRenderingCommands`, retains shared fence state, submits pending immediate
work, and waits the exact RHI serial. Destroying the fence object cannot leave
its queued callback with a dangling address; render rejection or RHI failure
makes the shared state terminal and wakes waiters.

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

- [Runtime lifecycle](../Core/RuntimeLifecycle.md)
- [Viewport rendering](ViewportRendering.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md)
- [Dedicated RHI Thread plan](../../Plans/Archive/2026-08/DedicatedRHIThread.md)
- [Build and run](../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
