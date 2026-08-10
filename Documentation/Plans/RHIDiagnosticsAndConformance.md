# RHI Diagnostics and Conformance Plan

Summary: Establish owned Vulkan diagnostics, backend-neutral GPU timing and statistics, and a public-RHI conformance matrix across execution, presentation, failure, and shutdown paths.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

M0 capability/startup, M1 resource transitions, M2 resource views/transfers,
M3 graphics state/bindings, and M4 memory/GPU completion are complete. Their
lasting contracts provide immutable startup capabilities, one Vulkan resource
state authority, counted view identity, complete graphics state, bounded
caches, explicit allocation classes, and exact-token native retirement.

This plan activates M5 from the
[RHI and Vulkan Backend Evolution roadmap](../Roadmaps/RHIAndVulkanEvolution.md).
The activation baseline is `11a048aa`, where the focused RHI and Vulkan suites,
full Debug Editor build, and validation-clean hidden runtime passed. The final
repository aggregate at that baseline recorded 1,385 of 1,386 targets passing;
the independently reproduced
`FSourceReferenceIndexTests.RelocatesSharedSourceAndAllReferencingPackages`
failure is unchanged Editor asset-source behavior outside this plan. It remains
visible baseline evidence and must not be silently attributed to or repaired by
M5 without a separate owning task.

The startup path already resolves `VulkanValidation` as `auto`, `on`, or `off`,
negotiates `VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation` as independent
optional diagnostics, and keeps their absence non-fatal. It does not yet own a
debug messenger or publish callback statistics. Vulkan command buffers emit a
debug-utils label only around named render passes. RHI resource creation
descriptors carry debug names and VMA allocations receive allocation names, but
native Vulkan buffers, images, views, samplers, shaders, layouts, pipelines,
render passes, framebuffers, command buffers, queues, and swapchain objects are
not named systematically.

The public RHI exposes graphics-cache and memory snapshots, and the command-list
executor exposes its own statistics. Vulkan additionally maintains completion,
pool, structural-cache, and failure-injection observations, some of which are
test-only. There is no consolidated diagnostic snapshot, no timestamp-query
capability or recorded timestamp commands, and no completion-aware query-pool
ownership. Existing RHI and hardware-backed Vulkan tests cover the individual
M0-M4 contracts, but they do not yet form an explicit cross-contract
conformance matrix for both executor modes, supported WSI lifecycle, diagnostic
availability, and orderly shutdown.

Stage 0 completed on 2026-08-11. The `windows-msvc-x64` /
`Win64-Debug-DurinEditor` baseline passed 83 public-RHI tests and 34 Vulkan
integration tests. The selected RTX 3090 graphics/present queue reports 64
valid timestamp bits and a 1 ns timestamp period; the same host's Intel
candidate reports 36 bits and about 52.0833 ns, proving that masking and
non-integral conversion are required. Diagnostic-off and diagnostic-on hidden
startup both initialized the RTX 3090; the instance extension count changed
from four to five when diagnostics were requested. No validation callback was
owned at this baseline, so no engine callback messages or counts existed.

Stage 1 completed on 2026-08-11. `FVulkanDynamicRHI` now owns exactly one
optional messenger, bounded arbitrary-thread callback accounting, and explicit
requested/supported/active availability. Injected messenger failure remains
recoverable, and lifecycle instrumentation proves messenger destruction before
instance destruction. The new callback exposed a pre-existing MRT validation
error; device selection now requires and enables `independentBlend`, while the
MRT fixture uses a matching two-target fragment shader. The Vulkan integration
target passed 36/36 with no callback warning/error. Diagnostic-off and
diagnostic-on Editor runs exited normally after 120 ticks; off emitted no
callback messages, on emitted 30, and neither emitted a callback warning/error.
The full Debug Editor `all` build passed.

Stage 2 completed on 2026-08-11. It centralizes debug-utils naming/labels and
adds owned public command regions without changing render-pass or submission
semantics.
The implementation and automated validation are complete: public names and
deterministic internal roles cover the Stage 0 matrix except query pools, whose
owner does not exist until Stage 3; nested owned labels replay identically,
coexist with render-pass labels, and transfer/readback work is identified by
command-buffer-local regions. `RHICommandListTests` passed 56/56,
`VulkanRHIIntegrationTests` passed 38/38, the full Debug Editor build passed,
and a validation-enabled 120-tick Editor run exited normally without a
validation warning/error (the unrelated incompatible persisted pipeline-cache
warning remains recoverable). The final validation-enabled RenderDoc capture
is archived as `DurinEditor-Debug.rdc` in the external LFS evidence store. Its
structured replay contains 177 `vkSetDebugUtilsObjectNameEXT` calls, four
balanced debug-label regions enclosing four render passes and 27 indexed
draws, and one present. Stable names identify the instance/device/queue,
swapchain images, `SceneColor`, `SceneDepth`, `StaticMeshPipeline_0`, and the
`SceneColorRenderPass`, `PostProcessOffscreenRenderPass`,
`EditorAssistanceOffscreenRenderPass`, and `ImGuiRenderPass` event groups.

Stage 3 has started. The immutable capability snapshot now publishes GPU
timestamp support and nanoseconds-per-tick solely from the selected immediate
queue family's nonzero timestamp-valid-bit count and the selected physical
device's finite positive timestamp period. Default/fake capability publication
remains unsupported with zero resolution. `RHIInitializationTests` passed 5/5
and the RTX 3090 Vulkan integration target passed 38/38 with supported,
positive-resolution assertions.

Stage 3 implementation completed on 2026-08-11. The public RHI owns reusable
counted timing intervals with balanced retained begin/end commands and
nonblocking `Unsupported`/`Pending`/`Ready`/`Invalid` results. Vulkan owns up
to eight lazily created timestamp query pages with 64 intervals per page,
names every pool, resets slots in the recording command buffer, associates
ended intervals with the exact submission token, and polls 64-bit results only
after that token completes without a wait flag. Masked wraparound, fractional
period conversion, saturation, pool creation rollback, 512-interval
exhaustion, reuse, release-before-completion, and shutdown ownership are
covered. A 4 KiB public buffer transfer produced `Pending` then a nonzero
`Ready` duration in both inline and threaded modes without GPU idle or a result
read causing submission. The focused RHI command target passed 57/57, Vulkan
integration passed 41/41, the full Debug Editor build passed, and a
validation-enabled 120-tick Editor run exited normally with no validation
warning/error.

