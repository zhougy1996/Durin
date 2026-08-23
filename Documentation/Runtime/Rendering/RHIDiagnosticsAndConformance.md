# RHI Diagnostics and Conformance

Summary: Define optional Vulkan diagnostics, native naming and command regions,
backend-neutral GPU timing and aggregate statistics, public-RHI conformance,
and diagnostic shutdown ownership.

Modules: RHI, VulkanRHI, RenderCore, ApplicationCore

## Diagnostic Configuration and Lifetime

`DURIN_VULKAN_VALIDATION` accepts `auto`, `on`, and `off`. Invalid or unset
input resolves to `auto`; Shipping always disables diagnostics. The Khronos
validation layer and `VK_EXT_debug_utils` are independent optional facilities.
Their absence or debug-messenger creation failure never prevents an otherwise
supported backend from starting.

`FRHIDiagnosticAvailability` distinguishes requested, supported, and active
state for debug utils, validation, and the messenger. Vulkan publishes this
state only after successful instance negotiation. One optional messenger is
owned by `FVulkanDynamicRHI`; its callback state remains valid until the
messenger is destroyed, and the messenger is destroyed before its instance.

The callback is safe for arbitrary driver threads, never mutates backend state,
and returns `VK_FALSE`. It maps Vulkan severity to the engine log, counts every
set message-type bit, bounds text to 4,096 UTF-8 bytes, and uses a thread-local
recursion guard. Message content remains in the ordinary log; only saturating
counters are retained by the diagnostic snapshot.

## Native Names and Recorded Regions

Debug names and regions are diagnostic metadata. They never participate in
resource, view, descriptor, render-pass, framebuffer, pipeline, or cache
identity. Public descriptor names propagate to their native resource family;
internal objects use deterministic `Durin.<role>[.<stable-index>]` names without
addresses, frame numbers, CPU serials, or completion tokens. Names are bounded
to 255 UTF-8 bytes at a code-point boundary. Unavailable debug utils is a
counted no-op.

Automatically synthesized buffer and texture views are keyed by their parent
resource, backend allocation identity, and complete portable view description.
Diagnostic names therefore cannot split that cache or turn repeated parameter
binding into native view creation. Mutable wrappers such as swapchain back
buffers must change backend allocation identity when their backing set is
recreated. Explicit view factories remain uncached for callers that request
distinct logical view objects.

`FRHICommandListBase::BeginDiagnosticRegion` and `EndDiagnosticRegion` record
owned names and replay identically inline or on the RHI thread. Regions nest to
64 levels. Empty/oversized names, underflow, overflow, an unclosed list, or a
region crossing a render-pass boundary is invalid command admission and
increments the saturating invalid-region observation. Render-pass and internal
transfer labels nest inside public regions; every successful begin owns one
matching end.

## GPU Timing

The immutable capability snapshot publishes `bSupportsGPUTimestamps` and
`GPUTimestampNanosecondsPerTick` for the selected immediate graphics queue.
Support requires nonzero queue `timestampValidBits` and a finite positive
timestamp period. Unsupported backends publish `false` and `0.0`.

`RHICreateGPUTimingQuery` returns one counted interval or null when unsupported
or the bounded pool is exhausted. Recorded begin/end commands retain it through
replay. Different queries may nest on one command list and must close in strict
last-in-first-out order; crossed or unclosed intervals are invalid command
admission. One query cannot overlap, cross command lists, or be reused while
recording or pending. `RHIGetGPUTimingResult` is a const, nonblocking read with
`Unsupported`, `Pending`, `Ready`, or `Invalid` state; it never submits, flushes,
waits, resets a query, or changes ordering.

Vulkan lazily allocates at most twenty pages of 64 intervals (128 timestamp
slots per page, 1,280 live intervals total). Each pair is reset and written in
its recording command buffer, associated with the exact submission token, and
polled without a wait flag only after that token completes. Slot reuse requires
completion and a released query generation. Exhaustion fails without waiting;
page-creation failure publishes no partial page.

