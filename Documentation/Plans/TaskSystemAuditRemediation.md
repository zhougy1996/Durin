# Task System Audit Remediation Plan

Summary: Bound process task-graph admission, remove lifetime task-history metadata, and make scheduler diagnostics coherent and cheap enough for runtime use.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

Planning is complete from the completed-task audit and the central task-system
baseline `97e046e9` (`feat(tasks): add bounded owner diagnostics`). The current
checkout baseline is `1ec8aa0a`; later commits do not change the reviewed task
implementation. Stage 0 is ready to begin.

This plan resolves the two P1, two P2, and one P3 findings in
[Completed CPU Task System Audit](../Investigations/CompletedTaskSystemAudit.md).
It is a prerequisite for resuming Stage 1 of
[Structured Task Scopes](StructuredTaskScopes.md), whose frozen Stage 0 remains
valid but whose implementation touches the same scheduler admission, terminal
publication, diagnostics, and test files.

The initial implementation working set is limited to `Task.h`, `Task.cpp`,
`ThreadingTests.cpp`, and `BuildAndRun.md`. `QueuedThreadPool.cpp` is a validated
direct dependency but is not initially writable: bounding scheduler-wide
nonterminal reservations also bounds the scheduler's ready Worker queue, while
the lower-level pool remains usable by independently owned callers.

## Goal

Make the completed CPU task graph safe for long-running and bursty production
use by enforcing one process-wide bound over every accepted nonterminal node,
removing observation-dependent lifetime history, separating runtime scheduling
queries from reporting work, and exposing terminal task diagnostics only after
result publication is coherent.

## Scope

- A configurable scheduler-wide nonterminal-node capacity with a finite engine
  default and deterministic rejection at the existing submission boundary.
- Capacity accounting for roots, waiting nodes, continuations, typed fan-in,
  unique-result sinks, and scheduled parallel-for chunks.
- Live terminal-state and result-state diagnostics without an `AllTasks`-style
  map or any other per-completed-task history.
- A lightweight immutable Worker-count query for `ParallelForCancelable`.
- Scheduler snapshots that pin lifetime briefly, build deep task diagnostics
  outside the global lifetime lock, and publish profiler plots explicitly.
- Barrier-coherent per-task and scheduler diagnostic snapshots.
- Focused saturation, soak, concurrency, contention, and regression coverage.
- Correction of the stale native Core test target in `BuildAndRun.md`.

## Non-Goals

- Adding owner quotas, scope quotas, priorities, backpressure waits, producer
  blocking, work stealing, fibers, coroutines, IO execution, or serialized
  lanes.
- Adding a second capacity policy to `FQueuedThreadPool` for callers outside the
  process task scheduler.
- Retaining completed task records, timelines, raw samples, or dynamic labels.
- Changing shared versus unique result ownership, continuation ordering,
  dependency failure precedence, GameThread queue limits, or shutdown mode
  semantics.
- Refactoring the public task header, option grouping, diagnostic atomic memory
  order, or unused fan-in/coalescing surface while correctness work is active.
- Migrating additional production owners before the audit gates pass.

## Design Decisions and Invariants

### Admission and capacity

- `FTaskScheduler::Submit` owns one linearizable nonterminal reservation. The
  scheduler accepts a node only when it is running, its prerequisites are
  valid, and capacity is available; otherwise it rejects before constructing
  or retaining task state and leaves caller-owned callables outside the graph.
- The bound counts every accepted node until terminal publication and direct
  dependent propagation have completed. It therefore includes waiting roots,
  ready Worker tasks, `GameThreadDeferred` nodes, continuations, typed fan-in,
  unique sinks, and scheduled parallel-for chunks, not only executing Workers.
- An accepted node needs no second capacity reservation when it moves from
  waiting to a ready executor queue. Internal dispatch for accepted graph nodes
  remains guaranteed by that original reservation.
- Capacity exhaustion uses the existing rejected/invalid-handle API contract
  and a distinct bounded diagnostic reason/counter. Unique-consumer claim
  rollback and parallel-for group cancellation retain their current behavior
  when admission rejects.
- The scheduler configuration exposes Worker count and maximum nonterminal
  nodes without breaking the existing `InitializeTaskScheduler(uint32)` entry
  point. Stage 0 selects the finite engine default and validation override from
  measured per-node memory and current production burst evidence; zero and
  overflow values reject initialization rather than meaning unbounded.
- Owner scopes may later impose narrower owner-controlled admission, but they
  consume the same process reservation and cannot enlarge or bypass it.