Stage 4 completed on 2026-08-11. `FRHIDiagnosticSnapshot` now composes the
existing executor, graphics-cache, memory/retirement, completion, validation
callback, debug-utils naming/label, command-region, and GPU timing authorities
without a second accumulating store or an implicit wait. The owner-thread
reset delegates to those authorities, preserves capabilities, capacities,
occupancy, live allocations, pending submissions, and live timing intervals,
and resets only interval observations; repeated reads are inert. Stable text
formatting is available for explicit runtime capture. Focused coverage includes
the unsupported base backend, saturating authorities, recoverable invalid
regions, repeated-read consistency, reset preservation, and inline/threaded
capability parity. `RHIInitializationTests` passed 5/5,
`RHICommandListTests` passed 58/58, and `VulkanRHIIntegrationTests` passed
43/43. The full Debug Editor build passed, and diagnostic-on/off hidden Editor
runs each completed 120 ticks and orderly shutdown without a warning/error.

Stage 5 completed on 2026-08-11. The maintained matrix below now binds every
M5 boundary to named public or focused evidence. A paired inline/threaded
public-RHI scenario compiles and creates shaders/pipeline/resources, uploads an
exact sampled texture, binds an exact view and sampler array, records a named
timed draw, verifies identical RGBA8 pixels, rejects an unsupported descriptor,
replaces/releases the target, and observes bounded diagnostics and completion.
The WSI qualification creates main and detached viewports, records public clear
and present work, covers unavailable output, resize, transactional candidate
failure/recovery, teardown, and records the unavailable synthetic
out-of-date/suboptimal seam honestly. That test exposed a real mismatch between
the preferred viewport format and an actual RGBA swapchain format; backbuffers
now publish the selected swapchain format, eliminating the framebuffer-view
VUID. Stress snapshots remain bounded across 16 replacement frames. The
focused targets passed: Vulkan 44/44, command list 58/58, RHI thread 10/10,
transition 6/6, view 3/3, and transfer 4/4. The full Debug Editor build and
diagnostic-on/off 120-tick hidden runs passed without warning, error, or VUID.

Stage 6 completed on 2026-08-11. The lasting contract is now
[RHI Diagnostics and Conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md),
with related startup, execution, memory/completion, viewport, and roadmap
contracts updated. On the `windows-msvc-x64` / `Win64-Debug-DurinEditor`
profile and NVIDIA GeForce RTX 3090, the final focused results were Vulkan
44/44, command list 58/58, RHI initialization 5/5, RHI thread 10/10,
transition 6/6, view 3/3, transfer 4/4, and World 62/62. The repository native
aggregate built and ran all 52 target groups; 51 passed and the only failure was
the unchanged, separately reproduced
`FSourceReferenceIndexTests.RelocatesSharedSourceAndAllReferencingPackages`
Editor asset-source case outside M5. The full Debug Editor build passed. Final
diagnostic-on and diagnostic-off hidden runs each completed 120 ticks and
orderly shutdown without warning, error, or VUID. Public conformance snapshots
showed identical inline/threaded pixels, fixed 512-interval timing capacity,
balanced zero active-region depth, and bounded stress gauges. The final
validation-enabled `DurinEditor-Debug.rdc` opens successfully in RenderDoc 1.28
and exports its 1920x1080 Editor thumbnail. Structured capture inspection
records the validation layer, 177 named Vulkan objects, four balanced named
render-pass regions containing 27 indexed draws, and one present. The artifact
and thumbnail are archived under
`Durin-LfsStorage/RHIDiagnosticsAndConformance/2026-08-11/`; M5 and the required
RHI/Vulkan roadmap are therefore closed, while C1-C4 retain their reviewed
deferred/transferred status.

## Goal

Make supported RHI work identifiable, measurable, and reproducibly conformant
without exposing Vulkan handles or changing execution order, so validation
messages and captures identify the responsible object and command region, GPU
timings and backend pressure are queryable without implicit waits, and future
graphics or compute features can extend one public test matrix instead of
creating backend-specific correctness paths.

## Scope

- Configuration-aware Vulkan debug-messenger creation, callback routing,
  bounded accounting, startup rollback, and destruction before instance
  teardown.
- Systematic native Vulkan object names derived from diagnostic-only public
  names or bounded deterministic internal identities.
- Backend-neutral, recorded command regions that map to Vulkan debug-utils
  labels when available and remain ordered no-ops otherwise.
- An immutable startup capability for GPU timestamp-query support and the
  timing resolution of the selected queue path.
- Counted GPU timing-query ownership, recorded begin/end timestamp writes,
  completion-aware native query storage, wrap-safe duration conversion, and
  nonblocking CPU result publication.
- One consolidated, stable RHI diagnostic snapshot composed from executor,
  queue/completion, cache, memory, diagnostics, and timing observations without
  duplicating existing counter authorities.
- Public-RHI conformance coverage for creation, transitions, views, bindings,
  draws, transfers, presentation, expected creation failure, execution modes,
  and shutdown, with native inspection limited to diagnostic assertions and
  existing failure-injection seams.
- Debug/non-debug and diagnostic-available/unavailable qualification plus a
  validation-clean hidden Editor runtime and supported main/detached viewport
  lifecycle.

## Non-Goals

- Turning validation layers, debug-utils, GPU timing, or capture tools into
  shipping startup requirements.
- Exposing Vulkan handles, stage flags, query pools, object types, callback
  structures, validation IDs, or native timestamps through the portable RHI.
- Adding synchronous query-result waits, `vkDeviceWaitIdle`, per-frame global
  idle, or a new submission boundary to make diagnostic results convenient.
- Building an editor profiler UI, telemetry uploader, persistent metrics
  database, automated RenderDoc capture controller, or vendor-specific tooling
  integration.
- Implementing compute PSOs/dispatch, asynchronous compute, separate transfer
  queues, queue-family ownership, render graphs, bindless descriptors, or
  device-loss recovery.
- Replacing Tracy or the CPU task/executor profiler. GPU timing augments CPU
  observations and remains a distinct clock domain.
- Rewriting all M0-M4 tests into one monolithic suite or duplicating coverage
  that already asserts the same public contract.
- Making debug names part of logical identity, cache keys, resource ownership,
  replacement generation, serialization, or correctness.
- Recovering from validation errors, device loss, submission failure, or state
  contract violations that are terminal under existing runtime policy.
- Repairing the unrelated Editor asset-source relocation baseline failure.

## Design Decisions and Invariants

### Diagnostic availability and messenger ownership

- The existing `VulkanValidation` policy remains the single user-facing switch
  for optional Vulkan diagnostics. `auto`, `on`, `off`, invalid-setting
  fallback, and Shipping behavior retain their M0 semantics; M5 must not add a
  second conflicting enablement policy.
