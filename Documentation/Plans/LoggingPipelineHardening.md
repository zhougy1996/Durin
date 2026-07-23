# Logging Pipeline Hardening Plan

Last reviewed: 2026-07-23

## Current Status

Stages 0 through 4 are complete. The logger now owns bounded structured session history, exposes the frozen sequence-cursor read contract, retains the default 5,000-record bootstrap window, reports bootstrap overflow explicitly, and resets history and sequence state across lifecycle sessions. Reliable Error and Fatal completion is published after active sinks have been attempted and intentionally flushed. The editor Console polls retained history by sequence cursor, continues consuming while hidden, keeps one combined bounded record model, surfaces eviction gaps, and supports Fatal throughout filtering and presentation. The callback-listener API and all callback-specific dispatch, synchronization, recursion, and producer-admission behavior have been removed; sequence-cursor history is now the only structured-log consumption model.

Validation evidence on 2026-07-23:

- `BuildTool test --target CoreTests --filter FLoggerTests.* --timeout 60`: 14 tests passed.
- `BuildTool test --target CoreTests --timeout 60`: 106 tests passed across 23 suites.

Validation evidence on 2026-07-23 after Stage 1:

- `BuildTool test --target CoreTests --filter FLoggerTests.* --timeout 60`: 22 tests passed.
- `BuildTool test --target CoreTests --timeout 60`: 114 tests passed across 23 suites.

Validation evidence on 2026-07-23 after Stage 2:

- `BuildTool test --target CoreTests --filter FLoggerTests.* --timeout 60`: 24 tests passed.
- `BuildTool test --target CoreTests --timeout 60`: 116 tests passed across 23 suites.

Validation evidence on 2026-07-23 after Stage 3:

- `BuildTool test --target EngineTests --filter FConsoleRecordModelTests.* --timeout 60`: 3 tests passed.
- `BuildTool test --target EngineTests --timeout 120`: 167 tests passed across 43 suites.
- `BuildTool build --target LevelEditor`: succeeded.
- `BuildTool build --target all`: succeeded for `Win64-Debug-DurinEditor-Tests`.
- `DurinEditor.exe --hidden-window`: remained running for the 8-second smoke window.

Validation evidence on 2026-07-23 after Stage 4:

- Repository-wide audit: no `FLogListener`, `AddListener`, or `RemoveListener` call sites remain outside this historical plan.
- `BuildTool test --target CoreTests --filter FLoggerTests.* --timeout 60`: 15 tests passed.
- `BuildTool test --target CoreTests --timeout 60`: 107 tests passed across 23 suites.
- `BuildTool build --target all`: succeeded for `Win64-Debug-DurinEditor-Tests`.

The current Console subscribes after most engine startup work has already begun. Listener delivery is decided when the asynchronous dispatcher processes a record rather than when the record is produced, so the Console receives an unpredictable tail of pre-subscription records and no already-processed history. The selected direction is to make `FLogger` own bounded structured session history and let the Console consume it by sequence cursor instead of using a callback listener as its data transport.

## Goal

Provide a deterministic, bounded, and thread-safe logging pipeline in which the editor Console shows the retained startup and runtime history in sequence order, slow UI consumption cannot delay reliable file logging, Error and Fatal calls return after their required sinks are durable rather than after arbitrary listener work, and overload is visible through explicit gap or drop records instead of silent partial output.

## Scope

- Define the ordering and retention contract for bootstrap, queued, processed, dropped, and recursively produced log records.
- Add a bounded structured session-history store owned by `FLogger`.
- Add a sequence-cursor read API suitable for main-thread UI polling and future diagnostic consumers.
- Migrate the editor Console away from `AddListener` and its unbounded pending vector.
- Correct Console support for every `ELogLevel`, including Fatal.
- Separate sink durability acknowledgement from optional observer delivery.
- Clarify or retire the callback-listener API after all repository consumers are migrated.
- Add unit, concurrency, overload, editor integration, and runtime smoke validation.
- Update long-lived logging and Console documentation after the implementation contract lands.

## Non-Goals

- Remote log streaming, telemetry upload, or network log protocols.
- Persisting Console search, filter, scroll, or command history across process launches.
- Reading arbitrary previous-session log files into the current Console.
- Changing the existing text log file format or rotating-file naming unless required to preserve correctness.
- Replacing `spdlog` or introducing a general-purpose event bus.
- Making logging a substitute for tracing, profiling, or crash-dump capture.
- Guaranteeing retention of every low-priority record during sustained overload; loss must instead be bounded and reported.

