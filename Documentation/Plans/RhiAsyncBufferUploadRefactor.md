# RHI Asynchronous Buffer Upload Refactor Plan

Summary: Decouple uniform and storage uploads from render-thread frame-slot waits through purpose-based RHI resources with internal deferred backing, and remove unconditional frame-start RHI synchronization while preserving completion-based reclamation.

Last reviewed: 2026-09-23

Status: Active
Completed:

## Current Status

User direction (2026-09-23): retain the original RTX 3090 baseline and budgets;
final performance qualification on that host is explicitly postponed. Current
host correctness results cannot close that acceptance gate. Implementation and
local correctness qualification continue.

Stage 0 inventory, revised interface decisions, and pre-refactor runtime
baseline are recorded below. Stage 1 now uses `FRHIUniformBuffer` and ordinary
`FRHIBuffer`/`FRHIBufferView` for CPU-authored contents, with private snapshots,
replay-ordered versions, and immutable Vulkan backing. The intermediate public
`FRHIDeferredBuffer` family, dedicated shader macros, and reflection flag have
been removed. Existing ordinary ranges/macros serve both native and
CPU-authored resources; native-only operations enforce content-mode boundaries.
Owned graph uploads retain the native graph-resource path.

The 2026-09-23 convergence checkpoint passed a Debug `all` build for the
workspace and all 21 targets selected by `test affected --report`, including
Vulkan integration and migrated lifetime/order/binding tests. The two new
native-operation and graph-import rejection cases also passed independently.
Stage 1 is complete. Compatible queued uploads now share bounded submission
batches and command lists while retaining per-pass barriers and exact handoff
locations. The batching checkpoint passed a fresh Debug `all` build and all
seven targets selected by `test affected --report`, including Vulkan integration.
Its three new contract cases also passed independently. Stage 2 now pools
immutable binding versions in bounded, queue-affine mapped pages. Snapshot
creation/update now reserves aligned virtual intervals for every physical queue
before recording; replay only materializes admitted pages. Native writes and
packed texture arrays now share the 32 MiB CPU-source budget with snapshots and
RDG sources. Delayed cross-queue completion retains reservations, and admission
rejects capacity or fragmentation without waiting on an active recording.
Legacy producers and their physical-placement APIs are now removed across the
workspace. Owning uniform ranges preserve prepared renderer lifetimes. Frame
start dispatches asynchronously; a separate three-frame CPU queue limit bounds
run-ahead while Vulkan retains its existing two-slot GPU pacing. Controlled
fixtures prove later-frame preparation during delayed replay and pressure only
at capacity. No measured runtime performance improvement is claimed until the
postponed RTX 3090 qualification. RHI replay, Vulkan frame-slot, queue-poll,
descriptor-preparation, and presentation retain distinct CPU profile zones.
The prior checkpoint passed a Release `all` build, Debug
`VulkanRHIIntegrationTests` (103/103), `RHICommandListTests` (95/95), and
changed-document validation. Current validation appears under Stages 1 and 2.

The final migration checkpoint passed the workspace Debug `all` build. The last
affected run passed 94 of 95 targets; `VulkanRHIIntegrationTests` then passed all
108 cases both under the debugger and in a normal whole-target rerun. The
implementation and local functional coverage are delivered, but the plan stays
Active: RTX 3090 runtime/performance acceptance is postponed and intermittent
Vulkan lifecycle access violations remain unexplained. Exact evidence and
limits are recorded under Stage 4.

Source inspection establishes the following baseline:

- `FDynamicRHI::RHIBeginFrame_RenderThread` submits `BeginFrame` using
  `FlushRHIThread`, blocking until preceding RHI work and this boundary complete.
- Vulkan `RHIBeginFrame` selects `FrameNumber % FrameInFlight` and calls
  `FVulkanFrame::Prepare`, which waits its GPU retirement prerequisites.
  `FrameInFlight` is currently two.
- Vulkan then performs a separate synchronous `PrepareForProducer` operation
  for dynamic uniforms, followed by render-thread storage producer reset.
  These operations are outside the `RHI.BeginFrame.FlushRHIThread` profile zone.
- The executor frame number advances during successful `EndFrame` replay;
  front-end storage allocation currently depends on that executed frame state.
- Command buffers and submission fences already use completion-tracker
  retirement. They do not require a new frame-age-based reclamation design.

The screenshot motivating this work identifies a long synchronization scope,
but does not establish whether RHI backlog, GPU slot waiting, or other backend
preparation dominates it. Instrumentation must separate those causes.

### Stage 0 Migration Inventory (2026-09-23)

The search covered source and test roots in all three `Durin.dworkspace`
projects. Sandbox and RoadWeaver have no direct references to
`AllocateDynamicUniformBuffer`, `AllocateDynamicStorageBuffer`, or the two
physical range types; their dependencies on Engine still require target builds
after a shared API change.

| Surface | Current owner and migration consequence |
| --- | --- |
| Dynamic allocations | `FRHICommandListImmediate` delegates to `FDynamicRHI`. Vulkan fast paths return mapped physical ranges; overflow calls a synchronous RHI operation. Other backends use synchronous context allocation. |
| Uniform producer | `FVulkanDynamicUniformBufferAllocator` has two producer states, each starting with a 4 MiB page, and up to eight chunks per producer. `RHIBeginFrame_RenderThread` synchronously prepares one, and `RHIEndFrame` attaches every reserved queue use. |
| Storage producer | `FVulkanDynamicStorageBufferAllocator` uses the executor frame number modulo two, with a 64 MiB and 16-chunk limit per frame. Render-thread `BeginFrameProducer` resets offsets after the frame-start flush. |
| Recorded uploads | `FRHICommandListBase::WriteBuffer` already copies source bytes, retains its destination, and replays in command order. `UpdateUniformBuffer` is an aligned wrapper over it. Native buffer creation and automatic view cache misses still return synchronous, fallible results. |
| Binding | `FRHIUniformBufferRange` and `FRHIStorageBufferRange` carry a physical `FRHIBuffer*`, offset, and size. RenderCore shader reflection reads those structs directly. RHI parameter commands retain resolved resources, while Vulkan descriptor snapshots cache native backing. A logical range migration must change reflection and command recording together. |
| RDG | `FRDGBuilder::CreateBuffer` declares a logical graph buffer, but there is no queued initial-data upload helper. Graph allocation and extraction follow the existing compile/execute and terminal-consumer contracts. |
| Frame slots | `FVulkanFrame::Prepare` waits retirement prerequisites for `FrameNumber % FrameInFlight`; context begin/end, swapchain minimum image count, descriptor pool capacity, dynamic upload allocators, and legacy test diagnostics reference the two-slot policy. Command buffers and fences already retire from completion tracking. |
| Queue pressure | `FRHIThreadLimits` currently bound accepted work to 8 entries, 16 batches, and 32 MiB of payload. These limits omit the producer's unsubmitted command storage and do not alone bound queued frame count or backend upload pages. |

