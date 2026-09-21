# RHI Sync Point Refactor Plan

Summary: Replace public GPU submission tickets and receipts with one-shot sync points, keep native completion coordinates inside the backend, and preserve multi-queue lifetime and failure guarantees.

Last reviewed: 2026-09-17

Status: Archived
Completed: 2026-09-17

## Current Status

All stages are complete. Public callers retain one sync-point identity;
physical completion coordinates and mutation are confined to the backend seam.
No ticket/receipt adapters or public resolution operations remain.

Final validation on 2026-09-17 (Win64-Debug-DurinEditor):

- Workspace `all` build passed, including Engine, Sandbox, RoadWeaver and
  RoadWeaverEditor. Log: `20260917-173633-380232-32076-cmake.log`;
  the earlier full migration build `20260917-173215-646892-10884-cmake.log`
  records the project target compilation/linking.
- `test affected --report`: 22/22 targets passed, zero failed/skipped targets.
  Final CTest log: `20260917-173724-528436-36408-ctest.log`; report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.
  The final run includes 93 RHI command/completion cases, 100 Vulkan integration
  cases and renderer resource reload tests, with no skipped GPU cases.
- GPU: NVIDIA GeForce GTX 1060 6GB, Vulkan API 1.4.312, driver `0x91908000`,
  Khronos validation enabled, native timeline semaphores available. Correctness
  fixtures exercised graphics `(0,0)` with compute `(0,1)` and `(2,0)`, legacy
  and synchronization2 ownership transfer, inline and threaded replay. No VUID
  or validation error/warning diagnostics appeared in the final run; deliberately
  injected factory/submission failures remain expected diagnostic output.
- CPU lifetime tests cover 2,048 submit/cancel cycles and release of retained
  reservation metadata. Vulkan tests retain 256 logical observers across 128
  submit/cancel cycles while native fence storage stays at its warmed baseline.
  Ordinary/device-lost native failure after one accepted payload verifies
  quarantine through shutdown. This test exposed and fixed CPU-only replay
  storage detach trying to reserve new work on a closed queue.
- Consumer search covered Engine, Sandbox and RoadWeaver source/test roots.
  Obsolete API search and `git diff --check` are clean. Changed-document and
  all-plan validation pass. Runtime contracts and the multi-queue plan are
  reconciled without closing its unrelated outstanding gates.

Logs above are under `Build/.agent-state/logs`; per-target case output is also
in `Build/Win64-Debug-DurinEditor/Testing/Temporary/LastTest.log`.
Baseline before migration was 88 RHI cases and 98 Vulkan cases, both passing
(`20260917-171331-853272-22568` and `20260917-171348-548838-36592`).

Contract decisions:

- Use a noncopyable metadata object and the existing `TRefCountPtr` convention,
  with immediate metadata-only destruction rather than `FRHIResource` deferred
  deletion. An owning reference offers null-safe state/retirement observation;
  `Signal->IsComplete()` observes successful completion only.
- Backend-only reservation state owns authority, generation, queue, value and
  atomic status. A mutex publishes the one-time association to each logical
  object. Coalesced signals share this authoritative reservation state without
  exposing another public synchronization object or retaining native handles.
- Preserve `ERHIGPUWaitResult` and bounded `RHIWaitForCompletion(Signal, ns)`.
  Unassociated recordings return Pending without dispatching replay. A free
  metadata-first wait supports retained terminal objects after shutdown;
  backend methods still reject associations belonging to replacement devices.
- Observer release never cancels. The shared executable recording lease cancels
  only an unassociated signal at final detach. Payload destruction cancels only
  pending associated work; accepted work completes or fails. Failure closes
  admission, and teardown does not claim completion.
- Retirement collections keep exact deduplicated logical references. Backend
  reservation order and canceled-prefix guards remain intact; a canceled later
  signal cannot hide an earlier live or failed use. Success waits never accept
  cancellation. Native query processing precedes success publication.

Consumer audit across Engine, Sandbox and RoadWeaver source/test roots:

| Consumers | Purpose and migration |
| --- | --- |
| RHICompletion, DynamicRHI, RHICommandList, RHIContext | One logical signal; backend-only mutation; CPU replay serial stays separate |
| VulkanCompletion, VulkanSubmission, VulkanContext, VulkanQueue | Reservation authority, signal association, preflight and native GPU waits |
| VulkanQueueTransfer, VulkanTransferArena, VulkanTexture | Exact producer dependency, pressure scheduling and successful readback |
| VulkanDevice, VulkanBuffer, VulkanDescriptorSets, VulkanMemory | All-queue and per-allocation retirement; coordinates only inside backend diagnostics |
| RDG, RendererResourceReloadVulkanTests | Owning execution signals and terminal-join readiness |
| RHICompletionTests, RHICommandListTests, VulkanMemoryPolicyTests, VulkanResourceTransitionTests, VulkanRHIPrivate test hooks | Preserve cancellation/failure/ordering assertions and add stable identity/lifetime coverage |

