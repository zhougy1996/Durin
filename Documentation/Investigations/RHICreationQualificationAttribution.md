# RHI Creation Qualification Memory And Frame-Tail Attribution

**Status:** Open — validation DLL lifetime retention reproduced independently; allocation-stack and host-wait attribution remain unresolved

**Last reviewed:** 2026-09-10

## Scope And Verdict

Follow-up to the [RHI resource creation refactor](../Plans/Archive/2026-09/RHIResourceCreationRefactor.md)
and its repeated qualification. No production behavior or validation checks were changed. The follow-up
standardizes validation DLL lifetime in the owning qualification fixtures; see
[the long-term test policy](../Development/Build/NativeTests.md#vulkan-creation-qualification-memory). The experiments identify two distinct
issues, not one asynchronous-creation regression:

- **P1: Process private-memory retention** is reproducible outside the engine
  through repeated Vulkan instance creation/destruction with the validation
  layer enabled. Keeping the validation DLL loaded across those lifetimes
  removes most growth in the isolated reproduction. DLL unload/reload and its
  allocator lifetime are now the primary causal lead; allocation stacks and
  exact retaining owners remain unresolved. This does not prove that every
  retained byte in the complete engine fixture has the same owner.
  Updating the validation layer to SDK 1.4.357.0 did not reduce retention in
  either an old-SDK binary or a newly rebuilt binary; see the controlled SDK
  follow-up below.
- **P2: Following-frame p95** is dominated by host-side completion waiting in
  this fully synchronized fixture. Measured GPU work is much shorter. Polling
  memory instrumentation perturbs the frame median but does not explain all
  tail latency. Driver, scheduling and wait implementation contributions still
  need host tracing; GPU timestamps alone cannot separate them.

These results do not waive the original performance budgets. Diagnostic-off
measurements are attribution controls, not replacement acceptance baselines.
The implemented ownership contract remains in
[Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md).

## Verified Findings

### P1: Diagnostics-dependent process memory

The September 9 comparisons used Windows Debug, the i7-12700/RTX 3090, driver 2480242688,
Balanced power and default process affinity/QoS. A live module inspection during
the final lifecycle probe confirmed
`G:/Programs/VulkanSDK/1.3.283.0/Bin/VkLayer_khronos_validation.dll` and the system
`vulkan-1.dll`. Diagnostic subfeature settings were taken from that SDK's
`Bin/VkLayer_khronos_validation.json`, not guessed environment names.

The comparable group contains 300 device lifetimes and 11,820 synchronous PSO
requests. Values below are sampled process private maxima, rounded MiB:

| Configuration | PSO-group maximum | Interpretation |
| --- | ---: | --- |
| Normal diagnostics, preceding repeat | 773 | Reproduces growth across lifetimes |
| Diagnostics off | 312 | Large reduction; both validation and debug-utils are disabled by this engine setting |
| Validation active, subsequent object names/labels disabled | 769 | Naming/test-event recording is not sufficient to explain the growth |
| Thread-safety checks disabled | 774 | Does not eliminate growth |
| Shader checks disabled | 775 | Does not eliminate growth |
| Core checks disabled | 548 | Partial reduction, not elimination |

The normal resource-batch maximum after the original PSO group was 847 MiB;
diagnostics-off was 329 MiB. These resource figures must not be compared with
the PSO-only maxima as if they represented the same process age. Shader/seed
inputs were restored identically. All listed selections passed correctness and
native-creation counts. Subfeature rows record requested `VK_LAYER_DISABLES`
settings while retaining the validation layer; they do not claim per-check
coverage telemetry. No SDK files or persistent environment settings were edited.

The separate lifecycle probe compares empty initialization, creation on the RHI
owner, caller creation, caller creation with timing capture, and caller creation
with memory sampling. Every lifetime ended with zero pending RHI deletions.
The expanded initial probe's shared Debug CRT live allocation count increased
from roughly 2.55 MB after its first initialized lifetime to 6.64 MB at the end,
while private commit reached roughly 349 MB. Debug CRT accounting does not cover
every DLL/driver heap and cannot prove absence of an engine leak. The probe did
not reproduce the complete suite's large increase from thread migration alone;
its cache reuse and process history differ from the frozen cold/hot suite.

The resource lifetime audit found no missing deletion corresponding to hundreds
of MiB in the inspected RHI retirement, creator service or thread-owner paths.
That negative code review is weaker evidence than allocation stacks. The next
useful evidence is allocation/retention stacks with the loaded validation DLL
identified, or the same inputs against a separately controlled validation-layer
version. Disabling correctness checks is not a production fix.

### SDK 1.4.357.0 follow-up, September 10

The user installed `D:/Programs/VulkanSDK/1.4.357.0`. The machine environment
and explicit-layer registry selected the new SDK, while the existing agent
process environment and four CMake Vulkan cache entries still selected
`G:/Programs/VulkanSDK/1.3.283.0`. Two sequential qualification runs separated
runtime layer selection from recompilation:

1. The first run retained the old-SDK VulkanRHI binary. Only the material
   diagnostic test source needed recompilation. Live process module inspection
   confirmed the new SDK's `Bin/VkLayer_khronos_validation.dll` and the system
   `vulkan-1.dll` were loaded.
2. Configure explicitly updated `Vulkan_INCLUDE_DIR`, `Vulkan_LIBRARY`,
   `Vulkan_GLSLANG_VALIDATOR_EXECUTABLE`, and `Vulkan_GLSLC_EXECUTABLE` to the
   new SDK. Command-local `VULKAN_SDK` and `VK_SDK_PATH` also selected it.
   Incremental compilation rebuilt VulkanRHI, including its PCH and allocator
   implementation, and both qualification executables successfully. The Ninja
   include paths and CMake cache selected 1.4.357.0; live module inspection
   again confirmed the new validation DLL. No source compatibility fix or
   clean rebuild was needed.

Both runs retained Debug default validation/debug-utils, normal memory polling,
the frozen cache seeds, and Balanced power. The GPU driver remained 2480242688.
The summarizer validated all fixed request/frame counts and asynchronous
contracts. Seed, shader and resource-input hashes matched the pre-refactor
baseline. The material scene receipt differs from the older repeat only by
five added diagnostic-setting lines, all unset; both new scene receipts are
byte-identical. The newer optional material metrics explain the changed metric
set. Raw hash/metric-set mismatch flags are retained, not overridden.

Process private-memory sampled maxima, MiB (not GPU memory or retained bytes
after shutdown):

| Group | Old SDK/layer repeat | New layer, old SDK binary | New layer, rebuilt SDK binary |
| --- | ---: | ---: | ---: |
| Legacy PSO group, 300 device lifetimes | 773.24 | 1868.64 | 1885.36 |
| Resource group, after legacy PSO group | 847.07 | 2077.26 | 2062.64 |
| Separate material process | 494.52 | 567.02 | 573.04 |

Timing, microseconds, median / nearest-rank p95:

| Measurement | Old SDK/layer repeat | New layer, old SDK binary | New layer, rebuilt SDK binary |
| --- | ---: | ---: | ---: |
| Cold graphics native creation | 5674.35 / 6988.30 | 6753.25 / 14169.80 | 6450.65 / 8324.40 |
| Cold compute native creation | 2341.85 / 2586.70 | 3189.80 / 7305.70 | 2694.25 / 4553.00 |
| Hot graphics request | 8.60 / 9.00 | 20.80 / 62.60 | 9.00 / 13.30 |
| Hot compute request | 2.60 / 3.70 | 10.00 / 33.10 | 2.60 / 7.50 |
| Material first frame | 321285.55 / 327830.00 | 351996.75 / 816868.40 | 346698.40 / 369381.80 |
| Material following frame | 3084.95 / 4960.00 | 3459.05 / 8035.00 | 3305.85 / 4954.00 |

The increased process peaks reproduce across both new-layer runs. Recompilation
does not remove them. This is evidence against SDK upgrade alone resolving the
observed memory issue, not allocation-stack proof of a layer leak. Timing
varies substantially between the two runs, especially hot requests that do not
perform native creation. These sequential, non-randomized observations do not
isolate all host load/scheduling effects or establish an SDK timing regression.
Following-frame p95 in the rebuilt run is effectively unchanged from the older
repeat. No performance acceptance or ordinary single-device editor memory
increase of this magnitude is claimed.

Both qualification targets passed in each run; Vulkan reported four passed
cases and the intentionally skipped opt-in lifecycle probe, and material
reported one passed case. This was bounded qualification, not a full functional
suite rerun. Local receipts and summaries:

- Layer-only build: `Build/.agent-state/logs/20260910-024501-869600-7548-cmake.log`.
- Layer-only test: `Build/.agent-state/logs/20260910-024512-541494-7548-ctest.log`.
- New SDK configure: `Build/.agent-state/logs/20260910-025048-302903-28220-cmake.log`.
- New SDK build: `Build/.agent-state/logs/20260910-025119-069802-43164-cmake.log`.
- New SDK test: `Build/.agent-state/logs/20260910-025142-470791-43164-ctest.log`.
- `Build/creation-sdk357-layer-only.json` and `Build/creation-sdk357-rebuilt.json`,
  compared against `Build/creation-baseline-complete-a.json`; the table's old
  SDK column comes from `Build/creation-refactor-repeat-comparison.json`.

Raw Vulkan runs are `run-p43560-1763a371bb984abe98f9416156ab57b0` and
`run-p30032-5eba670927e844cd983b94087c165d87`; corresponding material runs are
`run-p2860-a3f0af7447874793a87eb041d8a300f9` and
`run-p15684-a6077954ed2444499eef693000c75dc4`. They remain under each target's
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/<Target>/Work/Runs/` directory.
The tested source revision was `cf80c83b2`; this follow-up changes only this
investigation record. The active agent's inherited environment still requires
a restart to refresh globally; explicit command-local overrides were used here.

### DLL lifetime isolation and repeat, September 10

A fresh bounded qualification reproduced substantial private-memory growth.
The current driver receipt is **2584739840**, versus **2480242688** in the
earlier SDK follow-up. Absolute differences from that older run therefore
cannot be assigned to the SDK or engine alone. Both new controlled runs use
the same current driver, Debug binaries, SDK 1.4.357.0, frozen seed bytes and
normal memory polling. Live inspection confirmed the SDK validation DLL.

At revision `250054ed0`, the temporary opt-in
`DURIN_CREATION_DIAGNOSTIC_KEEP_LAYER_LOADED` accepted an absolute
DLL path and retains one `LoadLibraryW` reference until process termination.
Both qualification fixtures apply it before initialization and record it in
their host receipts. It does not disable validation or debug-utils; both
remain active in the retained-layer receipts. It changes DLL lifetime and
allocator reuse, so it is an attribution control, not an acceptance baseline
or a production fix.

Sampled private-memory maxima, MiB:

| Group, in process execution order | Normal DLL lifetime | DLL retained | Peak reduction |
| --- | ---: | ---: | ---: |
| Legacy PSO group, 300 device lifetimes | 2011.70 | 475.37 | 76.4% |
| Resource group, same process | 2332.86 | 627.83 | 73.1% |
| Async group, same process, another 300 lifetimes | 4177.93 | 516.59 | 87.6% |
| Material, separate process | 751.66 | 625.27 | 16.8% |

The first legacy round was approximately 246 MiB in both runs. These are
group maxima, not a sum, and the async group inherits earlier process history.
The new driver and larger complete-process coverage must not be hidden by
comparing the async maximum against an older PSO-only maximum.

The independent Windows DLL lifetime probe (preserved at revision `250054ed0`
as `Engine/Tests/Native/VulkanRHITests/Tools/ProbeValidationLayerLifetime.py`)
did not load the engine or call any Vulkan API. It repeatedly loads the exact
validation DLL and releases its reference, samples private commit after each
release, and checks whether that DLL is still loaded:

| 100 load/free pairs, fresh Python processes | After first pair | After last pair | DLL after each free |
| --- | ---: | ---: | --- |
| Normal unload | 11.60 MiB | 236.16 MiB | Absent |
| One extra reference retained | 12.68 MiB | 12.68 MiB | Present |

This isolates a reproducible retention path to DLL load/unload, independently
of shaders, PSOs, GPU resources, engine retirement, the memory polling thread,
or RHI thread migration. A separate `MIMALLOC_VERBOSE=1` two-pair run reports
per-load allocator initialization and teardown, `destroy_on_exit=0`, and about
**2.2 MiB still committed in a 1 GiB reserved arena** at teardown. The committed
amount agrees with roughly 2.27 MiB of private growth per load/free pair;
reserved address space is not physical memory. These logs strongly implicate
allocator arena retention during DLL unload. Allocation stacks and exact
allocator cleanup sites have not yet been captured.

Earlier isolated Vulkan-call probes give consistent supporting evidence:
100 empty device lifetimes rose from 50.63 to 289.38 MiB with validation,
versus 48.80 to 57.32 MiB without it. Changing to a fresh thread per lifetime
still reached 291.16 MiB. Instance-only lifetimes reached 250.02 MiB after
100 iterations; retaining the layer held 300 iterations to 38.90 MiB.
Disabling core checks alone still reached 285.21 MiB. An experimental
`MIMALLOC_DESTROY_ON_EXIT=1` run reached 56.53 MiB but did not complete normally
after its final sample and was interrupted; it is **not** a validated workaround.
No persistent environment or SDK installation was changed.

The immediate cause of most complete-fixture growth is therefore validation
DLL lifetime churn and associated allocator retention. The retained control
still has nonzero engine/driver/allocator memory, especially in the material
process; it does not establish the ownership of every remaining byte or prove
steady-state editor memory behavior. The external allocator cleanup remains unresolved. The owning qualification
fixtures now use a process-lifetime layer policy as described in
[Native Test Execution](../Development/Build/NativeTests.md#vulkan-creation-qualification-memory),
without an engine-runtime change. They bootstrap the loader-selected layer on
the test main thread before measurement and preserve all ordinary checks.
Acquiring a reference only after the first RHI initialization was insufficient:
that attempted implementation crashed both qualifiers and is not the adopted
policy. Its log is `Build/.agent-state/logs/20260910-032307-960848-31832-ctest.log`.

The standalone DLL probe, local Python helper scripts, opt-in
`CreationThreadMemoryAttribution` case, and temporary naming, endpoint-memory,
GPU-query and absolute-layer-path controls were removed after attribution.
The formal creation tests and continuous memory sampler remain. Historical
probe code is recoverable from `250054ed0`; the commands and settings below
describe past measurements, not current supported test options. CSV/log
evidence and the comparison summaries were retained.

The finalized main-thread bootstrap and cleanup passed both owning qualification
targets in `Build/.agent-state/logs/20260910-032736-315896-39784-ctest.log`.
The existing `ResourceBatches` case also passed with validation disabled in
`Build/.agent-state/logs/20260910-033014-842694-25900-ctest.log`, exercising the
bootstrap bypass. These are correctness checks of the adopted test policy,
not new performance baselines.

Both bounded selections passed. The existing summarizer validated legacy
request counts, resource observations, all 3,630 material frames, 60,000
instrumentation observations and async contracts. Seed, shader and resource
hashes match the prior rebuilt-SDK run; material shader hashes also match.
The material scene receipt changes with the new driver/API and added setting;
between the new pair it differs only in the retained-layer setting. Raw hash
mismatch flags remain visible. Timing is diagnostic only; no exclusive quiet
GPU lane or performance acceptance is claimed. No broader functional suite
was required for these opt-in qualification changes.

Local evidence:

- Normal test: `Build/.agent-state/logs/20260910-031220-823328-37572-ctest.log`.
- Retained-layer test: `Build/.agent-state/logs/20260910-031455-654278-23040-ctest.log`.
- Summaries: `Build/creation-sdk357-memory-repeat.json` and
  `Build/creation-sdk357-layer-retained.json`.
- Normal Vulkan/material raw runs: `run-p24148-e65e9cdcee5146e0b1b0a0cd46436ee4`
  and `run-p31504-3bbf8186124f4233a86b03036ff31f4d`.
- Retained Vulkan/material raw runs: `run-p34616-645bfd2a4c1e45a6beb8d1f5d2279514`
  and `run-p33680-5356189027c743719f26fc260f4decb7`.
- Independent probe: `Build/validation-dll-unload.csv`,
  `Build/validation-dll-retained.csv`, and `Build/validation-dll-verbose.log`.
- Supporting Vulkan-call evidence: `Build/minimal-*.csv`. The local scripts
  have been removed; the historical tracked probe is available in Git.

### P2: Host completion waits and observer perturbation

Both the pre-refactor baseline and post-refactor repeat contain **zero native
PSO creations across all 3,600 following frames**. The repeat records two
synchronous operations and six submission serials per following frame. The
baseline has the same counts in 3,570 frames and four additional synchronous
operations/serials in the first following frame of each lifetime. Additional
PSO compilation or increased steady-state submission count does not explain
the observed following-frame tail.

The qualifier deliberately flushes replay and then calls `RHIBlockUntilGPUIdle`
on every frame. Its result is a synchronized latency measurement, not normal
multi-frame throughput. Retained timing columns split replay completion from
the subsequent GPU-idle call. The historical opt-in reusable GPU timestamp query surrounded
the RenderView commands; query publication was polled on the owner after the
measured interval, before reuse.

Values are microseconds, median / nearest-rank p95:

| Measurement | Normal polling | Endpoint memory only | GPU-query diagnostic |
| --- | ---: | ---: | ---: |
| Complete following frame | 3065.35 / 4991.70 | 2520.50 / 4521.20 | 2963.50 / 4815.60 |
| Render preparation/recording | 1683.30 / 2317.50 | 1377.70 / 1773.80 | 1587.50 / 2068.30 |
| Replay flush completion | 531.30 / 736.70 | 413.65 / 612.20 | 526.00 / 729.20 |
| Subsequent host GPU-idle call | 717.60 / 2362.20 | 635.90 / 2423.00 | 733.75 / 2379.00 |
| RenderView GPU timestamp interval | Not captured | Not captured | 90.944 / 144.480 |

The slowest 5% of complete frames in the GPU-query run averaged only 122.475 us
of measured GPU work. The timestamp interval excludes work outside its begin/end
commands and cannot directly be subtracted from the host wait: execution and
waiting overlap. Nevertheless, these observations localize the large tail to
host completion rather than demonstrating expensive RenderView shader execution.
Turning all Vulkan diagnostics off also left following-frame p95 at 4995.60 us,
so the memory issue and frame-tail issue cannot be conflated.

The ordinary memory sampler creates a DXGI adapter and a polling thread for
each frame, querying process/video memory approximately every millisecond.
Endpoint-only sampling removes that thread while preserving two snapshots;
its lower median is evidence of observer perturbation. These are separate
process runs, not randomized paired observations, so the full difference cannot
be assigned exclusively to polling. Endpoint snapshots can miss transient peaks
and must never replace continuous peak-memory qualification silently.

## Reproduction And Evidence

Use the repository test launcher and `qualification` mode. The existing
`MaterialCreationQualificationTests` and `VulkanCreationQualificationTests`
remain the owning targets. The historical diagnostic settings below applied
to one child process and were recorded in its host receipt. The
`DURIN_CREATION_DIAGNOSTIC_*` controls and lifecycle attribution case have now
been removed; only ordinary qualification configuration remains supported:

- `DURIN_VULKAN_VALIDATION=off`: existing combined diagnostic-off control.
- `VK_LAYER_DISABLES`: SDK-defined subfeature control; tested individually with
  `VK_VALIDATION_FEATURE_DISABLE_THREAD_SAFETY_EXT`,
  `VK_VALIDATION_FEATURE_DISABLE_SHADERS_EXT`, and
  `VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT`.
- `DURIN_CREATION_DIAGNOSTIC_NO_OBJECT_NAMES=1`: only the legacy
  `ColdHotAndConcurrentRequests` case resets debug-utils device entry points
  after initialization, retaining the validation layer and initial device names.
- `DURIN_CREATION_DIAGNOSTIC_ENDPOINT_MEMORY=1`: endpoint-only memory snapshots.
- `DURIN_CREATION_DIAGNOSTIC_GPU_QUERY=1`: RenderView GPU duration and explicit
  query-result checks; zero `gpu_work_ns` in ordinary runs means not measured.
- `DURIN_CREATION_DIAGNOSTIC_LIFETIME_MODE=all`, or one of `empty`, `owner`,
  `caller`, `caller-traced`, `caller-sampled`: opt-in
  `CreationThreadMemoryAttribution`, 30 lifetimes per mode. It is last and skipped
  by default so normal qualification process history is preserved.

Logs under `Build/.agent-state/logs/`:

- `20260909-035715-091194-27580-ctest.log`: full diagnostics-off control.
- `20260909-040302-357698-25672-ctest.log`: expanded lifecycle probe.
- `20260909-040522-685444-3224-ctest.log`: endpoint-only material measurement.
- `20260909-040617-216453-8240-ctest.log`: normal polling with split host timing.
- `20260909-040859-426506-32500-ctest.log`: successful GPU-query measurement.
- `20260909-041027-150857-26412-ctest.log`: no subsequent object naming.
- `20260909-041312-227899-2428-ctest.log`: thread-safety-disable control.
- `20260909-041513-566481-20008-ctest.log`: shader-disable control.
- `20260909-041713-770377-28584-ctest.log`: core-check-disable control.
- `20260909-041920-712249-19724-ctest.log`: final opt-in lifecycle probe, all five modes.

`Build/creation-material-attribution.json` contains the backward-compatible
summarizer's six material datasets, including the optional replay/GPU fields.
Its validation checked complete frame counts, timestamps and memory observations.

Every log records its preserved raw work directory. The initial GPU-query
attempt returned Pending because ordinary resource collection does not poll
the timing manager. The diagnostic was corrected to use the existing owner
timing-poll seam and then passed all 3,630 frames. Its failed samples are not
included in the table. No production repair, broader functional-suite pass,
stable timing acceptance, or complete internal allocation attribution is claimed.

Relevant implementation:

- [Material qualifier](../../Engine/Tests/Native/EngineTests/Private/MaterialCreationQualificationTests.cpp)
- [Vulkan qualifier](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanCreationQualificationTests.cpp)
- [Memory sampler and configuration receipt](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanCreationQualificationSupport.h)
- [Vulkan diagnostic policy](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp)
- [RHI deferred retirement](../../Engine/Source/Runtime/RHI/Private/RHIResources.cpp)
- [Device-owned pipeline service](../../Engine/Source/Runtime/RHI/Private/RHIPipelineCreation.cpp)
