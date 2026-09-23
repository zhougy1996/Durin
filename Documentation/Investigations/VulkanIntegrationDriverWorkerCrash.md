# Vulkan Integration Driver Worker Crash

**Status:** Open — native fault mechanism captured; the writer or owner that invalidates the driver callback remains unresolved

**Last reviewed:** 2026-09-23

## Scope And Verdict

Normal Windows `VulkanRHIIntegrationTests` execution intermittently faults in an
NVIDIA driver worker's indirect callback. This is an observed native failure,
not merely an inference from the last RHI shutdown log. The evidence does not
yet establish whether the driver independently mishandles its worker lifetime
or earlier application/layer memory corruption invalidates the worker context.
Do not report the stability problem as fixed after a successful rerun.

Relevant owners are the [test environment](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanRHITestEnvironment.cpp),
[RHI startup and shutdown](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp),
[device lifecycle](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp),
and [driver pipeline cache](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp).
The implemented startup contract is [RHI capabilities and Vulkan startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md).

## Verified Findings

The current machine uses a GTX 1060 6GB, `nvoglv64.dll` version
`32.0.15.8266`, Vulkan API 1.4.312, and validation layer SDK 1.3.296.0.
The normal environment injects `VK_LAYER_NV_optimus`, `VK_LAYER_NV_present`,
and `VK_LAYER_OBS_HOOK` in addition to the explicitly requested validation layer.

Three normal-execution exception captures identify the same driver worker path:

| Capture | Fault | Driver caller |
| --- | --- | --- |
| PID 27876, TID 29084 | Execute address zero | `nvoglv64!DrvValidateVersion+0xdf88` |
| PID 39420 | Fault at `call qword ptr [rbx+10h]`, AV reports address `ffffffffffffffff` | `nvoglv64!DrvValidateVersion+0xdf85` |
| PID 36052, OBS disabled | Execute heap address `0000022a6483f8e0` | `nvoglv64!DrvValidateVersion+0xdf88` |

These are export-relative labels, not NVIDIA private function names. The
failing worker stack continues directly to `BaseThreadInitThunk` and
`RtlUserThreadStart`; it is not an engine RHIThread callback stack. The callback
target is invalid in different ways across captures. A stale or reused worker
record is a candidate explanation, not an allocation/free-stack proof.

In the OBS-disabled capture, the engine RHIThread was concurrently initializing
the persistent driver pipeline cache through `FVulkanDevice::InitGpu` and
`FVulkanPipelineManager::InitializeDriverPipelineCache`. The main thread was
running `ThreadedSubmissionScopesRoundTripOnDedicatedFamily`. This snapshot
does not prove that pipeline cache initialization caused the corruption.

A September 7 dump independently records the same kind of null callback in
driver `32.0.15.6094`, at `nvoglv64!DrvValidateVersion+0xdeb8`. Consequently,
this failure shape predates the September 23 upload changes and survives a
driver-version change. That history does not exonerate current engine code.

## Controls And Limits

- Four complete debugger-hosted iterations passed (108 cases each); the next
  iteration was deliberately stopped to switch to normal-execution capture.
- A subsequent normal run failed with `0xc0000005` after approximately 18
  seconds. A later normal run passed. Debugger-hosted passes are not a remedy.
- Disabling OBS still produced the worker fault above, so OBS is not necessary.
- The captured test `ThreadedSubmissionScopesRoundTripOnDedicatedFamily` passed
  300 consecutive standalone iterations under the default layer configuration.
- Disabling all implicit layers, only Optimus, or only NVIDIA present each
  passed three complete iterations. Restoring the default combination also
  passed three complete iterations. These timing-sensitive controls do not
  identify a culpable layer or qualify a workaround.
- Standalone Python/ctypes Vulkan probes, without engine DLLs, did not reproduce
  the crash: 200 device lifecycles; 500 with validation; 1,000 on successive
  owner threads; 1,000 with empty queue submission and device-idle wait; 1,000
  with a retained snapshot of the pipeline cache; 500 adding a Win32 surface;
  and 1,000 adding an empty compute pipeline. Some exploratory probes overlapped
  a full-suite diagnostic run; their passes are not isolation qualification.

No runtime fix or permanent environment change is justified by these results.
The next discriminating evidence is the allocation, initialization, and release
history of the driver worker record used by `[rbx+10h]`, or an independent
reproducer with the same fault. Driver private symbols or page-heap/data-write
instrumentation may be needed. A successful layer-disable run alone cannot
close this issue.

## Local Evidence And Reproduction

Local, ignored artifacts are under `Build/VulkanCrashInvestigation/`:

- `current-normal-analysis.log` and `current-normal-analysis-2.log`: live
  exception context and caller disassembly from the first two captures.
- `no-obs-analysis.log`, `no-obs-context.bin`, and `no-obs.dmp`: live thread
  stacks and a full accessible-memory snapshot for the OBS-disabled failure.
- `normal-run-1.log`, `normal-run-2.log`, `no-obs.log`, `no-implicit.log`,
  `no-optimus.log`, `no-present.log`, and `default-confirm.log`: control runs.
- `minimal.py`, `empty.comp`, `empty.spv`, and `cache-snapshot.bin`: exploratory
  standalone probes and their inputs. The cache SHA-256 is
  `40C63E6E919DE363C633AB903D42A6507B50FFD811718A03D403AB50F0F3F28C`.

The capture used a temporary, environment-gated vectored exception handler in
the integration executable. It wrote PID, TID, exception/context addresses and
their bytes, then suspended the faulting thread by sleeping. CDB attached only
after the fault; use the saved `.exr` and `.cxr` addresses, not the debugger's
attach-breakpoint context. Other threads could progress before attachment, so
later heap contents need not equal the contents at the faulting instruction.
The paused diagnostic processes were terminated after inspection; their forced
exit statuses are not additional original crash signatures.

Full-memory dumping with `/ma` failed on an unreadable page; `/mA` retained the
accessible memory successfully. The temporary capture source and test EXE are
retained locally; its matching test PDB was not archived before restoring the
fixture build. Preserve the live symbolized transcripts as evidence, and do not
substitute the restored test PDB when analyzing the captured executable. The
checked-in fixture does not install this handler.