- Validation-layer availability and `VK_EXT_debug_utils` availability remain
  independent. A missing optional layer or extension cannot prevent normal
  startup. The diagnostic snapshot records what was requested, supported, and
  activated so absence is distinguishable from silence.
- When debug-utils is activated, `FVulkanDynamicRHI` owns exactly one messenger
  associated with its published instance. Instance creation, messenger
  creation, device creation, and later initialization remain transactional;
  failure destroys only the candidate state. The messenger is destroyed before
  the instance and no callback may outlive RHI shutdown.
- The Vulkan callback may run on arbitrary driver threads. It performs bounded
  classification and thread-safe logging/accounting only; it does not allocate
  RHI resources, mutate backend state, wait, throw, or call back into Vulkan.
  Recursive logging is guarded, and any bounded suppression or truncation is
  counted rather than silent.
- Severity and type mapping are stable engine diagnostics, not native enum
  leakage. Errors and warnings remain actionable; verbose/information messages
  follow the selected configuration and cannot flood an unbounded retained
  buffer. Tests observe a narrow callback-classification seam rather than
  requiring the validation layer to emit a specific vendor/SDK message.

### Native names and command regions

- One backend-private debug-utils helper owns object naming and command-label
  emission. Every operation is a checked no-op when debug-utils is unavailable,
  when a handle is null, or when the relevant function pointer is absent.
- Public resource and pipeline debug names are copied only for the lifetime
  required by their owning object. Internal names are deterministic, bounded,
  and include object role where useful. Raw pointers, allocation addresses,
  frame timestamps, and mutable generations are not logical names.
- A name is attached only after successful native creation and before
  publication. Naming failure is diagnostic-only: it increments an attributable
  counter and cannot turn a complete resource candidate into failure.
- Recorded RHI command regions own their label bytes through replay. Begin/end
  commands preserve inline/threaded timeline order, validate underflow and
  unclosed scopes before native replay, and map to nested Vulkan command-buffer
  labels when available. Existing render-pass labels use the same helper and
  cannot double-close or conflict with an enclosing public region.
- Command regions may identify frame, viewport, pass, transfer, and later
  compute work, but they never introduce barriers, render-pass boundaries,
  submissions, CPU serials, or GPU completion tokens.

### GPU timestamp-query contract

- Startup publishes whether the selected immediate queue supports timestamps
  and the backend-neutral nanoseconds-per-tick resolution. Support requires a
  nonzero queue-family `timestampValidBits` value and a valid physical-device
  timestamp period. Unsupported hardware publishes an honest false capability
  and timing creation returns no usable query without failing runtime startup.
- The portable unit is a counted GPU timing query containing one begin/end
  interval and a generation. Recorded begin/end commands retain the query,
  reject double-begin, end-without-begin, reuse while in flight, and incomplete
  intervals, and preserve identical semantics in inline and threaded replay.
- Vulkan owns bounded query-pool pages. Native slots are reset and reused only
  after the exact submission token which wrote them has completed. Pool bounds,
  overflow policy, allocation failure, live occupancy, high-water, and reuse
  are observable. Frame number and CPU executor serial are not reuse evidence.
- Ordinary maintenance polls only queries whose GPU token is complete and uses
  Vulkan result availability without the wait flag. Public result access is
  nonblocking and reports `Unsupported`, `Pending`, `Ready`, or `Invalid`; it
  never submits work, flushes a command list, waits for a CPU serial, or blocks
  the GPU.
- Duration conversion masks/wraps native values according to the selected
  queue family's valid timestamp bits and converts once to unsigned
  nanoseconds using the published resolution with overflow checking. Raw ticks
  and the Vulkan timestamp period remain backend-private.
- Timing scopes measure GPU execution only. They do not claim CPU/GPU clock
  correlation, cross-queue comparability, pipeline-statistics support, or
  calibrated timestamps. M5 uses the single negotiated immediate queue.

### Statistics, threading, and reset semantics

- One backend-neutral diagnostic snapshot composes existing executor,
  graphics-cache, memory, completion/queue, validation, naming/region, and GPU
  timing statistics. Existing authorities remain canonical; aggregation does
  not maintain a second set of the same counters.
- Every field is either a live gauge, lifetime/resettable counter, capacity,
  high-water mark, duration, or capability state with documented units. Stage
  0 records the exact shape and reset behavior before implementation.
- Snapshot reads are bounded and do not wait for GPU completion. Any
  RHI-owner-only state is published through an immutable/atomic observation or
  a documented owner-thread snapshot; arbitrary callers do not race mutable
  cache, allocator, query-pool, or completion state.
- Reset clears accumulated interval counters and high-water marks only where
  the existing M3/M4 contracts permit it. It preserves capabilities, active
  configuration, capacities, live occupancy, pending queries/submissions,
  current heap budgets, and ownership state. Reset does not change rendering or
  query ordering.
- Counter overflow saturates and is itself detectable where practical. The
  public snapshot uses portable names and integer nanoseconds/bytes/counts;
  formatting and logging remain presentation helpers outside counter
  authority.

### Conformance ownership and failure policy

- The conformance matrix is organized by public contract boundaries, not by
  Vulkan file layout. Existing focused M0-M4 tests remain authoritative; M5
  inventories and references them, then adds only missing cross-contract,
  executor-mode, diagnostic, WSI, and shutdown scenarios.
- Fake-context RHI tests prove recording, ownership, validation, and
  inline/threaded equivalence without a GPU. Hardware-backed Vulkan tests prove
  native mapping, timing results, labels/names where inspectable, memory/cache
  observations, supported presentation, and validation cleanliness under the
  existing `durin-gpu` resource lock.
- Backend-private inspection is allowed only for diagnostic activation,
  object-name/callback observation, exact completion/query-pool reuse, and
  failure injection. Functional output must travel through public RHI draw,
  copy, transition, upload, and readback paths.
- Expected resource/candidate creation failures remain recoverable and publish
  no partial object. Query-pool exhaustion follows the bounded policy selected
  in Stage 0. Submission, presentation contract violations, invalid state
  replay, and device loss retain their existing terminal policies.
- Inline and dedicated-thread modes execute the same functional scenarios.
  Threaded qualification additionally proves payload lifetime, RHI affinity,
  FIFO labels/timestamps, failure wakeup, drain, and shutdown without callback
  or query storage escaping its owner.

## Current Foundations and Gaps

