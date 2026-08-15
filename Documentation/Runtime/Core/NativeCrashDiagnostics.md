# Native Crash Diagnostics

Summary: Define the bounded local artifact and crash-readable lifecycle contract for Windows and macOS native process failures.

Modules: Core, Launch

Last reviewed: 2026-08-16

## Ownership

Launch installs the platform process-crash owner at process entry, restores the
previous handlers only after a normal exit, selects the crash root, writes
artifacts, and preserves the terminal native status. Windows owns unhandled
exceptions and `std::terminate`. macOS owns fatal POSIX signals plus
`std::terminate`; its signal handler sends one fixed event through a pipe to a
prestarted reporter thread, waits only for that reporter's acknowledgement,
restores the default action, and re-raises the original signal.
Core owns only platform-neutral fixed diagnostic state. DurinDevTool owns
post-process discovery and optional symbolization.

The crash path does not log, flush, stop workers, unload modules, collect
objects, invoke application callbacks, allocate through the engine heap, or
acquire engine locks. One atomic writer is elected. A second faulting thread
emits one fixed stderr line and terminates without waiting or starting another
dump.

## Crash-readable state

`Diagnostics/ProcessCrashContext.h` exposes:

- a stable `EProcessCrashPhase` value at coarse startup, running, and shutdown boundaries;
- a 64-entry typed breadcrumb ring whose sequence is committed after its atomic fields;
- fixed runtime, build, executable-log-path, and start-time storage;
- atomic last-accepted, last-processed, and last-durable logger sequences.

Snapshots use fixed arrays and bounded retry. They do not allocate, wait, lock,
traverse engine objects, or expose Windows exception types. A missing ring
generation is omitted rather than interpreted as a committed record.

The logger publishes only at existing authority points. A larger accepted than
processed sequence proves that queued tail records may be absent from the log;
capture never tries to close that gap.

## Artifact contract

Windows x64 and macOS arm64 runtime variants install local capture. Intentional
`--native-crash-*` fixtures are unavailable in Shipping. The first qualified
layout is:

```text
Saved/Crashes/DurinEditor-20260811T135903.427Z-36740/
  DurinEditor-20260811T135903.427Z-36740-CrashContext-v1.txt
  DurinEditor-20260811T135903.427Z-36740.dmp
  Complete.marker
```

Before runtime Saved storage is ready, the fallback root is `Crashes` beside
the executable. After healthy storage preparation, new failures use
`Saved/Crashes`. A UTC timestamp, process id, and at most 15 collision suffixes
bound naming work. Existing directories are never replaced.

The line-oriented UTF-8 context is append-compatible within version 1. It owns
the crash id, reason and exception address, access-violation operation/address,
process and faulting-thread ids, runtime/build identity, executable, UTC time,
uptime, phase, breadcrumbs, log snapshot, dump path, and artifact error values.
`Complete.marker` is authoritative for completion and is created last, after
the context and dump handles have closed. A directory without it is partial.

On Windows the dump flags are `MiniDumpNormal | MiniDumpWithThreadInfo |
MiniDumpWithUnloadedModules`. Full memory, handle data, private read/write
memory, and indirect memory are not enabled. `MiniDumpWriteDump` is the accepted
in-process Windows boundary; failures still leave any writable context evidence.
On macOS the versioned context records `SystemManaged` dump ownership and keeps
the terminating POSIX signal intact so the operating system may publish its
ordinary `.ips` report. Durin does not fabricate a minidump path or parse a
system report from the signal path.

Healthy startup retains at most 16 complete directories for 30 days. Partial
directories older than 7 days are eligible for removal. Cleanup never runs in
the handler, skips directory links, and recognizes completion only by the
versioned marker.

## Supported and deferred failure classes

| Class | Policy |
| --- | --- |
| Access violation (read/write/execute) | Supported; isolated children qualify native status and context publication, plus Windows minidump or macOS system-report ownership. |
| Worker-thread access violation | Supported; the OS faulting thread id is retained. |
| `std::terminate` | Supported; Windows uses private status `0xE0000001`, while macOS records the terminate reason and preserves `SIGABRT`. |
| Stack overflow | Deferred; current hostile characterization can fault again while entering the in-process writer and does not reliably preserve `0xC00000FD` or artifacts. |
| Simultaneous or recursive faults | Best-effort; characterization terminates promptly without duplicate artifacts but may produce no complete set. |
| Assertions using the existing `abort` path | Deferred; assertion semantics are not changed merely to obtain a dump. |
| GPU device loss | Outside this contract. |
| Linux and unsupported architectures | Deferred to platform-specific adapters. |

Durin does not invoke a previous Windows unhandled filter or macOS signal
handler from the crash path. This avoids unknown callbacks and recursive
ownership; previous handlers are restored on successful exit.

## Privacy

Artifacts remain local. Nothing uploads, copies to source control, or enables a
symbol server automatically. Even a normal minidump can contain stack memory,
paths, identifiers, and fragments of project data. Treat the directory as
sensitive before sharing it.

## Related documentation

- [Runtime Lifecycle](RuntimeLifecycle.md)
- [Build and Run](../../Development/Build/BuildAndRun.md)
- [Native Tests](../../Development/Build/NativeTests.md)
