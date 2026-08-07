# Task Owner Diagnostics Plan

Summary: Attribute bounded task work, storage, terminal outcomes, and latency distributions to stable owners and categories that correlate with profiler events.

Last reviewed: 2026-08-07

Status: Archived
Completed: 2026-08-07

## Current Status

All stages are complete from the Stage 4 baseline commit `f8df7856`. Bounded
task attribution, per-task storage and duration, fixed owner/category
aggregates, Tracy correlation, and the AssetImport and source-image thumbnail
pilots passed focused, saturated, full-native, full-build, and lifecycle
validation. Stable rules now live in the CPU Task System and profiling
contracts. The roadmap records M3 complete, defers M5 for missing named blocking
occupancy evidence, and opens M4 Structured Task Scopes after independently
verifying two production-owner shutdown boundaries.

## Goal

Make task-system cost attributable without retaining task history. A developer
must be able to identify which bounded owner/category pair admitted work, how
long that work waited and executed, which terminal or rejection paths it hit,
and how much callable, payload, and result storage it retained. The same task id
and attribution must be visible in per-task snapshots, bounded aggregate
snapshots, and profiler events.

## Scope

- Process-lifetime registration of bounded owner/category identities.
- Explicit attribution in launch, continuation, unique-result, fan-in, and
  parallel-for options, with deterministic default inheritance.
- Per-task callable-storage, execution-duration, and attribution diagnostics.
- Per-owner/category counters, gauges, peaks, and fixed logarithmic
  distributions for queue latency, execution, and declared/retained bytes.
- Attribution of admission rejection, dispatch rejection, stale generation,
  supersession, cancellation, callback failure, and shutdown paths.
- Profiler correlation across enqueue, dispatch/start, and terminal phases.
- Production pilots in AsyncImportCore and the source-image thumbnail cache.
- Focused concurrency, saturation, restart, profiler-configuration, and
  lifecycle validation.

## Non-Goals

- Changing task order, priority, executor selection, queue bounds, or
  cancellation semantics.
- Adaptive scheduling, automatic throttling, Worker priorities, work stealing,
  a dedicated IO executor, or serialized lanes.
- Structured scope admission, descendant cancellation, or scope quiescence.
- Persisting metrics across process launches or serializing attribution ids.
- Retaining completed task records, unbounded task names, stack traces, or raw
  timing samples.
- Treating dynamic asset paths, object ids, request serials, or resource names
  as categories.
- Replacing existing GameThread queue diagnostics, task diagnostics, logging,
  or Tracy with a new telemetry service.

## Design Decisions and Invariants

### Bounded attribution identity

Core exposes an opaque, trivially copyable `FTaskAttribution` token returned by:

```cpp
auto RegisterTaskAttribution(
    std::string_view Owner,
    std::string_view Category) -> FTaskAttribution;
```

The process registry has these fixed rules:

- At most 256 distinct owners and 1024 distinct owner/category pairs exist,
  including reserved `Unattributed` and `Overflow` entries.
- Owner and category labels are non-empty UTF-8 strings of at most 63 bytes.
  The registry copies accepted labels into fixed-capacity storage.
- Re-registering the same exact pair returns the same token under concurrency.
- Invalid labels or exhausted capacity return `Overflow` and increment a
  bounded registration-overflow counter; they do not reject task work.
- Tokens are process-local diagnostic identity. They are never serialized,
  persisted, used as resource identity, or compared across process launches.
- Registration is process-lifetime, while aggregate measurements reset with
  each scheduler lifetime. Restarting the scheduler preserves tokens but not
  prior counters.
- Production callers register stable low-cardinality literals. Dynamic
  per-request, per-file, per-object, and per-resource labels are unsupported.

The task hot path stores only fixed-width owner and category ids. It does not
copy labels, intern arbitrary `FName` values, or grow a map when a task starts or
finishes. Diagnostic snapshots resolve ids to owned strings only when queried.

### Attribution placement and inheritance

`FTaskLaunchOptions`, `FTaskContinuationOptions`, and `FParallelForOptions`
gain an optional `Attribution` field. Existing aggregate initialization and
default construction remain source compatible.

Attribution is selected once at admission and never changes:

1. An explicit non-default token wins.
2. A continuation, consuming sink, or typed fan-in with no explicit token
   inherits its primary predecessor's attribution.
3. A root task launched while another task is executing inherits the executing
   task's attribution.
4. A root launched outside a task is `Unattributed`.

Additional prerequisites never select or merge attribution. Cross-owner graph
edges are legal and observable through existing prerequisite ids. A caller that
hands work to another subsystem supplies that subsystem's explicit token.
Unique producers, consuming sinks, shared continuations, and fan-in nodes follow
the same rules; result ownership mode does not imply owner identity.

Parallel-for logical work and every scheduled chunk use the token selected for
the operation. Stage 0 audits the current serial fallback and chunk-launch path
so aggregate counts distinguish one logical operation from task-node counts
without double reporting.

### Per-task diagnostics and callable bytes

`FTaskDiagnostics` adds attribution ids and resolved labels plus:

- `CallableStorageBytes`: the bytes retained by the erased pending callable,
  including inline wrapper storage or actual heap allocation, but excluding
  separately declared payload/result bytes;