| Area | Existing foundation | M5 gap |
| --- | --- | --- |
| Startup diagnostics | `VulkanValidation` policy and optional layer/debug-utils negotiation are covered by instance-negotiation tests. | Own a messenger, callback mapping/accounting, creation rollback, active-state publication, and destruction order. |
| Debug identity | Resource/pipeline descriptors carry debug names; VMA allocations are named; render passes can open one debug-utils label. | Name native Vulkan objects systematically and route all labels through one safe helper without changing identity. |
| Command recording | Owned commands replay identically inline/threaded and validate render-pass balance. | Add owned nested diagnostic regions and timing intervals with balance, lifetime, and ordering validation. |
| Capabilities | Startup publishes immutable rendering features/limits only after successful device selection. | Publish selected-queue timestamp support/resolution and reject unsupported timing before recording native work. |
| GPU completion | Exact submission tokens retire native resources and bounded pools without frame-age assumptions. | Bind query-slot reset, result publication, and query-resource release to the same exact completion evidence. |
| Statistics | Executor, graphics-cache, and memory statistics are public; Vulkan has additional completion/pool test observations. | Define one nonblocking snapshot and remove test-only access where stable public diagnostics now own the observation. |
| RHI tests | Focused tests cover command ownership, thread execution, capabilities, transitions, views, transfers, and graphics binding validation. | Add region/timing recording semantics and an explicit matrix showing which existing tests satisfy each portable boundary. |
| Vulkan tests | One GPU-locked aggregate covers startup negotiation, failure injection, transitions, sampling/draws, views/copies, memory, completion, and pools. | Add messenger/naming, query conversion/reuse/result, diagnostic statistics, supported WSI, cross-contract churn, and shutdown evidence. |
| Runtime evidence | Hidden Editor smoke has passed validation-clean after every required milestone. | Run diagnostic on/off qualification and identify main/detached viewport work and GPU scopes in captures/logs without changing output. |

## Implementation Stages

### Stage 0: Diagnostic Contract and Baseline Inventory

- [x] Inventory every existing diagnostic configuration input, instance
  extension/layer decision, startup diagnostic, render-pass label, debug-name
  source, public/test-only statistic, native object family, query-related device
  property, and M0-M4 test scenario.
- [x] Record the selected GPU queue family's timestamp-valid-bit and timestamp-
  period baseline on the Agent Build Profile; record diagnostic-on/off instance
  activation and current validation-message behavior without changing startup.
- [x] Freeze the public timestamp capability fields, timing-query resource and
  result states, recorded begin/end rules, nonblocking publication API, query-
  pool capacity/overflow policy, and exact-token reuse ownership.
- [x] Freeze the consolidated diagnostic snapshot schema, units, gauge/counter
  classification, reset semantics, thread-safe publication boundary, and which
  existing counter remains the authority for every field.
- [x] Freeze callback severity/type mapping, retained-message/suppression policy,
  object naming matrix, internal naming convention, region nesting rules, and
  diagnostic-unavailable behavior.
- [x] Produce the conformance inventory mapping every roadmap validation row to
  an existing focused test, a new M5 test, runtime qualification, or an explicit
  unsupported-topology result. Do not duplicate an already sufficient test.
- [x] Reproduce the focused RHI/Vulkan baseline and preserve the separately
  recorded Editor asset-source aggregate failure as out-of-scope evidence.

#### Acceptance Gate

- The diagnostic, naming, timing, statistics, thread, failure, and test
  contracts are singular and written down; no implementation stage depends on
  choosing between competing public APIs or ownership models.
- Every native object family and roadmap conformance boundary has one named
  owner and one planned observation/test, and the timestamp/query bounds are
  supported by captured hardware properties rather than guessed constants.
- Diagnostic-on and diagnostic-off startup baselines are reproducible, and the
  focused pre-change tests are green on the Agent Build Profile.

#### Frozen public timing contract

- `FRHICapabilities` gains `bSupportsGPUTimestamps` and
  `GPUTimestampNanosecondsPerTick` (`double`). The fields describe only the
  selected immediate graphics queue. The boolean is true only when that queue
  has nonzero `timestampValidBits` and the Vulkan timestamp period is finite
  and positive. Unsupported startup publishes `false` and `0.0`.
- `ERHIGPUTimingResultState` is exactly `Unsupported`, `Pending`, `Ready`, and
  `Invalid`. `FRHIGPUTimingResult` contains that state and one
  `DurationNanoseconds` value; duration is zero unless the state is `Ready`.
- `FRHIGPUTimingQuery` is a counted RHI resource representing one generation
  of one begin/end interval. `FDynamicRHI::RHICreateGPUTimingQuery()` returns
  null when unsupported or bounded storage is exhausted.
  `FDynamicRHI::RHIGetGPUTimingResult()` is a const, nonblocking observation.
  It never submits, flushes, waits, or resets native storage.
- `FRHICommandListBase::BeginGPUTimingQuery()` and
  `EndGPUTimingQuery()` record retained query references. A query must be begun
  once, ended once, and not be reused until its previous result is `Ready` or
  `Invalid`. Double begin, end without begin, cross-list end, unclosed timing,
  and reuse while pending invalidate command-list admission before replay.
- Vulkan query pages contain 128 timestamp slots (64 intervals), with at most
  eight pages and therefore 512 live intervals. A page is created lazily.
  Exhaustion is fail-fast and recoverable: creation returns null and increments
  `ExhaustionCount`; it never waits for an older interval. Each written pair is
  associated with the exact submission token, polled with availability and no
  wait flag after that token completes, then reset/recycled. Pool-creation
  failure publishes no page or query and increments `AllocationFailureCount`.
- Timestamp subtraction masks both values to the selected queue's valid-bit
  width and uses modular unsigned subtraction. Conversion multiplies once by
  the published resolution in extended precision, rounds to the nearest
  integer nanosecond, and saturates at `uint64` maximum while incrementing a
  conversion-overflow counter. Native ticks and period remain private.

#### Frozen aggregate diagnostic schema

`FDynamicRHI::RHIGetDiagnosticSnapshot()` returns one value snapshot and
`RHIResetDiagnosticStatistics()` resets only authorities that already permit
reset plus new M5 interval counters. Reads are bounded and do not wait. The
schema is composed as follows; all counts are `uint64`, sizes are bytes, and
durations are integer nanoseconds unless stated otherwise.