Sandbox and RoadWeaver have no direct ticket/receipt consumers. Their targets
remain covered by the required workspace `all` build.

The preceding cleanup commits are baseline work, not completion of this plan:
`abd52ff7d` simplifies descriptor ownership and removes unused interfaces;
`2fd3ced12` passes reserved tickets explicitly to payloads;
`9619cb388` shares graphics/compute descriptor writes. The latter two changes
passed 98 Vulkan integration tests. That evidence does not validate the future
sync-point implementation.

This plan owns the replacement of ticket/receipt API decisions in the
[RDG and RHI multi-queue plan](../../RdgRhiMultiQueueExecution.md).
Its scheduling, ownership-transfer, independent-compute, split-barrier and
aliasing goals remain owned there. Existing failure and lifetime guarantees
remain mandatory. Update that plan's affected API descriptions as each
migration stage lands; do not mark its outstanding gates complete by analogy.

## Goal

A caller retains one logical GPU sync point from command recording through
GPU completion. It does not resolve a receipt into a second object or inspect
physical timeline values to determine success. Backend scheduling and resource
retirement retain the information necessary to distinguish completion,
cancellation, failure and device replacement.

This is an ownership/API refactor, not a Ticket-to-SyncPoint rename.

## UE 5.8 Reference Findings

Reference checkout: `E:\Programs\Epic Games\UE_5.8`.
The following paths are relative to its `Engine/Source/Runtime/VulkanRHI/Private`.
They describe the inspected local version, not a requirement to copy UE code.

| Reference | Observed mechanism | Selected application |
| --- | --- | --- |
| `VulkanSubmission.h`, `FVulkanSyncPoint` | One-shot ref-counted object; CPU-capable instances expose completion through a graph event | Stable owning sync-point reference with a narrow observation API |
| `VulkanSubmission.h`, `FVulkanPayload` | Payload owns sync-point references and separate native fence/timeline fields | Separate logical completion objects from native queue bookkeeping |
| `VulkanUtil.cpp`, `FVulkanGPUFence::Poll/Wait` and `RHIWriteGPUFence_TopOfPipe` | GPU fence uses completion sync points; optional native event path also tracks submission | Distinguish CPU submission notification from GPU completion; do not expose native values to consumers |
| `VulkanSubmission.cpp`, completion processing | Publishes sync-point events after payload completion processing | Publish completion only after native completion evidence and required result processing |
| `VulkanMemory.cpp`, staging and block reuse | Uses context sync points to gate reuse | Keep allocation retention and completion prerequisites explicit |
| `VulkanSubmission.cpp`, `EnqueueEndOfPipeTask`; `VulkanMemory.cpp`, deferred deletion | Joins per-queue completion prerequisites before deferred deletion | Keep all-queue completion joins available without replacing precise per-allocation uses |
| `VulkanQueue.cpp`, `Submit` | Handles multiple payloads with native completion bookkeeping | Preserve the existing batch-facing interfaces; native multi-payload support remains separate work |

UE's sync-point comment describes GPU/CPU waiting, but the inspected Vulkan
implementation also carries explicit semaphore waits/signals separately.
Do not infer that its graph event alone provides cross-queue GPU ordering.
Durin must retain native semaphore and ownership-transfer lowering.

## Selected Design

### One Logical Object, Backend-Owned Coordinates

Introduce `FRHIGPUSyncPoint` and an owning `FRHIGPUSyncPointRef`. The object is
one-shot and noncopyable; references may be copied. Prefer the existing RHI
reference-counting convention; Stage 0 must confirm that metadata destruction
remains safe after backend shutdown and does not enqueue work on a dead device.

Creation does not reserve a native timeline value. A recording owns the signal
lease. Replay attaches the same sync-point object to its producing payload;
the backend assigns its physical submission association exactly once.
Several logical signals may attach to a coalesced payload. A signal may never
silently move to different work after publication.

Queue authority, generation, native values and ordered reservation metadata
remain private submission state. Preserve queue identity in capabilities and
submission descriptions where scheduling needs it. Diagnostic snapshots may
report coordinates, but such values are not caller-owned synchronization handles.