- `ExecutionNanoseconds`: zero before start, elapsed-so-far while running, and
  the frozen finish-minus-start duration after terminal publication.

The move-only callable wrapper exposes a non-allocating storage-size query.
Moving, rejecting, canceling, or destroying a callable transfers or releases
the same byte charge exactly once. Callable sizing must not invoke or inspect
user state and must not add an allocation to an inline callable.

Existing timestamps, `EstimatedPayloadBytes`, `EstimatedResultBytes`, and
`RetainedResultBytes` remain authoritative. M3 does not infer the heap capacity
of arbitrary user containers or relabel estimates as measured allocation.

### Aggregate snapshot

`FTaskSchedulerDiagnostics` gains a bounded collection of
`FTaskOwnerCategoryDiagnostics` plus attribution-registry overflow counts. A
pair snapshot contains:

- accepted, succeeded, failed, canceled, and rejected counts;
- terminal-reason counts for dependency failure/cancellation, explicit
  cancellation, dispatch rejection, supersession, stale generation, callback
  failure, and shutdown cancellation;
- current waiting, queued, running, and nonterminal counts;
- current and peak callable bytes, declared payload bytes, declared result
  bytes, and retained unique-result bytes;
- queue-residency, execution-duration, callable-byte, payload-byte, and
  result-byte histograms.

Histograms contain 32 fixed `uint64` buckets. Bucket 0 represents value zero;
a positive value uses
`min(31, 1 + floor(log2(value)))`. Time histograms use nanoseconds and byte
histograms use bytes. Consumers may derive approximate percentiles, but Core
does not retain samples or promise percentile accuracy beyond those buckets.

Only registered pair slots can own aggregates, so completed work cannot grow
the snapshot cardinality. Snapshot vectors allocate only during an explicit
diagnostic query and contain at most 1024 entries. Terminal accounting happens
once at the winning terminal transition; querying diagnostics never mutates
counters.

### Admission, rejection, and executor accounting

- Accepted counts and current gauges begin only after scheduler admission
  creates a node.
- Rejections before node creation use the requested or inherited attribution
  and increment `RejectedCount` without incrementing `AcceptedCount`.
- GameThread count, per-entry payload, total-payload, missing-executor, and
  closing-executor rejections retain existing public terminal reasons and queue
  counters. Internal reason-specific accounting attributes their budget and
  dispatch rejection events without changing task state semantics.
- Superseded and stale-generation nodes count as canceled once and increment
  their corresponding reason counter once.
- A retained unique result moves its byte charge from producer to consuming
  sink according to existing claim/dispatch state; the aggregate total must
  reconcile with global `RetainedUniqueResultBytes`.
- Aggregate updates never hold task-state and scheduler/queue locks in opposite
  order. No user callable move, destruction, label registration, formatting,
  or profiler call occurs while an internal queue mutex is held.

Global counters remain for compatibility and must equal the sum of all
owner/category buckets for the measurements they share.

### Profiler correlation

Core owns a task-profiler adapter in the existing profiling domain. With Tracy
disabled it compiles to no-ops and introduces no formatting or allocation.
With Tracy enabled it emits enqueue, execution, and terminal annotations that
carry the same nonzero `TaskId`, owner label, category label, target, and
terminal reason found in diagnostics. Execution retains a CPU zone named by
the existing bounded task debug name; task id and attribution are attached as
zone text rather than creating one profiler source location per task.

Tracy support is audited in Stage 0. If the pinned Tracy version lacks a native
cross-thread flow primitive, correlation uses the stable task id in enqueue
messages and execution-zone text; the implementation must not emulate arrows
with retained task history. Bounded owner/category plots may expose queue,
execution, rejection, and byte gauges, but plot names come only from registered
slots.

Profiler instrumentation observes state after the winning transition and never
controls scheduling. A profiling failure or disabled capture cannot affect task
state.

### Production pilots

- AsyncImportCore registers owner `AssetImport` with categories `PreparePlan`
  and `PublishPlan`. The unique producer and outcome sink remain under their
  existing request table, cancellation, mailbox, and drain policy.
- The source-image thumbnail cache registers owner `SourceImageThumbnail` with
  category `Decode`. Its cache, request serial, disk cache, decode concurrency,
  GameThread upload, and render/RHI boundaries remain unchanged.

These pilots provide unique-result bytes, Worker execution, explicit
cancellation, and cache-driven load. They do not migrate mailbox or resource
ownership and do not claim M4 structured-scope readiness.

## Current Foundations and Gaps

- `FTaskDiagnostics` already snapshots task ids, prerequisite relationships,
  queue/start/finish timestamps, target, terminal reason, declared bytes, and
  retained unique-result bytes.
- `FTaskSchedulerDiagnostics` already owns global lifetime counters and bounded
  live/handle-owned queries, but no completed-task history.
- `FTaskStateData::PublishTerminalLocked` provides one winning terminal
  transition, and `OnTaskTerminal` supplies a single aggregate update point.
- GameThread deferred admission already distinguishes count, per-entry payload,
  total payload, supersession, and stale-generation events.
