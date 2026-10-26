# Native Vulkan Presentation Pacing On Windows

**Status:** Open; a user-selected NVIDIA DXGI preference is a validated workaround

**Last reviewed:** 2026-10-27

## Scope And Verdict

On RTX 3090, driver 616.64, Windows build 26200 and Sandbox GrayboxStage15,
VSync-off native Vulkan presentation caused choppy desktop mouse movement and
window dragging. Video stayed smooth; losing focus or minimizing restored
interaction. An application-scoped NVIDIA DXGI preference removed the symptom
without a fixed FPS cap. A native repair without driver configuration remains
unproven; no temporary diagnostic code was adopted. The engine does not modify
persistent driver settings; the experimental automatic configuration was removed.

Affected users may manually choose **Vulkan/OpenGL present method > Prefer
layered on DXGI Swapchain** for their executable in NVIDIA Control Panel.
This is an optional driver workaround, not an engine setting or an engine-owned
DXGI presenter. Its effect is qualified only on the tested machine; the user
retains control of whether to apply or restore it.

## Verified Findings

- The production [swapchain](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp)
  uses Immediate with two images when unsynchronized. Acquire and render-done
  semaphores are connected; frame retirement bounds GPU work, not all downstream
  Windows presentation. Source inspection did not establish a missing wait.
- A 120/144 FPS cap, Mailbox, more images, all-commands acquire waiting, and
  waiting for the preceding present ID did not remove the native-path stalls.
- CPU acquire-fence waiting or queue-idle serialization suppressed long DWM
  intervals while reducing window throughput to approximately 60 or 33 FPS.
  Nonblocking acquire polling retained above-refresh scene rendering but only
  approximately 58 window presents/second; it is not a high-rate present fix.
- A UE 5.8-style chain also failed: three images, intermediate rendering,
  delayed acquire/copy, CPU acquire-fence waiting, render-done signaling and
  post-present previous-frame completion tracking. Tracy confirmed the waits
  executed. This was a functional comparison in Durin, not an actual UE run.

The final same-binary full-chain comparison retained native GDI-copy stalls
both with Explicit Native and with no application profile (automatic
configuration disabled). Maximum selected DWM intervals were 2923.506 ms and
1151.067 ms; the DXGI control used composed flip with a 2.982 ms maximum and
no interval over 100 ms. The trace reported zero lost events or buffers.

## Evidence And Remaining Gaps

Raw ETL, PresentMon CSV, Tracy, diagnostic patches and scripts remain under
ignored `Build/Diagnostics/Uncapped/`. Key receipts are
`system-path-ab.etl`, `system-engine-final.etl`,
`native-synchronization-summary.json`, `ue-chain-summary.json`,
`system-ue-reversal.etl`, `ue-reversal-summary.json`, `ue-full-native.tracy`,
`native-synchronization-probes.patch` and `ue-chain-probes.patch`.
Present rates use recent present-request intervals; DWM durations pair
`SCHEDULE_PRESENT` start/stop events on the same thread in selected steady-state
windows. PresentMon display counts do not mean one full refresh per request.

These interactive captures are diagnostic comparisons, not exclusive-GPU
performance acceptance; another game's presentation traffic was present in the
UE comparison. Subjective confirmation covers the tested DXGI workaround,
not the temporary native variants. Long DWM intervals alone cannot prove a
fault because independent flip can legitimately let DWM sleep; classification,
controlled reversal and observed input behavior must be considered together.

The private blocking stack, historical regression boundary and cross-machine
scope remain unresolved. GPU/acquire completion cannot be equated with complete
Windows presentation. Low-latency scheduling remains untested; no new
presentation backend has been selected or implemented. Temporary synchronization
code was removed, and this task's unmodified application profiles were deleted
after removing the automatic configuration code. No startup or shutdown path
creates or restores a driver profile.