### Terminal lifetime and diagnostics

- `AllTasks` is removed. Scheduler memory must not contain one key, weak
  pointer, or allocation per task ever accepted.
- Each task state participates in a scheduler-lifetime accounting object with
  fixed atomics for live terminal task states and live terminal result states.
  Terminal publication charges once after the publication barrier; task-state
  destruction releases once. The accounting object may outlive the scheduler
  while old handles exist, but contains no task ids, pointers, labels, or
  payloads.
- Scheduler restart creates a new accounting lifetime. Running diagnostics
  report only the current lifetime; stopped diagnostics report the most recent
  completed lifetime even while its surviving handles release later.
- `FTaskStateData::GetDiagnostics` applies the same visibility barrier as
  `GetState`, `Wait`, and dependent registration. Before publication finishes,
  it reports the coherent preterminal state and hides terminal-only reason,
  diagnostic, finish time, execution total, and result-presence fields that are
  not yet externally observable.
- The scheduler's `NonterminalTasks` result contains only snapshots whose
  barrier-visible state is nonterminal. Its count equals the returned logical
  population before any documented snapshot truncation; terminal-visible
  entries captured during concurrent completion are filtered rather than
  mislabeled as nonterminal.
- The nonterminal reservation is released exactly once at the end of
  `FTaskStateData::FinishTerminalPublication`, after completion/result handling
  and direct dependent notification. This release site is also the future
  balancing point required by structured scopes.

### Snapshot locking and reporting

- `GetTaskSchedulerDiagnostics` holds `GTaskSchedulerMutex` only long enough to
  pin the current scheduler or the last-lifetime accounting state and capture
  its running/stopped identity. It never holds the global lifetime lock while
  copying attribution registries, task snapshots, histograms, or strings.
- `FTaskScheduler::GetDiagnostics` copies scalar state and strong references to
  the current active-node cohort under the scheduler mutex, releases that
  mutex, then resolves task and attribution diagnostics. It does not acquire a
  task-state lock while holding the scheduler mutex.
- Snapshot operations are observational: they do not prune containers, mutate
  task counters, or publish profiler plots. Fixed profiler aggregate plots are
  emitted from an explicit update path owned by the frame/profiling lifecycle,
  not as a side effect of a getter.
- `ParallelForCancelable` obtains only a pinned running-state/Worker-count
  snapshot. Its chunk selection performs no registry copy, histogram copy,
  task traversal, pruning, or profiler publication.
- Concurrent shutdown may change the pinned snapshot while it is being built;
  the returned `bRunning` value describes the captured lifetime state, and all
  referenced storage remains valid until construction finishes.

### Sequencing with structured scopes

- This plan lands before Structured Task Scopes Stage 1. The scope plan may
  reuse the final terminal release hook and scheduler configuration, but it
  must not introduce a parallel process-capacity tracker.
- Scope diagnostics remain capped at their frozen 64-node view. The process
  admission cap is independent of and authoritative over that view.
- Any implementation discovery that changes scope admission, quiescence, or
  lock-order assumptions updates both plans in the same stage commit before
  scope implementation resumes.

## Current Foundations and Gaps

- `FTaskScheduler::Submit` already serializes scheduler admission and inserts
  every accepted task into `ActiveTasks`, but it has no capacity check.
- `ActiveTasks` already spans waiting and ready nodes, so it is the correct
  reservation population. `FQueuedThreadPool::Enqueue` is unbounded, but the
  scheduler cannot place more ready entries there than its admitted active
  population after the new bound exists.
- `OnTaskTerminal` removes `ActiveTasks`, while `AllTasks` retains a weak entry
  until `GetDiagnostics` happens to prune it. This violates the documented
  no-history contract and makes a later query proportional to lifetime work.
- `GetState`, waits, and dependent registration hide a terminal transition
  until `bTerminalPublicationFinished`; `FTaskStateData::GetDiagnostics`
  currently copies raw terminal fields and bypasses that barrier.
- `GetTaskSchedulerDiagnostics` holds the global scheduler lifetime mutex while
  the scheduler copies fixed aggregates, resolves labels, snapshots live tasks,
  scans history, and emits profiler plots.
- `ParallelForCancelable` calls that full diagnostic path only to obtain the
  immutable Worker count.
- `BuildAndRun.md` names `CoreTests`, while the maintained focused target for
  this work is `CoreConcurrencyTests`.

## Implementation Stages