- `TMoveOnlyFunction` distinguishes inline and heap storage but exposes no
  diagnostic byte query.
- The profiling layer currently wraps CPU zones, frames, thread names, and
  program identity only; it has no task-specific adapter.
- AsyncImportCore and source-image thumbnails already provide stable task names
  and explicit lifecycle owners but no general attribution token.

## Stage 0 Frozen Contract

### Identity and public layout

- `FTaskAttribution` is an opaque, default-constructible four-byte token holding
  two `uint16` ids. `OwnerId == 0, CategoryId == 0` is `Unattributed`;
  `OwnerId == 1, CategoryId == 1` is `Overflow`. Registered owner ids and
  globally unique owner/category slot ids begin at 2. The public trait contract
  is trivially copyable, standard-layout, and equality comparable; ids are
  exposed through diagnostics rather than token accessors.
- The registry owns 256 owner slots and 1024 pair slots including the two
  reserved entries. Labels must contain 1-63 UTF-8 bytes, with embedded NUL and
  malformed UTF-8 rejected to `Overflow`. Duplicate exact byte pairs converge
  under the registry mutex. The registry and its labels live for the process;
  the registry overflow counter is process-lifetime. Scheduler aggregate slots
  reset on every successful `InitializeTaskScheduler`, while tokens remain
  valid.
- The only Stage 3 production registrations are `AssetImport/PreparePlan`,
  `AssetImport/PublishPlan`, and `SourceImageThumbnail/Decode`: two owners and
  three pairs. They leave 252 non-reserved owner slots and 1019 non-reserved
  pair slots after the two reserved entries and therefore do not challenge the
  selected bounds.
- `RegisterTaskAttribution(std::string_view Owner, std::string_view Category)`
  and `FTaskAttribution` are declared in `Threading/Task.h`. New fields are
  appended to `FTaskLaunchOptions`, `FTaskContinuationOptions`, and
  `FParallelForOptions`, in that order, and named `Attribution`. Existing
  aggregate initialization and defaults therefore remain source compatible.
  Durin does not promise binary compatibility between separately built engine
  modules, so the required unified rebuild is the only ABI expectation.
- `FTaskDiagnostics` appends `AttributionOwnerId`, `AttributionCategoryId`,
  `AttributionOwner`, `AttributionCategory`, `CallableStorageBytes`, and
  `ExecutionNanoseconds`. `FTaskSchedulerDiagnostics` appends
  `AttributionRegistrationOverflowCount` and `OwnerCategoryDiagnostics`.
  `FTaskOwnerCategoryDiagnostics` contains the frozen counters, gauges, peaks,
  and five 32-element histograms described by this plan; histogram members are
  named `QueueResidencyHistogram`, `ExecutionHistogram`,
  `CallableBytesHistogram`, `PayloadBytesHistogram`, and
  `ResultBytesHistogram`.
- `TMoveOnlyFunction::GetStorageBytes()` returns zero when empty, `InlineSize`
  for an inline target, and the concrete target `sizeof(F)` for a heap target.
  The erased operations table stores that size, so querying is `noexcept`, does
  not inspect user state, and does not allocate. Moves preserve the value and
  clear the source.

### Attribution selection and parallel-for accounting

- Attribution is resolved before scheduler or queue locks are taken. An
  explicit non-default token wins; otherwise `LaunchContinuationTask` inherits
  its primary predecessor, and root submission inherits `GCurrentTaskState`
  only when it belongs to the same scheduler. All other launches select
  `Unattributed`. Additional prerequisites never participate in selection.
- Unique producers use `FTaskLaunchOptions`; consuming sinks and typed fan-in
  use their primary predecessor plus `FTaskContinuationOptions`. No additional
  overload or result-ownership rule is required.
- `ParallelForCancelable` selects one attribution before deciding serial versus
  parallel execution. The caller-executed chunk represents the one logical
  operation but creates no task node. Each worker chunk receives the selected
  token explicitly through its existing `FTaskLaunchOptions` and contributes
  ordinary node counts. Aggregate snapshots expose `ParallelForOperationCount`
  separately from accepted task-node counts; it increments once after nonempty,
  non-precanceled argument validation, including nested serial fallback, and
  has no callable/payload/result gauge.

### Accounting sites

