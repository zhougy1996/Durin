# Windows Uncapped Presentation Responsiveness

**Status:** Open

**Last reviewed:** 2026-10-26

## Scope And Verdict

The reported failure is near-total desktop unresponsiveness with VSync disabled
and no frame-rate cap. The user subsequently confirmed that it occurs beyond
empty NewLevel, with empty scenes making it worse; retaining a 60 FPS cap does
not produce the same pronounced stall. Empty NewLevel remains the historical
reproduction fixture, not a necessary condition.
The acceptance question is whether uncapped rendering remains responsive, not
whether a fixed FPS limit reduces load.

Static inspection at `947bde4f9` identifies a Windows presentation/message-pump
wait-cycle hazard. A bounded, GPU-free Win32 control verifies the underlying
wait mechanism. Neither the control nor the existing thread snapshots proves
that this cycle caused the reported desktop-wide incident. GPU scheduling,
presentation-engine latency, and driver/layer contributions remain unresolved.
A subsequent bounded editor A/B at `be4000ab9` reproduced user-observed desktop
stutter with normal Immediate presentation while real frame progress continued
at 159-181 FPS. Offscreen rendering showed no comparable long probe wait;
clear-only Immediate presentation also produced a seconds-long probe wait.
This prioritizes presentation/composition interaction over scene complexity,
but does not identify a WDDM scheduling cause. No driver or HAGS setting was
changed, and all temporary runtime instrumentation was removed after the test.

A subsequent production policy change selects Mailbox with FIFO fallback for
Windows Vulkan's `Unsynchronized` policy. The short Mailbox verification below
confirms selection but still observes a long external flush wait, and the user
confirmed that noticeable desktop stutter persisted. Mailbox did not resolve
the incident. The current mode mapping is defined by the viewport contract.

Initial Tracy-off and reduced-instrumentation controls did not show the
seconds-long external probe waits, but the user did not observe those runs.
An attended retest at `1234029af` reproduced uncapped desktop stutter after
the cleanup. At a 60 FPS configured cap, the user reports a stuttering live
Tracy interface but smooth command-line capture and offline viewing, including
offline viewing alongside the editor. The cleanup did not resolve the original
incident. Live-view processing and uncapped presentation remain separate leads;
the results do not yet prove a common cause.

Related contracts: [viewport rendering](../Runtime/Rendering/Pipeline/ViewportRendering.md),
[runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md), and
[RHI command execution](../Runtime/Rendering/RHI/RHICommandExecution.md).

## Verified Findings

### P1: Presentation dependencies can block the window message pump

The ordinary-frame dependency chain is:

1. [EngineFrame](../../Engine/Source/Runtime/Launch/Private/EngineFrame.cpp)
   performs `FFrameSync::Sync(EndFrame)` on the window-owning GameThread.
2. [FRenderCommandFence::Wait](../../Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp)
   calls `WaitTask`. The task wait's
   [FTaskWaitSignal](../../Engine/Source/Runtime/Core/Private/Threading/Task.cpp)
   uses a condition variable without dispatching Win32 messages.
3. [ImGuiRHIImpl](../../Engine/Source/Runtime/MonaImGui/Private/Backend/ImGuiRHIImpl.cpp)
   obtains the backbuffer synchronously on the rendering thread.
   [AcquireBackBufferSynchronously](../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp)
   waits for an RHI operation; [FRHIThread::WaitForSerial](../../Engine/Source/Runtime/RHI/Private/RHIThread.cpp)
   also uses a non-pumping condition-variable wait.
4. [VulkanViewport](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp)
   and [VulkanSwapchain](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp)
   perform native acquire, present, creation, and destruction on the RHI thread.

The [Vulkan Win32 WSI requirement](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateWin32SurfaceKHR.html)
explicitly permits those native operations to synchronously send window
messages. When that happens while the GameThread waits for the dependency
chain above, the sender needs the thread that is waiting for it. Pumping once
before each frame does not cover a wait inside the frame. Synchronous viewport
creation and resize introduce additional paths to the same hazard.