The baseline capture must use one fixed scene and resolution on a recorded
hardware/driver/build revision, with a quiet GPU lane and the same workload
after migration. Capture the render-thread `RHI.BeginFrame.FlushRHIThread` and
`Vulkan.BeginFrame.PrepareUniformBufferSync` scopes, RHI-thread
`RHI.BeginFrame.Replay`, `Vulkan.BeginFrame.FrameSlotWait`, queue polling and
descriptor preparation, uniform `WaitForUses`, upload overflow waits, and
presentation scopes. Record median and tail frame time, throughput, latency,
queue payload high-water marks, upload allocation high-water marks, and pressure
wait counts. A missing or inaccessible GPU leaves this gate open; a synthetic
CPU-only fixture cannot substitute for the runtime baseline.

### Stage 0 Runtime Baseline (2026-09-23)

Revision `305d4bfc794acc4089900eaafb04e91a13af477b` plus only the profile
zones in this stage was built with `Win64-Release-DurinEditor` (`all`). The
workload was Sandbox's default `/Game/Levels/GrayboxStage15` in DurinEditor,
3840x2064 FIFO swapchain with two images, on an Intel Core i7-12700 and NVIDIA
RTX 3090 (driver 616.64). Each independent process warmed for 40 seconds and
ran a 20-second Tracy capture; analysis removed two seconds from each end.
There were 966–967 complete frame intervals per retained sample. The three
runtime runs and a fourth memory-statistics run completed normally. Local raw
traces, exported zones, scripts, and logs are in
`Build/Profiling/RhiAsyncBufferUploadRefactor/`.

| CPU zone or matched interval | Run 1 median / p95 ms | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Render-thread `RHI.BeginFrame.FlushRHIThread` | 14.886 / 16.441 | 15.779 / 16.389 | 15.697 / 16.320 |
| Render dispatch to RHI `BeginFrame` replay start | 0.526 / 1.509 | 0.254 / 0.450 | 0.299 / 1.216 |
| RHI-thread `Vulkan.BeginFrame.FrameSlotWait` | 14.199 / 16.105 | 15.480 / 16.098 | 15.327 / 15.984 |
| Render-thread uniform preparation sync | 0.061 / 0.162 | 0.025 / 0.041 | 0.026 / 0.154 |
| Vulkan swapchain image acquisition | 0.012 / 0.022 | 0.007 / 0.009 | 0.007 / 0.021 |
| Vulkan presentation call | 0.207 / 0.547 | 0.104 / 0.207 | 0.123 / 0.526 |
| Frame interval | 16.694 / 19.189 | 16.666 / 17.369 | 16.666 / 18.837 |

The dispatch-to-replay interval matches each render-thread flush with the
RHI-thread replay event inside it; these rows are not obtained by subtracting
independent medians. Frame-slot wait accounts for nearly all of the replay
duration in this FIFO workload. Uniform preparation, queue polling, descriptor
preparation, and storage reset were much smaller. Neither upload-overflow zone
appeared. This supports a frame-slot pacing diagnosis for this workload, not a
claim that the GPU can render faster than FIFO cadence.

The fourth run's shutdown diagnostics reported 8 MiB of dynamic-upload arena
capacity (37,568 B live high-water), 8 MiB of transfer-upload arena capacity
(5,949,254 B live high-water), 207 upload operations / 6,609,094 B, and zero
arena overflow and wait events. Across runs, the RHI executor peaked at two
entries, seven batches, and 5,851,090 queued payload bytes, with no queue
backpressure or rejection. The memory diagnostics' dynamic-upload arena does
not account for every storage-producer byte, so Stage 2 must add a complete
upload-memory high-water measure before its pressure policy can be accepted.

Performance acceptance uses this exact FIFO scene and an additional controlled
delayed-RHI fixture. The ordinary render-thread BeginFrame path must have zero
unconditional RHI serial waits, and the fixture must demonstrate recording
frame N+1 while RHI frame N remains delayed. FIFO median frame interval must
stay at or below 17.5 ms and p95 at or below 20 ms on this host; queue limits
must bound frame count and payload bytes, with pressure waits and memory
high-water reported separately. An unconditional serial dependency moved to
upload or EndFrame does not pass; backpressure at a reached queue limit is
allowed when useful overlap and the timing budgets are demonstrated. The
controlled fixture must show useful CPU/RHI overlap; correctness and memory limits remain hard
gates even if these timing budgets pass.

### Stage 0 Interface and Ownership Contract

Decision revision (2026-09-23): expose resource purpose, not execution timing.
The previous separate deferred buffer/range/view family reduced initial
migration risk but would make callers choose two resource hierarchies for the
same shader uses. Converge now, before pooled allocation and consumer rollout,
while preserving the implemented snapshot, replay ordering, and lease machinery.
This is a resource-contract change, not a rename or a claim of UE API parity.

The selected public model is:

| Resource or operation | Target contract |
| --- | --- |
| `FRHIUniformBuffer` | New counted uniform resource with immutable size/layout metadata, owned constant bytes and retained resource sidecars; ordinary create/update/bind requires no synchronous native allocation response. |
| `FRHIBuffer` | Stable ordinary buffer identity, including CPU-authored storage and existing persistent/GPU-written/readback uses. Creation descriptors and operation capabilities distinguish supported access, not a separate public deferred type. |
| Buffer ranges and `FRHIBufferView` | Logical byte ranges and interpretations of a resource. Native views resolve against the replay-visible backing version; allocator page offsets and mapped pointers stay internal. Uniform binding uses the uniform resource interface. |
| Backend backing and snapshots | Internal allocation/version/lease state; a resource object need not imply native readiness on the ordinary queued upload path. |
| Synchronous native operations | Explicit contracts remain for fallible native creation, mapping, readback, and requested waits. They must not silently become deferred or enter the ordinary upload path. |

The revised Stage 0 signatures below are frozen for Stage 1. `FByteView` is
copied before return and `References` is copied into counted ownership, separate
from the raw constant bytes. `FRHIUniformBuffer` is a final subclass of
`FRHIBuffer`; both share private snapshot/version state rather than wrapping a
second public resource object. Its constructor is factory-only. Uniform layout
is an immutable value containing `ConstantBufferSize`, a nonzero multiple of
16. It does not embed the shader's resource table: existing shader reflection
owns binding compatibility, and sidecars remain lifetime-only references.

`FRHIBuffer` has immutable `ERHIBufferContentMode` (`Native`, `CPUAuthored`)
and a `GetContentMode()` query. Existing backend constructors select `Native`;
the new CPU-authored factories select `CPUAuthored`. This is an access contract,
not a mutable readiness flag: materializing a version never changes it.
`FRHIUniformBuffer::GetLayout()` returns the immutable layout and both new
resource forms expose `GetLifetimeUsage()`. `SingleDraw`, `SingleFrame`, and
`MultiFrame` are allocation hints, never readiness or lifetime guarantees.
Native resources do not participate in CPU snapshot versioning.