| Measurement | Sole update site and balancing path |
| --- | --- |
| Pre-node rejection | Resolve attribution at the public/private launch boundary, then increment `RejectedCount` in one `RecordRejectedTask(Attribution, Reason)` call for empty callables, invalid result-byte declarations, missing/stopped scheduler, closed admission, invalid/cross-lifetime prerequisites, and invalid fan-in/unique-consumer admission. It never increments `AcceptedCount` or gauges. |
| Accepted and nonterminal | `FTaskScheduler::Submit` increments after the node is inserted into `ActiveTasks`; `FTaskScheduler::OnTaskTerminal` decrements once after the winning publication. Submission rollback before insertion is a rejection; there is no rollback after insertion. |
| Waiting and queued | Initial state is charged at accepted insertion. `FTaskStateData::OnPrerequisiteTerminal` transfers waiting to queued under the state lock. `TryMarkRunning` transfers queued to running. `OnTaskTerminal`, using frozen `StateBeforeTerminal`, removes whichever preterminal gauge won. |
| Running and execution | `TryMarkRunning` increments running and freezes start time. The winning `PublishTerminalLocked` freezes finish time; `OnTaskTerminal` decrements running and records `FinishTimeNanoseconds - StartTimeNanoseconds`. A running cancellation remains charged until the callable returns and wins terminal publication. |
| Callable bytes | Charge the accepted node once in `Submit` from `GetStorageBytes`. Moves from node to worker closure, deferred entry, or execute-local owner transfer the charge without changing it. `OnTaskTerminal` releases it once for success, failure, queued/waiting cancellation, dispatch rejection, stale generation, supersession, or shutdown. Rejected pre-node callables contribute only to rejection and byte histograms, never a current gauge. |
| Declared payload/result bytes | Charge once with accepted insertion and release once in `OnTaskTerminal`. Their histograms record once at admission; zero uses bucket 0. GameThread reservation bytes remain queue-local compatibility diagnostics and do not create a second owner/category charge. |
| Queue residency | `TakeFunctionForQueue` freezes dispatch time. The successful `TryMarkRunning` records `StartTimeNanoseconds - DispatchTimeNanoseconds` once. Nodes canceled before start do not add a queue-latency sample. |
| Terminal outcome/reason | `PublishTerminalLocked` remains the only winning state/reason transition. `OnTaskTerminal` reads the frozen snapshot and increments exactly one succeeded/failed/canceled counter and at most one reason counter. Dependency failure/cancellation, explicit cancellation, dispatch rejection, supersession, stale generation, callback failure, and shutdown cancellation therefore cannot double count. |
| Dispatch/budget rejection | Worker-pool enqueue failure and missing deferred executor call `RecordRejectedTask` once and cancel the already accepted node with `DispatchRejected`. Deferred per-entry/count/total-payload/closing rejection returns one internal reason from `FGameThreadDeferredWorkQueue::Enqueue`, then follows the same path. Accepted and rejected both increment because admission succeeded but dispatch failed. |
| Supersession/stale/shutdown | Replacement selection and reservation release stay in the deferred queue lock; cancellation and owner accounting occur after unlock. `RequestCancellation` supplies `Superseded`, `StaleGeneration`, or `ShutdownCanceled`; only its winning terminal transition is counted. Queue reservation release remains guarded by `bReserved`. |
| Retained unique-result bytes | `TUniqueTaskResultState` continues to call the producer and consumer setters. `SetRetainedResultBytes` changes owner gauges by atomic delta. Claim dispatch sets producer to zero before assigning the same bytes to the sink; completion, failed dispatch, duplicate claim, cancellation, and handle destruction end at zero. The sum of slots must equal global `RetainedUniqueResultBytes`. |
| Restart | A successful `InitializeTaskScheduler` constructs zeroed aggregate slots for all registered pairs and clears the last scheduler snapshot. Registration labels/tokens and the process overflow count remain. Shutdown freezes the final lifetime snapshot without retaining completed nodes. |

All aggregate mutations use atomics or occur after releasing task-state and
deferred-queue locks. Snapshot conversion may lock the registry and allocate
its bounded vector, but no hot-path update registers labels, formats text,
destroys a callable, or invokes profiler code while holding an internal queue
mutex.

### Profiler contract

The pinned Tracy client is `v0.13.1`. Its available primitives are
`TracyMessage(txt, size)`, `ZoneScopedN(Name)` plus `ZoneText(txt, size)`, and
`TracyPlot(name, value)`. `TracyFiberEnter/Leave` models fiber execution context,
not a cross-thread task flow, and is not used. No suitable native cross-thread
flow primitive is present in the pinned public API.

Stage 3 therefore emits bounded enqueue and terminal messages containing the
same task id, owner/category labels, target, and terminal reason; execution uses
the existing bounded debug-name CPU zone and one zone-text annotation with the
same fields. Selected aggregate plots use names prebuilt once per registered
slot. Every adapter entry is compiled under `#if DURIN_WITH_TRACY`; the disabled
inline functions are empty and accept fixed-width ids/enums only, so disabled
builds have no formatting, allocation, label resolution, or Tracy call path.

### Qualification baseline

Baseline commit `4cdb7592`, preset `Win64-Debug-DurinEditor-Tests`, Tracy off,
target `CoreConcurrencyTests`, five identical runs on 2026-08-07. The filter
combined the existing ownership/no-op callable, heterogeneous fan-in,
representative deferred-dispatch, and diagnostic snapshot fixtures. All 20 test
executions passed; Ninja reported no work before each run.