Keep native reservation order during this migration. Decoupling logical
creation from physical association does not authorize reordering accepted work
or signaling past earlier pending reservations. Moving numeric assignment to
native submit time is not required by this plan.

### Observation and Waiting

The intended public shape is:

```cpp
// Names and placement are finalized in Stage 0.
FRHIGPUSyncPointRef Signal = Commands.BeginGPUSubmission(Desc);
// Record commands. Desc owns sync-point references for producer waits.
Commands.EndGPUSubmission();

bool bReady = Signal->IsComplete(); // Successful GPU completion only.
// An explicit status query distinguishes pending work from terminal failure.
// A backend-routed bounded wait returns Complete/Timeout/Pending/Canceled/
// Failed/DeviceLost/Invalid without submitting pending work implicitly.
```

`IsComplete()` is a thread-safe metadata read. Native polling remains on the
authorized backend thread. A waiter cannot block that thread waiting for its
own replay. Wait-after-shutdown must observe retained terminal metadata without
dereferencing the old backend. No new submission/interrupt worker is required.

Do not add a parallel public `GPUFence` abstraction solely to reproduce UE's
class list. If current readback consumers need a reusable fence facade, Stage 0
must justify it; each write would capture a fresh one-shot sync point so clearing
the facade cannot change a previously recorded signal.

### Internal State and Lifetime Rules

- Pending recordings, backend-associated pending work and native-accepted work
  remain distinguishable internally. CPU replay completion is never GPU completion.
- Successful native completion is the only transition that makes
  `IsComplete()` true. Submitted work cannot become canceled.
- Cancel unsubmitted work only after all executable owners detach. Destroying
  one observer reference cannot cancel work still owned by a recorder/payload.
- A failed or device-lost producer never satisfies successful output readiness.
  Ambiguous native submission failure quarantines native resources until teardown.
- Retained observation metadata must outlive backend teardown without retaining
  native fences or the device. Foreign/replacement-device associations are rejected.
- Keep command storage, allocations, transitions and query ownership alive across
  recording, replay, pending submission and accepted in-flight submission.
- A canceled signal may permit detached storage retirement, but must not conceal
  an earlier live use when queue-prefix compression is used. Keep the existing
  ordered-prefix guarantee or retain exact use sets until an equivalent proof exists.

### Readiness, Retirement and Cross-Queue Dependencies

Use sync-point reference collections for successful producer dependencies and
for retirement prerequisites, with separate predicates. A successful-output
dependency requires successful completion; detached canceled work is only
eligible for retirement. Failure/device loss use teardown, never ordinary reuse.

Keep exact logical dependencies before backend association. Deduplicate identical
references; perform queue-maximum compression only inside the backend after
authority/order validation. Do not erase a failed/canceled producer by replacing
its readiness dependency with a later successful queue value.

Batch admission still validates missing producers, cycles, queue-local pending
prefixes and foreign generations before the first native submission. Cross-queue
waits remain GPU waits, with the existing release/acquire ownership protocol.
No per-pass CPU waits or unconditional device-idle calls are introduced.

## Scope and Migration Inventory

| Owner | Required migration |
| --- | --- |
| RHI completion | Replace `FRHIGPUSubmissionTicket`, `FRHIGPUSubmissionReceipt` and public coordinate-based observation with stable sync-point references; hide timeline mutation |
| RHI command list/context | Recording leases, signal creation, wait descriptions, cancellation and replay association |
| Vulkan submission/completion/queue | Attach logical signals; keep native authority, reservation order, fences and timeline values private |
| Vulkan resource users | Queue transfers, buffers/textures, transfer arenas, deferred deletion, allocation/frame retirement and readback |
| RenderCore RDG | Execution waits/signals, extraction/readiness, retirement uses and diagnostics |
| Tests and project consumers | Completion contracts, command replay, Vulkan failure/lifetime cases and renderer reload consumers |

Search source and test roots of every project in `Durin.dworkspace` before
changing shared APIs, including Sandbox and RoadWeaver. Temporary adapters may
exist between stages but must have one authoritative state; remove them before
completion. Do not maintain parallel ticket and sync-point timelines.

Out of scope: native multi-payload submission, descriptor redesign, new queue
roles, split barriers, transient aliasing, UE task-graph adoption and a new
submission thread. Preserve the interfaces reserved for future batching.

## Implementation Stages

### Stage 0: Freeze Sync Point Contracts and Consumer Migration

Dependencies: none. Outcome: an implementation-ready ownership and API contract.