## Design Decisions and Invariants

### Record identity and ordering

- Every accepted structured record has one monotonically increasing, nonzero `Sequence` assigned before it becomes visible to sinks or readers.
- Sequence order is the authoritative order across concurrent producer threads. Timestamps remain presentation metadata and do not define ordering.
- Bootstrap records use the same sequence domain as running records and are transferred into normal processing without renumbering.
- Records dropped before acceptance do not consume normal record sequence numbers. A generated drop-summary record does consume a sequence number and reports the affected levels and count.
- A record logged from internal logging code or an observer must not recurse without a bound. Recursion handling must preserve sink visibility and explicitly define whether the record enters session history.

### Session history ownership

- `FLogger` owns a bounded deque or ring of fully owned `FLogRecord` values for the current process session.
- The default retained history is 5,000 records, matching the current Console capacity. A validated `HistoryCapacity` setting may override it within documented bounds.
- History is appended only after the record has passed producer-queue admission and reached ordered dispatch. The history therefore represents accepted records, not attempted log calls.
- Bootstrap storage is large enough to preserve the default history window before logger initialization. If it overflows, initialization emits a structured summary describing the missing bootstrap records.
- Oldest-record eviction is expected behavior. Readers detect eviction through sequence metadata and receive an explicit gap count rather than silently continuing from an unknown point.
- History lifetime ends at logger shutdown. Previous-session files remain available only through the filesystem.

### Reader contract

- UI and diagnostic consumers use a pull API based on the last consumed sequence, conceptually `ReadRecordsAfter(Sequence, MaxRecords)`.
- A read returns records in ascending sequence order, the newest available sequence, and any gap caused by history eviction or queue drops.
- Reads are non-blocking apart from a short history lock and never invoke consumer code while a logger lock is held.
- A newly created Console reader starts at the oldest retained sequence so it deterministically receives retained startup history.
- Per-read batch size is bounded so one frame cannot spend unbounded time copying or rendering a backlog.
- Reader filtering is applied after history retention. A reader's level filter does not change sink thresholds or global producer admission.

### Sink and reliability contract

- The logger dispatch path owns ordered writes to console and file sinks.
- Error and Fatal calls may wait for their required sink operation and flush barrier, but they do not wait for the editor Console or arbitrary consumer callbacks.
- Each reliable record performs one intentional durability path. Redundant `flush_on`, dispatch flush, and producer-side flush operations are consolidated and covered by tests.
- Periodic flushing remains available for lower-severity records.
- A sink failure is reported through the fallback path without recursively entering the failing sink.

### Overload behavior

- Producer queue capacity and history capacity are separate limits with separate diagnostics.
- Trace and Debug may be dropped immediately when the producer queue is full.
- Info and Warn may wait only for a bounded interval before being dropped.
- Error and Fatal remain reliable with respect to active sinks, but shutdown and sink-failure paths must prevent indefinite waiting.
- Drop-summary insertion cannot violate producer queue capacity. Summary generation is coalesced until capacity is available.
- The Console keeps no second unbounded copy of logger history. Its local records remain bounded across logs, command echoes, and command results.

### API compatibility

- The cursor reader is the preferred structured-log consumption API.
- The callback-listener API has no remaining repository compatibility consumer and is removed.
- Retained history and sink thresholds determine producer admission; observer presence cannot alter global logging cost or reliable completion.

## Current Foundations and Gaps

### Foundations to preserve

- `FLogRecord` owns message, module, source, thread, timestamp, level, and sequence metadata.
- Producers use a bounded asynchronous queue with severity-dependent overload behavior.
- A single dispatcher establishes a stable sequence order across concurrent producers.
- Error-level logging currently has a synchronous durability intent.
- Bootstrap records are retained before `Initialize()` and replayed into the running pipeline.
- Listener removal waits for an executing callback when called from another thread.
- Sink and listener exceptions are contained by a fallback stderr path.
- Log file rotation, session cleanup, configuration parsing, and lifecycle idempotence have unit coverage.

### Verified gaps