| Measurement | Five samples | Median |
| --- | --- | --- |
| Copyable/no-op callable, 128 tasks (ns) | 15,887,900; 16,773,500; 16,271,900; 15,990,900; 15,602,900 | 15,990,900 |
| Move-only callable, 128 tasks (ns) | 15,798,100; 15,713,700; 15,267,500; 15,263,400; 15,129,300 | 15,267,500 |
| Shared transfer, 32 x 64 KiB (ns) | 8,545,300; 9,157,100; 7,953,400; 8,114,200; 7,714,500 | 8,114,200 |
| Unique transfer, 32 x 64 KiB (ns) | 8,793,700; 7,569,500; 8,477,400; 8,625,000; 8,489,700 | 8,489,700 |
| Heterogeneous fan-in fixture (ms) | 1; 1; 1; 1; 1 | 1 |
| Diagnostic snapshot fixture (ms) | 118; 117; 115; 112; 110 | 115 |
| Deferred admission, 256 callbacks (ns) | 21,411,800; 23,303,400; 21,499,800; 21,701,700; 20,960,400 | 21,499,800 |
| Deferred pump (ns) | 9,907,700; 9,448,700; 9,811,200; 9,976,900; 9,751,000 | 9,811,200 |
| Deferred average residency (ns) | 19,853,895; 20,141,196; 19,855,422; 20,210,937; 19,353,808 | 19,855,422 |
| Four-fixture process total (ms) | 205; 206; 201; 198; 195 | 201 |

Raw test logs are the five `CoreConcurrencyTests` logs timestamped
`20260807-154804` through `20260807-154808` under
`Build/.agent-state/logs/`. Stage 4 must use the same preset, target, filter,
fixture sizes, and five-run median rule.

### Stage 1 working set

The initial audit was limited to `Task.h`, `Task.cpp`, `Profiling.h`,
`MoveOnlyFunction.h`, and `ThreadingTests.cpp`. Direct evidence required
expanding only to the pinned Tracy public header and the two named production
pilot launch sites. Stage 1 writes only:

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Core/Public/Templates/MoveOnlyFunction.h`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`

`Profiling.h` and the two pilot files remain Stage 3 work. No additional Stage 1
direct dependency was discovered.

## Implementation Stages

### Stage 0: Freeze accounting sites and qualification baselines

Dependencies: completed M1 ownership and M2 typed fan-in milestones; baseline
commit `4cdb7592`.

- [x] Inventory every task-node admission, pre-node rejection, dispatch,
  execution start/finish, terminal, supersession, stale-generation, unique-byte
  transfer, scheduler restart, and parallel-for chunk path.
- [x] Confirm the 256-owner, 1024-pair, 63-byte label limits and reserved token
  values against production caller cardinality.
- [x] Freeze exact public type/member names, initialization order, ABI/source
  compatibility expectations, and overflow diagnostics.
- [x] Audit the pinned Tracy API and record the exact enqueue/start/finish
  correlation primitives used by Tracy-on and no-op builds.
- [x] Measure the current no-op task, fan-in, deferred dispatch, and diagnostic
  snapshot baselines using existing qualification fixtures.
- [x] Record the Stage 1 working set and any direct dependencies discovered
  outside the five-file initial audit.

#### Acceptance Gate

- Every counter and byte gauge has one named increment/decrement site and a
  defined rollback or terminal path.
- The profiler contract uses only primitives present in the pinned Tracy
  version and remains a no-op when profiling is disabled.
- No identity, cardinality, parallel-for, or compatibility decision remains
  unresolved for Stage 1.

### Stage 1: Add bounded identity and per-task attribution

Dependencies: Stage 0 frozen contract and baseline.

- [x] Implement the fixed-capacity process attribution registry, duplicate
  registration, overflow behavior, snapshots, and scheduler-lifetime reset.
- [x] Add attribution to launch, continuation, fan-in, unique sink, and
  parallel-for option paths with the frozen inheritance precedence.
- [x] Add per-task attribution, callable-storage bytes, and execution-duration
  diagnostics without adding inline-callable allocation.
- [x] Preserve existing option defaults and legacy overload source behavior.
- [x] Add compile-time and focused tests for token traits, defaults, explicit
  override, root/continuation/child inheritance, cross-owner prerequisites,
  concurrent duplicate registration, capacity overflow, and restart.

#### Acceptance Gate

- Every accepted task has exactly one valid attribution token, including the
  reserved `Unattributed` and `Overflow` cases.
- Per-task byte and duration fields agree with callable storage and timestamps
  across success, rejection, cancellation, failure, and shutdown.

#### Stage 1 Handoff

- Entry baseline: `74aaf893` (`docs(tasks): freeze task attribution stage
  zero`). The Stage 1 working set remained the four frozen files: `Task.h`,
  `Task.cpp`, `MoveOnlyFunction.h`, and `ThreadingTests.cpp`.
- Key symbols: `FTaskAttribution`, `RegisterTaskAttribution`,
  `FTaskAttributionRegistry`, `FTaskAttributionAccess`,
  `FTaskStateData::GetDiagnostics`, and
  `TMoveOnlyFunction::GetStorageBytes`.
- The registry stores 256 fixed owner labels and 1024 fixed pair labels, uses
  the reserved `0/0` and `1/1` tokens, validates UTF-8 before locking, and
  resolves or snapshots owned strings only during diagnostic queries.
- Root admission resolves explicit-or-current-task attribution before the
  scheduler lock. Continuations resolve explicit-or-primary-predecessor;
  additional prerequisites do not participate. Parallel-for freezes one token
  before chunk selection and forwards it to every worker chunk. Existing typed
  fan-in and unique-consumer option copies preserve those rules.
