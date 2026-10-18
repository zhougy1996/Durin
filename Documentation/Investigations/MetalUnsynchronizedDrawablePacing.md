# Metal Unsynchronized Drawable Pacing

**Status:** Open for the private native display policy and cross-machine qualification; the production workaround is implemented.

**Last reviewed:** 2026-10-18

## Scope And Verdict

On the measured Apple M4/macOS/display configuration, a Metal viewport with
`displaySyncEnabled=false` and three drawables presents at 60 Hz. Two drawables
remove that ceiling in the measured Editor workload. The
[native-window follow-up](#native-window-follow-up-on-2026-10-18) reproduces the
same throttle in an AppKit/Metal program without the engine, GLFW, frame fences,
or an RHI command queue. The engine is not necessary to produce this behavior.

The sustained throttle is refresh-coupled drawable presentation/recycling:
`nextDrawable()` waits for Core Animation's drawable-availability semaphore, and
that wait propagates through the RHI queue to RenderThread and GameThread.
The [lifecycle and display-path controls](#lifecycle-and-display-path-controls-on-2026-10-18)
isolate a reversible interaction among `NSWindowStyleMaskTitled`, layer opacity,
and the three-drawable direct-to-display path. Clearing only the title bit or
only layer opacity removes the throttle; restoring either restores it. A Metal
System Trace confirms the opaque path displays the probe's surfaces directly,
whereas nonopaque phases use compositor output surfaces. The private system
policy behind that interaction remains unknown. This is not a universal
triple-buffering rule or an established Apple driver defect. Two drawables are
a verified local workaround, not a cross-machine qualification.

The earlier investigation restored its experimental source changes, leaving the
default three-drawable pool and the original symptom intact. The subsequent
production repair fixes `maximumDrawableCount=2` for every policy while preserving
`opaque=true`. VSync continues to change only `displaySyncEnabled`. The lasting
contract is in [viewport output policies](../Runtime/Rendering/ViewportRendering.md#output-policies);
this investigation retains the evidence and unresolved native policy boundary.

The [UE 5.8.3 source comparison](#ue-583-source-comparison-on-2026-10-18)
finds no VSync-dependent drawable-pool reduction. Its macOS native-present path
retains the default pool capacity of three and additionally forces layer
synchronization outside the running-game/fullscreen case.

Relevant contracts: [viewport rendering](../Runtime/Rendering/ViewportRendering.md)
and [performance measurement authority](../Development/Build/RenderingPerformanceBaseline.md).
Source: [viewport policy](../../Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalViewport.h),
[presentation](../../Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalCommandContext.cpp),
[resource ownership](../../Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalSubmission.h),
and [frame synchronization](../../Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp).

## Verified Findings

### Presentation timing remains quantized after removing GPU work

The display is an LV273HUPR at 3840x2160, with a 1920x1080 logical desktop at
60.00 Hz. The drawable is 3840x1920. Runtime inspection confirms
`displaySyncEnabled=false`, `presentsWithTransaction=false`, and the requested
pool capacity. Returned texture pointers cycle through three or two textures
respectively.

The following observations use the same existing Sandbox Editor scene. Rates
count drawable-acquisition calls, not physical panel refreshes. Each process runs
480 engine ticks; analysis selects RHI frame IDs 60 through 419 (360 samples).
The frozen-scene path stops scene redraw after game frame 60 while continuing UI
rendering and window presentation; it is not a standalone Metal reproducer.

| Experiment | Drawables | Acquisition rate (FPS) | Mean acquisition wait (ms) |
| --- | --- | --- | --- |
| Original ownership | 3 | 59.992 | 14.852 |
| Original ownership, repeat | 3 | 59.992 | 15.032 |
| Release application drawable/texture references after commit | 3 | 59.997 | 14.860 |
| Original ownership | 2 | 69.923 | 12.588 |
| Original ownership, repeat | 2 | 70.065 | 12.622 |
| Original ownership, second repeat | 2 | 69.825 | 12.664 |
| Release application drawable/texture references after commit | 2 | 69.827 | 12.775 |
| Freeze scene redraw | 3 | 60.011 | 15.889 |
| Freeze scene redraw, repeat | 3 | 60.006 | 15.915 |
| Freeze scene redraw | 2 | 151.850 | 5.470 |
| Freeze scene and omit final blit copy | 3 | 60.001 | 15.630 |
| Freeze scene and omit final blit copy, repeat | 3 | 60.003 | 15.642 |
| Freeze scene and omit final blit copy, second repeat | 3 | 60.000 | 15.628 |
| Freeze scene and omit final blit copy | 2 | 160.261 | 5.375 |

With three drawables, `presentedTime` intervals have mean, median, and p95 of
approximately 16.668 ms, including the frozen/no-copy experiment. With the
original scene and two drawables the mean interval is 14.304 ms.

The original three-drawable experiment has a 0.508 ms mean final-blit GPU span,
and a 32.095 ms mean interval from that command buffer's GPU end to
`presentedTime`. With two drawables that latter interval is 2.668 ms. In the
three-drawable no-copy experiment the empty final command buffer's GPU span is
approximately 0.001 ms and its mean commit-to-GPU-start interval is 0.085 ms;
yet acquisition still waits 15.630 ms and GPU-end-to-presentation is 48.127 ms.
The remaining UI command-buffer envelope is approximately 1.350 ms.

Consequently, scene rendering cost and final drawable-copy cost are not
necessary to reproduce this ceiling. The no-copy experiment does not validate
image contents; it deliberately isolates acquisition and presentation pacing.

### Application reference retention is not sufficient to explain the ceiling

The normal path retains both the drawable and destination texture in a completion
payload. Removing those extra references, releasing the local drawable after
commit, and draining the per-call autorelease pool does not remove the ceiling.
The command buffers use retained references. The diagnostic presented callback
captures only the frame number, not a drawable strong reference.

For the original three-drawable run, the completion callback arrives a mean
0.149 ms after GPU end, far below one refresh interval. An application-side
completion callback delayed by an entire refresh is not supported by this run.

### Engine frame fences are a separate mechanism

`FFrameSync` uses a fixed two-element RenderThread fence array. Its capacity is
not read from `maximumDrawableCount`, and an EndFrame fence is not a GPU
frames-in-flight counter or a drawable-presented fence. This audit found no
shared pool-size setting in these paths. CPU frame-sync backpressure can still
propagate a blocked RHI presentation upward; it has not been independently
removed in this experiment.

## Reproduction And Evidence

- Source baseline: `1d5369e4fd759a87ffb7a87dd044c1ec6259768e`.
- OS: macOS 27.0.1, build 26A434; GPU: Apple M4, 10 GPU cores.
- Compiler: Apple clang 21.0.0, clang-2100.1.1.101.
- Preset: `MacOS-arm64-Debug-DurinEditor`; Tracy enabled/on-demand; threaded RHI.
  These are diagnostic observations, not Release performance acceptance.
- A pre-existing user edit to `Sandbox/Content/Levels/GrayboxStage15.dasset` was
  preserved. Its SHA-256 during the experiment was
  `d312f41802313bef4776994370dea069e02b7c3c0507c6de97f5f9b43cf17af8`.
- Local raw CSVs, analysis script, original-file backups, and the temporary
  instrumentation patch are retained in `Build/.agent-state/metal-investigation/`.
  These ignored artifacts are machine-local, not distributed with Git.
- Runtime logs are in `Build/.agent-state/logs/`, beginning with
  `20261005-152526-898130-62643-DurinEditor.log` (three-held),
  `20261005-152555-373243-62673-DurinEditor.log` (two-held),
  `20261005-152603-471645-62683-DurinEditor.log` (three-early),
  `20261005-152612-715748-62693-DurinEditor.log` (two-early),
  `20261005-152906-018611-62864-DurinEditor.log` (three-no-blit), and
  `20261005-152916-065539-62873-DurinEditor.log` (two-no-blit).

The temporary probe selects `DURIN_RHI_BACKEND=metal`, forces synchronization off,
and chooses `DURIN_DRAWABLE_COUNT=2` or `3`. `DURIN_PROBE_CSV` is an absolute
output path; `DURIN_PROBE_EARLY=1` changes application reference retention;
`DURIN_PROBE_FREEZE_SCENE=1` stops scene redraw after warmup;
`DURIN_PROBE_NO_BLIT=1` omits the final copy. These are temporary probe settings,
not supported production environment variables.

Run through `./DevTool run --project Sandbox/Sandbox.dproject --
--exit-after-ticks=480`. Events are buffered in memory and written on shutdown.
GPU start/end intervals of separate command buffers overlap and must not be
summed as frame GPU cost. The analysis reports their envelope only, including
possible waits. Presented callback dispatch time is recorded separately from
`presentedTime`; neither is an exposed drawable-recycling timestamp.

## Remaining Validation Gaps

- The native-window follow-up removes the engine, GLFW, and custom title bar as
  necessary causes. It still shares the same OS, GPU, display, and BetterDisplay
  environment; it does not identify the private system scheduling rule.
- A Metal System Trace can distinguish GPU resource waits, WindowServer
  scheduling, and drawable recycling; the current timestamps do not reveal the
  private system policy responsible for the three-versus-two difference.
- The user-selected 30 Hz follow-up below confirms refresh-rate coupling for
  three drawables. Another display or OS version, and a BetterDisplay-off
  control, remain untested.
- Release/Tracy-off, cross-machine, sustained frame-time, resize, and presentation
  policy-switch qualification remain necessary before adopting a default change.

## Handoff Validation

All 14 diagnostic Editor processes exited successfully. The temporary source
changes were removed; the instrumentation patch remains only in the ignored
local evidence directory. No production drawable-count policy was changed.
The complete runtime build passed after restoration (receipt
`Build/.agent-state/logs/20261005-153223-843815-63030-cmake.log`).
`./DevTool doc validate --scope changed` and `git diff --check` passed.
The user's pre-existing scene asset edit remains outside this change.


## User-Selected 30 Hz Follow-Up

On 2026-10-05 the user changed the physical display to 30 Hz. System inspection
confirmed 3840x2160 output, 1920x1080 logical desktop, 30.00 Hz, and mirroring
off. The existing scene hash, Debug/Tracy configuration, 3840x1920 drawable size,
and original reference ownership were unchanged. Neither scene freezing nor
no-copy mode was used. Runtime logs confirmed synchronization and transaction
presentation were both false. BetterDisplay was not disabled for this control.

Four successful 480-tick runs alternated 3 -> 2 -> 3 -> 2 drawables; each
analysis again used RHI frames 60 through 419.

| Drawables / run | FPS | Acquire mean (ms) | Acquire p95 (ms) | Mean presented interval (ms) |
| --- | --- | --- | --- | --- |
| 3 / first | 29.982 | 29.490 | 30.745 | 33.355 |
| 2 / first | 69.608 | 12.748 | 23.423 | 14.366 |
| 3 / repeat | 29.989 | 29.559 | 31.503 | 33.355 |
| 2 / repeat | 68.908 | 12.941 | 26.426 | 14.572 |

The three-drawable ceiling follows the display from 60 to 30 Hz, while two
continue at approximately 69 FPS for this workload. This is stronger evidence
of refresh-coupled throttling in the three-drawable presentation path, rather
than a fixed 60 FPS engine cap. Two drawables still incur acquisition waits and
noticeable tails; exceeding 30 FPS does not mean the physical panel displays
69 complete images per second. The system's reason for treating the pool sizes
differently remains unresolved.

Raw samples: `30hz-three.csv`, `30hz-two.csv`, `30hz-three-repeat.csv`, and
`30hz-two-repeat.csv` under the same local evidence directory. Runtime receipts:
`20261005-173425-919500-64416-DurinEditor.log`,
`20261005-173444-002519-64429-DurinEditor.log`,
`20261005-173452-224338-64445-DurinEditor.log`, and
`20261005-173509-338574-64461-DurinEditor.log` in `Build/.agent-state/logs/`.

Only the two Metal source files were temporarily instrumented for this follow-up;
both were restored and the MetalRHI target build passed afterward
(`20261005-173525-107556-64483-cmake.log`). The display remains at the user's
30 Hz setting. No production policy change was retained.

## Native Window Follow-Up On 2026-10-18

### Current Editor And Independent Reproduction

The source baseline is `1c0c3718c0e02f5a0b27fc8d3266e7eb87a9171a`.
The running Editor's `20261018-053138-25980` log confirms
`policy=Unsynchronized`, `requestedSync=false`, `displaySyncEnabled=false`, and
`maximumDrawableCount=3`. A read-only stack sample reaches:

```text
FMetalCommandContextImpl::RHIEndDrawingViewport
  -[CAMetalLayer nextDrawable]
    CAMetalLayerPrivateNextDrawableLocked
      _dispatch_semaphore_wait_slow
        semaphore_timedwait_trap
```

This matches the reported Tracy chain: drawable acquisition blocks RHI replay,
RHI admission backpressure blocks RenderThread, and a render fence blocks
GameThread. These enclosing CPU zones are waiting time, not independent rendering
costs that should be summed. `GetPresentMode()` reports Immediate from the layer's
configuration; it does not measure the realized recycling cadence.

A separate Objective-C++ probe creates an AppKit window and `CAMetalLayer`, with
an optional GLFW host using the repository's installed GLFW library. It has no
Durin modules, RHI queue, render thread, or frame fences. The matched path clears
one shared offscreen texture, commits it, obtains a drawable, and submits a
second command buffer containing the final copy and presentation. Each frame has
an autorelease pool; application drawable references do not survive the frame.

Controlled runs use a 3840x1920 sRGB drawable, `framebufferOnly=false`,
`presentsWithTransaction=false`, and synchronization off. Every process runs
300 frames; analysis selects frames 60 through 279. The optional 5 ms CPU-side
sleep models time between submissions; it is not a 60 Hz timer or display link.
Each host alternates 3 -> 2 -> 3 drawables in separate processes.

| Native host / CPU delay | Drawables | Acquisition FPS | Mean acquisition wait (ms) | Mean final-copy GPU span (ms) | Mean GPU-end-to-present interval (ms) |
| --- | --- | --- | --- | --- | --- |
| AppKit / 5 ms | 3 | 59.985 | 10.282 | 0.788 | 31.685 |
| AppKit / 5 ms | 2 | 125.826 | 1.651 | 0.856 | 8.616 |
| AppKit / 5 ms, repeat | 3 | 60.002 | 10.234 | 0.877 | 31.582 |
| GLFW / 5 ms | 3 | 59.998 | 10.281 | 0.881 | 31.590 |
| GLFW / 5 ms | 2 | 132.668 | 1.244 | 0.812 | 8.501 |
| GLFW / no delay | 3 | 59.987 | 16.452 | 0.854 | 31.564 |
| GLFW / no delay | 2 | 184.548 | 5.222 | 0.957 | 8.521 |
| GLFW / no delay, repeat | 3 | 59.997 | 16.448 | 0.863 | 31.562 |

The three-drawable presented intervals remain approximately 16.668 ms. The
offscreen clear precedes the measured final-copy command buffer, so the GPU span
column is not total frame GPU cost. This remains a diagnostic comparison, not a
Release performance baseline or a physical-panel FPS measurement.

Per-run metadata records no running DurinEditor or Tracy profiler before or after
these controlled runs. BetterDisplay remains running. Logs identify Apple M4,
macOS 27.0.1/build 26A434, and a 60.000 Hz display. Each process reports all
300 GPU completions, all 300 presented callbacks, and zero command-buffer errors.

Earlier exploratory runs had a concurrent Editor/profiler and different host
and submission settings. Some submitted about 120 frames/s with three drawables,
with approximately half receiving `presentedTime=0`; others submitted about
60 frames/s with two. The concurrent Editor exited before the controlled series.
These observations reinforce the configuration boundary of the workaround;
they do not isolate a necessary window style, CPU delay, or concurrent-process
cause. Custom title bars and GLFW are not necessary to reproduce the controlled
throttle, and three drawables do not universally impose 60 FPS.

### Presentation API And Runtime Policy Controls

On the native GLFW/custom-title-bar path with the 5 ms delay, replacing
`MTLCommandBuffer::presentDrawable()` with `MTLDrawable::present()` in a scheduled
handler still produces 59.999 FPS with three drawables. Waiting until scheduled
and then calling `present()` directly produces 59.997 FPS. These API substitutions
do not repair this reproduced ceiling. They may have other submission benefits;
[MoltenVK uses the scheduled-handler form](https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm),
but that is not evidence that it fixes this issue.

A single native GLFW process then changes both layer properties at the drawing
boundary, without recreating the layer or adding a GPU-idle wait. Each phase has
120 frames; analysis selects local frames 40 through 109:

| Phase | Drawables | Synchronization | Acquisition FPS | Mean acquisition wait (ms) |
| --- | --- | --- | --- | --- |
| Initial | 3 | On | 60.004 | 16.426 |
| Disable and reduce pool | 2 | Off | 183.339 | 5.268 |
| Restore pool only | 3 | Off | 59.994 | 16.451 |
| Reduce pool again | 2 | Off | 190.468 | 5.029 |
| Restore synchronized policy | 3 | On | 60.001 | 16.414 |

The measured texture identities cycle through two or three textures as requested.
All 600 GPU completions and presented callbacks arrive with zero errors. This
verifies the native transition sequence locally; it does not qualify production
Editor policy switching, resizing, multiple windows, or image contents.

### Bounded Repair Candidate And Remaining Attribution

For `Unsynchronized`, explicitly request two drawables along with
`displaySyncEnabled=false` in `FMetalViewport::ApplyRequestedPresentationPolicy()`.
Retain three drawables for `FramePaced`; apply both settings on the existing RHI
drawing boundary and log their read-back values. Qualify the existing native
viewport tests plus visible Editor switching, resize, and multiple-window paths
before adopting that policy. Do not introduce a GPU-idle wait every frame or
expand the RHI/frame-fence queue to conceal the presentation wait. Changes to
`BestEffort` need their own bounded-acquisition contract review.

This is a targeted workaround for the independently reproduced native
presentation/recycling behavior. Public APIs expose pool size and synchronization
intent, not the private WindowServer/Core Animation recycling decision. Apple's
[drawable acquisition contract](https://developer.apple.com/documentation/quartzcore/cametallayer/nextdrawable())
explicitly permits waiting when the pool is occupied; disabling synchronization
does not turn acquisition into a nonblocking operation. No evidence here assigns
the private behavior specifically to an Apple GPU driver defect.

An attempted five-second Metal System Trace could not attach because Editor
process 25980 had already exited. No trace was recorded and no replacement Editor
was launched. A BetterDisplay-off control, another OS/display/device, and system
trace attribution remain open. The native program was compiled with Apple clang
using `-O2`; it is not an engine Release/Tracy-off acceptance run.

Probe source, runner, raw CSVs, per-run logs, process metadata, the Editor policy
excerpt, and stack sample are retained under ignored
`Build/.agent-state/metal-pacing-20261018/`. The matched controls are
`*-large-control-appkit-delay5`, `*-large-control-glfw-delay5`, and
`*-large-control-glfw-fast`; the transition samples are `policy-switch.csv` and
`policy-switch-summary.json`. The local `README.md` records reproduction commands.
The October 5 raw-evidence directory was not present in this checkout or the main
checkout during this follow-up; its historical receipts above have not been
revalidated. Production source remains unchanged.

## UE 5.8.3 Source Comparison On 2026-10-18

### Public Artifact And Verification Boundary

A publicly pullable, third-party development image contains the requested source:
[`wukakuki/unreal-engine:dev-5.8.3`](https://hub.docker.com/layers/wukakuki/unreal-engine/dev-5.8.3/images/sha256-cb46b38e25eae49cbe1480255c796622679ad7ef53b732c9309bb29923894745).
The pinned manifest digest is
`sha256:cb46b38e25eae49cbe1480255c796622679ad7ef53b732c9309bb29923894745`.
The extracted `Engine/Build/Build.version` identifies 5.8.3, changelist 58210709,
compatible changelist 55116800, promoted build, branch `++UE5+Release-5.8`.
This establishes the artifact's internal version identity; it does not authenticate
its source against Epic's access-controlled upstream Git repository.

The manifest digest and the complete base-layer digest were verified. A bounded
stream of the source layer saved all 106 source/header/build-rule files in
`Engine/Source/Runtime/Apple/MetalRHI/`, with individual SHA-256 receipts.
The source-layer stream stopped after that directory; its complete layer digest
was **not** verified. No container executable or engine binary was run.
The three key files also match the publicly readable
[Arkfall mirror at commit 546b25d9e1eead201ec92ae6580cd471abec39ea](https://github.com/Arkfall-Development/UnrealEngine/tree/546b25d9e1eead201ec92ae6580cd471abec39ea/Engine/Source/Runtime/Apple/MetalRHI),
whose own version file says 5.8.2. That is a corroborating code comparison,
not an independent 5.8.3 version certificate.

| Extracted file | SHA-256 |
| --- | --- |
| `Private/MetalViewport.cpp` | `f2c87a1e972a5c9545f2e67acd87c2e64789f3c1e6f136ca50062fda26dda877` |
| `Private/MetalDevice.cpp` | `e1e351754ce1ca0ef361ca2502c2ac4a616fc7d3f2a6c30eb3aee6d327768291` |
| `Public/MetalViewport.h` | `32b02fe49ff23114fdd9a27008cbf5e5db39348438fd51f0d9228f005ca4ba06` |

The extracted files, version, retrieval helper, manifest, and receipts remain in
ignored `Build/.agent-state/ue583-source-review/`. The following line numbers are
relative to that extracted MetalRHI directory. Findings describe static source
behavior, not a UE runtime measurement on this Mac.

### Drawable Capacity And VSync Policy

`Private/MetalViewport.cpp:244` creates a new `CAMetalLayer`. Searches across the
extracted module find no `maximumDrawableCount` getter/setter or corresponding
capitalized selector. Consequently the native initialization path retains
Apple's [default maximum drawable count of three](https://developer.apple.com/documentation/quartzcore/cametallayer/maximumdrawablecount).
Neither enabling nor disabling VSync changes that capacity. Capacity does not
prove that three textures are allocated or simultaneously held in every frame.

`FMetalViewport::Present()` sets layer synchronization at lines 1117 and 1121
using `bLockToVsync || (!(IsRunningGame() && bIsFullScreen))`:

| Native-present case | `displaySyncEnabled` | Maximum drawable capacity |
| --- | --- | --- |
| VSync requested | `true` | Default 3 |
| VSync disabled; running game and viewport marked fullscreen | `false` | Default 3 |
| VSync disabled; editor or viewport not marked fullscreen | `true` | Default 3 |

This condition does not by itself distinguish exclusive fullscreen from borderless
fullscreen. Caller-side classification and custom-present overrides were not
audited. The two `BackBuffer[2]` slots in `Public/MetalViewport.h:137,200` are
accessor storage, not the layer's drawable pool. `ResizeBackBuffer()` at lines
500-501 can assign both slots the same `FMetalSurface`; the array is not evidence
of either two drawables or two independently allocated intermediate textures.

### Acquisition, Presentation, And Independent Throttles

- `Private/MetalViewport.cpp:50-55,740-742`: `rhi.Metal.NonBlockingPresent` defaults
  to zero, so acquisition calls `nextDrawable()` without an availability precheck.
  Enabling this independent option gates acquisition on `isDrawableAvailable`,
  declared through `CAMetalLayerSPI`. VSync does not toggle it. This is an SPI
  path, not a recommendation to adopt an undocumented availability API in Durin.
- `Private/MetalViewport.cpp:1060-1076`: macOS attaches a scheduled handler that
  calls each drawable's `present()`. The adjacent `presentDrawable()` and minimum
  presentation-duration code is the non-macOS branch. A completion handler at
  lines 1038-1058 releases retained drawables and signals the frame semaphore.
- `Private/MetalDevice.cpp:290` creates that GPU-completion semaphore with capacity
  three, or one with `-gpulockstep`. `Private/MetalViewport.cpp:1098` waits on it.
  This limits outstanding GPU frames independently of the layer's drawable pool
  and does not change with VSync.
- `Private/MetalDevice.cpp:256-260,434-437` also supports an optional platform
  frame-pacer event. Its platform implementation and effective configuration were
  not retrieved, so this review does not establish whether that throttle is
  enabled in a particular UE application.
- Intermediate-backbuffer selection is separate: the module's initial
  `GMetalSupportsIntermediateBackBuffer` value is zero, and command-line/config
  state can affect it. The intermediate path blits to the drawable at
  `Private/MetalViewport.cpp:961-978`. It does not set drawable-pool capacity.

The native call chain is `RHIEndDrawingViewport()` -> queued graphics command ->
`FMetalRHICommandContext::EndDrawingViewport()` -> `FMetalViewport::Present()` ->
`PresentDrawLayers()`. At lines 1315-1347 a custom presenter may replace native
presentation; otherwise the incoming `bLockToVsync` reaches the condition above.

UE therefore does not supply precedent for an automatic two-drawable VSync-off
policy. Its macOS scheduled-handler presentation is also already covered by the
native control above, where changing only that API did not remove the reproduced
three-drawable ceiling. The two-drawable Durin repair candidate remains a locally
measured workaround, with the same production and cross-machine qualification
requirements; the UE source comparison adds no runtime acceptance result.

## Lifecycle And Display-Path Controls On 2026-10-18

### UE And Durin Lifetime Comparison

The following comparison uses the extracted UE native macOS direct-backbuffer
path. It is source inspection plus isolated native controls, not execution of
UE 5.8.3 or a complete emulation of its renderer.

| Boundary | UE 5.8.3 | Current Durin Metal |
| --- | --- | --- |
| Acquire | Presentable `FMetalSurface::GetDrawableTexture()` lazily acquires the cached drawable on texture access | Submit offscreen rendering, then acquire in `RHIEndDrawingViewport()` |
| Record and submit | Default unretained command-buffer resource references; queue payload submission commits work | Retained command-buffer resource references; commit a separate final-copy command buffer |
| Present | Scheduled handler calls drawable `present()` | Command buffer `presentDrawable()` |
| Application ownership | Retain cached drawable; clear viewport/surface references at viewport end; an extra drawable reference survives to GPU completion | Completion payload owns drawable and final-copy textures; release them in completion |
| Outstanding frames | GPU-completion semaphore initially 3, or 1 with `-gpulockstep` | RHI/backpressure and frame synchronization; no equivalent native present semaphore |

Additional UE locations are `Private/MetalTexture.cpp:1268-1318`,
`Private/MetalCommandQueue.cpp:106-137`, `Private/MetalSubmission.cpp:202-208`,
and `Private/MetalViewport.cpp:784-788,893-918,1038-1076,1347-1354`.
Neither implementation needs to keep an application-owned drawable reference
until physical scanout. GPU completion and displayed-surface retirement are
different events.

### Isolated Lifecycle Controls

The new native controls use Apple M4, macOS 27.0.1/build 26A434, and a
PHL 27E1N5900 at 3840x2160 / logical 1920x1080, 60 Hz, without display mirroring.
The ordinary drawable is 3840x1920, BGRA8 sRGB, and animated every frame.
Each run has 300 frames; analysis selects frames 60-279. All valid runs report
300 GPU completions, 300 presented callbacks, and zero command-buffer errors.
Rates below count acquisitions/submissions, not physical monitor refreshes.

| Control, ordinary titled opaque window | Drawable capacity | Submission FPS |
| --- | --- | --- |
| Durin-style late acquisition, final blit, completion ownership | 3 | 59.993 |
| Drop extra application drawable/texture ownership after commit | 3 | 59.995 |
| Leave the layer's default count untouched | 3 | 59.994 |
| Acquire before offscreen rendering | 3 | 59.984 |
| Encode offscreen rendering and final copy in one command buffer | 3 | 59.987 |
| Direct render, early acquisition, scheduled present, 3-frame semaphore | 3 | 59.997 |
| Same direct path, GPU semaphore reduced to 1 or 2 | 3 | 59.997 / 59.996 |
| Direct path with unretained command-buffer references, as in UE | 3 | 59.997 |
| Durin-style path with two drawables | 2 | 210.133 |
| Direct/scheduled path with two drawables | 2 | 194.753 |
| Synchronized two-drawable control | 2 | 60.003 |

The titled opaque three-drawable baseline waits 16.488 ms on acquisition.
Commit-to-scheduled is only 0.151 ms, but commit-to-GPU-start is 15.416 ms;
the final GPU span is 0.824 ms and GPU-end-to-completion is 0.333 ms.
GPU-end-to-presented is another 31.596 ms. With two drawables, commit-to-GPU-start
falls to 0.280 ms and GPU-end-to-presented to 8.107 ms. Combining commands moves
waiting inside the command-buffer GPU envelope; it does not remove the throttle.
Explicit `waitUntilCompleted()` likewise only moves the waiting location.
These results do not support repairing this ceiling by changing extra retains,
acquisition timing, the present API, or GPU frame-semaphore capacity alone.

### Reversible Window And Opacity Controls

Fullscreen coverage is not necessary to remove the ceiling. A small window with
the title bit removed also submits faster, whereas a titled window covering the
screen remains near 60 FPS. `framebufferOnly=true` with direct rendering still
produces 59.992 FPS. The stronger controls keep the same native window, view,
layer, drawable dimensions, count of 3, and `displaySyncEnabled=false`, changing
one property repeatedly at the main-thread/native boundary:

| Phase | `Titled` bit control: mask and FPS | Opacity control: value and FPS |
| --- | --- | --- |
| 0 | 15: 59.998 | `true`: 59.978 |
| 1 | 14: 412.237 | `false`: 423.613 |
| 2 | 15: 59.996 | `true`: 59.989 |
| 3 | 14: 386.384 | `false`: 489.523 |
| 4 | 15: 59.986 | `true`: 59.983 |

Only `NSWindowStyleMaskTitled` differs between masks 15 and 14. The opacity
control leaves the style mask at 15. All 600 completions and presented callbacks
arrive in each control, without errors. Positive presented timestamps are a
separate measure: the fast nonopaque/no-title phases have about 35-36 positive
timestamps out of 70 selected frames, versus 70 in paced phases. Increased
submission rate does not mean every submitted frame is physically displayed.
A matched two-drawable title-bit sequence remains above 150 FPS in every phase.

An attached Metal System Trace successfully records probe PID43567. Its IOSurface
events identify three probe surfaces, IDs 304, 347, and 353. Of 504 displayed-probe
intervals, 502 carry `Direct to Display: Yes`; their mean duration is 16.668 ms.
A second successful trace records the same-window opacity sequence in PID45806,
with an 8 ms synthetic CPU delay to limit event volume. It alternates between
the probe's three directly displayed surfaces (IDs 195, 196, 292) and compositor
output surfaces (IDs 7, 8, 10) marked `Direct to Display: No`, matching the
opaque/nonopaque alternation. Both probes finish normally. This corroborates
display-path selection, not just a change in CPU-side reference counting.
Apple documents opacity as a condition for
[direct-to-display eligibility](https://developer.apple.com/documentation/metal/managing-your-game-window-for-metal-in-macos).
The exact private reason that this configuration retains refresh pacing with
three drawables and synchronization disabled is not exposed by these traces.

A third successful trace, PID47349, keeps the titled opaque window but requests
two drawables, again with an 8 ms synthetic CPU delay. Its input surface IDs are
32 and 197; neither appears as a directly displayed probe surface, and all 824
displayed-output intervals carry `Direct to Display: No`. The complete traced
probe reports 1800 completions, 1800 presented callbacks, and zero errors; its
selected frames 300-1499 submit at 96.165 FPS under tracing and the added delay.
Thus the two-drawable candidate also leaves the reproduced direct-display path,
without changing window decorations or the layer's opacity contract.

With the user's authorization, BetterDisplay is then quit for a matched control
and relaunched in a `finally` block. Display reports before, during, and after
are identical. With its process absent, the titled opaque three-drawable repeats
produce 60.296 and 59.990 FPS; two drawables produce 210.721 FPS, and three with
`opaque=false` produce 299.420 FPS. A running BetterDisplay process is therefore
not necessary for this reproduction; this does not undo or exclude persistent
display settings it may have applied previously.

### Cause Boundary And Repair Choice

The causal boundary is the native displayed-surface path and its retirement
pressure under the measured titled/opaque/three-drawable configuration.
`displaySyncEnabled=false` does not prevent that path from progressing at the
display cadence. Its wait reaches Durin at `nextDrawable()` and propagates through
the existing RHI queue to the render/game waits visible in the supplied trace.
The source/runtime difference with UE is also narrower than pool size: UE's
ordinary editor/windowed native path explicitly keeps layer synchronization on.
Its default three-drawable pool is not evidence of unlimited windowed submission.

Durin's [`FGlfwWindow`](../../Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp)
creates decorated windows and an opaque layer. Its
[custom-title bridge](../../Engine/Source/Runtime/ApplicationCore/Private/MacOS/MacOSCustomTitleBarBridge.mm)
hides the title and adds `FullSizeContentView` while preserving the `Titled` bit.
That matches the independently reproduced configuration. Replacing the command
submission/lifetime pipeline is not justified by the controls.

The user-selected production repair fixes the pool capacity at two for all
policies, retaining the opaque layer and existing completion ownership. No
per-frame GPU-idle wait or presentation/lifetime rewrite is introduced. A
synchronized native control produces approximately 60 FPS with either capacity;
it supplies no evidence that the synchronized path needs three. Keeping a fixed
capacity also avoids pool reconfiguration during VSync toggles. The lasting
behavior belongs to the viewport contract linked above.

Two drawables can incur WindowServer composition work in exchange for avoiding
the observed direct-display throttle. This remains a compatibility workaround
for the measured system. Another OS/device, manual pointer-driven window gestures,
and the underlying private display policy remain unqualified. `BestEffort` still
needs its separate bounded-acquisition review. No native timing assertion should
require every Mac to exceed 60 FPS. Native probes and Debug/Tracy Editor timings
do not establish a Release/Tracy-off performance baseline.

New raw CSVs, process/display receipts, versioned native sources, runners, trace
exports, and source fingerprints are retained under ignored
`Build/.agent-state/metal-lifecycle-20261018/`. Its README records commands and
sample selection. The initial launched trace was killed at its recording limit;
a later no-title trace timed out during finalization. Neither is attribution
evidence. The three successful attached traces above are distinct from those
attempts. Those experiments did not change production policy; the later fixed-pool
adoption is recorded above and in the qualification receipt below.

### Fixed-Capacity Production Qualification On 2026-10-18

The [Metal plan's bounded Stage 1/4 receipt](../Plans/MetalRHI.md#closeout-evidence-and-remaining-work)
records the final source selection, four passing Metal-API-validation viewport
tests, restored-production Editor startup/presentation/shutdown, and the failed
secondary-window shrink case. It also distinguishes completed multi-window
policy checks from the incomplete full lifecycle run. The short real Editor
sample reaches approximately 190-230 FPS with one unsynchronized window and
160 FPS with three windows, versus approximately 60 FPS when synchronized. All
sampled layers keep capacity two and opacity true. These Debug/Tracy submission
rates confirm removal of the local refresh ceiling, not a cross-system or
Release performance guarantee.

Raw test logs, temporary diagnostic sources, fingerprints and runtime phase
summaries are retained under ignored
`Build/.agent-state/metal-two-drawables-editor-20261018/`. The production change
only sets capacity at viewport creation; policy switches still change only
`displaySyncEnabled`. The secondary resize failure remains a separate lifecycle
qualification gap and is not concealed by the passing presentation-policy cases.