```cpp
enum class ERHIBufferContentMode : uint8 { Native, CPUAuthored };
enum class ERHIBufferLifetimeUsage : uint8 { SingleDraw, SingleFrame, MultiFrame };
enum class ERHIBufferUploadError : uint8 {
    InvalidDescriptor, InvalidRange, InvalidUsage, PayloadBudgetExceeded
};
struct FRHIUniformBufferLayout { uint32 ConstantBufferSize = 0; };

auto FRHICommandListBase::TryCreateUniformBuffer(
    const FRHIUniformBufferLayout& Layout, ERHIBufferLifetimeUsage Usage,
    FByteView InitialData, std::span<FRHIResource* const> References = {})
    -> std::expected<TRefCountPtr<FRHIUniformBuffer>, ERHIBufferUploadError>;
auto FRHICommandListBase::TryCreateStorageBuffer(
    const FRHIBufferDesc& Desc, ERHIBufferLifetimeUsage Usage, FByteView InitialData)
    -> std::expected<TRefCountPtr<FRHIBuffer>, ERHIBufferUploadError>;
auto FRHICommandListBase::TryUpdateUniformBuffer(
    FRHIUniformBuffer* Buffer, FByteView Data,
    std::span<FRHIResource* const> References = {})
    -> std::expected<void, ERHIBufferUploadError>;
auto FRHICommandListBase::TryUpdateBuffer(
    FRHIBuffer* Buffer, uint32 Offset, FByteView Data)
    -> std::expected<void, ERHIBufferUploadError>;
auto FRHICommandListBase::TryCreateBufferView(
    FRHIBuffer* Buffer, const FRHIBufferViewDesc& Desc)
    -> std::expected<TRefCountPtr<FRHIBufferView>, ERHIBufferUploadError>;
auto FRDGBuilder::QueueBufferUpload(
    FRDGBufferHandle Buffer, uint32 Offset, FByteView Data) -> FRDGPassHandle;
auto FRDGBuilder::QueueBufferUploadOwned(
    FRDGBufferHandle Buffer, uint32 Offset, FByteBuffer Data) -> FRDGPassHandle;
auto FRDGBuilder::CreateStructuredBuffer(
    std::string_view Name, uint32 Stride, FByteView Data,
    EBufferUsageFlags AdditionalUsage = EBufferUsageFlags::None)
    -> FRDGBufferHandle;
auto FRDGBuilder::CreateStructuredBufferOwned(
    std::string_view Name, uint32 Stride, FByteBuffer Data,
    EBufferUsageFlags AdditionalUsage = EBufferUsageFlags::None)
    -> FRDGBufferHandle;
auto FRHICommandListBase::UploadBuffer(
    FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void;
```

The new command-list view helper accepts CPU-authored parents only; native
parents return `InvalidUsage`. Existing fallible `RHICreateBufferView` and
`RHIGetOrCreateBufferView` retain their native-readiness contract and reject
CPU-authored parents before backend work. The shader recorder chooses the
appropriate path by content mode; callers bind the same `FRHIBufferView` type.
Logical view validation uses published device limits without an RHI round trip.
This keeps native allocation failure out of `ERHIBufferUploadError` and prevents
the ordinary queued path from acquiring a hidden synchronous view-cache miss.

Ordinary queued uniform and CPU-authored storage creation returns a usable
resource reference after validation
and CPU-payload admission, never a claim of native readiness. Initial bytes and
references live in the logical object until first backend use. Any command
list may be the first consumer: that replay operation materializes initialization
before use, independent of which list created the object. Updates are copied
and ordered by accepted RHI command replay, including across command lists.
Inline replay runs the identical operations in the same order.

Initialization covers the entire nonempty buffer. The uniform factory derives
`FRHIBufferDesc` from the layout with stride zero and `UniformBuffer` usage.
The storage factory accepts exactly one of structured (nonzero stride dividing
size) or byte-address storage (stride four, size multiple of four), optionally
with `ShaderResource`; other flags are rejected. `TryUpdateBuffer` accepts only
CPU-authored storage; uniform and native parents return `InvalidUsage`.
Uniform updates
require offset zero and the full size. Versioned CPU-authored storage is
read-only on the GPU; GPU-written buffers retain their existing update/state
semantics within `FRHIBuffer`. These restrictions apply to the CPU-versioned
creation mode, not every ordinary buffer. Usage values are allocation hints,
not lifetime limits. Uniform references are lifetime-only resource sidecars
replaced with each complete update. Preserve the existing restriction to
native-backed sidecars and reject versioned-resource sidecars to prevent
reference cycles; type unification must not accidentally broaden admission.
Storage accepts no sidecar references. These restrictions make partial storage versioning
well-defined without reconstructing GPU-written contents.

Every accepted update creates a content version. Uniform updates replace the
whole contents; storage uploads may name an exact subrange and must preserve
untouched bytes. A draw or dispatch resolves the version visible at its replay
point, retaining its physical backing and descriptor lease through all
submissions that use it. For `Update(A) -> Draw -> Update(B) -> Draw`, old GPU
work retains A even when the second draw binds B. Prepared parameter batches
hold logical references; they do not cache a native view or choose a version
before the binding command replays. A later update after a prepared bind must
invalidate that binding at the next draw/dispatch. Cross-queue consumers retain
the exact queue-qualified prerequisites. No-copy APIs are deferred until their
ownership through RHI replay can be proved.

Existing `WriteBuffer`, synchronous `RHITryCreateBuffer`, mapping/readback,
and explicit native-view readiness contracts remain for resources requiring
them. Migrate uniform updates from the current `FRHIBuffer*` overload to the
uniform resource API; compatibility overloads are temporary. `WriteBuffer`
already owns bytes and remains the general
upload command for such resources. The new RDG helpers declare an exact
`TransferWrite` graph use, copy or move their CPU source into graph-owned
storage, and record `UploadBuffer` into an RHI command. Unlike `WriteBuffer`,
this command leaves the written range in `TransferWrite` so the graph's next
declared barrier sees the actual backend state. After the command copies the
bytes, graph destruction may release its source. Graph cancellation drops
unconsumed source owners.

### Stage 0 Revised Binding and Native-Access Audit (2026-09-23)

Keep `FRHIUniformBufferRange` and `FRHIStorageBufferRange` as the existing
`FRHIBuffer*`, byte-offset, byte-size values. A new uniform resource upcasts
through the ordinary range; the pointer identifies a resource, not an arena
allocation. Creation/update remains typed through `FRHIUniformBuffer`.
Ranges never expose backend page placement. Exact subranges and dynamic
uniform offsets keep their current alignment and bounds rules; complete
uniform updates still replace the whole parent. Native uniform ranges remain
usable for explicitly native workflows during and after producer migration.

Use the existing uniform/storage shader macros, including arrays, optional
members, and dynamic uniform bindings. Remove `bDeferredBuffer` from reflection
metadata and the `DURIN_SHADER_PARAMETER_DEFERRED_*` macros after migrating
their current tests. Representation no longer depends on execution timing.
Prepared batches retain ordinary logical views; Vulkan resolves CPU-authored
parents before any descriptor write/native cast and rechecks versions at each
draw/dispatch. Resolving a view never mutates its immutable parent/description.
Native descriptor cache keys retain backing version and allocation generation.

The pre-convergence audit covered the source and test roots of Engine, Sandbox,
and RoadWeaver. Only Engine named the intermediate deferred family or the
legacy dynamic allocation/range APIs. The concrete migration and rejection boundaries
are:

| Audited surface | Required implementation action |
| --- | --- |
| `RHIResources.h`, `RHIDeferredBuffer.h`, `RHIDeferredBuffer.cpp`, `RHIDeferredBufferBackend.h` | Move public resource identity to ordinary buffers/views; retain snapshots, ordered partial merges, budgets, and backend leases privately. Replace deferred resource-kind checks in sidecar validation with content-mode checks on buffers and view parents, rejecting all CPU-authored sidecars. |
| `RHICommandList.cpp` parameter canonicalization; `Shader.h`, `ShaderParameters.cpp`, `RHIShaderParameters.h` | Route ordinary ranges/views by parent mode, retain source resources through replay, and migrate typed metadata and prepared-batch tests together. No native factory or snapshot selection during CPU-authored binding recording. |
| `VulkanDeferredBuffer.cpp`, `VulkanPendingState.cpp` | Resolve ordinary CPU-authored views to immutable native versions before native descriptor lowering. Preserve update-after-bind invalidation, queue-context backing separation, and exact submission leases. |
| `VulkanContext.cpp`, `VulkanQueueTransfer.cpp` | Validate native mode before buffer downcasts in copy, transition/ownership, vertex/index binding, upload, and resumed binding paths. CPU-authored resources have no public GPU write/transfer/state-transition capability in this plan. |
| `VulkanView.cpp`, `VulkanTexture.cpp` native view factories/cache | Reject CPU-authored parents before native view construction or cache lookup. Generic logical views must never be cast directly to `FVulkanBufferView`. Backend-created snapshot backing remains native and can use these factories. |
| `RHICommandList.cpp`, `DynamicRHI.cpp` write/upload/lock | Preserve native `WriteBuffer`, graph `UploadBuffer`, and existing write-only CPU lock staging. Reject CPU-authored parents at the closest recording/API boundary, before allocating lock storage or recording native work; use the fallible update APIs for versioned contents. Lock/unlock is not to be converted into a new RHI-thread wait. |
| `RDG.cpp` external import and graph upload helpers | Reject CPU-authored external buffers as declaration errors before compile/record; a changing backing cannot satisfy the existing physical-identity initial/final-access contract. Graph-created storage continues through owned RDG uploads to native resources. This does not add versioned external graph resources. |
| Renderer, TextureEditor, MonaImGui, and their Engine fixtures | Stage 3 replaces dynamic producer allocation with the typed factories and handles admission errors. Existing range values and ordinary shader macros need no deferred counterpart. Native persistent/GPU-written uses retain their current contracts. |

Rejections at fallible surfaces return the existing typed validation error or
null/error result; add an `InvalidUsage`-equivalent validation result where the
existing enum has no suitable value. Void programmer-contract surfaces use an
always-enforced precondition before recording and backend downcasting, including
Release builds. Backend checks also protect direct context use. No rejection
may flush, enqueue partial work, or reinterpret a CPU-authored object as native.

Stage 1 must preserve and migrate the existing command-list source lifetime,
cross-list ordering, cancellation, sidecar, and budget cases, the typed prepared
binding case in `ShaderFoundationTests.cpp`, and
`DeferredVersionsSurvivePreparedDrawsAndDispatches` in Vulkan integration tests.
Add coverage for mode-confused native operations, native/CPU-authored view
factories, RDG import rejection, and unified range metadata. Exercise both
inline and threaded binding; validate all affected targets and an `all` build
before handing off a shared API migration. The audit closes interface selection,
not these implementation or validation gates.

### Stage 0 Failure and Pressure Contract

Invalid descriptors, incompatible usage, ranges, or front-end payload admission
return `ERHIBufferUploadError` before any command is recorded. They leave no
partially ready logical reference. Existing recoverable native creation remains
on `RHITryCreateBuffer`; existing synchronous readback, locks, swapchain
acquisition, and explicitly requested GPU waits remain synchronous. A deferred
operation accepted for replay is frame-critical: native allocation/submission
failure follows the executor's terminal failure and Vulkan quarantine/device-
loss path, not a later false-ready result. The legacy dynamic-storage empty
result is preserved until each caller handles the new admission error
explicitly. RDG allocation failure remains an `FRDGExecutionResult` error.

Initial limits are a 16 MiB maximum individual deferred upload, 32 MiB of
front-end unsubmitted owned bytes, the existing RHI queue limits of eight
entries / sixteen batches / 32 MiB, and at most three completed render frames
awaiting RHI replay. Backend dynamic-uniform capacity is bounded to 64 MiB:
4 MiB normal pages plus tracked oversize allocations up to 16 MiB each.
Dynamic storage is bounded to 128 MiB under the same normal and oversize rules.
The existing transfer-upload arena remains four 8 MiB pages plus tracked
oversize ranges. These are total live capacities, including recorded but not
submitted owners; allocation counters and high-water marks must include them.
The front-end budget is shared across producers of the active RHI, including
logical initial data, graphs, and command lists. Charge every live CPU copy
(including reference arrays and retained version snapshots) until released;
any graph-to-command copies overlap in accounting; sharing one immutable
allocation charges it once until the last owner releases it. Per-builder limits alone do
not satisfy this gate.

On pressure, reclaim completed leases, grow within the class limit, then wait
only for the oldest actual submitted owner that can release capacity. If only
unsubmitted work owns the needed capacity, submit an eligible recording first;
otherwise reject before recording, with the declared admission error. Never
wait on an unsubmitted reservation whose submission needs the blocked
allocator. Queue-frame admission may wait only after the three-frame limit and
must report count and duration. Reaching that bound may cause steady-state
FIFO pacing; it must not restore a per-frame serial dependency below the bound.
Legacy two-slot Vulkan frame pacing stays on the RHI thread until its remaining owners
are audited.

## Goal

Let the render thread prepare and record a later frame while the RHI thread
processes earlier work. Ordinary uniform and storage uploads must not require
an RHI round trip, a GPU frame-slot acquisition, or a shared active producer
to become ready on the render thread.

Keep resource reuse safe, memory bounded, command ordering deterministic, and
inline and threaded execution equivalent. Necessary resource-pressure and
latency-control waits remain explicit and measurable.

## Scope and Selected Decisions

Owners are RHI, VulkanRHI, RenderCore, and their renderer/frame-loop consumers.
Consumer migration must cover every project in `Durin.dworkspace`: Engine,
Sandbox, and RoadWeaver, including their source and test roots.

Reuse the existing GPU sync-point, payload ownership, submission coordinator,
and retirement machinery. The [RDG and RHI multi-queue plan](RdgRhiMultiQueueExecution.md)
continues to own async-compute rollout, split barriers, and transient aliasing;
this work must preserve its implemented queue and lifetime contracts without
claiming its remaining qualification gates are complete.

### Logical Resources and Deferred Data

The selected front-end model records owned CPU data and purpose-based resources;
the backend chooses physical storage during ordered execution. Returning a
logical object does not certify native allocation, upload, or GPU completion.

The following interface families describe intent; Stage 0 freezes exact names,
signatures, error behavior, and migration against existing APIs:

| Family | Intended semantics |
| --- | --- |
| Create uniform buffer | Accept contents, layout, and single-draw, single-frame, or multi-frame usage; return a retained logical reference with an ordered initialization dependency. |
| Update uniform buffer | Own a content snapshot and referenced resources; order the update relative to consuming commands. |
| Bind uniform buffer | Bind the resource version visible at that point in the command stream. |
| Create buffer / buffer views | Describe size, usage, and logical SRV/UAV ranges independently of upload allocator placement. |
| Upload buffer | Own source bytes, destination reference, offset, and size; execute before dependent GPU use. |
| RDG structured-buffer creation / queued upload | Express initial data and graph dependencies; batch uploads where compatible. |

Usage declarations inform allocation strategy, never authorize frame-age reuse.
Uniform layouts must retain referenced resources as well as raw constant bytes.
Structured/storage buffers may be persistent, CPU-uploaded, or GPU-written;
they are not all frame-local upload allocations.

For `Update(A) -> Draw -> Update(B) -> Draw`, the first draw must see A and the
second B, even if both execute after front-end recording ends. A mutable
"latest contents" pointer alone is insufficient. Backend versioned storage or
correctly ordered GPU updates must preserve every outstanding use and invalidate
descriptor caches when the effective backing changes.

Default uploads copy data into command/graph-owned storage. Any no-copy or move
overload requires explicit ownership through the last CPU read, including graph
deferral and RHI replay where applicable. Graph execution ending alone is not a
universal source-release signal. Cancellation must release unconsumed owners.

Replace front-end dependencies on physical `FRHIUniformBufferRange` and
`FRHIStorageBufferRange` placement with the uniform resource API and logical
ordinary-buffer ranges/views. Existing range names may survive where their
revised semantics are unambiguous; removing placement dependencies does not
require deleting every range struct. Logical buffer offsets remain meaningful;
backend arena offsets remain private. Preserve alignment, bounds checks,
reflected binding compatibility, and exact transition ranges. A temporary
adapter is allowed during migration, but cannot remain a hidden synchronous
allocation path in the accepted ordinary upload flow.
`FRHIDeferredBuffer`, its public range/view types, and deferred-specific shader
parameter entry points must be removed from the final caller-facing API.
Internal snapshot/backing helpers may retain implementation-specific names.

### Backend Allocation and Retirement

Retain distinct allocation classes where appropriate; shared lifetime rules do
not require a single uniform/storage/staging allocator. CPU payloads survive
until copied; staging storage survives its transfer; destination storage survives
all GPU reads/writes. Flush non-coherent mapped writes and emit required GPU
barriers before consumption.

Represent recorded-but-unsubmitted ownership explicitly. A missing GPU sync
point does not imply free storage. Transfer ownership into submission payloads,
then reclaim only when the existing retirement rules permit it. Cross-queue use
must retain all applicable prerequisites, not a graphics-only projection.

Pressure policy is reclaim, bounded growth, then a wait on actual submitted
work that can release capacity. Never wait for an unsubmitted reservation whose
submission requires the blocked allocator; split/submit eligible pending work
or reject according to the declared failure contract. Budget front-end owned
bytes as well as backend pages, oversize allocations, and queued frames.
Submission failure and device loss must preserve quarantine/teardown semantics;
they do not count as successful completion.

### Frame Boundaries and Pacing

`BeginFrame` becomes an ordered asynchronous boundary. Front-end frame identity
is captured when recording and does not select reusable GPU memory. Backend
execution counters may remain for diagnostics and ordered bookkeeping.

Initially retain `FVulkanFrame::Prepare` and existing GPU pacing on the RHI
thread. Removing the render-thread flush does not require deleting the two GPU
slots. Audit every frame-slot consumer before changing this boundary.

Separately enforce a documented bound on queued work/frames and CPU payload
memory. Do not replace the removed frame-start wait with an unconditional wait
on the same serial in another upload or allocation helper. Legitimate pressure,
presentation, readback, and explicit synchronization boundaries remain observable.

### Command Buffers, Pools, and Descriptors

Preserve the existing completion-tracker return path for command buffers and
fences. A submitted command buffer cannot reset before its submission completes.
The current command pools permit individual command-buffer reset; whole-pool
reset would require every affected command buffer to be safe. Maintain queue
family and recording-owner constraints.

Descriptor batches already retain recording/in-flight leases. Preserve that
mechanism and allocation-generation cache invalidation while introducing buffer
versions; an old descriptor cannot silently bind recycled backing storage.

Fixed GPU frame slots may later be reduced to pacing metadata or removed from
resource ownership after all consumers are audited. Their deletion is not an
acceptance requirement for this plan. Any remaining removal scope must be
recorded explicitly rather than rewriting proven command-buffer retirement.

## Implementation Stages

### Stage 0: Freeze Interfaces, Ownership, and Baseline

Dependencies: current runtime contracts and the implemented multi-queue model.

- [x] Inventory dynamic allocation, uniform creation/update/binding, storage
  ranges, frame counters, synchronous resource creation, descriptor caching,
  and frame-slot consumers across all workspace projects and tests.
- [x] Re-freeze exact purpose-based API signatures and the migration map from
  both legacy physical ranges and the intermediate deferred family. Specify
  uniform layout/reference ownership, resource capabilities, native readiness,
  error boundaries, and backend cast/view resolution. Preserve established
  content-version, cross-list initialization, and inline ordering guarantees.
- [x] Decide how deferred allocation failure is reported; do not silently
  change existing recoverable/terminal failure boundaries or return false
  readiness from logical resource creation.
- [x] Specify page, oversize, CPU payload, and queued-frame limits plus pressure
  progress rules. Identify calls that genuinely need synchronous results.
- [x] Capture comparable baseline profiles and separate RHI queue wait, GPU
  slot wait, uniform preparation, allocation pressure, and presentation wait.
  Record workload, revision, hardware, and performance acceptance budgets.

Completion: reviewed migration inventory, executable ownership contracts, and
baseline evidence; no claim that deleting one Flush alone solves the stall.

### Stage 1: Converge Public Buffer APIs and Internal Deferred Operations

Dependency: Stage 0 interface and failure decisions.

- [x] Implement intermediate logical creation/update/views with owned CPU
  snapshots, retained resource references, and replay-ordered versions.
- [x] Bind intermediate logical uniform/storage resources with replay-visible
  versions and prepared-binding invalidation.
- [x] Introduce the selected `FRHIUniformBuffer` interface and evolve ordinary
  `FRHIBuffer`/view contracts. Reuse existing snapshots and leases internally;
  migrate the intermediate deferred binding implementation and its tests to
  the purpose-based interfaces without discarding version-ordering coverage.
- [x] Update shader reflection, parameter retention, prepared-binding
  invalidation, and native backing resolution together. Audit synchronous
  native-only operations and enforce capability checks before backend casts.
- [x] Remove public deferred resource/range/view entry points after their
  current callers and fixtures migrate. Legacy producer adapters may remain
  until Stage 3 but must target the final interfaces; add no new callers of
  the intermediate deferred family.
- [x] Add owned graph buffer uploads and RDG queued upload/structured-buffer
  helpers with copied/moved sources and exact transition declarations.