- [x] Inspect local UE 5.8 sync points, GPU fences and completion-driven recycling.
- [x] Enumerate all current ticket/receipt/coordinate consumers and map each to
  readiness, scheduling, retirement, diagnostics or CPU replay.
- [x] Finalize object/reference storage, status and bounded-wait signatures,
  backend binding authority, signal coalescing and thread-safe publication.
- [x] Specify observer destruction, recording cancellation, shutdown and
  canceled-prefix retirement behavior with concrete state-transition cases.
- [x] Select existing test targets from the registry and record the baseline;
  reconcile affected API sections of the multi-queue plan before implementation.

Acceptance: no caller needs receipt resolution; each migration site has an owner
and every failure path has explicit lifetime and observation behavior.

### Stage 1: Implement Sync Point State and Recording Ownership

Dependencies: Stage 0. Outcome: one stable signal identity across recording/replay.

- [x] Implement the owning sync-point abstraction and backend-only mutation seam.
- [x] Migrate recording signal leases, wait descriptions and replay association.
- [x] Cover observer copies, detached cancellation, duplicate association,
  interleaved recordings, retained metadata and unsuccessful terminal states.
- [x] Keep any adapter temporary and backed by the same authoritative state.

Acceptance: focused CPU completion/command-list tests pass; successful completion
cannot be fabricated by cancellation, reference release or backend destruction.

### Stage 2: Migrate Vulkan Submission and Retirement

Dependencies: Stage 1. Outcome: backend completion drives logical sync points.

- [x] Migrate payloads, native submission tracking and coalesced logical signals.
- [x] Migrate cross-queue dependencies/transfers and all allocation retirement users.
- [x] Preserve preflight ordering, native failure quarantine and per-queue joins.
- [x] Test delayed completion, reverse observation order, pending-prefix holes,
  cross-queue fan-in, canceled producers, partial acceptance and device replacement.

Acceptance: Vulkan integration passes, including independent-compute coverage
where required; no GPU-wait dependency is replaced by a CPU wait, and early
reuse of command storage, descriptors or upload allocations is rejected.

### Stage 3: Complete Consumer Migration and Remove Ticket APIs

Dependencies: Stage 2. Outcome: all production consumers use sync-point vocabulary.

- [x] Migrate RDG dependencies, extraction/readiness, renderer users and diagnostics.
- [x] Remove ticket/receipt adapters, public resolve/get-ticket operations and
  public physical completion coordinates not justified as diagnostic data.
- [x] Verify all workspace project source/test consumers and remove obsolete tests
  only after their behavioral assertions have migrated.
- [x] Confirm batch-facing interfaces and existing rendering behavior remain intact.

Acceptance: no obsolete production API references remain; equivalent cancellation,
failure, readiness and retirement coverage passes across affected targets.

### Stage 4: Validate Integration and Publish the Contract

Dependencies: Stage 3. Outcome: validated implementation and one authoritative contract.

- [x] Run affected native tests and explicit Vulkan multi-queue correctness gates;
  record hardware, backend capability, pass/skip counts and validation diagnostics.
- [x] Complete the required `all` build for the shared Engine API migration,
  including affected targets from all workspace projects.
- [x] Verify sync-point/native fence storage returns to a bounded baseline after
  repeated submit/retire and cancellation cycles; retained observers may retain
  metadata only. Do not introduce an unbounded completed-signal registry.
- [x] Update the owning runtime contracts and reconcile the multi-queue plan.
- [x] Validate documentation, record final evidence, and complete this plan only
  after every required gate is satisfied.

## Validation and Handoff

Follow [native-test selection](../../../Agents/Testing.md),
[build workflow](../../../Agents/BuildAndRun.md) and
[documentation validation](../../../Agents/Documentation.md).
Run focused tests during migration and the affected selection for handoff.
Existing multi-queue hardware gates remain outstanding if skipped; unavailable
GPU access is recorded as not run, never converted into success.

Each implementation commit updates this plan's status/checklists and uses exact
Plan/Stage trailers from the repository handoff rules. Creating this document
does not authorize marking any implementation stage complete.

## Owning Code and References

- [Current RHI completion API](../../../../Engine/Source/Runtime/RHI/Public/RHICompletion.h)
- [Command recording and replay](../../../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp)
- [Vulkan submission](../../../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.h)
- [Vulkan completion tracking](../../../../Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.h)
- [RDG execution](../../../../Engine/Source/Runtime/RenderCore/Private/RDG.cpp)
- [Render Graph contract](../../../Runtime/Rendering/RenderGraph.md)
- [Render resource lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