- Task nodes freeze callable bytes and attribution at admission. Diagnostic
  execution duration is zero before start, elapsed while running, and frozen
  from the existing start/finish timestamps after terminal publication.
- Validation: the focused attribution fixture passed, followed by the complete
  `CoreConcurrencyTests` target with 99/99 tests passing under
  `Win64-Debug-DurinEditor-Tests`. One intervening full-suite run exposed the
  existing shutdown-test release race and the identical rerun passed 99/99;
  `git diff --check` passed before handoff.
- Stage 2 initially writes `Task.h`, `Task.cpp`, and `ThreadingTests.cpp`.
  `MoveOnlyFunction.h` is an established read-only dependency. No accounting
  or locking question remains open.

### Stage 2: Implement bounded aggregate accounting

Dependencies: Stage 1 attributed task nodes.

- [x] Add fixed aggregate slots, atomic counters/gauges/peaks, 32-bucket
  histograms, and bounded snapshot conversion.
- [x] Account pre-node rejection, accepted lifecycle state, terminal reason,
  stale, superseded, dispatch/budget rejection, and unique-result byte transfer
  exactly once.
- [x] Reconcile shared global counters and retained-byte totals with the sum of
  attribution buckets under normal and saturated load.
- [x] Prove diagnostic queries do not retain completed nodes and scheduler
  restart resets aggregates without invalidating registered tokens.
- [x] Add race tests for simultaneous terminal publication, cancellation,
  supersession, queue saturation, result consumption, and concurrent snapshots.

#### Acceptance Gate

- Owner/category totals deterministically match per-task outcomes and existing
  global diagnostics in focused normal, failure, cancellation, and saturation
  tests.
- Aggregate memory is fixed by registry limits and no completed-task history is
  retained.

#### Stage 2 Handoff

- Entry baseline: `0e94825d` (`feat(tasks): add bounded task attribution`). The
  Stage 2 working set was `Task.h`, `Task.cpp`, `ThreadingTests.cpp`, and this
  plan; `MoveOnlyFunction.h` remained the established read-only dependency.
- Key symbols: `FTaskOwnerCategoryAggregate`, `FTaskAccountingSnapshot`,
  `FTaskScheduler::RecordAcceptedTask`, `OnTaskQueued`, `OnTaskStarted`,
  `OnTaskTerminal`, `OnRetainedResultBytesChanged`, and
  `RecordParallelForOperation`.
- Each scheduler owns 1024 fixed atomic aggregate slots indexed by the process
  registry's globally unique category id. Hot paths never resolve labels or
  allocate; diagnostic queries combine the fixed atomics with the bounded
  registry snapshot.
- Accepted insertion charges the initial state and declared bytes once.
  Waiting-to-queued and queued-to-running transitions transfer gauges, while
  terminal publication releases the frozen preterminal state and bytes before
  waiters observe completion. The winning terminal reason supplies exactly one
  outcome and at most one reason count.
- Retained unique-result setters apply atomic deltas to the producer or sink
  attribution. The scheduler-wide retained total is derived from the same
  slots, so it reconciles without scanning or retaining completed nodes.
  Dispatch rejection increments rejected once in addition to the accepted node
  and lets terminal publication own the reason count.
- Validation: the focused Stage 2 fixture and the combined attribution fixture
  passed, followed by the complete `CoreConcurrencyTests` target with 100/100
  tests passing under `Win64-Debug-DurinEditor-Tests`; `git diff --check`
  passed before the final plan update.
- Stage 3 initially inspects/writes `Task.cpp`, `Profiling.h`,
  `ThreadingTests.cpp`, `AsyncImport.cpp`, and
  `SourceImageThumbnailCache.cpp`. `Task.h` is added only if compilation proves
  the profiler adapter needs a public task declaration. No accounting or
  locking question remains open.

### Stage 3: Correlate profiler events and migrate production pilots

Dependencies: Stage 2 stable attribution and aggregates.

- [x] Add the Core task-profiler adapter and task-id correlation to enqueue,
  execution, and terminal phases with Tracy-on/no-op compilation coverage.
- [x] Expose bounded aggregate plots selected in Stage 0 without dynamic plot
  cardinality.
- [x] Attribute AsyncImportCore prepare/publish tasks without changing mailbox,
  result, cancellation, or drain semantics.
- [x] Attribute source-image thumbnail decode work without changing cache,
  decode/upload throttling, or render/RHI ownership.
- [x] Capture representative normal and saturated profiler evidence showing the
  same task id and owner/category in diagnostics and profiler phases.

#### Acceptance Gate

- Profiler captures correlate enqueue and execution for both pilots and agree
  with per-task and aggregate snapshots.
- Tracy-disabled builds contain no task-profiler formatting path, and pilot
  behavior/tests remain unchanged apart from diagnostics.

#### Stage 3 Handoff

- Entry baseline: `e488f608` (`feat(tasks): aggregate owner diagnostics`). The
  production working set was `Profiling.h`, `Task.cpp`, `AsyncImport.cpp`, and
  `SourceImageThumbnailCache.cpp`; `ThreadingTests.cpp` supplies Tracy-off
  compile coverage. Acceptance validation expanded to the existing
  `AssetImportCoreTests.cpp` and `SourceImageThumbnailTests.cpp` fixtures so
  both pilots assert their exact aggregate owner/category entries.
