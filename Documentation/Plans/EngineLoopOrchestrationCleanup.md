# Engine Loop Orchestration Cleanup Plan

Summary: Reduce `FEngineLoop` to explicit process and frame orchestration while preserving reliable Engine-owned completion, startup, rendering, and shutdown contracts.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

Design is complete and implementation has not started. The current
`LaunchEngineLoop.cpp` is 539 lines and combines four distinct concerns:

- process startup, frame sequencing, and ordered shutdown;
- runtime saved-directory preparation and legacy-file migration;
- a task-scheduler lifecycle qualification workload;
- render-frame helpers and an Engine-specific Texture2D completion pump.

The Texture2D coordinator currently places move-only build results in its own
mailbox and relies on `FEngineLoop::Tick()` to call
`PumpTexture2DBuildCompletions()` immediately after the bounded
`GameThreadDeferred` pump. That direct Texture dependency is the first boundary
to remove. The mailbox itself remains intentional: results may retain 4K and
16K platform payloads far beyond the deferred executor's default 1 MiB
per-entry and 8 MiB total payload bounds.

The selected design does not use a best-effort deferred continuation as the
mailbox's sole wakeup. Deferred admission can reject work when bounded capacity
is exhausted or shutdown has begun; without a separate reliable poll, one
rejected wakeup can leave the last completion unpublished indefinitely.

## Goal

- Make `LaunchEngineLoop.cpp` read as one direct process protocol: initialize,
  tick, render, and shut down in documented order.
- Remove Texture2D-specific knowledge and task-qualification implementation
  detail from the EngineLoop translation unit.
- Preserve exactly-once Texture2D completion, bounded normal-frame work,
  explicit wait behavior, and shutdown drain guarantees.
- Keep `FEngineLoop::Exit()` as the single visible owner of process shutdown
  ordering.

## Scope

- Add an Engine-owned asset-service frame pump and route Texture2D completion
  consumption through it.
- Keep the normal-frame pump at the existing post-engine-tick safe point and
  preserve its ordering after `PumpGameThreadDeferredWork()`.
- Move runtime storage preparation and legacy migration into a private Launch
  component with an explicit result.
- Move task-scheduler lifecycle qualification into a private Launch validation
  component.
- Move render-frame helper implementation into a private Launch frame
  component while leaving the frame call site visible in `FEngineLoop::Tick()`.
- Update the runtime lifecycle, task-system, and texture-system documentation
  where the lasting ownership contract changes.

## Non-Goals

- Replacing the Texture2D coordinator mailbox with a typed task result or with
  `GameThreadDeferred` payload storage.
- Adding a generic global callback registry, engine-exit phase registry, or
  subsystem tick registry.
- Redesigning Texture2D worker admission, build algorithms, DDC persistence,
  render upload, readiness phases, or transactional editor changes.
- Changing the task scheduler's queue capacities, dispatch guarantees, or
  shutdown coordinator.
- Moving process lifecycle ownership out of Launch or hiding the ordered body
  of `FEngineLoop::Exit()` behind a phase framework.
- General source formatting or unrelated include cleanup.

## Design Decisions and Invariants

### Engine-owned completion boundary

- `EngineAssetServices` owns initialization, normal-frame completion pumping,
  and shutdown of Engine-owned asynchronous asset services.
- Launch calls one Engine-level asset-service pump and does not include or name
  `Texture2DBuildCoordinator`.
- The first implementation routes only Texture2D completions through this
  boundary. It is not a registration mechanism; another asset service must add
  an explicit Engine-owned call with a documented order.
- The normal-frame order remains:

  ```text
  DEngine::Tick
  -> PumpGameThreadDeferredWork
  -> PumpEngineAssetServiceCompletions
  -> increment logic frame counter
  -> application tick and rendering
  ```

- Completion callbacks execute only on the GameThread. Request id, generation,
  object identity, source path, and complete settings remain the commit
  correctness boundary.

### Mailbox and budget policy

- Large `FTexture2DBuildResult` ownership remains in the Texture2D coordinator
  mailbox until the GameThread accepts or discards it.
- Normal frames retain an item budget. Stage 0 measures completion callback
  cost and freezes whether the existing 64-item cap also needs an elapsed-time
  budget; no unmeasured time constant is introduced.
- Explicit `WaitForPendingBuild()` continues to pump without the normal-frame
  budget after its request reaches the completion mailbox.
- Coordinator shutdown stops admission, cancels queued and running work, waits
  for workers, and drains all callbacks before the process task scheduler is
  closed.
- No wakeup rejection may strand a completion. A future optional deferred
  notice may reduce average latency, but it cannot replace the reliable
  Engine-owned frame pump or shutdown drain without a new durable-admission
  contract in Core.

### Launch component boundaries

- `LaunchEngineLoop.cpp` retains `FEngineLoop::PreInit`, `Init`, `Tick`, and
  `Exit`, plus the global `GEngineLoop`.
