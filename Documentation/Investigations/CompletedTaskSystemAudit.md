# Completed CPU Task System Audit

**Status:** Open; resolve before broad task-system adoption
**Last reviewed:** 2026-08-08

## Scope And Verdict

This audit covers the completed task-system milestones through bounded owner
diagnostics:

- [Task Continuations and Thread Dispatch](../Plans/TaskContinuationsAndThreadDispatch.md);
- [Move-Only Tasks and Consuming Results](../Plans/MoveOnlyTasksAndConsumingResults.md);
- [Typed Task Fan-In](../Plans/TypedTaskFanIn.md);
- [Task Owner Diagnostics](../Plans/TaskOwnerDiagnostics.md).

The reviewed central task-system files last changed in completion commit
`97e046e9` (`feat(tasks): add bounded owner diagnostics`).

The active Structured Task Scopes plan is outside the implementation review
except where its entry assumptions depend on completed behavior. The reviewed
implementation is primarily [`Task.h`](../../Engine/Source/Runtime/Core/Public/Threading/Task.h),
[`Task.cpp`](../../Engine/Source/Runtime/Core/Private/Threading/Task.cpp),
[`MoveOnlyFunction.h`](../../Engine/Source/Runtime/Core/Public/Templates/MoveOnlyFunction.h),
and the underlying
[`QueuedThreadPool`](../../Engine/Source/Runtime/Core/Private/Threading/QueuedThreadPool.cpp).

The architectural direction is sound. Logical executors are separated from
native thread identity, shared and unique result ownership are type-distinct,
continuations remain graph nodes, shutdown has an explicit cross-executor
coordinator, and render/RHI queues and subsystem mailboxes retain their
stronger domain contracts. The roadmap also correctly refuses to add IO,
serialized lanes, work stealing, fibers, or render targets without evidence.

The implementation is suitable for its current low-volume pilots, but it is
not ready for broad adoption. Two P1 findings allow memory/state to grow with
unbounded lifetime work, and two P2 findings make diagnostics both expensive
and temporarily incoherent. These are foundational issues rather than reasons
to replace the current task graph.

## Current Adoption And Complexity

Production adoption is deliberately narrow:

| Owner | Current use |
| --- | --- |
| Asset Compatibility Audit | Shared typed worker result plus one `ThenOutcome` publication on `GameThreadDeferred`. |
| AsyncImportCore | Unique cancelable result plus one `ConsumeThenOutcome` Worker sink. |
| Source Image Thumbnail | Void Worker launch with owner/category attribution; domain queues still own decode and upload policy. |

There is no production use of `WhenAll`, `WhenAllOutcome`, or task coalescing.
Between the continuation baseline and M3 completion, the central implementation
added 2,836 lines across `Task.h`, `Task.cpp`, and `MoveOnlyFunction.h`, plus
1,856 lines in the Core threading test file. Current file sizes are 1,301 lines
for `Task.h`, 2,728 for `Task.cpp`, and 3,426 for `ThreadingTests.cpp`.

This asymmetry is not by itself a defect: concurrency primitives need more
validation than call-site code. It does mean that future surface expansion
should require production evidence, and that the unused completed features
should be held stable rather than extended speculatively.

## Verified Findings

### P1 — Worker admission is unbounded despite the bounded contract

`FQueuedThreadPool::Enqueue` checks only whether the pool is running and
accepting work, then appends to an unconstrained `std::deque`. `FTaskScheduler`
also inserts every accepted waiting or ready node into `ActiveTasks` without a
process or owner capacity check. The only bounded execution property is the
fixed Worker count.

This conflicts with the stable statement that `AnyWorker` "remains count
bounded" and with repeated roadmap descriptions of a bounded task graph. The
GameThread deferred executor does have count and byte bounds, but those bounds
do not apply to Worker tasks or waiting graph nodes.

**Impact:** a burst producer, accidental loop, or stalled prerequisite graph
can retain an arbitrary number of task states, callables, dependency edges,
and queue entries. Memory use and shutdown latency can therefore grow until
process exhaustion even though callers were told that admission is bounded.

**Candidate direction:** either narrow the lasting contract to "bounded Worker
concurrency with an unbounded backlog" or add an explicit scheduler-wide
nonterminal/admission reservation. A real bound must include waiting nodes and
accepted continuations, not only the ready Worker deque, so accepted internal
dispatch remains guaranteed. Owner-specific throttling from structured scopes
can complement but not replace a process safety bound.

