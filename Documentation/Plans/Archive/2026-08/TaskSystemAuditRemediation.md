# Task System Audit Remediation Plan

Summary: Bound process task-graph admission, remove lifetime task-history metadata, and make scheduler diagnostics coherent and cheap enough for runtime use.

Last reviewed: 2026-08-08

Status: Archived
Completed: 2026-08-08

## Current Status

All stages are complete. The two P1, two P2, and one P3 audit findings are
resolved: scheduler-wide admission is finite, terminal accounting retains no
completed-task history, diagnostics obey the publication barrier without deep
global-lock work, `ParallelForCancelable` uses a shallow Worker-count query,
and the maintained focused test target is documented. The resolved
investigation was removed according to its lifecycle after lasting behavior
moved to the CPU Task System and CPU Profiling contracts.

Stage 4 passed the focused and aggregate native suites, observation-free
saturation/soak accounting, Tracy-disabled and Tracy-enabled complete builds,
and the hidden-window scheduler lifecycle smoke. [Structured Task Scopes](StructuredTaskScopes.md)
Stage 1 is ready to resume from its frozen contract using the resulting
configuration, final terminal hook, and lock boundaries.

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

## Stage 0 Frozen Contract

### Capacity configuration and rejection

- The engine default is 16,384 process task reservations. The measured Debug
  retained-heap cost ranged from 2,130 to 3,813 bytes per graph node, so the
  worst measured cohort consumes about 59.6 MiB at the default. Current
  production pilots create only roots plus one sink/continuation at a time;
  16,384 leaves substantial burst headroom without accepting a quarter-gigabyte
  Debug graph as the 65,536 alternative would. This is a count bound, not a
  payload-byte quota.
- `FTaskSchedulerConfig` contains `uint32 NumWorkerThreads = 0` and
  `uint64 MaxNonterminalTasks = 16'384`. A new
  `InitializeTaskScheduler(const FTaskSchedulerConfig&)` overload owns explicit
  configuration. `InitializeTaskScheduler(uint32 InNumThreads = 0)` remains
  source-compatible and delegates with the finite default. Worker zero retains
  its existing automatic-selection meaning; capacity zero and capacity above
  `uint32` maximum reject initialization. Reinitialization while running keeps
  its existing idempotent behavior and never changes the active configuration.
- Focused saturation tests use capacity 8. `FTaskScheduler::Submit` validates
  scheduler state and prerequisites, then reserves under the scheduler mutex
  before constructing `FTaskStateData`. Exhaustion returns the existing invalid
  handle, leaves unique-consumer claims available for rollback, and destroys
  callables/results after the global and scheduler locks have been released.
- `ETaskTerminalReason::CapacityExhausted` is the distinct rejection vocabulary
  even though no rejected task state is constructed. Scheduler diagnostics add
  `TaskReservationCapacity`, `CurrentTaskReservationCount`,
  `PeakTaskReservationCount`, and `CapacityRejectedTaskCount`; owner/category
  diagnostics add `CapacityExhaustedCount`. Existing `RejectedTaskCount` and
  owner `RejectedCount` continue to include capacity rejection.

### Measurement baseline and thresholds

The repeatable manual fixture is
`DISABLED_FTaskSystemAuditBenchmarks.RecordsStageZeroCapacityAndLatencyBaseline`.
The recorded run used MSVC 14.44 Debug, four Workers, 11 latency samples, 4,096
nodes per memory cohort, a full 1,024-pair attribution registry, and 50,000
completed tasks without intervening diagnostics. Times are one-run medians and
maxima in milliseconds; Stage 3 repeats the identical fixture before broad
qualification.

| State | ParallelFor median / max | Snapshot median / max | Concurrent root admission median / max |
| --- | ---: | ---: | ---: |
| Empty scheduler | 0.0280 / 0.1152 | 0.0085 / 0.0326 | 0.0786 / 0.1535 |
| Full attribution registry | 3.1451 / 3.4668 | 3.1000 / 3.3962 | 2.8662 / 3.0787 |
| Full registry plus completed lifetime | 3.1256 / 55.7599 | 3.1010 / 3.5989 | 2.8680 / 3.2891 |

