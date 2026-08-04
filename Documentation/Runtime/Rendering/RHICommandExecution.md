# RHI Command Execution

Durin's renderer records backend-neutral RHI commands into owned command-list
storage and replays them through one executor. The current production executor
runs inline on the rendering thread; `Immediate` identifies the primary
timeline and coordination surface, not a direct backend-call path.

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

The executor groups the pending immediate segments and admitted regular lists,
assigns each nonempty group a monotonically increasing nonzero serial, and
replays its batches in timeline order. Empty work without an ordered event does
not manufacture a serial or backend submission.

Ordered events execute in this sequence:

1. replay every batch;
2. optionally submit backend commands;
3. optionally end the frame;
4. release all batch-owned payloads and RHI references;
5. optionally drain deferred RHI deletion;
6. publish the completed serial.

An executor fence targets the last accepted serial. CPU completion means replay
and the ordered executor events above have finished; it does not imply GPU idle.
Vulkan queue and frame fences continue to represent GPU completion.

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
back-buffer acquisition, GPU-idle waits, and backend-dependent allocation—use
declared synchronous executor operations. They first complete required earlier
recorded work. CPU-only dynamic uniform suballocation does not split an active
render pass merely to preserve this coordination rule.

Ordinary end-of-frame dispatch is not a GPU-idle boundary. `SubmitToGPU`,
`EndFrame`, present-related context work, and `DeleteResources` remain ordered
relative to recorded commands without adding a device-wide wait.

## Runtime Drain And Diagnostics

Render shutdown closes render-command admission, drains every accepted command,
flushes the immediate timeline and deferred deletion, then verifies all of the
following before `RHIExit()`:

- both render command queues and the active-command count are zero;
- executor pending-batch count is zero;
- submitted and completed serials match;
- the render-resource registry, deferred C++ cleanup, and pending RHI deletion
  are empty.

The final rendering-thread audit logs cumulative command count, payload bytes,
submitted batches, submission groups, inline replay nanoseconds, waits, and
rejected submissions. `FRHICommandListExecutor::GetStats()` exposes the same
snapshot for focused tests and for sizing the future bounded RHI-thread queue.
These counters describe CPU-side recorder/executor work; they are not GPU timing
measurements.

## Threaded Executor Boundary

A dedicated RHI thread may replace the inline consumer, but it must consume the
same immutable submission groups and preserve the same serial, fence, ordering,
failure, payload ownership, and resource-retention behavior. The producer may
continue recording only after successful ownership transfer. Queue depth and
byte backpressure belong to the threaded executor and must be derived from
measured command, batch, and payload volume rather than changing command
representation.

## Related Documentation

- [Runtime lifecycle](../Core/RuntimeLifecycle.md)
- [Viewport rendering](ViewportRendering.md)
- [Dedicated RHI Thread plan](../../Plans/DedicatedRHIThread.md)
- [Build and run](../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