**Validation gap:** Core tests exercise multiple producers and GameThread queue
saturation, but no test fills or rejects a Worker/task-graph capacity because
no such capacity exists.

### P1 — `AllTasks` retains one map entry per task until diagnostics are queried

Every accepted task is inserted into `FTaskScheduler::AllTasks` as a weak
pointer. Terminal publication erases the strong `ActiveTasks` entry but never
erases `AllTasks`. Expired entries are pruned only inside
`FTaskScheduler::GetDiagnostics`.

The weak pointer does not retain the task payload, but its `unordered_map` node
is retained. A process that launches tasks without querying scheduler
diagnostics therefore accumulates one allocation and task id per completed task
for the entire scheduler lifetime. The next snapshot also has to traverse the
complete lifetime population before it can prune it.

**Impact:** long-running or high-throughput use creates unbounded scheduler
metadata and makes the first later diagnostic query proportional to all tasks
ever accepted. This contradicts the M3 non-goal of retaining completed task
history and the stable claim that diagnostics retain no terminal history.
The mechanism predates the recently completed milestones, but the M3 contract
made its boundedness part of the completed design and did not close the gap.

**Candidate direction:** make cleanup independent of observation. Prefer
explicit live-terminal-state accounting tied to `FTaskStateData` lifetime, or
another bounded ownership tracker, over a lifetime map that relies on snapshot
calls for garbage collection. If opportunistic pruning remains, it needs a
bounded per-operation budget and a hard cardinality guarantee.

**Validation gap:** no soak test launches a large task population while never
calling `GetTaskSchedulerDiagnostics`, and no diagnostic exposes the
`AllTasks` container cardinality.

### P2 — `ParallelFor` obtains Worker count through the full diagnostic path

`ParallelForCancelable` calls `GetTaskSchedulerDiagnostics` only to read
`WorkerCount`. The public diagnostic function holds `GTaskSchedulerMutex` while
the scheduler:

- copies all registered owner/category labels and five 32-bucket histograms;
- reads every aggregate counter and gauge;
- emits profiler aggregate plots;
- snapshots all nonterminal tasks; and
- scans and prunes `AllTasks`.

The M3 qualification records a 112 ms median for its diagnostic snapshot
fixture at high registry cardinality and separately attributes a 4–5 ms tiny
fixture cost to zeroing 1,024 fixed aggregate slots at scheduler startup.
Regardless of those fixture details, a scheduling primitive should not use a
diagnostic/reporting operation to obtain one immutable configuration value.

**Impact:** `ParallelFor` latency grows with registered attribution pairs,
live tasks, and unpruned lifetime entries. While the snapshot runs, root task
and continuation admission also block on the global task-system mutex. A
getter additionally causes profiler publication as a side effect.

**Candidate direction:** give `ParallelFor` a lightweight internal Worker-count
query or retain the configured count in scheduler lifetime state. Separately,
pin the scheduler under the global lifetime lock and build deep snapshots
without holding that global lock where lifecycle safety permits. Profiler plot
publication should be explicit rather than a side effect of a state getter.

**Validation gap:** there is no benchmark for `ParallelFor` after filling the
attribution registry, after a long task lifetime, or while other threads admit
tasks during a diagnostic snapshot.

### P2 — `GetDiagnostics` bypasses the terminal-publication barrier

Terminal publication intentionally has two phases. `PublishTerminalLocked`
stores the terminal state, then the completion hook publishes or discards typed
result storage outside the task-state lock. `GetState`, `Wait`, and dependent
registration hide that terminal state until `bTerminalPublicationFinished` is
true.

`FTaskStateData::GetDiagnostics` does not apply the same visibility rule: it
copies the raw `State`, `TerminalReason`, diagnostic, and finish time while the
completion hook may still be running. A concurrent caller can therefore see a
diagnostic `Succeeded` state before a typed result is published, or see a
terminal task inside the scheduler's `NonterminalTasks` collection.

**Impact:** public snapshots can be internally inconsistent and violate the
stable rule that terminal state becomes externally observable only after result
publication or discard. Current continuations remain safe because dependency
registration waits for the barrier, but monitoring and future scope logic can
make an incorrect decision from the diagnostic snapshot.

**Candidate direction:** derive the diagnostic-visible state from the same
barrier as `GetState` and publish terminal-only fields as one coherent snapshot.
Add a test-only completion barrier so a concurrent diagnostic read can
deterministically inspect the publication window.