The 55.7599 ms maximum is the first `ParallelForCancelable` call pruning the
unobserved lifetime history. Stage 3 investigates an empty-cohort
`ParallelForCancelable` median regression above 10 percent and any root
admission stall above 1 ms while a deep snapshot runs. Snapshot construction
may retain its registry-dependent cost, but it must no longer transfer that
cost to admission or Worker-count lookup.

| Retained waiting graph cohort | Debug heap bytes per accepted node |
| --- | ---: |
| Root waiting on a stalled prerequisite | 3,813 |
| Continuation waiting on a stalled predecessor | 2,652 |
| Typed two-input fan-in | 2,321 |
| Unique producer plus consuming sink | 2,130 |

The unique figure divides the pair delta by its two accepted nodes. These
measurements include the current active and lifetime-map bookkeeping and are
capacity-selection evidence, not a stable ABI-size promise.

### Terminal ordering and locks

The terminal winner and future scope hook use this exact order:

1. Under the task-state mutex, store the raw terminal state and terminal-only
   fields, detach the completion hook and direct dependents, then release the
   task-state mutex.
2. Publish or discard typed result storage and release pending callable storage
   with no scheduler, task-state, Worker-queue, or GameThread-queue lock held.
3. Remove the task from shared cancellation tracking without holding the
   task-state mutex.
4. Under the task-state mutex, charge the fixed lifetime task/result counters
   once and set `bTerminalPublicationFinished` as the final visibility write;
   release the mutex and notify waiters.
5. Notify every copied direct dependent with no predecessor lock held. A
   dependent may become terminal or dispatch using its already-owned process
   reservation; no second capacity decision occurs.
6. Enter the scheduler mutex, then the optional scope mutex, balance the scope
   terminal count, remove the active-node entry, and release the process task
   reservation exactly once. Quiescence notification follows the balanced
   counters.

The global order remains global scheduler lifetime mutex, scheduler mutex,
scope mutex, then task-state mutex. Terminal publication never holds the
task-state mutex while entering cancellation, scheduler, scope, or executor
state. Deep diagnostics may pin/copy at each level in that order but must
release the scheduler/scope lock before calling task diagnostics. The final
terminal hook is the sole scope and process-reservation release site.

## Implementation Stages

### Stage 0: Freeze capacity configuration and regression baselines

Dependencies: audit baseline `97e046e9`; no task-system implementation change.

- [x] Measure retained bytes for representative waiting roots, continuations,
  fan-in nodes, and unique sinks, then select the finite default maximum
  nonterminal count and a small deterministic test override.
- [x] Freeze the scheduler configuration/API shape, initialization validation,
  capacity-exhaustion terminal reason, public diagnostic fields, and source
  compatibility behavior for `InitializeTaskScheduler(uint32)`.
- [x] Record baseline latency for `ParallelForCancelable`, a full diagnostic
  snapshot, and concurrent root admission under an empty scheduler, a full
  attribution registry, and a large completed lifetime without intervening
  diagnostics.
- [x] Record the exact terminal-publication ordering and lock order shared by
  capacity release, live terminal accounting, dependent notification, and the
  future scope release hook.
- [x] Replace the stale `CoreTests` native-test examples in `BuildAndRun.md`
  with `CoreConcurrencyTests`, preserving general `all` examples.

#### Acceptance Gate

- The finite default, override mechanism, rejection semantics, counters, lock
  order, and performance comparison cohort are explicit and testable.
- The documented focused Core command resolves to a maintained target.

#### Stage 0 Handoff

- Baseline commit: `1aadfcc0` (`docs(tasks): plan task system audit
  remediation`). No reviewed task implementation changed after the central
  task-system baseline `97e046e9`.
- Working set: Stage 1 writes `Task.h`, `Task.cpp`, and
  `ThreadingTests.cpp`. `BuildAndRun.md` is complete for this plan unless later
  validation discovers another stale example.
- Key symbols: `FTaskStateData::PublishTerminalLocked` owns the raw winner;
  `FinishTerminalPublication` owns barrier publication, direct dependent
  propagation, and the final balanced release; `FTaskScheduler::OnTaskTerminal`
  moves to that final hook; `AllTasks` is replaced by fixed lifetime counters.
- Decisions: 16,384 default reservations, capacity-8 focused override,
  `FTaskSchedulerConfig`, distinct `CapacityExhausted` evidence, public
  reservation diagnostics, the six-step terminal order, and the 1 ms admission
  stall threshold are frozen in `Stage 0 Frozen Contract`.