| Section | Frozen fields | Kind and authority | Reset behavior |
| --- | --- | --- | --- |
| Availability | requested, debug-utils supported/active, validation-layer supported/active, messenger active | capability/configuration; `FVulkanDynamicRHI` instance state | preserved |
| Executor | existing `FRHICommandListExecutorStats` value | mixed lifetime counters/live gauges; `GCommandListExecutor` | preserved because the existing authority has no reset contract |
| Graphics cache | existing `FRHIGraphicsCacheStatistics` value | capacities/gauges/resettable counters; `FVulkanDevice` | existing reset; capacities and occupancy preserved |
| Memory | existing `FRHIMemoryStatistics` value | heap/live gauges plus resettable counters/high-water; memory manager/arenas/retirement | existing reset; live allocation, arena occupancy/capacity, heap budgets, and pending retirement preserved |
| Completion | last submitted token, completed token, pending submissions, retirement pending/high-water/released/max lag | token/gauges and resettable pressure; completion tracker plus existing memory authority | tokens and pending gauges preserved; duplicate retirement values are composed from memory statistics |
| Messages | total, error, warning, information, verbose, general, validation, performance, truncation, recursion-drop | saturating callback counters; messenger callback state | counters reset |
| Naming/regions | naming attempts/failures/unavailable skips, label begins/ends/unavailable skips, invalid region count, active region depth/high-water | helper counters plus command-buffer live gauge | active depth preserved; counters reset; high-water becomes current depth |
| Timing | page/interval capacity, allocated pages, live/pending/ready intervals, interval high-water, exhaustion, allocation failure, invalid recording, result polls, ready results, conversion overflow | query manager capacity/gauges and saturating counters | capacities/live states preserved; counters reset; high-water becomes live interval count |

The aggregate does not copy counters into a second accumulating store. Snapshot
construction reads each named authority once. Owner-thread-only Vulkan values
are mirrored into atomics/immutable state when they change; heap-budget refresh
retains the existing memory snapshot boundary. Reset runs on the RHI owner
thread, does not alter rendering or query state, and saturating counters never
wrap silently.

#### Frozen callback, name, and region policy

- Vulkan severities map `verbose -> trace`, `info -> debug`, `warning ->
  warning`, and `error -> error`. A callback increments total and its single
  severity counter. Every set native type bit increments the corresponding
  general, validation, or performance counter; unknown/no type maps to general.
- Callback text is not retained in a second message buffer: engine logging is
  the content sink and snapshot counters are the queryable record. Input is
  bounded to 4,096 UTF-8 bytes, with truncation counted. A thread-local
  recursion guard drops and counts recursive callbacks. The callback returns
  `VK_FALSE` and never changes backend state.
- Logical names are UTF-8, limited to 255 bytes plus terminator, and truncated
  only at a code-point boundary with a counted diagnostic. Internal names use
  `Durin.<role>[.<stable-index>]`; addresses, frame numbers, CPU serials, and
  completion tokens are forbidden. Unavailable debug-utils is a counted no-op.
- Public command regions own up to 255 UTF-8 bytes and nest to a maximum depth
  of 64. Underflow, depth overflow, or unclosed scopes invalidate admission.
  Render-pass labels nest inside the current public region and use the same
  helper; each successful begin has exactly one end in replay order.

| Native family | Name source and owner | Planned observation |
| --- | --- | --- |
| Instance, physical device, logical device, graphics queue | deterministic backend role; dynamic RHI/device | helper unit seam plus startup snapshot/capture |
| Buffer/image and VMA allocation | public create descriptor; resource owner | creation/replacement/failure integration |
| Buffer view/image view/sampler/shader module | parent/public descriptor name plus role; resource/view owner | exact counted resource integration |
| Descriptor-set layout/pipeline layout/descriptor pool | deterministic structural/cache role; owning cache/pool | cache reuse/failure integration |
| Graphics pipeline/render pass/framebuffer | public PSO/pass name plus stable role; pipeline/render-pass cache | draw/cache integration and capture |
| Command pool/command buffer/fence/semaphore | deterministic frame/pool role; pool/frame/viewport owner | completion and repeated-shutdown integration |
| Surface/swapchain/swapchain image/view | viewport name plus image ordinal; viewport/swapchain owner | WSI create/resize/recovery integration |
| Query pool/timing interval | deterministic page and slot role; query manager/query resource | timing pressure/reuse integration |

#### Stage 0 baseline and conformance inventory

Agent Build Profile: `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor`, Debug editor runtime. `vulkaninfo` on 2026-08-11
reported an NVIDIA GeForce RTX 3090 selected candidate, graphics/present family
0, 64 valid timestamp bits, and 1 ns/tick. The unselected Intel UHD Graphics
770 candidate reported 36 bits and approximately 52.0833 ns/tick.

With `DURIN_VULKAN_VALIDATION=off`, hidden startup initialized the RTX 3090 with
four instance extensions. With `on`, it initialized with five; debug-utils and
the locally available `VK_LAYER_KHRONOS_validation` are independently selected
by the proven negotiation contract. The baseline owns no messenger, therefore
no validation messages are routed or counted by Durin. These startup probes
were intentionally stopped after initialization; orderly repeated shutdown is
a Stage 1 and Stage 5 gate.

| Roadmap boundary | Existing authoritative evidence | Missing M5 evidence |
| --- | --- | --- |
| Configuration -> instance/device | `RHIInitializationTests`; `FVulkanInstanceNegotiationTests`; `FVulkanDeviceCandidateTests`; failure-injection initialization tests | messenger lifecycle/classification and on/off repeated startup/shutdown |
| Descriptor -> resource/view | `RHIResourceViewValidationTests`; exact support/factory failure tests; `CreatesExactCountedBufferAndTextureViews` | native naming observation and one public cross-contract scenario |
| Upload/copy -> use | `RHITransferValidationTests`; `PublicCopyMatrixPreservesExactBytesInlineAndThreaded`; texture sampling integration | timing around public transfer/draw work |
| Render pass -> next use | `RHIResourceTransitionValidationTests`; `VulkanResourceTransitionTests`; public copy/sampling tests | nested public region/render-pass label coexistence |
| View/resource -> bindings | command-list reflected binding validation; texture sampling descriptor/cache paths | named descriptor structures in cross-contract churn |
| Graphics state -> draw | command-list graphics state/key tests; Vulkan sampling/draw variants from M3 | named/timed public conformance draw |
| Memory -> completion | `FVulkanMemoryBaselineTrackerTests`, allocation/arena/completion integration; executor serial/frame tests | exact-token query result/reset/reuse and aggregate snapshot |
| Window -> present | viewport candidate rollback/recovery tests and existing hidden editor runtime | supported main/detached create/minimize/resize/recovery/teardown matrix; unsupported topology is an explicit skip/result |
| Diagnostics -> evidence | negotiation tests, existing render-pass label, cache/memory/executor statistics | callback, systematic names/regions, timing, consolidated snapshot |
| Inline -> threaded | command-list/thread suites and paired Vulkan resource/copy tests | paired regions/timing plus cross-contract scenario |
| Final shutdown | RHI thread drain/failure tests, completion tests, prior hidden runtime | messenger stop, pending-query release, repeated diagnostic on/off shutdown |