### Stage 0: Freeze capacity configuration and regression baselines

Dependencies: audit baseline `97e046e9`; no task-system implementation change.

- [ ] Measure retained bytes for representative waiting roots, continuations,
  fan-in nodes, and unique sinks, then select the finite default maximum
  nonterminal count and a small deterministic test override.
- [ ] Freeze the scheduler configuration/API shape, initialization validation,
  capacity-exhaustion terminal reason, public diagnostic fields, and source
  compatibility behavior for `InitializeTaskScheduler(uint32)`.
- [ ] Record baseline latency for `ParallelForCancelable`, a full diagnostic
  snapshot, and concurrent root admission under an empty scheduler, a full
  attribution registry, and a large completed lifetime without intervening
  diagnostics.
- [ ] Record the exact terminal-publication ordering and lock order shared by
  capacity release, live terminal accounting, dependent notification, and the
  future scope release hook.
- [ ] Replace the stale `CoreTests` native-test examples in `BuildAndRun.md`
  with `CoreConcurrencyTests`, preserving general `all` examples.

#### Acceptance Gate

- The finite default, override mechanism, rejection semantics, counters, lock
  order, and performance comparison cohort are explicit and testable.
- The documented focused Core command resolves to a maintained target.

### Stage 1: Make terminal publication and lifetime accounting coherent

Dependencies: Stage 0 configuration and ordering decisions.

- [ ] Make `FTaskStateData::GetDiagnostics` derive its visible state and every
  terminal-only field from `bTerminalPublicationFinished` as one locked copy.
- [ ] Move active-node release to the frozen end-of-publication hook without
  changing failure/cancellation precedence or shutdown quiescence.
- [ ] Replace `AllTasks` with fixed scheduler-lifetime terminal task/result
  counters balanced by publication and task-state destruction.
- [ ] Preserve stopped-lifetime diagnostics across scheduler restart without
  retaining individual task state or mixing lifetime counters.
- [ ] Add a test-only completion barrier and deterministic tests for raw
  terminal transition versus result publication, task diagnostics, scheduler
  nonterminal snapshots, dependents, and state destruction.
- [ ] Add an observation-free soak that completes a large task population,
  never calls scheduler diagnostics during the run, then proves tracker
  cardinality remains fixed and the first later snapshot is not lifetime-linear.

#### Acceptance Gate

- No scheduler-owned container grows with completed lifetime work.
- A terminal state, reason, finish time, and result-storage status become
  visible together only after completion publication succeeds or discards.
- Current and final retained terminal task/result counts balance to surviving
  state lifetimes without scanning task ids.

### Stage 2: Enforce scheduler-wide nonterminal admission

Dependencies: Stage 1 provides the one final reservation-release hook.

- [ ] Add validated scheduler capacity configuration and current/peak/capacity-
  rejection diagnostics while preserving the existing initialization entry
  point.
- [ ] Reserve before node construction in `FTaskScheduler::Submit`; release
  exactly once at the Stage 1 terminal completion hook.
- [ ] Ensure every task form uses the same reservation and that accepted
  waiting nodes dispatch without a second capacity decision.
- [ ] Add small-capacity tests for roots, stalled prerequisites, continuations,
  typed fan-in, unique claim rollback, parallel-for chunks, GameThread deferred
  work, multiple producers, capacity reuse, and both shutdown modes.
- [ ] Prove callable/result destruction occurs outside scheduler, task-state,
  Worker-queue, and GameThread-queue locks on every capacity rejection path.

#### Acceptance Gate

- Current nonterminal reservations never exceed configured capacity under
  concurrent producers, waiting graphs, continuation release, or shutdown.
- Saturation rejects deterministically without leaks, hidden graph nodes,
  claim loss, deadlock, or failure of already-accepted internal dispatch.
- Reservation counts return to zero after drain and cancel shutdown.

### Stage 3: Decouple runtime queries from diagnostic reporting

Dependencies: Stage 1 removes history traversal; Stage 2 exposes capacity
state used by the snapshot.

- [ ] Add a lightweight internal running/Worker-count query and switch
  `ParallelForCancelable` to it.
- [ ] Pin scheduler lifetime under `GTaskSchedulerMutex`, release it, and build
  the scheduler snapshot from a bounded active-node cohort without holding the
  global lock or nesting scheduler and task-state locks.
- [ ] Separate fixed profiler aggregate plot publication from diagnostic
  getters and attach it to the profiling/frame update boundary selected in
  Stage 0.
