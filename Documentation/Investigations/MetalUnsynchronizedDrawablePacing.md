# Metal Unsynchronized Drawable Pacing

**Status:** Open

**Last reviewed:** 2026-10-05

## Scope And Verdict

On the measured Apple M4/macOS/display configuration, a Metal viewport with
`displaySyncEnabled=false` and three drawables presents at 60 Hz. Two drawables
remove that ceiling. The remaining question is why the system presentation path
changes its behavior with pool capacity; no production policy change is selected
by this investigation.

The evidence locates the sustained throttle at drawable acquisition and
presentation/recycling. It does not establish a universal triple-buffering rule,
an Apple driver defect, or an independently reproduced Core Animation defect.
Two drawables are a verified local workaround, not a cross-machine qualification.

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

Run through `./DevTool run --project Sandbox/Sandbox.dproject --args
--exit-after-ticks=480`. Events are buffered in memory and written on shutdown.
GPU start/end intervals of separate command buffers overlap and must not be
summed as frame GPU cost. The analysis reports their envelope only, including
possible waits. Presented callback dispatch time is recorded separately from
`presentedTime`; neither is an exposed drawable-recycling timestamp.

## Remaining Validation Gaps

- A minimal native Metal window outside the engine is needed before assigning
  the behavior exclusively to macOS/Core Animation rather than an interaction
  with the host window and submission path.
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
`./DevTool build --target all` passed after restoration (receipt
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
both were restored and `./DevTool build --target MetalRHI` passed afterward
(`20261005-173525-107556-64483-cmake.log`). The display remains at the user's
30 Hz setting. No production policy change was retained.