- [x] Batch compatible queued uploads. Group consecutive retained helper passes
  on the same logical queue into one execution-plan submission and owned RHI
  command list, capped at 64 uploads and 16 MiB of source bytes. Preserve pass handles, culling,
  dependencies, exact uses, and per-pass barriers; stop at other callbacks or
  queue changes. Remap submission dependencies and preserve the exact consumer
  pass for each resource handoff. This reduces command-list batches without
  claiming fewer GPU copy commands or merging disjoint destination ranges.
- [x] Verify source lifetime, repeated updates, initialization dependencies,
  cancellation, and inline/threaded equivalence using deterministic tests.

Completion: purpose-based APIs support the owned upload and binding path, with
no caller-facing deferred resource family; legacy callers remain safe until
Stage 3 migration. Revalidate ordering, lifetimes, and inline/threaded binding
through the revised interfaces. Earlier intermediate-API results alone do not
satisfy this gate. Complete this convergence before Stage 2.

Stage 1 progress (2026-09-23): `FRHICommandListBase::UploadBuffer` owns replay
bytes and retains its physical destination. Vulkan leaves its written range in
`TransferWrite`, unlike the canonical-restoring `WriteBuffer`. RDG's
`QueueBufferUpload` and `QueueBufferUploadOwned` build Copy recording passes
with exact byte uses and 16 MiB single / 32 MiB per-builder source-capacity
limits. `CreateStructuredBuffer` and its owned-source variant queue complete
initial contents through that same path. The helpers require or supply
`DestinationCopy` and report invalid inputs as graph compilation errors. This
completes the owned-upload helper item; compatible upload batching remains
pending. Debug `all` build, `RHICommandListTests` (96/96), and
`RenderContractTests` (192/192) passed for
this slice. The Vulkan graph-upload and copy-matrix integration cases passed
in both inline and threaded modes. A full `VulkanRHIIntegrationTests` run
initially timed out after 300 seconds, and a later repetition exited with an
access violation. The viewport transaction test recorded a present on the
render thread and then directly recreated the swapchain on the RHI thread
without submitting that pending present. It now flushes the RHI command list
before recreation and before releasing the detached viewport. Five consecutive
full-suite runs passed (104/104 each, approximately 15 seconds per run) with
a 90-second process limit after this ordering fix.

The logical-resource slice adds validated creation/views and immutable CPU snapshots.
Updates merge against the replay-visible predecessor, including reversed
cross-list submission order; cancellation releases the unpublished version.
A shared 32 MiB deferred-snapshot budget counts bytes and reference arrays,
conservatively reserving a full snapshot even for a partial storage update.
This checkpoint covered CPU operations; its validation was:
Debug `all` passed; five new deterministic cases passed, followed by all nine
targets selected by `test affected --report`, including RHI command lists,
resource-view validation, RenderContract, and Vulkan integration. Changed-document
validation passed. The report is `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.

The binding slice adds typed deferred ranges and prepared logical views.
Draw/dispatch resolves the current version, invalidating descriptors after an
update without requiring a rebind. Vulkan uses one immutable mapped backing
per snapshot and queue context, with exact snapshot/view leases in submission
payloads. Front-end view checks use published device limits. This establishes
binding correctness; pooled allocation, complete native/CPU memory accounting,
pressure progress, and multi-queue qualification remain Stage 2 work.

Public API convergence (2026-09-23): the typed uniform and ordinary storage
factories now use private CPU snapshots through ordinary buffer/view identity.
Uniform layout, lifetime hints, and immutable content modes follow the revised
Stage 0 contract. Updates preserve replay-visible predecessor contents and
sidecar ownership. Existing ranges and shader metadata resolve logical views;
Vulkan keeps the prior immutable backing and prepared-binding invalidation
behavior. All intermediate public types/entry points were removed from every
workspace source/test root. Native view factories reject CPU-authored parents;
recorded native operations and Vulkan downcasts enforce the same boundary.
RDG reports `ExternalBufferContentModeInvalid` before recording an invalid import.

Validation: `Win64-Debug-DurinEditor` `all` passed, including Engine, Sandbox,
and RoadWeaver. `test affected --report` passed all 21 selected targets. The
report is `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.
`FRHICommandListTests.CPUAuthoredBuffersRejectNativeOperations` and
`FRDGTests.RejectsCPUAuthoredExternalBuffersBeforeRecording` each passed alone.
Migrated Vulkan draw/dispatch tests exercise both inline and threaded modes,
including native-view factory rejection without synchronous-operation growth.
Changed-document validation and diff checks passed. This convergence checkpoint
preceded the batching completion below.

Batching completion (2026-09-23): the compiler groups consecutive retained
upload helpers on one logical queue into bounded submissions. Recording puts
their barriers and uploads into one owned RHI command list. Each helper keeps
its handle, dependency declarations, and exact range; ordinary callbacks and
the 64-upload / 16 MiB limits split batches. Submission dependencies and handoff
endpoints are remapped, with a separate consumer-pass index locating each
barrier. Merely combining recording inside the old one-pass submissions could
not reduce batches, so grouping must happen in the compiler as well.

Validation: a fresh Debug `all` build and all seven targets selected by
`test affected --report` passed. The three new isolated contract cases cover
count/byte splitting, destruction before inline/threaded replay, overlapping
writes, culling, callback boundaries, and exact cross-queue handoff locations.
The Vulkan upload case now verifies disjoint uploads plus an overlapping patch
through RDG and GPU readback in both executor modes. Stage 1 is complete;
Stage 2 backend pooling, complete pressure accounting, and later pacing,
multi-queue qualification, and performance gates remain open.

### Stage 2: Move Upload Storage Ownership to the Backend

Dependency: revised Stage 0 contracts and Stage 1 public API convergence,
including binding validation. Allocate/version backing internally without
reintroducing a public deferred resource family.

Implementation sequence (2026-09-23): first pool immutable version backing in
queue-affine mapped ranges with weak caches and exact payload leases. Normal
and oversize binding pages share the class capacity limit; oversize pages remain
available for reuse instead of creating unaccounted pending native deletions.
Then integrate shared front-end admission and pressure progress. Allocation
during an active render pass cannot submit that recording as-is; until a safe
admission/progress path is validated, exhaustion owned by unsubmitted work must
fail terminally rather than wait for an impossible completion. This intermediate
failure policy does not satisfy the Stage 2 pressure gate and blocks Stage 3.

Shared-source decision (2026-09-23): immutable native buffer upload payloads
share the snapshot admission counter. RDG and its recorded upload command
retain one immutable allocation instead of copying the graph source again.
Span callers still receive a copy-before-return guarantee; owned callers move
their vector and charge its capacity. Each actual allocation is charged once
until its last owner releases it. Independently copied sources remain separate
charges. `TryUploadBuffer` reports admission errors before recording, while the
existing void entry point retains its enforced programmer-contract behavior.
Legacy `WriteBuffer` and texture command payloads retain their existing queue
limits; integrating all legacy source and producer accounting remains open.