- Key symbols: `RegisterTaskProfilerAttribution`, `TaskEnqueued`,
  `DURIN_PROFILE_TASK_EXECUTION_ZONE`, `TaskTerminal`, and
  `TaskAggregatePlots`. Tracy-only state is a fixed 1024-slot label/plot cache;
  each registered slot prebuilds seven plot names once. Disabled entry points
  are inline no-ops whose parameters are only fixed-width ids and values.
- Enqueue messages occur after accepted aggregate charging, the execution zone
  begins only after the winning queued-to-running transition, and terminal
  messages occur after the unique aggregate terminal update. Diagnostic-query
  plot publication happens before the scheduler mutex is acquired. No task or
  deferred-queue lock invokes profiler code.
- `AssetImport/PreparePlan`, `AssetImport/PublishPlan`, and
  `SourceImageThumbnail/Decode` are the only production registrations. The
  pilots add only explicit launch/continuation attribution options and preserve
  their existing request tables, cancellation, result/mailbox transfer,
  decode/upload throttling, and render/RHI ownership.
- The profiling capture
  `Build/Profiling/Tracy/TaskOwnerDiagnostics-saturated.tracy` contains 41 task
  messages and 3040 zones across the normal lifecycle and parallel saturation
  cohorts. Official `tracy-csvexport` output correlates executed task ids such
  as 1, 3, and 17 between enqueue messages, `Task.Execute` zone text, and
  terminal messages; ids 3 and 4 retain terminal reasons 7 and 1. The same
  export contains the fixed
  `Tasks.<Owner>.<Category>.{QueueDepth,Running,Rejected,CallableBytes,
  PayloadBytes,ResultBytes,RetainedResultBytes}` plot family. The companion
  `TaskOwnerDiagnostics-normal.tracy` records the five-second 308-frame runtime
  baseline; its late on-demand connection intentionally contains no startup
  task events.
- Validation: Tracy-off Core attribution tests passed, the complete
  `CoreConcurrencyTests` passed 100/100, `AssetImportCoreTests` passed 23/23,
  and `ThumbnailTests` passed 46/46. `Win64-Release-DurinEditor-Profiling`
  compiled Core and both pilots, then completed an `all` build. Pilot assertions
  observe one accepted/succeeded node per expected category with terminal
  gauges at zero. An additional profiling lifecycle smoke was stopped after two
  minutes in the existing immediate-cancel `EngineSmoke.Canceled` wait; no
  process remained and `DevTool status` was clean. Stage 4 must disposition
  that extra runtime result before claiming its required lifecycle smoke.
- Stage 4 begins with the frozen Stage 0 qualification filter and five-run
  median rule, then the focused/full validation and documentation working sets.
  No profiler-cardinality, pilot-ownership, or task-accounting question remains
  open.

### Stage 4: Qualify, document, and close M3

Dependencies: Stage 3 production evidence.

- [x] Compare post-change no-op, fan-in, deferred dispatch, and snapshot
  qualification numbers with Stage 0; investigate and disposition any median
  regression above 10 percent across five identical runs.
- [x] Run focused Core, AssetImportCore, thumbnail/editor workflow, full native
  aggregate, complete `all` build, and hidden-window task lifecycle smoke
  through the repository build contract.
- [x] Validate profiling-disabled and registered profiling-enabled presets
  required by the Stage 0 Tracy audit.
- [x] Move stable attribution, aggregate, boundedness, and profiler rules into
  the CPU Task System and profiling documentation.
- [x] Record M3 completion in the Task System Evolution roadmap, review M5 IO
  evidence, and open M4 only if its two production-owner shutdown-boundary gate
  is independently satisfied.
- [x] Complete the final handoff and plan lifecycle validation.

#### Acceptance Gate

- M3 roadmap exit criteria pass under normal and saturated loads.
- Diagnostics remain bounded, globally reconcilable, profiler-correlated, and
  behavior-neutral for existing task callers.
- Any M5 or M4 activation is evidence-backed and recorded separately; M3 does
  not silently implement either milestone.

#### Stage 4 Handoff

- Entry baseline: `f8df7856` (`feat(tasks): correlate task profiler ownership`).
  The implementation working set added only
  `LaunchEngineLoop.cpp`; stable-document work updated `TaskSystem.md`,
  `Profiling.md`, `TaskSystemEvolution.md`, this plan, and the new
  `StructuredTaskScopes.md` plan.
- The frozen compact five-run cohort passed all 20 executions. Current samples
  were: copyable callable 15,599,800; 14,006,800; 15,396,300; 15,371,100;
  15,319,300 ns; move-only callable 14,240,500; 14,878,100; 16,151,300;
  15,009,200; 15,063,900 ns; shared transfer 9,008,000; 8,147,500;
  8,110,000; 7,901,900; 8,071,700 ns; unique transfer 8,262,700;
  8,337,600; 8,503,600; 8,426,900; 8,168,400 ns. Their medians changed
  -3.88%, -1.69%, -0.05%, and -1.79% from Stage 0.