- Runtime listener registration has no historical replay contract and receives whichever older records remain queued at dispatch time.
- The editor Console registers well after logger initialization and therefore misses most startup records.
- Error and Fatal producers wait until observer callbacks complete even after the file sink has flushed.
- The Console allocates visibility state for five levels while `ELogLevel` defines six, causing Fatal records to be filtered out.
- Console pending records are unbounded until the next UI drain and copy every owned record.
- Any listener forces `ShouldLog()` to admit every runtime log level, coupling observer presence to producer cost.
- Dispatcher-thread recursive logs bypass every listener without an explicit public contract.
- Error records can be flushed redundantly through multiple mechanisms.
- Existing tests do not cover a late reader, the history/live boundary, history eviction, Fatal Console visibility, or a stalled observer during reliable logging.

### Stage 0 contract freeze

The current path is:

```text
producer thread
  -> ShouldLog and message formatting
  -> bounded producer queue
  -> logger dispatch thread
  -> console/file sinks
  -> listener callbacks
  -> processed-sequence acknowledgement

editor main thread
  -> constructs FConsolePanel after engine and editor startup work
  -> registers a listener
  -> listener copies records into PendingLogs
  -> Draw drains PendingLogs into the displayed record deque
```

Shutdown changes the logger from Running to Stopping, drains accepted queue records, joins the dispatcher from a non-dispatch thread, flushes sinks, and enters Stopped. Logs attempted after Stopped use the fallback stderr path and do not re-enter the asynchronous queue.

The repository-wide listener audit found no production consumer other than `FConsolePanel`. Remaining call sites are the public declaration, logger implementation, and Core tests. The listener API can therefore be removed after the Console migrates unless an external compatibility requirement is identified before Stage 4.

The selected capacity contract is:

- `HistoryCapacity` defaults to 5,000 records.
- Configuration clamps `HistoryCapacity` to the inclusive range 256 through 65,536 records.
- A history read defaults to 512 records and clamps a requested batch to the inclusive range 1 through 4,096 records.
- The Console may perform one default-sized read per frame. It does not loop without a frame boundary to consume an arbitrarily large backlog.
- Bootstrap retention uses the configured history capacity once configuration is available and the default capacity before `Initialize()`.

The selected public read contract is equivalent to:

```cpp
struct FLogReadResult
{
    std::vector<FLogRecord> Records;
    uint64 OldestAvailableSequence = 0;
    uint64 NewestAvailableSequence = 0;
    uint64 NextSequence = 1;
    uint64 EvictedRecordCount = 0;
};

auto ReadRecords(uint64 NextSequence, uint32 MaxRecords = 512) const
    -> FLogReadResult;
```

- The input and output cursor identify the next sequence requested, not the last sequence consumed. A new session reader starts with `NextSequence == 1`.
- Empty history reports zero oldest/newest sequences and returns the input cursor unchanged.
- If the input cursor precedes retained history, `EvictedRecordCount` is the distance to the oldest retained sequence and reading resumes there.
- Returned records are ascending and capped by the validated batch size. The output cursor is one greater than the last scanned sequence, or remains unchanged when nothing is available.
- Logger reads do not apply presentation filters. The Console advances its cursor across every returned record and applies level/search filters to its own bounded display model.

The selected reliability and shutdown contract is:

- Error and Fatal wait only until every active required sink has attempted the record and the required flush has completed.
- A sink exception completes that sink attempt, reports through fallback stderr, and cannot leave a producer waiting indefinitely.
- Once shutdown begins, already accepted records are drained in sequence order. Producers waiting for queue capacity are awakened; a record that can no longer be admitted uses fallback stderr and returns.
- A reliable producer already waiting for sink completion is released when its sink attempt completes or the logger reaches Stopped, whichever occurs first.
- Observer delivery, history reading, and editor UI work are never part of reliable completion.
- Calls made after Stopped remain fallback-only and are not inserted into session history.

## Implementation Stages

### Stage 0: Freeze the logging contract and baseline

- [x] Record the current logger, Console, startup, and shutdown paths in implementation notes attached to the change.
- [x] Confirm through a repository-wide search whether any project or module outside `ConsolePanel` uses the public listener API.
- [x] Select and document validated minimum and maximum values for `HistoryCapacity` and per-read batch size.
- [x] Define the exact result type for cursor reads, including records, oldest/newest available sequence, next cursor, and gap count.
- [x] Define shutdown behavior for blocked Error/Fatal producers and failed sinks.
- [x] Add deterministic characterization tests for late registration boundaries and listener-delayed reliable logging before changing implementation. Bootstrap/history limit tests begin in Stage 1 when the configurable history contract exists.