Sealed-pressure decision (2026-09-23): pressure may submit sealed payloads
retaining the exact allocation, plus their queue prefixes and pending completion
dependencies. Preflight the complete closure before moving ownership or making
native calls. Never finalize an active context from binding resolution: it may
be inside a render pass or partway through resolving a draw's bindings. Missing
reservations, unavailable producers, and cycles leave pending work untouched.
Active-recording pressure remains a separate admission/progress gate.

Admission revision (2026-09-23): reserve aligned virtual page intervals for each
snapshot on every provisioned physical queue before accepting create/update.
This CPU-only reservation does not create native resources or wait for replay.
Replay materializes the reserved page and offset; payload-held snapshots keep
intervals unavailable until all consumers retire. Retain page geometry within
the class cap, and reject capacity/fragmentation before recording. This replaces
late binding allocation as the ordinary path: selective sealed submission alone
cannot make a mid-render-pass allocation safe. Reserving unused queue copies is
a deliberate conservative admission cost. Native allocation failure remains
terminal. Snapshot lifetime, rather than frame age, governs interval reuse.

Consumer lifetime correction (2026-09-23): prepared renderer state may outlive
the command list that created its uniform. A raw range plus a no-op command
capture is therefore insufficient. Keep the existing buffer/offset/size fields
and add an optional counted resource owner to uniform ranges. The convenience
factory returns an owning logical range; explicit borrowed native ranges remain
valid. Reflection still reads the same logical fields and uses the actual struct
stride. This supersedes the earlier assumption that all ranges could remain
unowned scalar values through producer migration.

- [x] Implement completion-aware allocation/versioning using existing payload
  leases and sync points, without a render-thread active producer or modulo-two
  upload slot selection.
- [x] Handle staging and destination lifetimes, multi-queue use, alignment,
  non-coherent writes, descriptor invalidation, and exact range transitions.
- [x] Enforce bounded pressure and forward progress for submitted and
  unsubmitted work; preserve failure quarantine and safe shutdown.
- [x] Verify delayed GPU completion prevents reuse without blocking unrelated
  front-end preparation; retain current command-buffer/fence retirement.

Completion: ordinary new uploads need no synchronous RHI allocation response,
and backend memory reuse is proven independently of frame age.

Pooling checkpoint (2026-09-23): native binding versions now occupy private
4 MiB pages; uniform/storage pools cap total normal plus oversize page capacity
at 64/128 MiB, with 16 MiB maximum allocations. Padded reservations isolate
noncoherent flush ranges. Logical offsets remain unchanged while native views
include the selected page offset. Each payload retains the snapshot, view,
backing, and exact allocation lease. Weak version/binding caches allow reuse
after completion; repeated uses refresh the producer locator. Oversize pages
are reused and stay charged to capacity. `DynamicUpload` arena gauges include
these pools, but do not yet account for all legacy storage producers or shared
front-end copies. Submitted pressure waits exact owners; unsubmitted pressure
and incompatible retained page layouts still fail terminally, so the pressure
item remains unchecked. Delayed-GPU/multi-queue qualification remains open.

Validation: Debug `all` passed for Engine, Sandbox, and RoadWeaver. All 16
targets selected by `test affected --report` passed, including Vulkan
integration and the renderer/editor GPU targets. The new bounded-capacity,
oversize reuse, retained-lease, and producer-locator case passed alone; the
existing prepared draw/dispatch version case also passed alone in inline and
threaded modes. Changed-document validation and diff checks passed. These
results validate the pooling checkpoint, not the remaining Stage 2 gates.

CPU-source checkpoint (2026-09-23): snapshot bytes and reference arrays,
RDG owned sources, and native upload command data now use one atomic 32 MiB
reservation counter. Copied spans reserve before allocating their owned copy;
moved sources reserve actual capacity. RDG's structured-buffer helper uses the
same admission path. Graph callbacks and recorded commands share immutable
sources, so an already admitted graph can record at the full budget without a
second allocation. Cancellation and the final owner release return admission;
logical resources still release their snapshots through ordinary deferred
resource deletion. `TryUploadBuffer` rejects input/admission errors with no
partially recorded command. Tests cover cross-path rejection, full-budget graph
recording, source ownership after graph destruction, shared command cancellation,
capacity accounting, and concurrent producers. Legacy native writes, texture
payloads, legacy producer memory, and backend unsubmitted-pressure progress are
still outside this checkpoint; Stage 2 remains incomplete.

Validation: final Debug `all` passed for the workspace, all 14 targets selected
by `test affected --report` passed, and seven RHI upload cases plus four RDG
upload cases passed in isolation. The report remains
`Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`. Whole-target
coverage caught a test-owned logical snapshot awaiting deferred deletion; the
test now drains its released resource and verifies the budget returns before
the next case. Changed-document validation and diff checks passed.

Sealed-pressure checkpoint (2026-09-23): binding-page exhaustion now attempts
selective submission of sealed allocation owners, their queue prefixes, and
transitive pending producers. Ordinary submission and pressure reuse the same
dependency/authority/prefix preflight. An ineligible closure leaves all pending
ownership untouched; an eligible closure preserves active contexts and unrelated
sealed work, then waits for exact allocation owners before retrying allocation.
Native failures still use the existing quarantine boundary. Tests cover missing
reservations, cycles, an active latest use, selective prefix submission, bounded
page reuse after lease retirement, and unavailable cross-queue producers becoming
eligible once sealed. This closes only the sealed-owner pressure path; active
render-pass admission, incompatible page geometry, legacy accounting, and the
remaining delayed-GPU qualification still block Stage 2 completion.