This is a concrete integration risk, not evidence that every Present sends a
message. More frequent frames increase exposure to the wait paths, but no
failure-probability relationship has been measured. An application message-pump
cycle alone also does not establish why unrelated desktop applications stall.

Candidate repair: make window-owner waits that depend on WSI compatible with
Win32 message dispatch, or avoid blocking that owner through asynchronous frame
admission and viewport operations. Do not insert a general engine tick or an
unrestricted UI pump into all Core task waits. Sent-message dispatch itself can
reenter window procedures; preserve frame reentrancy guards, object lifetime,
deferred resize/close semantics, and release engine locks before dispatch.
Cover frame fences, synchronous RHI creation/resize waits, and teardown rather
than changing only one call site.

### P1: Uncapped presentation can wait indefinitely for acquisition

[GetSwapchainAcquireTimeout](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.h)
uses a 1 ms acquire timeout only for `BestEffort`; both `Unsynchronized` and
`FramePaced` use `UINT64_MAX`. At the inspected baseline, the editor's VSync-off
mapping selected `Unsynchronized`, which preferred Immediate presentation.
The subsequent Windows Mailbox change retains the same acquire timeout.

Consequently, the fact that a temporary-unavailability branch can skip a
viewport does not bound the normal editor acquire call. Frame retirement and
present-resource waits also contain infinite native waits. A slow WSI call can
propagate through the synchronous dependencies above.

A finite acquire timeout and deferred retry are useful candidate protections,
but do not replace the message-pump fix: a native implementation's synchronous
window message is a separate dependency. A timeout must never be interpreted
as GPU completion or permission to recycle in-use resources.

### P2: Existing frame bounds do not identify the Windows scheduler failure

The code already has real completion-based backpressure:

- [RHIDefinitions](../../Engine/Source/Runtime/RHI/Public/RHIDefinitions.h)
  defines two GPU frame slots. [RHIEndFrame](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp)
  publishes a graphics completion point, and
  [FVulkanFrame::Prepare](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.cpp)
  waits for that slot's retirement before reuse.
- [RHIThread](../../Engine/Source/Runtime/RHI/Public/RHIThread.h)
  separately limits outstanding CPU replay frames to three.
- Rendering-done semaphores are stored per swapchain image. Present fences,
  when supported, participate in image/generation retirement.

These mechanisms contradict a claim that Durin has no GPU-frame backpressure.
They do not measure actual desktop display progress or all WDDM/driver work.
There is no evidence yet of unbounded presentation queuing.

[Device creation](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp)
sets ordinary per-process queue priorities to `1.0f`; it does not request a
global HIGH/REALTIME queue priority. No process-priority or D3DKMT priority
override was found in the inspected Runtime sources. Per-process priority is
not a request to outrank DWM; see
[Vulkan queue priorities](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-priority).

Current machine inspection reports GTX 1060 6GB, NVIDIA 582.66, WDDM 3.2,
2560x1440 at a reported 59 Hz, hardware scheduling enabled, graphics preemption
at Pixel granularity, and compute preemption at Dispatch granularity. Registry
absence of `HwSchMode` did not mean HAGS was disabled; DxDiag supplied the
effective enabled state. No explicit TDR overrides were found. Of 87 System
events queried for 11:00-16:15 on October 26, none matched Display, nvlddmkm,
DxgKrnl, WHEA, or Resource-Exhaustion providers. Absence of these events does
not exclude scheduling stalls or presentation faults.

