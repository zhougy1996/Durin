# Logging

Summary: Define process log ordering, bounded admission and history, sink durability, and shutdown behavior.

Modules: Core

Last reviewed: 2026-08-18

`FLogger` owns log ordering, sink delivery, and bounded structured history for
one process session. Editor presentation is a consumer of this contract and
does not participate in producer admission or sink completion.

## Ordering And Admission

Every accepted record receives one monotonically increasing, nonzero sequence
before it enters ordered dispatch. Sequence order, not timestamp order, is
authoritative across concurrent producers. Bootstrap records share the same
sequence domain and are transferred into normal dispatch without renumbering.

The asynchronous producer queue and retained history are independent bounded
resources. Trace and Debug records may be dropped immediately when the queue is
full; Info and Warn wait only for a bounded interval. Error and Fatal wait for
queue admission while the logger is running. Dropped lower-priority records are
coalesced into an ordered Warn summary when queue capacity becomes available.

## Structured History

History retains accepted records after they reach dispatch, independently of
terminal and file sink thresholds. Oldest-history eviction is normal and cursor
readers receive an explicit gap count instead of a silent discontinuity.

Structured-log consumers call `FLogger::ReadRecords` with the next desired
sequence. Reads return ascending records in a bounded batch, the retained
oldest/newest sequences, the next cursor, and any history-eviction count. They
copy owned records while holding only the history lock and never execute
consumer code inside the logger. UI visibility and consumer speed therefore do
not affect producer admission or sink completion.

## Sink Durability And Shutdown

The dispatcher owns terminal and file sink writes. Error and Fatal calls return
after active sink attempts and the intentional flush path complete; they never
wait for editor UI work. Sink failures use the fallback stderr path and still
release reliable producers.

Shutdown drains accepted records in sequence, wakes producers waiting for
capacity or durability, flushes sinks, and then ends the session history. Calls
after shutdown are fallback-only and are not inserted into retained history.
Process placement of logger initialization and finalization is defined by
[Runtime Lifecycle](RuntimeLifecycle.md).

## Related Documentation

- [Runtime Lifecycle](RuntimeLifecycle.md)
- [Native Crash Diagnostics](NativeCrashDiagnostics.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Logging/Logger.h`
- `Engine/Source/Runtime/Core/Private/Logging/Logger.cpp`