Validation: Debug `all` and all eight targets selected by `test affected
--report` passed. Four isolated Vulkan cases passed, including both same-family
and dedicated-family compute dependency paths and native failure quarantine.
The cross-queue fixture explicitly retires its producer before destroying its
local command pool; waiting for the consumer's allocation alone does not retire
a producer that does not own that allocation. The affected report remains
`Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.

Admission checkpoint (2026-09-23): frontend reservations now own virtual page
intervals for every physical queue. They include alignment and preserve pooled
normal/oversize geometry. Backend materialization cannot exhaust capacity inside
a draw; capacity/fragmentation rejects before recording. Snapshots and payloads
retain intervals through completion, cancellation, and quarantine. Diagnostics
separate live/peak backing reservations and virtual page capacity from native
materialization. Buffer writes and packed 2D/3D texture command copies now share
CPU admission; failed texture admission happens before a command node exists.
Native buffer creation reports initial-upload admission failure as recoverable
`RequestNotAdmitted`. Explicit native allocations and caller-owned input memory
remain outside the owned-copy budget.

Validation: all 16 affected targets passed; isolated admission/fragmentation,
concurrent producers, prepared draw/dispatch versions, delayed same-family and
dedicated-family compute, native-write/texture admission, and transfer reuse
cases passed. The old transfer pressure fixture now exercises frontend rejection
and explicit retry instead of recording over 32 MiB of owned sources. Legacy
mapped producers are the remaining aggregate-capacity gap. Consumer migration
and their removal are pulled forward into Stage 2; asynchronous frame start
still depends on completing that removal.

Producer-removal checkpoint (2026-09-23): all workspace consumers use ordinary
typed resources or owning logical uniform ranges. The old mapped producers,
synchronous overflow path, allocation APIs, and untyped uniform update overload
are removed. CPU-source and binding-page admission now cover the migrated paths;
there is no remaining legacy upload pool outside those bounds. Stage 2's
implementation gates are closed. Historical checkpoint limitations above describe
their intermediate revisions, not the final admission path.

### Stage 3: Migrate Consumers and Remove Frame-Start Synchronization

Dependency: Stage 2 lifetime and pressure gates.

- [x] Migrate all workspace consumers and fixtures to the final uniform and
  ordinary-buffer interfaces; remove obsolete physical-placement APIs and
  compatibility overloads/adapters after the last consumer is converted.
  Confirm no caller-facing deferred resource family remains.
- [x] Remove `PrepareUniformBufferSync` and front-end storage producer reset.
  Submit `BeginFrame` asynchronously and remove its unconditional RHI wait.
- [x] Introduce or adapt explicit queue/latency limits; verify later ordinary
  upload helpers do not reintroduce the same serial wait.
- [x] Retain backend frame pacing initially; audit remaining frame-slot owners
  and document any intentionally retained two-slot policy.
- [ ] Demonstrate frame N+1 preparation while RHI frame N is delayed, with
  correct output and bounded memory, in a controlled fixture and runtime trace.

Completion: the ordinary frame-start/upload path has no unconditional
render-thread RHI flush, and all consumer targets compile and pass their gates.

Implementation (2026-09-23): BeginFrame uses asynchronous dispatch. Queue
admission charges each EndFrame boundary until replay completes or rejects it,
with a default limit of three alongside the existing entry/batch/payload limits.
Current/peak frames and pressure count/duration are published separately.
Vulkan still owns two pacing slots, at most two descriptor batches, and the
corresponding swapchain minimum image-count policy; uploads no longer select
memory from a frame slot. Search of Engine, Sandbox, and RoadWeaver source/test
roots found no legacy allocation API callers.

The isolated `LaterFramesPrepareWhileRHIReplayIsDelayed` fixture verifies two
later frames and their uniform/native upload bytes while replay is gated, with
zero synchronous operations or waits below capacity. The isolated
`ThreeFrameLimitAllowsOverlapAndBlocksOnlyAtCapacity` fixture verifies the
fourth frame waits and all credits return. `OwningUniformRangeSurvivesItsCreatingList`
verifies preparation ownership independently of command-list lifetime. Runtime
trace and performance comparison remain explicitly postponed with RTX 3090
qualification; therefore the combined fixture-and-trace checkbox stays open.

### Stage 4: Qualify Performance, Lifetimes, and Handoff

Dependency: Stage 3 migration complete.

- [x] Cover repeated update/draw ordering, multiple submissions per frame,
  frames with no draws, delayed RHI/GPU work, and multi-queue resource use.
- [x] Cover page exhaustion, oversize requests, pending unsubmitted work,
  canceled recording, rejected/failed submission, device loss, resize, and
  shutdown with live uploads. Assert no premature reset or overwrite.
- [ ] Compare identical workloads and instrumentation against Stage 0: report
  render-thread wait time, throughput, CPU/GPU overlap, frame latency, queued
  payload bytes, upload memory high-water marks, and pressure waits. A renamed
  or relocated wait is not performance acceptance.
- [x] Run affected native suites and project targets plus the required `all`
  build for a shared Engine API migration. Record exact evidence.
- [ ] Resolve or localize the intermittent Vulkan lifecycle access violations;
  a later passing run does not establish that the cause is fixed.
- [ ] Update owning runtime contracts, close evidence-backed checklists, and
  complete this plan only after all required acceptance gates pass.

Completion: demonstrated overlap, preserved rendering correctness and resource
lifetime, bounded memory/latency, and no unexplained regression against the
recorded budgets.

Local qualification (2026-09-23): repeated prepared draws/dispatches verify exact
versioned bytes in inline and threaded modes. Empty-frame and multi-submission
fixtures cover executor ordering; delayed same/dedicated-family queue fixtures
retain allocation leases through every consumer. Admission/fragmentation,
oversize, concurrent source ownership, cancellation, and sealed-pressure tests
cover bounded storage. `PartialNativeFailureQuarantinesOwnersUntilTeardown` now
retains actual uniform backing reservations through ordinary submission failure
and injected device loss; the isolated case passed. RHI-thread failure and
external-failure cases now assert frame-credit reclamation and passed separately.
Viewport resize ordering and renderer resource invalidation tests passed in the
affected run. Native buffer/texture churn and shutdown paths remain covered by
the whole Vulkan suite. These are functional tests, not performance acceptance.

Evidence:

- Debug `all` passed for Engine, Sandbox, and RoadWeaver; build log
  `Build/.agent-state/logs/20260923-140957-577743-41836-cmake.log`.
- The final affected run passed 94/95 targets, including sky, static mesh,
  clouds, viewport, RHI command lists, and RHI threads. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`; log:
  `Build/.agent-state/logs/20260923-140407-099809-2000-ctest.log`.
- Vulkan whole-target rerun passed 108/108; report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/VulkanRHIIntegrationTests.xml`;
  log `Build/.agent-state/logs/20260923-141037-632250-30104-VulkanRHIIntegrationTests.log`.
  Debugger run also passed 108/108 (`Build/rhi-upload-debugger-output.log`).
- Earlier runs reported lifecycle access violations in static-mesh and Vulkan
  integration targets. The latter reproduced without parallel target execution;
  debugger and subsequent normal runs passed without identifying a fault stack.
  Do not attribute the failure to scheduling or a driver without evidence.
  [Multi-queue execution](RdgRhiMultiQueueExecution.md) already tracks a similar
  unresolved lifecycle stability issue; a shared cause is not established.
- The sky retained-generation fixture now polls the published request identity
  before retaining the next generation. It no longer assumes BeginFrame waits
  for asynchronous generation progress and passed in the final affected run.

Remaining handoff: preserve the Stage 0 RTX 3090 captures and budgets, resume
the same-host workload/trace comparison when the user schedules it, and retain
the lifecycle stability gate until supported by a diagnosis. No replacement
GTX 1060 baseline or claimed performance improvement is introduced.

## Validation and References

Follow [build guidance](../Agents/BuildAndRun.md) before configuration/build/run
and [testing guidance](../Agents/Testing.md) before selecting or running native
tests. Follow [documentation guidance](../Agents/Documentation.md) for this
plan and subsequent contract updates. A design-only commit does not require
native validation and does not renew earlier runtime evidence.

Current authorities:

- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [Vulkan memory and GPU completion](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)

UE provides interface inspiration, not a verified implementation template for
a specific Vulkan backend version. Public references support usage declarations,
deferred uploads, explicit data ownership, and render/RHI overlap; they do not
establish that every UE backend avoids synchronization:

- [Uniform buffer usage](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/EUniformBufferUsage)
- [Uniform buffer creation](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FDynamicRHI/RHICreateUniformBuffer)
- [RDG buffer uploads](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/QueueBufferUpload)
- [Initial-data ownership](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/ERDGInitialDataFlags)
- [Parallel rendering](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine)
