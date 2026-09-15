# Native Test Qualification

Summary: Define GPU environment admission, quiet timing lanes, and Vulkan creation memory measurement policy for explicit native-test qualification.

Last reviewed: 2026-09-15

Use this document only for hardware, performance, scale, or memory qualification.
Routine selection follows [Agent Testing Workflow](../../Agents/Testing.md);
selectors, modes, and reports follow [Native Test Execution](NativeTests.md).

## GPU Qualification Environments

On macOS, MoltenVK cannot access Metal services from the default Codex sandbox
and reports `Metal is not available on this device`. Do not run a GPU test or a
Metal probe in the sandbox merely to reproduce that expected failure. For
optional coverage, report GPU execution as unavailable under the
[agent testing rules](../../Agents/Testing.md). When an
explicit user request or acceptance gate requires GPU execution, request the
normal sandbox-escalation approval and run the exact registered qualification
selection outside the sandbox. Record the unsandboxed device name and receipt;
never bypass authorization or weaken the test to turn sandbox initialization
failure into a pass.

## Performance Qualification and Concurrent Agents

Material feature targets retain correctness coverage without repeated latency
sampling. Run `./DevTool test MaterialQualificationTests --mode qualification --report`
explicitly (on Windows, use `.\DevTool.bat` as the launcher) for
maximum-graph layout median/p95, graph-load timing, cold/warm shader compilation,
and instance-variant payload sizes. These CPU qualification cases are excluded
from ordinary `fast-all` and affected-test execution.

Ordinary correctness builds and tests may run while other agents are active,
subject to the repository's single-writer and no-overlapping-build rules. GPU
timing qualification is different: results are authoritative only from an
exclusive quiet GPU lane with no competing agent test, editor, browser workload,
capture tool, or other GPU application. A machine reboot is not required when
the qualification supplies its documented warm-up.

The `durin-gpu` resource lock serializes physical GPU owners within one CTest
scheduler. `durin-rhi-lifecycle` separately serializes real backend startup,
shutdown, and module replacement while allowing CPU-only tests to overlap.
Neither lock coordinates independent DevTool/CTest invocations, separate
worktrees, agents, or external applications. When any of those may be competing,
run correctness coverage normally but label timing output diagnostic only: do
not rebaseline a threshold, accept a performance gate, or claim a regression
from it. Rerun the exact qualification selection in a quiet window; prefer
consecutive passes and report the warm-up/sample count and median/p95.
Statistical stability checks can reject bursty contention, but stable sustained
contention is indistinguishable from a code regression without exclusive
execution.

## Vulkan Creation Qualification Memory

On Windows, `VulkanCreationQualificationTests` and
`MaterialCreationQualificationTests` retain the active Khronos validation DLL
until test-process exit. Before the first `RHIInit`, the fixture creates and
destroys a minimal Vulkan instance on the test main thread and acquires one reference to the validation
DLL selected by the Vulkan loader. The bootstrap is outside measured rounds.
It runs only when engine policy requests diagnostics; an unavailable validation
layer leaves ordinary RHI capability negotiation in control. Initializing the
DLL on a short-lived RHI owner thread and retaining it afterward is insufficient
and caused crashes in the tested layer. It does not use an SDK path override
or request a layer when validation is disabled. Every RHI instance, device and resource still follows
its ordinary teardown. Host receipts record
`validation_layer_lifetime=main_thread_bootstrap_process_if_available`.

This is a test-host lifetime policy: repeated validation DLL unload/reload can
retain allocator arenas and dominate process private-memory measurements.
The verified SDK 1.4.357.0 Windows reproduction grew even with only
`LoadLibrary`/`FreeLibrary` and no Vulkan calls. Keeping the DLL loaded allows
its allocator to reuse memory while preserving validation checks. The evidence
does not require an engine-runtime ownership change or establish that all
remaining process memory is an engine leak. Detailed historical measurements
remain in the [creation qualification investigation](../../Investigations/RHICreationQualificationAttribution.md).

Use the same layer-lifetime policy, validation settings, SDK/layer, GPU driver,
cache seeds and instrumentation for both sides of a memory comparison. Old
DLL-unload measurements are attribution evidence, not a directly comparable
acceptance baseline. Retaining the layer does not waive memory budgets or
replace the documented warm-up and quiet-lane requirements. A test whose
purpose is DLL unloading must use a separate process or dedicated
characterization fixture; do not add its churn to creation qualification.
Do not disable validation or set `MIMALLOC_DESTROY_ON_EXIT` as a general fix.
The temporary absolute-path diagnostic override and standalone probe were
removed after attribution; the ordinary test command applies the policy.