#### Acceptance Gate

- The selected public contracts have no unresolved ordering, retention, thread, shutdown, or compatibility decision.
- Characterization tests reproduce the nondeterministic or undesirable current behavior without relying on timing-only assertions.

### Stage 1: Introduce bounded ordered session history

Depends on Stage 0.

- [x] Add `HistoryCapacity` to `FLogSettings`, configuration parsing, validation, and test fixtures.
- [x] Assign sequence numbers at the accepted-record boundary and preserve them through bootstrap transfer, sink dispatch, history insertion, recursive dispatch, and drop summaries.
- [x] Add the bounded history container to `FLogger::FImpl` and append accepted records in dispatcher order.
- [x] Expand bootstrap retention to the default history window with bounded overflow accounting.
- [x] Generate an explicit structured bootstrap-overflow summary when pre-initialization retention is exceeded.
- [x] Implement the non-blocking cursor read API with bounded batches and explicit eviction-gap reporting.
- [x] Ensure history snapshots invoke no external code and hold the history lock only for the bounded snapshot operation.
- [x] Add unit tests for empty history, complete startup history, ordered concurrent history, batched reads, eviction gaps, bootstrap overflow, recursive ordering, and lifecycle restart behavior.

#### Acceptance Gate

- A reader created after engine-style startup obtains the retained records in strictly increasing sequence order.
- Concurrent production and reads show no duplicate sequence, unexplained gap, deadlock, or data race under the native test suite.
- Memory retained by history is bounded by the configured record count.

### Stage 2: Decouple reliable sink completion from observers

Depends on Stage 1.

- [x] Split sink completion state from any observer or compatibility-listener completion state.
- [x] Mark a reliable sequence durable immediately after its required sinks complete their intentional flush path.
- [x] Consolidate redundant Error/Fatal flush mechanisms while retaining the existing file-visibility guarantee when the logging call returns.
- [x] Ensure a stalled or throwing observer cannot delay the durability acknowledgement for the current record.
- [x] Prevent reliable producers from waiting indefinitely after shutdown begins or after all required sinks fail.
- [x] Make drop-summary admission respect queue capacity under producer/dispatcher races.
- [x] Add deterministic tests using barriers rather than sleeps for stalled observers, sink completion, shutdown release, queue saturation, and summary ordering.

#### Acceptance Gate

- The Error durability test still observes the record in the file immediately after `Log()` returns.
- A deliberately blocked observer does not delay that return once the sink is durable.
- Queue capacity is never exceeded by producer records or internally generated summaries in stress tests.

### Stage 3: Migrate the editor Console to cursor consumption

Depends on Stages 1 and 2.

- [x] Remove the Console's logger listener handle and `PendingLogs` callback buffer.
- [x] Store the Console's last consumed sequence and poll a bounded batch at the start of `Draw()`.
- [x] Continue draining bounded batches while the panel is hidden so visibility does not alter retention behavior.
- [x] Represent history eviction or producer drops with a visible Console warning record.
- [x] Preserve the combined 5,000-record cap across log records, commands, results, and errors without first constructing an unbounded intermediate collection.
- [x] Add Fatal to level naming, coloring, filter state, search, copy, and documentation.
- [x] Keep Console-owned `Records` on the UI thread; make the `clear` command marshal or assert that mutation occurs on that thread.
- [x] Avoid rebuilding avoidable filter allocations every frame when neither records nor filters changed.
- [x] Add focused tests around the non-ImGui Console record model where practical, leaving rendering-specific behavior for integration validation.

#### Acceptance Gate

- Opening the Console after editor initialization displays the complete retained startup tail deterministically.
- Fatal records are visible, filterable, searchable, copied correctly, and styled as fatal/error output.
- A synthetic burst larger than both batch and display capacity keeps memory and per-frame work bounded and visibly reports any gap.

### Stage 4: Resolve the callback-listener API

Depends on Stage 3.

- [x] Repeat the repository-wide consumer audit after Console migration.
- [x] Remove the listener API, listener count, callback execution tracking, recursive-listener special cases, and listener-specific tests because no compatibility consumer remains.
- [x] Keep retained history and sink thresholds as the only producer-admission inputs.
- [x] Replace callback-dependent characterization with direct history, concurrency, overload, durability, and lifecycle coverage.
- [x] Update the public header so the structured cursor reader is the sole consumer-facing log delivery API.