Focused pre-change results on 2026-08-11: `RHIInitializationTests` 5/5,
`RHICommandListTests` 55/55, `RHIThreadTests` 10/10,
`RHIResourceTransitionValidationTests` 6/6,
`RHIResourceViewValidationTests` 3/3, `RHITransferValidationTests` 4/4,
and `VulkanRHIIntegrationTests` 34/34. The repository-wide baseline remains
1,385/1,386 with only the separately reproduced Editor asset-source relocation
failure described in `Current Status`; Stage 0 did not rerun or reattribute it.

### Stage 1: Owned Vulkan Debug Messenger

Dependencies: Stage 0 callback, availability, and lifecycle contract.

- [x] Extend instance negotiation/publication only as required to distinguish
  requested, supported, and active diagnostic facilities without making them
  required runtime extensions.
- [x] Implement the device-independent debug-utils callback classifier,
  severity/type mapping, recursion guard, bounded message handling, and stable
  counters with unit coverage independent of an installed validation layer.
- [x] Create exactly one messenger after successful instance publication when
  debug-utils is active; include startup/instance-creation messages through the
  selected creation chain if Stage 0 proves that path necessary for complete
  diagnostics.
- [x] Make messenger creation transactional and diagnostic-only. Injected or
  native failure leaves no handle or callback alive, reports one owned
  diagnostic, and follows the Stage 0 policy without corrupting later startup.
- [x] Destroy the messenger before instance teardown after RHI admission has
  stopped and no backend callback can reference destroyed logging state.
- [x] Cover `auto`, `on`, `off`, invalid settings, Shipping policy,
  layer-present/absent, debug-utils-present/absent, callback classification,
  rollback, reinitialization, and shutdown.

#### Acceptance Gate

- Debug and non-debug startup both succeed when their required runtime
  facilities exist; missing optional diagnostics remain non-fatal and visible
  in the snapshot.
- Callback tests prove stable mapping, bounded accounting, recursion safety,
  and no backend mutation, while lifecycle/failure tests prove exactly-once
  messenger destruction before the instance.
- A validation-enabled hidden startup emits through the owned callback and
  shuts down without a late callback, duplicate messenger, or leaked handle.

### Stage 2: Native Object Names and Recorded Command Regions

Dependencies: Stage 1 active debug-utils ownership and Stage 0 naming matrix.

- [x] Add one backend-private helper for naming supported dispatchable and
  non-dispatchable objects and for opening/closing/inserting command-buffer
  labels; centralize function-pointer and extension checks there.
- [x] Propagate existing public debug names to buffers, images, default and
  explicit views, samplers, shaders, graphics pipelines, and other selected
  resources without adding name-based caches or ownership.
- [x] Give internal queues, command pools/buffers, descriptor pools/layouts,
  pipeline layouts/cache, render passes, framebuffers, allocator-owned arena
  pages, swapchains, swapchain views, semaphores, and fences the deterministic
  Stage 0 names that apply to their native object lifetime. Query pools are
  named by their Stage 3 owner when that owner is introduced.
- [x] Add owned recorded begin/end diagnostic-region commands, command-list
  validation, fake-context replay, nested-scope support, and inline/threaded
  payload-lifetime/order tests.
- [x] Route the existing render-pass label through the common helper and label
  selected command-buffer-local pass, transfer, and readback work without
  introducing new submissions or changing render-pass balance. Frame and
  viewport identity use named command buffers, swapchains, images/views, and
  passes because their CPU scopes can span native command-buffer submission;
  no label is kept open across that boundary.
- [x] Add test observation for naming calls and label order without requiring a
  capture tool.
- [x] Capture one validation-enabled Editor frame to prove
  names and nested regions are useful in a real Vulkan capture/diagnostic.

#### Acceptance Gate

- The Stage 0 object matrix is either named after successful creation or
  explicitly documented as unnameable/unavailable; debug-utils absence and
  naming failure do not change resource publication or rendering output.
- Nested regions replay in the same order in both executor modes, retain label
  bytes until replay, reject imbalance before native mutation, and coexist with
  render-pass labels without double-close.
- Validation output or a supported capture identifies selected resources,
  pipeline/pass, viewport, and transfer work by stable diagnostic names.

### Stage 3: Completion-Aware GPU Timing Queries

Dependencies: Stage 0 timing contract, Stage 2 command regions, and the M4 exact
GPU completion authority.

- [x] Publish immutable timestamp-query support and resolution from the
  selected physical-device and immediate-queue properties only after device
  selection succeeds.
- [x] Add the counted public timing-query resource, explicit begin/end recorded
  commands, result-state API, command ownership, balance/reuse validation, and
  fake-context inline/threaded replay coverage.
- [x] Implement bounded device-owned Vulkan query-pool pages, slot generations,
  native reset/write/result operations, creation rollback, failure injection,
  occupancy/high-water/reuse/overflow counters, and shutdown ownership.
- [x] Associate written intervals with the exact submission token, poll only
  completed tokens without Vulkan wait flags, copy ready native values to
  stable CPU results, and recycle slots only after publication and resource
  lifetime permit it.
- [x] Implement valid-bit masking/wrap and checked nanosecond conversion with
  unit tests for 32-bit, wider, wraparound, zero/invalid capability, fractional
  timestamp period, and duration overflow.
- [x] Prove timing around a public-RHI transfer and graphics pass produces a
  pending result before completion and a nonzero ready result afterward in both
  executor modes without `RHIBlockUntilGPUIdle` or a new submission boundary.

#### Acceptance Gate

- Supported hardware publishes honest timing capability and returns bounded,
  monotonic-duration results in nanoseconds; unsupported hardware follows the
  explicit unsupported path without startup or render failure.
- No ordinary result read waits, flushes, submits, or mutates GPU ordering.
  Query slots reset/reuse only from exact completion evidence and remain safe
  across irregular frames, empty submissions, query destruction, pool pressure,
  executor-mode transitions, and shutdown.
- Failure injection leaves no partial query resource/pool and later creation
  recovers according to the selected bounded policy.

### Stage 4: Consolidated RHI Diagnostic Snapshot

Dependencies: Stages 1-3 counters and stable M3/M4 statistics contracts.

- [x] Implement the Stage 0 backend-neutral aggregate snapshot by composing,
  not duplicating, command executor, queue/completion, graphics-cache, memory,
  validation, naming/region, and timing-query authorities.