- Open questions: none for Stage 1. Stage 1 must preserve the structured-scope
  lock order and leave capacity enforcement itself to Stage 2.
- Validation: the manual Stage 0 benchmark passed on
  `CoreConcurrencyTests`; the target resolved and produced the recorded memory
  and latency cohort. The benchmark remains disabled for explicit reruns.

### Stage 1: Make terminal publication and lifetime accounting coherent

Dependencies: Stage 0 configuration and ordering decisions.

- [x] Make `FTaskStateData::GetDiagnostics` derive its visible state and every
  terminal-only field from `bTerminalPublicationFinished` as one locked copy.
- [x] Move active-node release to the frozen end-of-publication hook without
  changing failure/cancellation precedence or shutdown quiescence.
- [x] Replace `AllTasks` with fixed scheduler-lifetime terminal task/result
  counters balanced by publication and task-state destruction.
- [x] Preserve stopped-lifetime diagnostics across scheduler restart without
  retaining individual task state or mixing lifetime counters.
- [x] Add a test-only completion barrier and deterministic tests for raw
  terminal transition versus result publication, task diagnostics, scheduler
  nonterminal snapshots, dependents, and state destruction.
- [x] Add an observation-free soak that completes a large task population,
  never calls scheduler diagnostics during the run, then proves tracker
  cardinality remains fixed and the first later snapshot is not lifetime-linear.

#### Acceptance Gate

- No scheduler-owned container grows with completed lifetime work.
- A terminal state, reason, finish time, and result-storage status become
  visible together only after completion publication succeeds or discards.
- Current and final retained terminal task/result counts balance to surviving
  state lifetimes without scanning task ids.

#### Stage 1 Handoff

- Baseline commit: `1aadfcc0` (`docs(tasks): plan task system audit
  remediation`). Stages 0-4 land together in the squashed completion commit.
- Working set: Stage 2 continues in `Task.h`, `Task.cpp`, and
  `ThreadingTests.cpp`, plus this plan for status and handoff. No additional
  dependency was required in Stage 1.
- Key symbols: `FTaskStateData::GetDiagnostics` and `GetDiagnostic` gate raw
  terminal fields on `bTerminalPublicationFinished`;
  `FTaskStateData::FinishTerminalPublication` charges
  `FTaskSchedulerLifetimeAccounting`, publishes the barrier, propagates direct
  dependents, then calls `FTaskScheduler::OnTaskTerminal` as the final release;
  `FTaskStateData::~FTaskStateData` balances the lifetime counters.
- Decisions: scheduler diagnostics copy active task owners under the scheduler
  mutex and resolve their task snapshots after releasing it; stopped
  diagnostics pin only the prior lifetime's two-counter accounting object;
  restart diagnostics use only the new scheduler lifetime. The native-test
  hook has an atomic disabled fast path and is invoked before completion/result
  publication on success, failure, and pre-execution cancellation paths.
- Open questions: none for Stage 2. Reservation release must attach to the
  existing final `OnTaskTerminal` call and must not move it earlier than direct
  dependent propagation.
- Validation: `FTaskDiagnosticsTests.*` passed 4/4, all task-system tests passed
  48/48, and the complete `CoreConcurrencyTests` binary passed 103/103 on
  `Win64-Debug-DurinEditor-Tests`. The observation-free 1,000 versus 50,000
  task first-snapshot comparison passed, and retained task/result counters
  balanced through publication, destruction, stopped state, and restart.

### Stage 2: Enforce scheduler-wide nonterminal admission

Dependencies: Stage 1 provides the one final reservation-release hook.

- [x] Add validated scheduler capacity configuration and current/peak/capacity-
  rejection diagnostics while preserving the existing initialization entry
  point.
- [x] Reserve before node construction in `FTaskScheduler::Submit`; release
  exactly once at the Stage 1 terminal completion hook.
- [x] Ensure every task form uses the same reservation and that accepted
  waiting nodes dispatch without a second capacity decision.
- [x] Add small-capacity tests for roots, stalled prerequisites, continuations,
  typed fan-in, unique claim rollback, parallel-for chunks, GameThread deferred
  work, multiple producers, capacity reuse, and both shutdown modes.
- [x] Prove callable/result destruction occurs outside scheduler, task-state,
  Worker-queue, and GameThread-queue locks on every capacity rejection path.