[Microsoft's preemption documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-preemption)
explains how delayed preemption can delay DWM. It is a hypothesis to test here,
not a diagnosis from GPU utilization or the HAGS setting alone. Native queue
submission, WDDM scheduling delay, GPU execution, and display delivery must be
distinguished; see [GPUView context queues](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/context-cpu-queue).

### P2: Frame-end submission adds work even after presentation submitted the frame

`FVulkanViewport::Present` submits its context. `RHIEndFrame` subsequently calls
`GetPayload()` unconditionally to obtain a retirement marker and submits again.
`Finalize()` obtains and ends a native command buffer even for a marker-only
payload. Thus a single-window path with no work after Present still creates an
additional native submission/fence for the frame end.

This is an identifiable source of per-frame submission overhead, not a measured
cause of system starvation. A possible optimization is to reuse the last
applicable submitted completion point when no new work exists, while retaining
correct frame and cross-queue retirement. Measure before selecting it as a fix.

## Bounded Presentation A/B (2026-10-26)

Three valid runs used `/Game/Levels/NewLevel`, a 2560x1380 maximized window,
VSync disabled, and a logged Immediate/Unsynchronized swapchain. Startup was
capped at 60 FPS. An external marker enabled each uncapped diagnostic mode
after project startup; the editor requested exit after three seconds and an
independent process watchdog allowed five seconds before forced termination.
All three exited normally. FPS below is counted frames divided by unclamped
wall time, not the viewport HUD. Means cover the logged sample intervals
(approximately 2.78 seconds); each run continued to its three-second exit.

| Mode | Mean FPS (sample range) | External DwmFlush observation |
| --- | --- | --- |
| Scene rendering, skip window Acquire/Present | 224.18 (209.32-241.93) | 45 completed intervals overlapping the active period; maximum 22.571 ms |
| Skip scene drawing and ImGui GPU drawing; clear and Present | 486.58 (455.30-514.53) | One completed call took 2478.317 ms; a later call had no recorded completion |
| Normal scene/UI rendering and Present | 173.13 (159.15-181.34) | No recorded completion between the first call, 0.346 s before activation, and editor exit 3.697 s later |

The user explicitly reported desktop stutter during the last (normal) run.
Its render-frame sections stayed below 7.664 ms and RHI completion serials
continued advancing. This is direct evidence that engine throughput can remain
near 180 FPS during the reported symptom. A sustained GameThread/RHI deadlock
does not fit this captured interval, although the separate wait-cycle hazard
remains valid.

The monitor was a separate Python process, logging before and after each
`DwmFlush`, then sending a bounded `WM_NULL` to the editor window. Offscreen
window-message responses were at most 6.851 ms in the interior active sample
window, with no failures. Because these two measurements were sequential, a
blocked flush prevented window-message sampling in the normal run. Do not
infer message-pump failure from missing samples. Uncompleted flush entries
are observations censored by monitor termination, not measured call durations.

[DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush)
waits on the calling application's queued DirectX updates; it does not flush
the entire session. This monitor had no rendered surface of its own. Its
large timing change is a correlated probe result, not a direct measurement
of DWM GPU execution, desktop frame latency, or GPU starvation. ETW is still
needed to distinguish composition/presentation behavior from scheduling or
driver waits. WPR GPU/DesktopComposition capture failed with `0xc5585011`
(performance profiling policy); no trace was obtained and no policy changed.

The offscreen mode bypassed the whole window rendering path, including ImGui
GPU drawing and Acquire, while retaining scene rendering and frame retirement.
It was not a one-line removal of `vkQueuePresentKHR`, which would leave
acquired images and semaphore lifetimes requiring different handling. The
clear-only control also changed frame rate; this is not a matched-rate study.
Only one valid three-second sample per mode was recorded. These observations
support investigating WSI/composition first; they neither prove that Present
alone causes the issue nor demonstrate a permanent fix.

Raw results are in ignored `Build/Diagnostics/PresentAB/`:

- `offscreen-0-172011`, `clear-0-172036`, and `normal-0-172052` contain inputs,
  engine logs, external samples, summaries, and derived `analysis.json`.
- Earlier attempts are excluded: newline conversion in the harness caused
  host settings to fail parsing. Valid runs have no such warnings and confirm
  Immediate from swapchain creation. The harness was corrected before reruns.
- `run_probe.py`, `temporary-instrumentation.patch`, and
  `PresentationABProbe.h` preserve the temporary experiment implementation.

Three edited source files and both configuration files were verified against
their original bytes, the temporary header was removed, and `Launch` was
rebuilt successfully with the original sources. No runtime behavior change is
included in this documentation update.

## Windows Mailbox Verification (2026-10-26)

The production selector now uses Mailbox with FIFO fallback for Windows Vulkan
`Unsynchronized`. It does not change frame caps, acquire timeouts, BestEffort,
FramePaced, or other platforms' native mappings. The shared enum's comment was
updated; its values, layout, and consumer API are unchanged.

Validation used `Win64-Debug-DurinEditor`: all nine
`VulkanRHIIntegrationTests FVulkanSwapchainSelectionTests.*` cases passed, then
`Launch` built successfully. This bounded selection covers the changed mode and
image-count policy and retained timeout, rather than unrelated GPU workloads
selected by the shared-header comment. Non-Windows execution was not performed.

The uninstrumented production editor opened NewLevel at 2560x1380 with
`VSyncEnabled: 0`, `MaxFrameRate: 0`, and `--exit-after-ticks=480`. It logged
`presentMode=Mailbox, images=3, policy=Unsynchronized` and exited normally after
480 running ticks. The startup-ready log and automated-exit request were 3.014 s
apart; this is a coarse throughput observation, not per-frame timing.

An independent message-monitor thread recorded 37 samples after monitor
activation, excluding destroyed-window errors: maximum 20.613 ms, no timeouts.
The separate DwmFlush loop still recorded one valid-window completion taking
1476.850 ms, followed by an uncompleted call. Its limitations described above
still apply. The user reported that the entire computer still noticeably
stuttered during this run. Mailbox selection is verified, but switching modes
did not fix the reported symptom. The issue remains open.

Raw inputs, logs, and samples are in ignored
`Build/Diagnostics/PresentAB/mailbox-normal-0-173547/`; `run_mailbox.py` uses the
production tick-bounded exit plus the external watchdog, without temporary C++
instrumentation. The project configuration and original 60 FPS cap were
restored afterward; the local editor VSync switch was then disabled to select
the requested Mailbox policy on subsequent launches.

## RHI, Layer, And Tracy Controls (2026-10-26)

Further production controls used `dce5ba126`, NewLevel, Mailbox, no frame cap,
and the same 480-tick automatic exit with an external watchdog. Each valid
editor run exited normally. Diagnostic scripts and run directories are under
ignored `Build/Diagnostics/PresentAB/`:

| Run directory | Changed condition | Maximum completed DwmFlush with valid window | User observation |
| --- | --- | --- | --- |
| `rhi-threaded-normal-0-174151` | Threaded control | 1958.158 ms | No separate verdict |
| `rhi-inline-normal-0-174245` | Inline RHI execution | 1208.285 ms | Still noticeably stuttering |
| `validation-off-rhi-threaded-normal-0-174825` | Vulkan validation disabled | 1840.291 ms | Still noticeably stuttering |
| `present-layers-off-rhi-threaded-normal-0-174924` | OBS and NVIDIA present implicit layers disabled for this process | 732.349 ms | Still noticeably stuttering |
| `tracy-off-rhi-threaded-normal-0-180517` | `DURIN_ENABLE_TRACY=OFF`, all runtime targets rebuilt | 16.439 ms across 41 samples | User did not observe this run |

The DwmFlush probe limitations above apply to every row; these are not direct
desktop latency measurements or a statistical performance comparison. Separate
WM_NULL samples had no timeout in the threaded/inline controls. In the Tracy-off
run, 51 valid-window message samples peaked at 9.175 ms, and startup-ready to
the automatic exit request was 2.965 s. Its generated build rules confirmed
`DURIN_WITH_TRACY=0`; validation and normal implicit-layer settings were retained.

Thread CPU sampling covered approximately 3.224 s in the threaded control and
4.627 s in the inline control. Summed engine CPU use was approximately 1.885
and 1.239 core equivalents respectively. Fourteen EngineWorker threads showed
no CPU-time increase at the GetThreadTimes resolution in either window.
RHIThread and RenderingThread dominated the threaded control. This does not
support worker-pool CPU saturation in these samples. Inline reproduction rules
out the dedicated RHI queue as a necessary condition, but retains shared RHI
replay and Vulkan submission code.

The first threaded probe's configuration restoration encountered one write
failure after normal editor exit. Original project bytes were restored manually
and verified; subsequent harnesses preserve input backups and verify restoration.
An earlier validation-off attempt at 17:47:28 was blocked before editor startup
by an unrelated build and is excluded. The implicit-layer control's loader log
confirmed validation and Optimus remained, with OBS and NVIDIA present absent.

The user recalls normal behavior about a month earlier. Ancestor `757536eb2`
(September 26) is a historical comparison, not an established good revision:
that version defaulted to FramePaced/FIFO. `54687b583` introduced the VSync-off
Unsynchronized/Immediate mapping on October 4. Thus this recollection alone
does not establish a task-system regression under equivalent presentation.
The October 20 render-fence task-event change (`1f2986c6f`) did not newly remove
message pumping: the old custom condition-variable wait also did not pump.

The user also reports the same whole-machine symptom at a normal frame cap
after Tracy connects and starts capture. The client uses on-demand capture;
ordinary zones do not enqueue while disconnected. Inspection nevertheless
found task-owner snapshots copied and aggregated every frame before the final
plot helper checked the connection, including in Tracy-disabled builds.
Per-buffer mapped-write snapshots, formatted text and multiple plots also ran
for each write while connected, independently of the disabled-by-default Vulkan
detail switch. These are verified avoidable profiling costs, not proof of the
uncapped stall's root cause.

The profiling cleanup removes per-draw/bind/cache-hit detail probes, per-write
buffer profiling snapshots and plots, and per-task admission/terminal messages
and execution identity text. It retains task execution duration and coarse
render/submit/present/wait zones. Task-owner plot collection now checks the
connection before collecting and samples at most ten times per second. See
[CPU Profiling](../Development/Build/Profiling.md) for the maintained policy.
The issue remains open: the attended retest below rejects uncapped
responsiveness acceptance. The Tracy-off observation alone is insufficient to
select a cause.

### Cleanup Verification

The cleanup was built over `dce5ba126` with `DURIN_ENABLE_TRACY=AUTO` and
`DURIN_WITH_TRACY=1`, using `build all --jobs 4` in
`Win64-Debug-DurinEditor`. The all-target build covers the workspace consumers
of the shared profiling header and restores a consistent instrumented runtime
after the compile-time-off control. No presentation or task scheduling policy
was changed in this cleanup.

| Cleaned run directory | Condition | Valid-window completed DwmFlush samples / maximum | WM_NULL samples / maximum |
| --- | --- | --- | --- |
| `tracy-clean-none-rhi-threaded-normal-0-181606` | Uncapped, no capture connection | 51 / 16.242 ms | 62 / 10.388 ms |
| `tracy-clean-cli-rhi-threaded-normal-60-181625` | 60 FPS, command-line capture | 117 / 16.969 ms | 154 / 2.207 ms |
| `tracy-clean-gui-rhi-threaded-normal-60-181702` | 60 FPS, official profiler launched with automatic connection arguments | 104 / 16.636 ms | 137 / 13.700 ms |

All three editors exited normally after 480 ticks, with no WM_NULL timeouts.
The harness terminated its own GUI profiler after editor exit; its exit code 1
is not an editor failure or a saved-capture receipt. The command-line capture
exited successfully and saved `capture.tracy`: 4.11 seconds, 186 frame markers,
52,550 zones, approximately 645 KiB. CSV exports confirm retained Task.Execute,
Acquire, Submit and Present zones; no per-task phase messages or removed
dynamic-buffer plots. Twenty task-owner plots had at most 35 samples each,
with a minimum within-plot sample interval of 100.030 ms. This verifies the
collection limit with a real connection. It is not a before/after event-count
benchmark because no matching pre-cleanup capture was collected.

Native validation passed 228 CoreConcurrencyTests cases, 145 RHICommandListTests
cases, and
`FVulkanResourceIntegrationTests.DynamicDiscardBuffersSharePageAndPreserveExtendedPatches`.
The affected-test analysis also selects unrelated Core consumers and GPU scenes;
the bounded selection covers task lifecycle, replay and the mapped-write path
whose profiling calls changed. Functional draw/bind code is unchanged. XML
receipts are under `Build/NativeTestResults/Win64-Debug-DurinEditor/`.

Raw traces, exports and logs remain in those ignored run directories. Original
project and host bytes were restored after every cleaned run (GrayboxStage15,
60 FPS, VSync disabled). The user subsequently reported not observing those
initial runs; their probe results are not user-confirmed responsiveness evidence.

### Attended Retest At 1234029af

The user observed the following controls on October 26. No runtime sources,
driver settings, or build flags changed between them. Run directories are
under `Build/Diagnostics/PresentAB/`.

| Run | Condition | User report |
| --- | --- | --- |
| `tracy-clean-none-rhi-threaded-normal-0-184027` | Uncapped, no Tracy connection | Entire machine still noticeably stutters |
| `tracy-clean-gui-rhi-threaded-normal-60-184051` | Configured 60 FPS, live Tracy GUI | Noticeable stutter in the Tracy interface |
| `tracy-clean-cli-rhi-threaded-normal-60-184144` | Configured 60 FPS, command-line capture | No noticeable stutter |
| `offline-profiler-retest-1843.log` | Open the preceding capture offline for 12 seconds, no editor | Smooth |
| `tracy-offline-with-engine-gui-rhi-threaded-normal-60-184412` | Same offline capture alongside the editor, no live connection | Tracy smooth; editor possibly somewhat choppy |

The uncapped editor exited normally after 480 ticks. Its first DwmFlush call
returned after 1867.018 ms, but the editor window was already destroyed at the
following WM_NULL check. There were no completed valid-window DwmFlush samples,
so this value is not a valid-window latency measurement. Thirty-two independent
WM_NULL samples peaked at 57.530 ms. User observation is the decisive evidence
that the cleanup did not fix this reproduction.

The live-GUI run had 55 valid-window DwmFlush completions, peaking at 1555.782 ms,
and 245 independent message samples, peaking at 9.724 ms. The 12-second watchdog
terminated it before 480 ticks. This is not hang evidence:
[GetHostFrameInterval](../../Engine/Source/Runtime/Launch/Private/EngineFramePhases.h)
caps an unfocused editor at 10 FPS, so observing Tracy can extend 480 ticks to
approximately 48 seconds. A configured 60 FPS cap is not a measurement of
actual foreground/background throughput. Future timed probes must account for
focus and use a duration independent of the ready-state tick count.

The CLI capture saved 4.10 seconds, 178 frame markers, 50,232 zones and about
622 KiB, then exited successfully. Capture lasted through startup and the first
few seconds of editor readiness, rather than the entire run. Its editor exited
normally; 104 valid-window DwmFlush completions peaked at 64.255 ms. The
offline-plus-editor run also exited normally. All harnesses restored original
configuration bytes and stopped only their own processes.

These controls make generic event ingestion or simultaneous GUI presence
insufficient explanations for the observed Tracy stutter. Live data processing,
live-view rebuilding/rendering and connection-specific interaction remain
candidates. Different focus, capture durations and live/offline view contents
prevent treating this as a definitive isolation of graphics versus CPU work.
The original uncapped reproduction remains separately unresolved; no observed
Tracy-disabled responsiveness verdict exists yet.

## Evidence And Validation Gaps

The user's later scope clarification prioritizes submission/presentation rate
and sustained execution over scene-specific content. It does not establish a
particular GPU-utilization level or prove CPU versus GPU saturation. Static
inspection at `5a064ab7b` confirms:

- MainFrame maps VSync on/off to FramePaced/Unsynchronized, which selects
  FIFO/Mailbox respectively on Windows. The existing 60 FPS cap is independent.
- A ready, focused editor with no cap gets a zero host frame interval in
  `EngineFramePhases.h`. No configured pacing sleep remains on that path.
- `FrameInFlight=2`; Vulkan begin-frame waits for a reused frame slot's GPU
  retirement prerequisites. This bounds outstanding frame work, not submissions
  per second or desktop scheduling latency.
- `FVulkanViewport::WaitForFrameResource` waits on a presentation resource
  fence. Per the [Vulkan fence contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html),
  this fence need not wait for actual presentation completion. Per the
  [present-mode contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html),
  Mailbox replaces a pending request and releases its image for reuse; it does
  not impose FIFO's display-paced queue behavior.

These properties explain why Mailbox and bounded in-flight work can still
sustain high submission rates. They do not explain the severity of the desktop
stall by themselves or establish an invalid synchronization operation. A future
control should separate queue depth, submission frequency and GPU work, with
frame throughput recorded so an incidental FPS reduction is not mistaken for
a specific fix. No additional runtime trial accompanied this clarification.

- Existing local captures: `Saved/Diagnostics/NewLevel-hang-23044.txt`,
  `NewLevel-retry-48364.txt`, and `NewLevel-gpu-comparison.csv`. The thread
  snapshots show the GameThread in event waiting and the render/RHI workers
  idle, not the hypothesized wait cycle. They cannot be called hang-time proof.
- Existing editor logs under
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/Saved/Logs/` confirm
  Immediate/Unsynchronized, windowed 2560x1380 output, shared graphics/compute/
  transfer queue family zero, and occasional out-of-date presentation results.
  Gaps before recreation may include minimized intervals, not native call time.
- GPU-free control and raw results are in ignored
  `Build/Diagnostics/PresentationWaitProbe/probe.py` and `result.json`.
  A hidden message-only window waits for a worker that synchronously sends it
  a message with a 500 ms timeout. Three non-pumping waits all timed out
  (500.384, 502.303, 513.200 ms). Three sent-message-aware waits completed
  (1.251, 0.123, 0.121 ms). This is a Win32 mechanism control, not a Durin or
  Vulkan regression test. The probe has bounded waits and performs no GPU work.
- `dxdiag.txt` in the same ignored directory preserves capability inspection.
  WPR, xperf, and WPA are installed. No fault-time ETW trace was captured.
- The custom-title-bar `DwmFlush()` in
  [GlfwWindow](../../Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp)
  runs only during `WM_MOVING`. It is not a normal stationary-frame throttle;
  record its duration if dragging is part of the reproduction.

The next discriminating evidence should combine bounded external capture and
an independent stop mechanism, with no manual interaction required after
uncapped rendering starts. Preserve the triggering Immediate configuration in
the baseline. Capture Game/Render/RHI wait stacks, entry/exit timestamps for
Acquire/Submit/Present/recreation, frame completion serials, and DxgKrnl/DWM
scheduling events. Distinguish a WSI sender waiting for the window owner from
DWM work delayed behind GPU execution, high-frequency submission overhead,
and CPU-side driver stalls. HAGS or present-mode comparisons are separate
controls, not substitutes for that baseline.

A future wait-path fix needs a deterministic native regression that sends a
synchronous Win32 message from an actual render/RHI dependency while the owner
waits, including resize and shutdown cases. The existing generic fence tests
and this standalone control do not validate such a fix. Uncapped reproduction
must remain the final acceptance condition; a fixed FPS cap is not acceptance.