- [ ] Add deterministic contention tests in which diagnostics overlap root and
  continuation admission, terminal publication, and shutdown.
- [ ] Repeat the Stage 0 latency cohort and investigate any `ParallelFor`
  regression above 10 percent or admission stall above the frozen threshold.

#### Acceptance Gate

- `ParallelForCancelable` performs no diagnostic/reporting work to choose its
  chunk count.
- Deep scheduler snapshots do not monopolize global admission and never emit
  profiler plots as an observation side effect.
- Concurrent snapshots contain no barrier-visible terminal entry in
  `NonterminalTasks` and remain lifetime-safe through shutdown.

### Stage 4: Integrate, qualify, and close the audit

Dependencies: Stages 1-3 complete and stable.

- [ ] Run the focused Core concurrency tests, task-focused filters, full native
  aggregate, profiling-disabled/enabled configurations, complete `all` build,
  and hidden-window lifecycle smoke through the repository build contract.
- [ ] Run saturation and soak qualification without diagnostics in the hot
  loop, then query final counters and reconcile accepted, rejected, active,
  completed, failed, and canceled totals.
- [ ] Update the CPU Task System contract with the finite admission bound,
  terminal accounting lifetime, snapshot locking boundary, and explicit
  profiler-publication behavior.
- [ ] Update Structured Task Scopes to the resulting configuration, terminal
  release hook, and lock order; resume its Stage 1 only after this plan passes.
- [ ] Record resolution in the completed-task audit, remove the investigation
  and its open-index entry according to the investigation lifecycle, and
  complete/archive this plan through the plan lifecycle.

#### Acceptance Gate

- Every resolution boundary in the completed-task audit has direct test,
  benchmark, build, or documentation evidence.
- Broad production adoption can rely on a finite process task-graph bound,
  observation-independent metadata bounds, coherent diagnostics, and a valid
  focused test command.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Root/continuation -> admission | One reservation includes ready and waiting nodes and rejects before task-state retention. |
| Accepted node -> executor | Queue transition requires no second reservation and succeeds until shutdown policy terminalizes it. |
| Terminal hook -> capacity | Result publication and direct dependent propagation finish before exactly one release. |
| Terminal hook -> diagnostics | State, reason, diagnostic, timing, and result presence cross one visibility barrier. |
| Task state -> lifetime accounting | Publication charges fixed counters once; destruction releases once; no id/pointer history remains. |
| Scheduler lifetime -> snapshot | A pinned lifetime survives concurrent shutdown and never mixes old and new scheduler counters. |
| Snapshot -> admission | Deep copying and label resolution occur outside the global lifetime and scheduler locks. |
| ParallelFor -> Worker count | Chunk selection uses only immutable runtime configuration. |
| Saturation -> ownership | Rejected callables/results/unique claims return or destroy exactly once outside internal locks. |
| Documentation -> validation | Every focused native-test example resolves to `CoreConcurrencyTests`. |

## Definition of Done

- The task graph has a finite scheduler-wide nonterminal capacity covering all
  accepted task forms and exposing current, peak, and rejection evidence.
- Scheduler-owned storage is independent of the number of completed tasks and
  diagnostic observation frequency.
- Task and scheduler diagnostics obey the terminal publication barrier and can
  run concurrently with admission and shutdown without global-lock stalls.
- `ParallelForCancelable` no longer invokes the full diagnostic path.
- Focused, saturation, soak, contention, build, profiling, and lifecycle
  validation pass; lasting contracts are updated; the investigation is closed.
- Structured Task Scopes resumes from its existing frozen contract using the
  repaired admission and terminal-lifecycle foundation.

## Deferred Follow-ups

- Owner-specific throttling belongs to Structured Task Scopes and cannot
  replace the process safety bound.
- Count-plus-byte admission may be proposed separately if measured retained
  byte variance shows the finite node count is not a sufficient safety bound.
- Relaxed ordering for diagnostic atomics, task-header decomposition,
  target-specific continuation options, and test registry isolation remain
  measured cleanup after correctness qualification.
- New executors, task vocabulary, and broader production migration remain
  evidence-gated by the task-system roadmap.

## Related Documentation

- [Completed CPU Task System Audit](../Investigations/CompletedTaskSystemAudit.md)
- [Structured Task Scopes](StructuredTaskScopes.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Task System Evolution Roadmap](../Roadmaps/TaskSystemEvolution.md)
- [Implementation Plan Rules](AGENTS.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Core/Private/Threading/QueuedThreadPool.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