#### Acceptance Gate

- Current nonterminal reservations never exceed configured capacity under
  concurrent producers, waiting graphs, continuation release, or shutdown.
- Saturation rejects deterministically without leaks, hidden graph nodes,
  claim loss, deadlock, or failure of already-accepted internal dispatch.
- Reservation counts return to zero after drain and cancel shutdown.

#### Stage 2 Handoff

- Baseline commit: `1aadfcc0` (`docs(tasks): plan task system audit
  remediation`). Stages 0-4 land together in the squashed completion commit.
- Working set: Stage 3 continues in `Task.h`, `Task.cpp`, and
  `ThreadingTests.cpp`, plus this plan for status and handoff. No additional
  dependency was required in Stage 2.
- Key symbols: `FTaskSchedulerConfig` preserves the legacy initialization API
  with a default capacity of 16,384; `FTaskScheduler::Submit` validates state
  and prerequisites before reserving and constructing a node;
  `FTaskScheduler::OnTaskTerminal` performs the single balanced release;
  `FTaskSchedulerDiagnostics` and `FTaskOwnerCategoryDiagnostics` expose the
  frozen capacity and rejection fields.
- Decisions: capacity is validated to the nonzero `uint32` range and enforced
  under the scheduler mutex. Rejection records `CapacityExhausted` aggregate
  evidence without constructing a hidden task state. Callable and result-owner
  closures remain caller-owned until both global and scheduler admission locks
  have unwound; capacity rejection never enters Worker or GameThread queue
  locks. Accepted waiting nodes proceed directly to their target queue without
  another capacity check.
- Open questions: none for Stage 3. Its shallow runtime query can consume the
  immutable Worker count and running state without changing reservation
  ownership or release ordering.
- Validation: focused capacity tests passed 4/4, all task-system tests passed
  52/52, and the complete `CoreConcurrencyTests` binary passed 107/107 on
  `Win64-Debug-DurinEditor-Tests`. Capacity-8 tests covered concurrent roots,
  stalled graphs, continuations, typed fan-in, unique claim rollback,
  `ParallelFor`, GameThread deferred work, reuse, drain, and cancel; current
  reservations returned to zero and peak reservations never exceeded eight.

### Stage 3: Decouple runtime queries from diagnostic reporting

Dependencies: Stage 1 removes history traversal; Stage 2 exposes capacity
state used by the snapshot.

- [x] Add a lightweight internal running/Worker-count query and switch
  `ParallelForCancelable` to it.
- [x] Pin scheduler lifetime under `GTaskSchedulerMutex`, release it, and build
  the scheduler snapshot from a bounded active-node cohort without holding the
  global lock or nesting scheduler and task-state locks.
- [x] Separate fixed profiler aggregate plot publication from diagnostic
  getters and attach it to the profiling/frame update boundary selected in
  Stage 0.
- [x] Add deterministic contention tests in which diagnostics overlap root and
  continuation admission, terminal publication, and shutdown.
- [x] Repeat the Stage 0 latency cohort and investigate any `ParallelFor`
  regression above 10 percent or admission stall above the frozen threshold.

#### Acceptance Gate

- `ParallelForCancelable` performs no diagnostic/reporting work to choose its
  chunk count.
- Deep scheduler snapshots do not monopolize global admission and never emit
  profiler plots as an observation side effect.
- Concurrent snapshots contain no barrier-visible terminal entry in
  `NonterminalTasks` and remain lifetime-safe through shutdown.

#### Stage 3 Handoff

- Baseline commit: `1aadfcc0` (`docs(tasks): plan task system audit
  remediation`). Stages 0-4 land together in the squashed completion commit.
- Working set: Stage 4 continues from `Task.h`, `Task.cpp`,
  `ThreadingTests.cpp`, and this plan. Stage 3 also added the direct frame
  boundary dependency `LaunchEngineLoop.cpp`; Stage 4 expands to the contract,
  audit, build documentation, and structured-scope plan named in its checklist.
- Key symbols: `FTaskScheduler::GetWorkerCount` supplies the immutable shallow
  query; `GetTaskSchedulerDiagnostics` pins the live scheduler or stopped
  snapshot under `GTaskSchedulerMutex` and performs all deep copying after
  release; `FTaskScheduler::GetDiagnostics` resolves a strong active-node
  cohort outside its mutex; `PublishTaskSchedulerProfilerPlots` publishes
  fixed aggregates explicitly from `FEngineLoop::Tick`.