Timestamp subtraction masks both samples to the selected queue width and uses
modular unsigned subtraction. Conversion multiplies once in extended precision,
rounds to the nearest nanosecond, and saturates on overflow. Native ticks,
query-pool handles, and queue properties remain backend-private.

## Aggregate Snapshot and Reset

`RHIGetDiagnosticSnapshot` returns one backend-neutral value from the RHI owner
thread. It composes each existing authority once and never waits for GPU work:

| Section | Authority and stable meaning |
| --- | --- |
| Availability | requested/supported/active diagnostic configuration |
| Executor | existing command executor counters, serials, backlog, and queue peaks |
| Graphics cache | existing capacities, occupancy, hits/misses, native creation, eviction/failure, descriptor and persistence observations |
| Memory | existing allocation classes, heap budgets, transfer/wait pressure, arena gauges, and native retirement |
| Completion | submitted/completed tokens and pending submissions, plus retirement values composed from Memory |
| Messages | saturating severity/type, truncation, and recursion-drop counters |
| Naming/regions | naming/label attempts, failures/skips, invalid regions, active depth, and high-water |
| Timing | fixed capacity, allocated pages, live/pending/ready intervals, high-water, failure/reuse/poll/result/overflow counters |

Counts are `uint64`, sizes are bytes, and durations are integer nanoseconds.
`FormatRHIDiagnosticSnapshot` is deterministic text for explicit logs/captures;
the value structs remain the machine-readable contract and no per-frame log is
emitted by default.

`RHIResetDiagnosticStatistics` delegates to reset-capable authorities on the
same owner thread. It preserves diagnostic availability, executor lifetime
statistics, cache capacity/occupancy, heap budgets, live allocations and arena
bytes, completion tokens and pending work, active region depth, timing pages,
and live/pending/ready queries. Interval counters clear; high-water values
restart from their corresponding live gauge. Repeated snapshots do not change
rendering, submission, counters, or query state.

## Conformance Boundary

Public functional results never depend on a Vulkan handle or test-only native
callback. The GPU-serialized `VulkanRHIIntegrationTests` target owns paired
inline/threaded resource, copy/readback, timing, named draw/pixel, failure,
snapshot, stress, and WSI scenarios under the `durin-gpu` resource lock.
Backend-neutral validation and executor rejection/failure remain in focused
headless RHI targets.

The supported Win64 topology qualifies main and ImGui detached viewports for
create, public clear/present, unavailable output, resize, transactional
candidate recovery, teardown, and shutdown. The backbuffer publishes the actual
selected swapchain format rather than the preferred request. Out-of-date and
suboptimal native results enter the same recreate path; the current test seam
does not synthesize those driver results and records that limitation honestly.

Conformance requires identical functional bytes/pixels and ordering in inline
and threaded modes. Optional diagnostic activation and measured timing values
may differ. Stress observations must remain within declared executor, cache,
arena, query, and viewport ownership bounds.

## Shutdown

Shutdown first closes RHI admission and drains CPU replay. Vulkan then waits all
published completion tokens, releases pending timing intervals, destroys
viewport-owned presentation resources, query pools, command/descriptor/transfer
pools, and deferred native objects, stops and destroys the messenger, and only
then destroys the instance. No callback, query, command label, or native object
may outlive its owner. Diagnostic-on and diagnostic-off startup/shutdown use the
same ordering.

## Related Documentation

- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [RHI command execution](RHICommandExecution.md)
- [RHI resource views and transfers](RHIResourceViewsAndTransfers.md)
- [Graphics state and bindings](GraphicsStateAndBindings.md)
- [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md)
- [Viewport rendering](ViewportRendering.md)
- [RHI and Vulkan backend evolution](../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
- [Native tests](../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/DynamicRHI.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDiagnostics.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDiagnostics.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanGPUTiming.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanGPUTiming.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