- [x] Publish diagnostic activation/capability state, queue work/completion
  pressure, query occupancy/results, message severity counts, naming/label
  failures, and existing cache/memory statistics with stable units.
- [x] Make snapshots safe and bounded from their documented caller context;
  replace test-only statistic access with the public observation only where the
  same stable contract now exists, retaining narrow native seams for failure
  and ownership assertions.
- [x] Implement one reset boundary that delegates to existing authorities,
  preserves gauges/capacities/live/pending state, and cannot race destruction,
  query publication, cache mutation, or allocator updates.
- [x] Add deterministic formatting/logging helpers for runtime capture without
  making formatted strings the machine-readable contract or logging every
  frame by default.
- [x] Cover snapshot consistency, reset preservation, counter saturation,
  inline/threaded parity, device-unavailable state, and shutdown snapshots.

#### Acceptance Gate

- One snapshot attributes executor backlog, GPU completion/query pressure,
  cache/descriptor behavior, memory/transfer pressure, diagnostics, and timing
  without native types or implicit waits.
- Repeated reads do not alter rendering or counters; reset clears only declared
  interval observations and preserves every live gauge, capacity, capability,
  pending token/query, and current resource.
- Stable public observations replace equivalent test-only counters where
  appropriate, and focused M3/M4 statistics tests remain green.

### Stage 5: Public-RHI Conformance and WSI Qualification

Dependencies: Stages 1-4 and the completed M0-M4 lasting contracts.

#### Maintained Conformance Matrix

| Boundary | Public/focused evidence | Qualification result |
| --- | --- | --- |
| Configuration, capability, repeated startup/shutdown | `RHIInitializationTests`; `FVulkanInstanceNegotiationTests`; `InitializationFailuresRollbackAndReleaseTheBackendModule`; diagnostic-on/off hidden Editor | supported on the Agent Build Profile; optional diagnostics remain recoverable |
| Creation, expected failure, replacement, release | `CapabilitiesAndExactTextureSupportRejectBeforeNativeCreation`; `RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread`; `FVulkanPublicRHIConformanceTests.PublicRHIConformanceDrawMatchesPixelsAndDiagnosticsAcrossModes` | public unsupported descriptors fail before native creation; replacement and final release drain cleanly |
| Upload, transition, copy, readback | `PublicCopyMatrixPreservesExactBytesInlineAndThreaded`; `RHITransferValidationTests`; `VulkanResourceTransitionTests` | exact bytes match in both executor modes |
| Exact view/binding, graphics state, draw | `CreatesExactCountedBufferAndTextureViews`; `PublicRHIConformanceDrawMatchesPixelsAndDiagnosticsAcrossModes`; M3 graphics-state tests | public descriptor binding and RGBA8 pixels match in both modes |
| Regions, timing, completion, snapshot | `PublicTransferTimingBecomesReadyWithoutImplicitWait`; `DiagnosticSnapshotComposesAuthoritiesAndResetPreservesLiveState`; paired public conformance draw | balanced labels, nonblocking timing, exact completion, and bounded public observations pass |
| Main/detached WSI lifecycle | `FVulkanPublicRHIConformanceTests.ViewportOutputCandidatesFailAtomicallyAndRecover`; hidden Editor | main and detached create, public clear/present, unavailable output, resize, candidate rollback/recovery, teardown pass; no synthetic out-of-date/suboptimal result seam exists |
| Irregular/stress behavior | `EmptyIrregularFramesAdvanceBySubmissionRatherThanFrameAge`; timing exhaustion/reuse; `ThreadedResourceCreationAndUniformOverflowStayRHIThreadOwned` | empty frames, bounded arenas/cache/query capacity, 16-frame replacement churn, and drained public snapshot gauges pass |
| Executor drain/failure/shutdown | `RHIThreadTests`; pending-query release; messenger lifecycle tests; repeated WSI teardown; diagnostic-on/off hidden Editor | waiters wake, late work rejects, owners drain in order, and no callback/query/viewport survives shutdown |

GPU-backed rows remain in `VulkanRHIIntegrationTests`, which owns the
`durin-gpu` resource lock. Validation-only rejection paths remain in the
headless RHI targets. Minimize is represented by the public unavailable-output
contract because the platform window abstraction has no minimize request API;
live Win32 out-of-date/suboptimal results are handled but cannot be injected by
the current test seam, so the matrix records that limitation explicitly.

- [x] Materialize the Stage 0 conformance inventory as a maintained matrix in
  the test organization and this plan, referencing existing tests and adding
  focused cases only for missing boundary combinations.
- [x] Add public-RHI inline/threaded scenarios that cross creation,
  upload/transition, exact view/binding, draw, copy/readback, diagnostics,
  completion, replacement, expected creation failure, and resource release
  without Vulkan escape hatches for functional results.
- [x] Exercise main and detached editor viewport create, render/present,
  minimize/unavailable output, resize, suboptimal/out-of-date recovery where
  injectable, teardown, and shutdown on the negotiated supported topology.
- [x] Prove validation-on and validation-off startup, optional diagnostics
  unavailable, repeated initialize/shutdown where supported, executor drain,
  failure wakeup, and no callback/query/native object surviving its owner.
- [x] Run stress with irregular frames, empty submissions, cache pressure,
  transfer-arena reuse, pending timestamps, viewport churn, and resource
  replacement while observing bounded snapshot gauges and exact completion.
- [x] Keep GPU-backed cases under the existing `durin-gpu` resource lock and
  preserve headless/fake coverage for rejection and failure paths that do not
  need hardware.

#### Acceptance Gate

- The roadmap validation matrix has explicit passing evidence or an honest
  unsupported-topology result for every row; no required row relies only on a
  raw Vulkan integration callback.
- Both executor modes produce identical functional bytes/pixels and ordering,
  with diagnostic differences limited to configured optional facilities and
  timing values.
- Supported main/detached viewport lifecycle and orderly shutdown complete
  validation-clean without cross-viewport device idle, stale callbacks,
  pending query leaks, unretired native objects, or unbounded diagnostic state.

### Stage 6: Lasting Contract and Final Handoff

Dependencies: Stages 0-5 accepted.

- [x] Move stable diagnostic configuration, messenger lifetime, native naming,
  command-region, GPU timing, statistics, conformance, and shutdown rules into
  the owning `Documentation/Runtime/Rendering/` contract.
- [x] Update the RHI/Vulkan roadmap with M5 completion evidence, mark or defer
  C1-C4 using their reviewed entry evidence, and close the required roadmap
  only when its synchronous-compute wording remains accurately owned by the
  Compute Shader Pipeline roadmap.