- Runtime storage preparation returns the chosen app-config path and migration
  warnings. It owns no mutable namespace-global warning collection.
- The task-scheduler lifecycle smoke remains a Launch validation because it
  qualifies process shutdown, but its workload, handles, and diagnostics move
  out of the EngineLoop translation unit.
- Render begin/end callbacks and `RenderEngineFrame()` remain Launch-private.
  `FEngineLoop::Tick()` keeps the visible decision of whether a frame renders.
- Startup and shutdown extraction must not reorder subsystem operations or
  replace explicit failure unwind with implicit static destruction.

### Public API policy

- `FEngineStartupParams` and `FEngineLoop` do not gain subsystem-specific state.
- The Engine asset-service pump is exported only because Launch consumes the
  Engine module boundary. Private Launch helpers are not exported.
- Any added public function documents its GameThread affinity, frame budget,
  and shutdown availability.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| EngineLoop | Lifecycle order is direct and documented. | Auxiliary implementations and a Texture-specific frame call obscure the protocol. |
| Texture completion | Coordinator owns move-only results, cancellation, explicit wait, diagnostics, and shutdown drain. | Launch reaches through the Engine asset-service boundary to pump it. |
| Deferred executor | Bounded GameThread queue, payload accounting, coalescing, diagnostics, and shutdown drain. | Its bounded rejection contract cannot provide the sole durable wakeup for a separate mailbox. |
| Runtime storage | Saved paths and legacy migration are established. | Hidden warning state and file-policy code live inside EngineLoop. |
| Task qualification | End-to-end scheduler shutdown smoke covers failure, cancellation, deferred work, and diagnostics. | Roughly one third of the EngineLoop file is validation workload implementation. |
| Render frame | Begin, UI/viewport render, end, sync, and counters are ordered. | Thread callbacks and RHI command details sit beside process lifecycle code. |

## Implementation Stages

### Stage 0: Freeze frame-completion behavior and baseline

- [ ] Record the baseline source revision and current `LaunchEngineLoop.cpp`
  line count, includes, and responsibility ranges.
- [ ] Add or extend deterministic Engine tests for normal-frame bounded
  Texture2D completion, queued cancellation, stale generation rejection,
  explicit wait, and shutdown drain.
- [ ] Verify whether any completion callback depends on running after rather
  than before other `GameThreadDeferred` callbacks; preserve the current order
  unless evidence requires a documented change.
- [ ] Measure GameThread completion callback time for representative 1K and 4K
  successful, failed, cancelled, and stale results using the existing Agent
  Build Profile.
- [ ] Freeze the normal-frame item/time budget decision in this plan and in the
  owning Texture System documentation.

#### Acceptance Gate

- Baseline tests fail if a completion is lost, applied off the GameThread,
  applied more than once, or survives coordinator shutdown; the selected frame
  budget is supported by recorded measurements rather than an arbitrary value.

### Stage 1: Introduce the Engine asset-service frame boundary

Dependencies: Stage 0.

- [ ] Add `PumpEngineAssetServiceCompletions()` beside Engine asset-service
  initialization and shutdown, with an explicit GameThread contract and the
  frozen normal-frame budget.
- [ ] Route Texture2D completion pumping through that Engine-owned function.
- [ ] Replace the direct Texture2D pump in `FEngineLoop::Tick()` without changing
  its position relative to engine tick, deferred work, frame counting,
  application events, rendering, or GC.
- [ ] Remove `Texture2DBuildCoordinator.h` from Launch and verify Launch has no
  remaining Texture2D-specific symbols.
- [ ] Keep explicit wait and shutdown paths directly drainable and independent
  of deferred queue admission.

#### Acceptance Gate

- Focused Engine tests prove bounded frame pumping, exactly-once completion,
  queued cancellation, stale rejection, explicit wait, and complete shutdown
  drain. Launch compiles without a Texture2D coordinator dependency, and a
  bounded-tick runtime reaches clean shutdown with an asynchronous completion.

### Stage 2: Extract runtime storage preparation

Dependencies: Stage 1.

- [ ] Add private Launch runtime-storage files that own directory creation,
  legacy log/config migration, app-config path selection, and warning capture.
- [ ] Return warnings and the selected app-config path explicitly to
  `FEngineLoop::PreInit()`.
- [ ] Remove the namespace-global migration warning vector and preserve the
  rule that warnings are logged only after logger initialization.
- [ ] Add focused tests for no-op migration, rename success, copy/remove
  fallback, existing destination, and filesystem errors where the current test
  seams permit deterministic coverage.

#### Acceptance Gate

- Startup selects the same configuration path, migration is idempotent, warning
  emission order is preserved, and repeated preparation in one process retains
  no warning state from an earlier invocation.

### Stage 3: Extract task-scheduler lifecycle qualification

Dependencies: Stage 2.

- [ ] Move the task graph, cancellation probe, deferred callback, diagnostics,
  and post-shutdown validation into private Launch validation files.