#### Acceptance Gate

- There is one documented structured-log consumption model for UI/history consumers.
- No public observer can silently change global log-level admission or block reliable sink completion.
- Removed APIs have no remaining repository call sites or tests; retained APIs have complete behavioral tests.

### Stage 5: End-to-end validation and documentation

Depends on Stages 1 through 4.

- [ ] Run the complete Core native test target, not only filtered logger tests.
- [ ] Build the full editor target using the repository BuildTool workflow.
- [ ] Run `DurinEditor` with `--hidden-window` and verify clean startup and shutdown without logger deadlocks or unexpected fallback output.
- [ ] Perform an interactive Console check for startup history, level filters, Fatal display, search, copy, clear, auto-scroll, and command output.
- [ ] Exercise a controlled high-volume logging source and confirm bounded memory, responsive UI, ordered sequences, and visible loss summaries.
- [ ] Verify the current-session log file contains the same accepted records required by its configured level, including Error/Fatal durability.
- [ ] Update `Documentation/Editor/Console.md` with retained-history, gap, capacity, and Fatal behavior.
- [ ] Add the adopted long-lived logging ownership, ordering, reliability, and overload rules to an Architecture document.
- [ ] Record commands, results, executable profile, and any accepted limitations in `Current Status` before closing the plan.

#### Acceptance Gate

- All validation-matrix rows pass on the same build profile.
- Documentation describes the implemented contract rather than the previous listener behavior.
- No acceptance condition depends on timing luck or manual interpretation of partial logs.

## Validation Matrix

| Area | Validation | Required evidence |
| --- | --- | --- |
| Record ordering | Concurrent producer unit test | Unique, strictly increasing accepted sequences in reader output |
| Startup history | Late-reader unit and editor integration test | Retained bootstrap and initialization records appear deterministically |
| History eviction | Small-capacity unit test | Exact gap count and oldest available sequence are reported |
| Producer overload | Saturated-queue unit test | Severity policy is enforced and one ordered drop summary is emitted |
| Reliable logging | Error/Fatal sink test with blocked observer | Call returns after sink durability without waiting for observer release |
| Shutdown | Reliable producer and shutdown concurrency test | No indefinite wait, deadlock, or lost required sink flush |
| Console levels | Console model or integration test | Trace through Fatal can each be shown and hidden independently |
| Console bounds | Burst larger than history/display limits | Bounded retained records, bounded batch work, visible gap summary |
| Console workflow | Interactive editor check | Search, copy, clear, follow, command output, and startup history work together |
| File behavior | Existing rotation and failure tests | Rotation, cleanup, fallback, source metadata, and durability remain correct |
| Runtime integration | Full build and hidden-window smoke run | Editor remains running through the smoke interval and exits cleanly when stopped |

## Definition of Done

- The editor Console deterministically displays the retained current-session startup and runtime history.
- History and every intermediate Console buffer are bounded with explicit loss reporting.
- Sequence cursors provide ordered, duplicate-free reads across concurrent production and history eviction.
- Fatal is handled everywhere other log levels are handled in the Console.
- Error and Fatal durability does not depend on UI or observer callback completion.
- Producer queue, history, and optional observer overload policies are independently defined and tested.
- The callback-listener API is either removed or retained with explicit live-only, level, threading, recursion, and backpressure contracts.
- Existing file rotation, fallback, bootstrap, lifecycle, and structured metadata behavior remains covered.
- Core tests, full editor build, hidden-window smoke run, and interactive Console checks pass on one profile.
- Long-lived architectural rules and user-facing Console behavior are documented, and this plan's status contains final validation evidence.

## Deferred Follow-ups

- Previous-session log browsing inside the editor.
- Exporting filtered Console records to a user-selected file.
- Structured JSON sinks or external log collectors.
- Runtime Console statistics for queue occupancy, history occupancy, and per-level drops.
- Source-location navigation from a Console record into the IDE.
- Separate tracing spans or high-frequency diagnostic channels that bypass ordinary log formatting.

## Related Documentation

- `Documentation/Plans/README.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Editor/Console.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/Core/Public/Logging/Logger.h`
- `Engine/Source/Runtime/Core/Public/Logging/LogMacros.h`
- `Engine/Source/Runtime/Core/Private/Logging/Logger.cpp`
- `Engine/Source/Programs/Tests/CoreTests/Private/LoggerTests.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ConsolePanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ConsolePanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