- Decisions: stopped snapshots are immutable shared snapshots so their vector
  and string copies also occur outside the global lifetime lock. Diagnostic
  getters never emit profiler plots. A deterministic native-test hook pauses
  only after the active cohort is pinned and both lifetime/scheduler locks are
  released, allowing admission, terminal publication, and shutdown to prove
  forward progress while the deep snapshot remains outstanding.
- Open questions: none for Stage 4. Profiling-enabled/disabled qualification
  should verify the explicit frame publication path without changing its
  ownership boundary.
- Validation: the new contention test passed, all task-system tests passed
  53/53, the complete `CoreConcurrencyTests` binary passed 108/108, and the
  `Launch` target built on `Win64-Debug-DurinEditor-Tests`. The corrected
  capacity-100,000 manual fixture passed: empty `ParallelFor` median was
  0.0139 ms versus the Stage 0 0.0280 ms baseline, and the maximum concurrent
  root-admission latency across empty, full-registry, and large-completed-
  lifetime cohorts was 0.634 ms, below the frozen 1 ms threshold.

### Stage 4: Integrate, qualify, and close the audit

Dependencies: Stages 1-3 complete and stable.

- [x] Run the focused Core concurrency tests, task-focused filters, full native
  aggregate, profiling-disabled/enabled configurations, complete `all` build,
  and hidden-window lifecycle smoke through the repository build contract.
- [x] Run saturation and soak qualification without diagnostics in the hot
  loop, then query final counters and reconcile accepted, rejected, active,
  completed, failed, and canceled totals.
- [x] Update the CPU Task System contract with the finite admission bound,
  terminal accounting lifetime, snapshot locking boundary, and explicit
  profiler-publication behavior.
- [x] Update Structured Task Scopes to the resulting configuration, terminal
  release hook, and lock order; resume its Stage 1 only after this plan passes.
- [x] Record resolution in the completed-task audit, remove the investigation
  and its open-index entry according to the investigation lifecycle, and
  complete/archive this plan through the plan lifecycle.

#### Acceptance Gate

- Every resolution boundary in the completed-task audit has direct test,
  benchmark, build, or documentation evidence.
- Broad production adoption can rely on a finite process task-graph bound,
  observation-independent metadata bounds, coherent diagnostics, and a valid
  focused test command.

#### Stage 4 Handoff

- Baseline commit: `1aadfcc0` (`docs(tasks): plan task system audit
  remediation`). Stages 0-4 land together in the squashed completion commit.
- Working set: `ThreadingTests.cpp`, the CPU Task System and CPU Profiling
  contracts, this plan, `StructuredTaskScopes.md`, and the investigation index;
  the resolved `CompletedTaskSystemAudit.md` investigation was removed.
- Key symbols and decisions: `FTaskSchedulerConfig::MaxNonterminalTasks` remains
  the authoritative process bound; `FTaskStateData::FinishTerminalPublication`
  opens the barrier and propagates direct dependents before the final
  `FTaskScheduler::OnTaskTerminal` release; deep diagnostics use pin-copy-release
  locking; `FEngineLoop::Tick` explicitly publishes fixed profiler plots.
  Structured scopes reuse these boundaries and do not add a parallel process
  capacity tracker.
- Open questions: none. Structured Task Scopes Stage 1 is ready.
- Validation: the observation-free 32-round saturation soak passed and
  reconciled 2,048 accepted, 512 rejected, zero active, 2,048 completed, 256
  failed, and 768 canceled tasks. Task-focused tests passed 54/54, complete
  `CoreConcurrencyTests` passed 109/109, and the full native aggregate passed.
  Complete `all` builds passed for `Win64-Debug-DurinEditor-Tests` (Tracy off)
  and `Win64-Release-DurinEditor-Profiling` (Tracy on). The hidden-window
  lifecycle smoke exited normally and reported 29 completed, one failed, two
  canceled, and one rejected task.

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

- [Structured Task Scopes](StructuredTaskScopes.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Task System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/TaskSystemEvolution.md)
- [Implementation Plan Rules](../../AGENTS.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Core/Private/Threading/QueuedThreadPool.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