- [x] Update related capability, command execution, graphics binding, memory,
  transition/view, viewport, build/test, and code comments only where M5
  changed their lasting public contract.
- [x] Run focused RHI and Vulkan targets, the repository native aggregate, the
  full Debug Editor build, diagnostic-on/off hidden runtime, supported viewport
  qualification, and final snapshot/capture collection through DurinDevTool.
- [x] Record exact test/build/runtime evidence, hardware/profile, any unchanged
  external failure, counter/capture comparison, and deferred conditional work
  in `Current Status` before marking the plan completed.

#### Acceptance Gate

- Lasting documentation, code, tests, configuration, and roadmap status agree
  on the supported diagnostics/timing/conformance contract, optional facility
  behavior, and remaining conditional milestones.
- Focused tests, required aggregate validation, full Debug Editor build,
  diagnostic-on/off runtime, supported WSI lifecycle, and orderly shutdown pass
  with attributable evidence and no M5 regression.
- The plan is marked completed only after all required stages and acceptance
  gates pass; implementation details no longer live solely in this plan.

## Validation Matrix

| Boundary | Required coverage | Evidence target |
| --- | --- | --- |
| Configuration -> diagnostic activation | `auto`/`on`/`off`, invalid and Shipping policy, layer/debug-utils present/absent, requested/supported/active state | RHI initialization and Vulkan instance-negotiation tests plus diagnostic-on/off runtime |
| Instance -> messenger -> shutdown | Transactional creation, callback classification, recursion/suppression, injected failure, exactly-once destruction before instance | Vulkan focused unit/integration tests and repeated hidden startup/shutdown |
| Public debug name -> native object | Selected resource and internal-object matrix, failed candidate, replacement, debug-utils unavailable, naming failure | Vulkan naming observation seam, failure injection, and capture evidence |
| Recorded region -> native labels | Owned text, nesting, imbalance rejection, render-pass coexistence, inline/threaded replay order | RHI fake-context command tests and Vulkan label-order integration |
| Device/queue -> timing capability | Valid bits, period, unsupported path, publication only after successful init | Capability/selection unit tests and hardware property evidence |
| Timing commands -> CPU result | Begin/end balance, retained lifetime, pending/ready/invalid states, wrap/conversion, no implicit wait | RHI command tests, conversion unit tests, Vulkan hardware transfer/draw timings |
| Query slot -> GPU completion | Exact token association, pool pressure, reset/reuse, destruction, irregular/empty frames, shutdown | Vulkan completion/query integration and failure injection |
| Authorities -> diagnostic snapshot | Stable units, gauges/counters, thread-safe read, reset preservation, saturation, unavailable device | RHI/Vulkan focused statistics tests |
| Resource creation -> use -> release | Supported descriptors, views, bindings, graphics state/draws, transfer/readback, replacement and expected failure | Existing M0-M4 focused tests plus one public cross-contract conformance scenario |
| Main/detached window -> present | Create, unavailable/minimized, resize, suboptimal/out-of-date, recovery, teardown, negotiated topology | Vulkan viewport integration and hidden Editor runtime |
| Inline -> threaded executor | Same functional output/order; payload retention, RHI affinity, backpressure/failure wake, drain | Existing RHI thread tests and paired Vulkan conformance scenarios |
| Runtime -> final shutdown | Admission stop, CPU drain, GPU completion, query publication/release, callback stop, reverse native destruction | Stress integration and diagnostic-on/off hidden runtime |

Routine implementation stages use the smallest affected native target through
the root [native-test workflow](../Development/Build/NativeTests.md). The full
native aggregate is reserved for the Stage 6 cross-target gate. Configure,
build, runtime, and recovery operations follow the root
[build-and-run contract](../Development/Build/BuildAndRun.md).

## Definition of Done

- The Vulkan instance owns an optional, configuration-aware debug messenger
  whose callback, rollback, accounting, and shutdown lifetime are tested.
- Supported native Vulkan objects and command regions carry systematic,
  diagnostic-only names/labels without affecting publication, cache identity,
  execution, or Shipping startup requirements.
- The immutable RHI capability snapshot honestly publishes selected-queue GPU
  timestamp support and resolution.
- Counted timing intervals record/replay identically inline/threaded, publish
  nonblocking nanosecond results, and recycle bounded native query storage only
  after exact GPU completion.
- One backend-neutral snapshot exposes diagnostic activation, executor,
  queue/completion, timing, cache, memory, message, naming, and region pressure
  with safe reads and documented reset semantics.
- Existing M0-M4 tests and new M5 tests form an explicit public-RHI conformance
  matrix across resource, draw, transfer, failure, WSI, execution-mode, and
  shutdown boundaries.
- Debug/non-debug and diagnostic-available/unavailable startup work; supported
  main/detached viewport runtime is validation-clean and identifies selected
  objects/passes/scopes in diagnostics or capture.
- No new whole-device idle, implicit query wait, Vulkan escape hatch,
  unbounded retained diagnostic state, name-based correctness, or test-only
  implementation path is introduced.
- Lasting behavior is documented under `Documentation/Runtime/Rendering/`, the
  roadmap records M5 evidence, required validation passes, and all conditional
  follow-ups remain explicitly gated.

## Deferred Follow-ups

- Render graph and transient aliasing remain C1 and require measured transition
  complexity, transient pressure, or scheduling cost.
- Bindless descriptors remain C2 and require measured descriptor pressure plus
  an acceptable target-hardware capability floor.
- Separate transfer/compute queues, timeline coordination, calibrated
  timestamps, and cross-queue timing remain C3 or the Compute Shader Pipeline
  roadmap and require measured overlap benefit.
- Device-loss recovery remains C4 and requires product policy plus a complete
  Renderer resource-resubmission inventory.
- Pipeline-statistics queries, occlusion queries, timestamp calibration,
  vendor performance counters, continuous frame profiling, capture automation,
  and an Editor diagnostics UI require selected consumers and separate plans.
- The Editor asset-source relocation aggregate failure remains outside M5 and
  requires its own investigation or task if it is still present at final
  qualification.

## Related Documentation

- [RHI and Vulkan Backend Evolution roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [Compute Shader Pipeline roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [RHI capabilities and Vulkan startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [RHI resource transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Graphics state and bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Vulkan memory and GPU completion](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and run](../Development/Build/BuildAndRun.md)
- [Native tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICapabilities.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/DynamicRHI.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanExtensions.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanExtension.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCommandBuffer.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCommandBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDiagnostics.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDiagnostics.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIInitializationTests.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanMemoryPolicyTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
