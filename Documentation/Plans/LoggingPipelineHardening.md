# Logging Pipeline Hardening Plan

Last reviewed: 2026-07-23

## Current Status

Planning complete. The existing logger and editor Console paths have been reviewed, and the current `FLoggerTests` suite passes. Implementation has not started.

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
- `AddListener` remains only until repository consumers and tests are migrated. Its exact live-only semantics, callback thread, recursion behavior, and removal guarantees must be documented if it remains public.
- If no external compatibility requirement is identified, remove `AddListener`, `RemoveListener`, listener-count-based `ShouldLog`, and their synchronization machinery after migration.
- If callback compatibility must remain, callbacks are observational and cannot participate in reliable sink acknowledgement. They must have an independently bounded delivery policy.

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

## Implementation Stages

### Stage 0: Freeze the logging contract and baseline

- [ ] Record the current logger, Console, startup, and shutdown paths in implementation notes attached to the change.
- [ ] Confirm through a repository-wide search whether any project or module outside `ConsolePanel` uses the public listener API.
- [ ] Select and document validated minimum and maximum values for `HistoryCapacity` and per-read batch size.
- [ ] Define the exact result type for cursor reads, including records, oldest/newest available sequence, next cursor, and gap count.
- [ ] Define shutdown behavior for blocked Error/Fatal producers and failed sinks.
- [ ] Add failing characterization tests for late registration behavior, listener-delayed reliable logging, and bootstrap/history limits before changing implementation.

#### Acceptance Gate

- The selected public contracts have no unresolved ordering, retention, thread, shutdown, or compatibility decision.
- Characterization tests reproduce the nondeterministic or undesirable current behavior without relying on timing-only assertions.

### Stage 1: Introduce bounded ordered session history

Depends on Stage 0.

- [ ] Add `HistoryCapacity` to `FLogSettings`, configuration parsing, validation, and test fixtures.
- [ ] Assign sequence numbers at the single accepted-record boundary and preserve them through bootstrap transfer, sink dispatch, history insertion, and drop summaries.
- [ ] Add the bounded history container to `FLogger::FImpl` and append accepted records in dispatcher order.
- [ ] Expand bootstrap retention to the default history window or implement equivalent bounded bootstrap accounting.
- [ ] Generate an explicit structured bootstrap-overflow summary when pre-initialization retention is exceeded.
- [ ] Implement the non-blocking cursor read API with bounded batches and explicit eviction-gap reporting.
- [ ] Ensure history snapshots copy or move no data while invoking external code and hold locks only for the bounded snapshot operation.
- [ ] Add unit tests for empty history, complete startup history, ordered concurrent history, batched reads, eviction gaps, bootstrap overflow, and lifecycle restart behavior.

#### Acceptance Gate

- A reader created after engine-style startup obtains the retained records in strictly increasing sequence order.
- Concurrent production and reads show no duplicate sequence, unexplained gap, deadlock, or data race under the native test suite.
- Memory retained by history is bounded by the configured record count.

### Stage 2: Decouple reliable sink completion from observers

Depends on Stage 1.

- [ ] Split sink completion state from any observer or compatibility-listener completion state.
- [ ] Mark a reliable sequence durable immediately after its required sinks complete their intentional flush path.
- [ ] Consolidate redundant Error/Fatal flush mechanisms while retaining the existing file-visibility guarantee when the logging call returns.
- [ ] Ensure a stalled or throwing observer cannot delay the durability acknowledgement for the current record.
- [ ] Prevent reliable producers from waiting indefinitely after shutdown begins or after all required sinks fail.
- [ ] Make drop-summary admission respect queue capacity under producer/dispatcher races.
- [ ] Add deterministic tests using barriers rather than sleeps for stalled observers, sink completion, shutdown release, queue saturation, and summary ordering.

#### Acceptance Gate

- The Error durability test still observes the record in the file immediately after `Log()` returns.
- A deliberately blocked observer does not delay that return once the sink is durable.
- Queue capacity is never exceeded by producer records or internally generated summaries in stress tests.

### Stage 3: Migrate the editor Console to cursor consumption

Depends on Stages 1 and 2.

- [ ] Remove the Console's logger listener handle and `PendingLogs` callback buffer.
- [ ] Store the Console's last consumed sequence and poll a bounded batch at the start of `Draw()`.
- [ ] Continue draining bounded batches while the panel is hidden so visibility does not alter retention behavior.
- [ ] Represent history eviction or producer drops with a visible Console warning record.
- [ ] Preserve the combined 5,000-record cap across log records, commands, results, and errors without first constructing an unbounded intermediate collection.
- [ ] Add Fatal to level naming, coloring, filter state, search, copy, and documentation.
- [ ] Keep Console-owned `Records` on the UI thread; make the `clear` command marshal or assert that mutation occurs on that thread.
- [ ] Avoid rebuilding avoidable filter allocations every frame when neither records nor filters changed.
- [ ] Add focused tests around the non-ImGui Console record model where practical, leaving rendering-specific behavior for integration validation.

#### Acceptance Gate

- Opening the Console after editor initialization displays the complete retained startup tail deterministically.
- Fatal records are visible, filterable, searchable, copied correctly, and styled as fatal/error output.
- A synthetic burst larger than both batch and display capacity keeps memory and per-frame work bounded and visibly reports any gap.

### Stage 4: Resolve the callback-listener API

Depends on Stage 3.

- [ ] Repeat the repository-wide consumer audit after Console migration.
- [ ] Remove the listener API, listener count, callback execution tracking, recursive-listener special cases, and listener-specific tests if no compatibility consumer remains.
- [ ] Otherwise, define listeners as live-only observers with an explicit minimum level and independently bounded delivery.
- [ ] Ensure retained history and sink thresholds, rather than listener count alone, determine producer admission.
- [ ] Define and test observer ordering, callback thread, self-removal, shutdown, exceptions, and recursive logging if listeners remain.
- [ ] Update public header comments so no callback behavior depends on implementation inference.

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