- Deferred admission, pump, and average-residency medians were 22,195,800 ns,
  9,759,800 ns, and 20,251,606 ns, changing +3.24%, -0.52%, and +2.00%.
  The four-fixture process median was 207 ms, +2.99%. Diagnostic-snapshot
  detailed-output median was 112 ms, -2.61%.
- Heterogeneous fan-in's integer GoogleTest duration appeared as 3 ms versus
  the 1 ms Stage 0 value. Investigation reproduced 4/4/4/4/5 ms when the tiny
  fixture ran alone and identified the new scheduler-lifetime zeroing of 1,024
  fixed aggregate slots as the dominant fixed startup cost, not fan-in task hot
  paths. Full-output Trace streaming also perturbed the callable cohort. The
  fixed startup cost is accepted because direct task/transfer metrics remain
  below baseline and the complete frozen process median increased only 2.99%.
- Focused validation passed `CoreConcurrencyTests` 100/100,
  `AssetImportCoreTests` 23/23, and `ThumbnailTests` 46/46. The complete native
  aggregate passed 1,105 cases with two environment skips and the existing
  disabled benchmark. Both `Win64-Debug-DurinEditor-Tests` and
  `Win64-Release-DurinEditor-Profiling` completed `all` builds.
- The prior profiling lifecycle stall was reproduced and diagnosed with the
  official capture exporter: `EngineSmoke.Canceled` was enqueued but never
  terminal because Release compiled the side-effecting `CancelTask(...)` call
  out of `checkf`. `LaunchEngineLoop.cpp` now evaluates cancellation and the
  admission-probe wait before asserting their results. Three-tick hidden-window
  smokes then passed in Debug and Release Profiling, each reporting 25 completed,
  one failed, two canceled, one rejected, one long wait, and 11 retained
  handles. No runtime process remained and DurinDevTool recovery stayed clean.
- M5 is deferred: captures do not isolate material Worker blocking occupancy in
  a named owner/category or document platform cancellation limits. M4 is open
  because AssetImport explicitly closes/cancels/drains owner/provider work and
  SourceImageThumbnail explicitly closes request admission and result
  publication; the two independent boundaries are recorded in
  `StructuredTaskScopes.md`.
- No M3 accounting, profiler-cardinality, production ownership, lifecycle, or
  validation question remains open. Continue with Stage 0 of Structured Task
  Scopes from the M3 completion commit.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Registration -> token | Exact duplicates converge; invalid/capacity overflow maps to one reserved bucket without rejecting work. |
| Root/child/continuation -> attribution | Explicit override and default inheritance follow the frozen precedence across Worker and GameThread targets. |
| Callable owner -> bytes | Inline, heap, move, rejection, cancellation, terminal, and shutdown paths charge and release exactly once. |
| Task terminal -> aggregate | Success, failure, cancellation, dependency, callback, stale, superseded, dispatch, and shutdown totals match task snapshots. |
| Unique producer -> sink | Retained bytes transfer/release without double counting and reconcile with the global gauge. |
| Queue/start/finish -> histograms | Fixed logarithmic buckets match per-task nanoseconds and remain stable under completion races. |
| Saturation -> owner budget metrics | GameThread count/payload rejection and stale/superseded events reach the correct bounded bucket. |
| Scheduler restart -> diagnostics | Tokens remain valid; aggregate counters and gauges reset; no prior completed task is retained. |
| Task -> profiler | One task id and attribution correlate enqueue, execution, and terminal evidence with Tracy enabled. |
| Profiling disabled -> runtime | Adapter is a no-op with no formatting/allocation path and task behavior is identical. |
| Production pilots | Asset import and source-image thumbnail behavior remains green while their costs become separately attributable. |

## Definition of Done

- Stable, bounded owner/category tokens cover every task API and inherit
  deterministically.
- Per-task callable bytes and execution duration are correct across every
  terminal and rejection path.
- Aggregate counters, gauges, peaks, and distributions remain fixed-cardinality
  and reconcile with existing global diagnostics.
- Profiler evidence correlates task phases and attribution without affecting
  scheduling or disabled builds.
- Both production pilots retain their domain ownership and pass focused tests.
- Stable documentation owns the completed contract, required validation passes,
  and the roadmap records M3 completion plus explicit M4/M5 gate disposition.

## Deferred Follow-ups

- Structured task scopes, owner close/cancel/drain, and descendant quiescence
  remain M4.
- A bounded IO executor remains M5 and requires measured Worker blocking from a
  named owner/category after M3.
- Adaptive scheduling, sampling, external telemetry export, persistent metrics,
  and arbitrary runtime labels require separate plans.
- Per-resource or per-request drill-down remains in subsystem diagnostics and
  must not consume task attribution cardinality.

## Related Documentation

- [Task System Evolution Roadmap](../../../Roadmaps/TaskSystemEvolution.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Profiling](../../../Development/Build/Profiling.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Implementation Plan Rules](../../AGENTS.md)
- [Move-Only Tasks and Consuming Results](MoveOnlyTasksAndConsumingResults.md)
- [Typed Task Fan-In](TypedTaskFanIn.md)
- [Structured Task Scopes](../../StructuredTaskScopes.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Core/Public/Profiling/Profiling.h`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