- [ ] Expose only the minimum begin/validate lifetime seam required around
  `ShutdownTaskSystem()`.
- [ ] Keep workload state alive across task-system shutdown without placing
  task handles or ThreadEvent types in `LaunchEngineLoop.h`.
- [ ] Preserve every existing workload assertion, diagnostic threshold, and
  command-line opt-in behavior.

#### Acceptance Gate

- The lifecycle smoke passes with the same completed, failed, cancelled,
  rejected, long-wait, retained-handle, and deferred-executor expectations;
  disabling the option creates no workload and changes no normal shutdown
  behavior.

### Stage 4: Extract render-frame implementation

Dependencies: Stage 3.

- [ ] Move render-thread begin/end callbacks and frame render submission into a
  private Launch frame component.
- [ ] Preserve the order `BeginFrame -> Mona::NewFrame -> viewport redraw ->
  Mona::Render -> EndFrame -> FFrameSync -> render counter`.
- [ ] Keep window-minimized policy, application tick, GC, FPS timings, profiler
  plots, and frame mark in `FEngineLoop::Tick()`.
- [ ] Reduce includes in `LaunchEngineLoop.cpp` only where ownership moved; do
  not perform unrelated include normalization.

#### Acceptance Gate

- Thread-affinity assertions, logic/render counter snapshots, UI frame-scoped
  allocation order, end-frame sync, minimized behavior, and rendering shutdown
  remain unchanged in threaded and inline RHI modes.

### Stage 5: Document and qualify the cleaned lifecycle

Dependencies: Stages 1-4.

- [ ] Update Runtime Lifecycle with the Engine asset-service completion phase
  and the final Launch component boundaries.
- [ ] Update Texture System with mailbox ownership, frame budget, explicit wait,
  and shutdown drain behavior.
- [ ] Update Task System only if implementation changes the documented
  relationship between subsystem mailboxes and deferred execution.
- [ ] Run changed-document validation, the smallest affected native test
  targets during development, a final full `all` build for the Engine/Launch
  export boundary, and normal plus inline-RHI bounded runtime shutdown smokes.
- [ ] Record final file line counts and validation evidence in Current Status,
  then complete the plan only after every acceptance gate passes.

#### Acceptance Gate

- Documentation and implementation agree; targeted native tests pass; the full
  build passes; normal and inline-RHI runtime smokes initialize, tick, render,
  apply or discard asynchronous Texture2D results, and exit without live jobs,
  deferred objects, render resources, or RHI work.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Completion scheduling | Current post-engine-tick ordering, bounded normal-frame consumption, and no dependency on best-effort wakeups. |
| Texture ownership | Large move-only results remain mailbox-owned; matching commits once; stale/cancelled results cannot mutate assets. |
| Explicit boundaries | Wait pumps the requested completion; shutdown cancels, waits, and drains before task-system closure. |
| Startup storage | Config selection, idempotent migration, fallback, and post-logger warnings remain deterministic. |
| Task qualification | Failure, cancellation, dependencies, deferred work, admission close, quiescence, and diagnostics retain existing coverage. |
| Frame rendering | Begin/end order, UI frame scope, viewport work, sync, counters, and minimized pacing remain unchanged. |
| Integration | EngineTests, affected task tests when touched, full build, and bounded runtime smokes in normal and inline-RHI modes. |

## Definition of Done

- `LaunchEngineLoop.cpp` contains no Texture2D coordinator symbol, runtime file
  migration implementation, or task-scheduler qualification workload body.
- Its remaining code directly expresses process startup, frame policy, render
  decision, and ordered shutdown.
- Texture2D normal-frame, explicit-wait, and shutdown completions are reliable,
  bounded, GameThread-only, and exactly once.
- No new generic registry or unbounded queue has been introduced.
- Long-lived ownership and ordering rules are published in Runtime Lifecycle,
  Texture System, and Task System as applicable.
- All stage acceptance gates and the final validation matrix pass.

## Deferred Follow-ups

- A durable direct-enqueue GameThread executor API may be considered only when
  a second subsystem demonstrates the same wakeup need and can define bounded
  admission, retry, shutdown, and payload-accounting semantics.
- A common asynchronous asset-build framework remains deferred until another
  asset type proves shared request, result, budget, and commit contracts.
- Initialization rollback may move to an explicit transaction only after more
  recoverable startup stages make the current direct unwind materially
  repetitive.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Asynchronous Texture2D Build and Readiness](Archive/2026-08/AsynchronousTexture2DBuildAndReadiness.md)
- [Engine Termination Lifecycle](Archive/2026-07/EngineTerminationLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/Launch/Public/LaunchEngineLoop.h`
- `Engine/Source/Runtime/Engine/Public/EngineAssetServices.h`
- `Engine/Source/Runtime/Engine/Private/EngineAssetServices.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DBuildCoordinator.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DBuildCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/Texture2DBuildCoordinatorTests.cpp`