### P3 — The documented Core test target does not exist

`BuildAndRun.md` gives `DevTool.bat test --target CoreTests` as its native-test
example, but the current CMake graph defines `CoreUtilityTests`,
`CoreFileSystemTests`, and `CoreConcurrencyTests`. The documented command fails
at Ninja target resolution; the completed task plans correctly use
`CoreConcurrencyTests`.

**Impact:** the first documented verification command for this subsystem fails
before tests run and obscures the actual focused target.

**Candidate direction:** replace the stale example with a real target or add a
maintained aggregate target if `CoreTests` is intended as a stable interface.

## Design Assessment

### Confirmed strengths

- Task graph state, executor dispatch, and domain result ownership are kept
  separate. The three pilots retain their request serials, mailboxes,
  cancellation, upload, and shutdown policy instead of moving those policies
  into Core.
- Shared fan-out and unique one-consumer ownership are explicit in the type
  system. Unique claim rollback preserves the source handle when consumer
  admission fails.
- Continuations never execute inline for a named target, and the GameThread
  executor has real count, payload, item, and time bounds.
- Failure/cancellation precedence, dependency propagation, and terminal
  publication are deterministic and extensively tested.
- RenderThread, RHIThread, IO execution, serialized lanes, work stealing,
  fibers, and coroutines remain evidence-gated. This is good restraint, not
  missing generality.

### Overdesign and optimization risks

- The public feature set is ahead of production demand. Typed fan-in and
  coalescing have no production caller, while owner diagnostics already reserve
  1,024 aggregate slots with five fixed histograms per slot. Removing validated
  behavior now would create churn, but extending it before a second caller
  would compound speculative surface area.
- `FTaskContinuationOptions` combines Worker-generic fields with fields that
  are meaningful only to `GameThreadDeferred` (`Priority`, generation,
  coalescing, and declared payload). Target-specific option grouping would make
  invalid or ignored combinations harder to express, but changing it is lower
  priority than the capacity and lifecycle findings.
- Diagnostic atomics consistently use acquire/release ordering even though the
  counters do not publish scheduler state. After correctness fixes, relaxed
  ordering is a reasonable measured optimization for counters, gauges, and
  histogram buckets.
- `Task.h` contains result-state locks, condition variables, unique claim
  machinery, and all fan-in templates in one public header. Moving internal
  state types into an explicit private/detail namespace and isolating optional
  composition helpers would reduce accidental API surface and compile cost.
- The process-lifetime attribution-capacity test permanently fills the global
  registry in its test process. Current definition order keeps later tests
  working, but shuffled/repeated tests are not isolated. A test-only registry
  seam or a dedicated test executable would remove this ordering dependency.

The right response is to freeze new task vocabulary, repair the four verified
findings, and collect production evidence. Rewriting the graph, removing
shared/unique ownership modes, or adding more executors is not justified.

## Validation Performed

Review combined the completed plans and stable
[CPU Task System](../Runtime/Core/TaskSystem.md) contract with targeted source,
test, recent-commit, and production-call-site inspection.

Current `Win64-Debug-DurinEditor-Tests` results:

- `CoreConcurrencyTests`: 100/100 passed;
- task-focused filter across 12 suites: 78/78 passed;
- `AssetImportCoreTests`: 23/23 passed;
- `ThumbnailTests`: 46/46 passed;
- `EditorAssetWorkflowTests`: 70 cases ran, 69 passed and one was skipped.

These results confirm the existing functional baseline. They do not exercise
unbounded Worker backlog, observation-free lifetime growth, diagnostic
publication coherence, or diagnostic contention under sustained admission.

## Resolution Boundary

This investigation is ready to close when:

1. the Worker/task-graph admission contract is either truly bounded or
   accurately documented as an unbounded backlog, with matching saturation
   coverage;
2. completed-task tracking has a hard memory/cardinality bound independent of
   diagnostic queries;
3. `ParallelFor` no longer takes the full diagnostic path and snapshot work no
   longer monopolizes global admission unnecessarily;
4. diagnostic terminal visibility uses the result-publication barrier and has
   deterministic concurrency coverage; and
5. the documented focused Core test command resolves to an existing target.

Structured scopes may proceed as design work, but broad production migration
should not treat the completed M1–M3 evidence as satisfying these gates.
