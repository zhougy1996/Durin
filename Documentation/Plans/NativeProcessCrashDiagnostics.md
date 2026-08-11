# Native Process Crash Diagnostics Plan

Summary: Add deterministic local native-crash artifacts, bounded process-lifecycle context, and DurinDevTool-assisted symbolization without depending on the asynchronous logger after a fault.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

At the selected baseline revision `17833709`, Durin had an ordered asynchronous
logger whose accepted Error and Fatal records wait for active sink attempts and
flushes, and whose orderly shutdown drained accepted records. It did not have a
process-level unhandled-exception owner, minidump writer, crash-context format,
or post-crash symbolization workflow.

The motivating failure was a deterministic `0xC0000005` during Editor shutdown.
The runtime log ended after `Asset manager stopped accepting new requests.`
because the access violation bypassed normal control flow and `LoggerShutdown()`.
Running the same executable under CDB identified a stale Level picking observer
through `FViewportPickingSceneIndex::ReceiveBatch`, but that required a manual
debugger launch and a second reproduction. The repaired failure and its focused
regression test are outside this plan; this plan owns making a future equivalent
failure diagnosable from the first crashed process.

Implementation on 2026-08-11 added the Core fixed crash state, the Launch-owned
Windows handler, lifecycle and logger publication, DurinDevTool attribution and
CDB analysis, isolated crash characterization, and lasting documentation.
Focused Debug Editor access-violation measurement on the current Windows 11
x64 host produced a 462,461-byte normal minidump plus a 1,037-byte context in
0.26 seconds. A later end-to-end `DevTool run` associated only its current PID,
named `0xC0000005`, reported `ProcessEntry`, and resolved
`Durin::RunWindowsProcessCrashFixture` from the adjacent Launch PDB.

Supported isolated read/write/execute access violations, worker-thread access
violation, and `std::terminate` each create one complete artifact set and
terminate within 15 seconds. Injected dump failure preserves the original
status and context. Simultaneous faults are best-effort and terminate without a
duplicate set but may leave no artifact. Stack overflow remains deferred: the
in-process writer can fault again on the exhausted stack, so neither a preserved
`0xC00000FD` nor a readable dump is claimed.

Final focused evidence: `CoreUtilityTests` passed 66/66,
`CoreConcurrencyTests` passed 121/121, and
`NativeCrashCharacterizationTests` passed 12/12 in Debug Editor, Debug Game,
and Release Editor. The DurinDevTool suite passed 316/316 and documentation
validation passed all 100 documents. The final Debug Editor `all` build succeeded. Hidden
threaded and inline-RHI two-tick runs exited normally without increasing either
crash-root directory count. The post-build threaded Running-phase qualification
produced a 713,762-byte dump, a 1,318-byte context, and a 23-byte marker under
`Saved/Crashes`; context reported `accepted=processed=74`, `durable=0`, and the
active log path. CDB resolved the exact fault, `FEngineLoop::Init`, and `main`
to repository source lines from adjacent PDBs. Release passed the same twelve
supported-crash and hostile-policy characterizations. An
automated pre-initialization logger-tail crash observed accepted sequence 4,170
and processed sequence 2,583 at capture while the already-written log remained
readable. Pure policy tests qualified access metadata, root validation,
age/count/partial retention, and directory-link avoidance. Shipping DurinGame
built successfully, rejected diagnostic injection/root-override arguments with
exit code 1, and created no
crash directory.

The repository already emits PDBs for the Windows Debug Editor build, keeps the
runtime executable and module PDBs together in the runtime output, captures
child output through DurinDevTool, and has explicit process-isolation rules for
intentional crash characterization. Windows SDK Debuggers, including CDB, may
be installed locally but are not a runtime prerequisite. At the baseline no
code used
`SetUnhandledExceptionFilter`, `MiniDumpWriteDump`, a vectored exception
handler, `std::set_terminate`, or DbgHelp.

## Goal

- Preserve enough first-crash evidence to identify the exception, faulting
  thread, process phase, native modules, registers, and symbolized call stack.
- Produce local artifacts even when the asynchronous logger, worker scheduler,
  heap, or ordinary shutdown path cannot make progress.
- Make `DevTool run` report an actionable crash summary and, when a compatible
  local CDB is available, a bounded source-level stack without requiring a
  second reproduction.
- Keep normal logging, startup, frame, and shutdown behavior unchanged when no
  crash occurs.

## Scope

- Windows x64 native crashes in DurinEditor and DurinGame as the first qualified
  platform and runtime variants.
- A Launch-owned process crash handler installed before subsystem startup and
  removed only during successful process termination.
- Local minidumps, a versioned fixed-format crash-context file, and an atomic
  completion marker written below the runtime Saved crash directory.
- A small Core-owned process phase and fixed-capacity lifecycle breadcrumb
  surface usable without allocation or locks at capture time.
- A logger-to-crash-context snapshot containing only already-published atomic
  sequence numbers and the precomputed active log path; the crash handler never
  drains or flushes the asynchronous logger.
- DurinDevTool crash discovery, stable summary output, optional local CDB
  analysis, artifact-path reporting, and deterministic Python coverage.
- Isolated intentional-crash characterization for access violation and selected
  terminal runtime paths.
- Documentation of artifact location, privacy, retention, symbol requirements,
  and manual/offline analysis.

## Non-Goals

- Recovering from an access violation, continuing the crashed process, or
  attempting normal Engine shutdown after native process corruption.
- Replacing `FLogger`, making ordinary logs synchronous, or treating minidumps
  as a second logging sink.
- Uploading dumps, logs, symbols, source, hardware information, or user data to
  a server; prompting for consent; or building a crash-reporting backend.
- Shipping an in-editor crash viewer, restart/recovery UI, safe-mode workflow,
  or automatic issue creation.
- Capturing full-process memory by default. Heap dumps and full-memory dumps can
  contain project content, paths, credentials, and unrelated process data.
- Walking or symbolizing native stacks inside the faulting process.
- Depending on CDB, WinDbg, a Windows symbol server, network access, or a Vulkan
  SDK for runtime crash artifact creation.
- Handling GPU device loss as a native CPU crash. Device-loss policy remains an
  RHI concern unless it terminates through an ordinary native exception.
- General-purpose arbitrary-string breadcrumbs, a global subsystem callback
  registry, continuous tracing, telemetry, or a replacement for Tracy.
- Qualifying macOS, Linux, consoles, ARM64, or sandboxed distribution platforms
  in the first implementation. Their platform adapters require separate plans
  after the platform-neutral context contract is stable.

## Design Decisions and Invariants

### Ownership and layering

- Launch owns process installation, uninstallation, fatal exception capture,
  crash-directory selection, artifact naming, and terminal process behavior.
  `FEngineLoop` remains the normal lifecycle owner and does not gain a crash
  callback registry.
- Core owns only the platform-neutral bounded diagnostic state required by
  multiple modules: current process phase, lifecycle breadcrumbs, build/runtime
  identity, and a read-only logger crash snapshot. It does not depend on Launch
  or DbgHelp.
- The Windows Launch implementation privately links `Dbghelp` and contains all
  SEH, `EXCEPTION_POINTERS`, and `MINIDUMP_*` types. No Windows exception or
  DbgHelp type enters a public cross-platform header.
- DurinDevTool owns offline discovery and symbolization because it already owns
  runtime process execution, output capture, local tool discovery, and concise
  failure presentation.

### Installation and terminal behavior

- The crash handler is installed at process entry before logger initialization,
  project admission, task-system startup, module loading, or RHI creation. A
  two-step path update may publish the final Saved directory later, but early
  startup always retains a precomputed writable fallback beside the executable
  or in the process temporary directory.
- Windows uses one process-wide unhandled-exception filter for final unhandled
  SEH faults. It does not use a vectored handler because first-chance exceptions
  include expected C++ and system exceptions and would create false artifacts.
- Installation records the previous filter. Normal shutdown restores it. The
  selected chaining policy is frozen in Stage 0 after tests prove that exactly
  one Durin artifact set is created and no handler recursion occurs; arbitrary
  third-party callbacks are never invoked while Durin-owned locks are held.
- A process-wide atomic compare/exchange elects one crash writer. A second
  crashing thread writes at most one fixed fallback line to the inherited
  standard-error handle and terminates; it never waits for the first faulting
  thread or attempts a second dump.
- After the selected handler finishes or fails, the process terminates with the
  original native exception status. It does not return to Engine code and does
  not translate the fault into exit code 1.
- `std::terminate` integration uses the same single-writer artifact path with a
  distinct reason code, then terminates. Assertions and explicit fatal policy
  enter this path only if their current terminal implementation already ends
  the process; this plan does not change assertion semantics merely to gain a
  dump.

### Crash-time safety boundary

- The handler does not call `FLogger`, spdlog, `LoggerShutdown`, module unload,
  garbage collection, task shutdown, RHI shutdown, editor teardown, or user
  callbacks.
- Crash-time code does not acquire engine mutexes, wait on engine threads,
  allocate through the ordinary C++ heap, format with `std::format`, construct
  filesystem objects, or inspect mutable object graphs.
- Paths, runtime/build identity, phase names, handles, and fixed file headers
  are prepared during healthy execution. Crash-time writes use bounded buffers,
  atomics, `CreateFileW`, `WriteFile`, `FlushFileBuffers`, and
  `MiniDumpWriteDump` only.
- `MiniDumpWriteDump` is accepted as the Windows platform primitive even though
  DbgHelp itself is not async-signal-safe. The implementation documents this
  boundary, uses a recursion guard, and treats dump failure as recoverable for
  context-file creation. An out-of-process writer remains deferred until
  characterization proves an in-process reliability gap worth its process and
  synchronization cost.
- Stack overflow is recorded as an exception category, but reliable dump
  creation from a fully exhausted faulting stack is not claimed until a
  dedicated characterization passes. It cannot silently count as supported
  based only on access-violation coverage.

### Artifact layout, completion, and privacy

- Every crash receives one stable `CrashId` formatted as
  `<runtime>-<YYYYMMDDTHHMMSS.mmmZ>-<pid>`, using UTC and fixed-width numeric
  fields so lexical order is chronological. A bounded `-<collision>` suffix is
  appended only when that exact id already exists.
- The default layout is one immutable directory per crashed process. The dump
  and context repeat the `CrashId` so either file remains attributable after it
  is copied out of its directory:

  ```text
  Saved/Crashes/DurinEditor-20260811T135903.427Z-36740/
    DurinEditor-20260811T135903.427Z-36740-CrashContext-v1.txt
    DurinEditor-20260811T135903.427Z-36740.dmp
    Complete.marker
  ```

- Directory and filenames use only the prevalidated `CrashId`. The handler
  never recursively removes or replaces an existing crash directory.
- `Complete.marker` is written last only after every attempted artifact handle
  is closed. Its absence means the directory is partial. DevTool may inspect a
  partial context or dump but labels it incomplete.
- The default dump type is `MiniDumpNormal | MiniDumpWithThreadInfo |
  MiniDumpWithUnloadedModules`. Full memory, private read/write memory, handle
  data, and indirectly referenced memory are excluded by default.
- Crash artifacts are local-only and never automatically copied into source,
  Git, command logs, test results, clipboard, or network services. User-facing
  documentation warns that even a normal minidump can contain stack memory,
  project paths, identifiers, and fragments of project data.
- Retention cleanup runs only on a later healthy startup, never in the crash
  handler. It keeps a frozen bounded count and age selected in Stage 0, skips
  directories without a recognized versioned manifest unless they exceed the
  separately selected stale-partial age, and never follows directory links.
- A test-only or explicit diagnostic override may redirect the crash root to a
  validated directory. Production paths cannot escape the selected Saved root
  through relative traversal.

### Versioned crash context

- The `*-CrashContext-v1.txt` file is line-oriented ASCII/UTF-8 with one stable
  key per line. It avoids JSON escaping and parser dependencies in crash-time
  code.
- Required fields are format version, completion state, exception/reason code,
  exception address, process id, faulting thread id, runtime variant, build
  configuration, executable image path, UTC timestamp, process uptime, current
  lifecycle phase, breadcrumb write sequence/range, active log path, last
  accepted/processed/durable log sequences, dump path, dump result, and OS
  error when an artifact operation fails.
- Exception parameters include access type and attempted address for access
  violations when `EXCEPTION_RECORD` supplies them. Unknown or unavailable
  values are emitted explicitly rather than omitted.
- Build identity includes the generated Durin version and a source revision or
  build identity already available through the build. Stage 0 selects the
  authoritative value and must not run Git from the crashed process.
- The dump remains the authority for registers, thread stacks, module images,
  and exception context. The text file does not duplicate a home-grown stack
  walker or claim source lines.
- The format is append-only within version 1: existing keys retain meaning and
  new optional keys may be added. An incompatible interpretation creates a new
  versioned filename and parser.

### Process phase and breadcrumbs

- `EProcessCrashPhase` is a compact stable enum with at least process entry,
  pre-initialization, engine initialization, running, consumer detachment,
  asset-service shutdown, task-system shutdown, asset-manager shutdown, object
  collection, module shutdown, rendering shutdown, RHI shutdown, application
  shutdown, and exited phases.
- Phase publication is one relaxed or release atomic store at coarse lifecycle
  boundaries. It does not wrap every function and does not replace direct
  lifecycle code or logging.
- Breadcrumbs are fixed-size typed records, not arbitrary strings. Each record
  contains a monotonically assigned sequence, compact event id, thread id,
  monotonic timestamp, and at most two fixed-width numeric arguments.
- The ring has a frozen small power-of-two capacity selected in Stage 0. Writers
  reserve a slot atomically and publish its sequence last; the crash snapshot
  accepts only slots whose committed sequence matches the expected generation.
  Torn, overwritten, or in-progress slots are reported as gaps rather than read
  as valid data.
- Initial production breadcrumbs cover only process/lifecycle transitions and
  selected terminal object-drain boundaries. Feature modules do not gain a
  generic breadcrumb API until a concrete crash demonstrates that the process
  phase plus dump is insufficient.
- Breadcrumb insertion is bounded, nonblocking, allocation-free, and legal from
  any native thread. Reading them during normal operation has no side effects.

### Relationship to asynchronous logging

- Existing logger delivery, queue capacity, severity durability, history, and
  shutdown semantics remain unchanged.
- Logger initialization publishes the final active file path into a fixed crash
  snapshot during healthy execution. Logger dispatch publishes accepted,
  processed, and durable sequence counters atomically after their existing
  authoritative transitions.
- The crash handler reads those atomics without waiting. `LastAccepted` greater
  than `LastProcessed` explicitly proves that tail records may not have reached
  a sink; it never triggers a drain attempt.
- DevTool presents the runtime log and crash context as complementary evidence:
  the log owns rich ordered messages already delivered, while the dump/context
  own terminal exception evidence.

### Offline symbolization and PDB matching

- Runtime artifact creation succeeds without CDB. DurinDevTool first prints the
  parsed crash-context summary and artifact paths, then attempts symbolization
  only when a compatible debugger is discovered or explicitly configured.
- CDB runs in a separate process after the runtime has terminated. Its command
  script selects the exception context and prints a bounded verbose stack; it
  does not modify the dump or contact a symbol server by default.
- The initial symbol path contains the exact runtime binary directory and its
  adjacent PDBs. Caller-provided symbol environment may extend it explicitly;
  network symbol lookup is never silently enabled.
- Analysis detects missing or mismatched PDBs and reports that state separately
  from a corrupt dump. It never presents module-offset-only output as a
  source-symbolized stack.
- DevTool stores complete debugger output in its existing command-log area and
  prints a bounded faulting-thread excerpt containing exception code, phase,
  top Durin frames, dump path, context path, and analysis-log path.
- Crash discovery is constrained to the configured crash root, runtime variant,
  process launch interval, and process id when available. A stale crash from an
  earlier run cannot be attributed to the current command.

## Current Foundations and Gaps

| Area | Existing foundation | Gap owned by this plan |
| --- | --- | --- |
| Logging | Ordered asynchronous dispatch, bounded queue/history, reliable Error/Fatal delivery, file-session retention, and orderly shutdown drain. | Native faults bypass logging calls and normal shutdown; no crash-safe snapshot explains queued tail loss. |
| Launch | One visible process lifecycle with explicit startup, tick, and shutdown ordering. | No early process exception owner or terminal artifact path. |
| Core diagnostics | Thread identity, process ids, clocks, generated build version, assertions, and profiler milestones exist. | No stable crash phase or fixed-capacity crash-readable breadcrumb state. |
| Windows platform | Win32 process/file APIs and Debug Windows builds are supported. | No private DbgHelp integration, dump policy, recursion guard, or failure fallback. |
| Build outputs | Executables, DLLs, and PDBs are colocated for Debug runtime builds. | Crash context does not record build identity and tooling does not qualify PDB/dump compatibility. |
| DurinDevTool | Owns runtime launch, process exit status, complete child logs, local toolchain discovery, and concise failure excerpts. | It reports only the numeric native exit code and cannot discover or analyze a matching dump. |
| Tests | Native tests support isolated processes and retain failed/crashed sandboxes. | No intentional-crash characterization verifies artifact completeness, parser behavior, or no-hang termination. |

## Implementation Stages

### Stage 0: Freeze formats, policies, and failure baseline

- [x] Record the baseline source revision, the current access-violation run
  output, the manual CDB stack workflow, runtime/PDB output paths, and current
  logger accepted/processed/durable behavior.
- [x] Freeze `EProcessCrashPhase`, breadcrumb record layout and capacity,
  `CrashContext-v1.txt` required keys, dump flags, crash directory retention
  count/age, partial-directory policy, and Shipping enablement policy.
- [x] Freeze unhandled-filter chaining, `std::terminate`, assertion, stack
  overflow, and second-crashing-thread behavior with explicit supported,
  best-effort, or deferred labels.
- [x] Inventory the generated version/build identity and select one value that
  can be embedded without invoking Git at runtime.
- [x] Measure normal dump size and capture latency for one Debug Editor access
  violation using the selected dump flags; record the host and values before
  setting any timeout or retention constant.
- [x] Define the platform-neutral interfaces and private Windows types so Core,
  Launch, tests, and DurinDevTool do not form a circular dependency.

#### Acceptance Gate

- One reviewed format/policy table unambiguously defines ownership, supported
  crash classes, dump privacy, retention, recursion, completion, and PDB
  matching. The measured baseline proves that the selected defaults are bounded
  and that the motivating crash would have enough evidence for a source stack.

### Stage 1: Add bounded Core crash context state

Dependencies: Stage 0.

- [x] Add the process phase enum and atomic publication/read API without
  exposing platform exception types.
- [x] Add the fixed-capacity typed breadcrumb ring with generation-safe commit,
  wraparound, torn-slot rejection, and bounded snapshot APIs.
- [x] Publish build/runtime identity and healthy-runtime start time into fixed
  storage before concurrent subsystem startup.
- [x] Add a crash-readable logger snapshot for active file path and accepted,
  processed, and durable sequence counters without changing logger queue or
  sink ordering.
- [x] Instrument only the selected `FEngineLoop` startup/shutdown boundaries and
  logger transitions; reject arbitrary string breadcrumbs and feature-specific
  expansion in this stage.
- [x] Add focused Core tests for phase transitions, concurrent ring publication,
  wraparound, incomplete slots, monotonic sequences, fixed storage, logger
  counter publication, and repeated inert snapshots.

#### Acceptance Gate

- Core tests prove every crash-readable field can be obtained without locks,
  allocation, waits, object traversal, or platform exception types; concurrent
  publication cannot expose a torn record as valid; existing logger tests retain
  their delivery and durability behavior.

### Stage 2: Implement the Windows crash artifact owner

Dependencies: Stage 1.

- [x] Add a private Windows Launch component that precomputes paths and fixed
  headers, installs/uninstalls the unhandled filter and terminate handler, owns
  the single-writer guard, and privately links DbgHelp.
- [x] Implement early-startup fallback path selection and later atomic
  publication of the final Saved crash root without exposing a partially
  written path.
- [x] Write `CrashContext-v1.txt`, the selected minidump, and final completion
  marker in the fixed order; retain explicit result/error fields when directory,
  context, dump, flush, or marker creation fails.
- [x] Encode access-violation operation/address and the original exception
  status while preserving exact terminal process status.
- [x] Implement bounded collision handling, recursion/second-fault behavior,
  no-dialog unattended behavior, and restoration on successful normal exit.
- [x] Add pure tests around synthetic exception records, context serialization,
  path validation, collision handling, incomplete artifacts, dump-callback
  policy, retention selection, and injected Win32/DbgHelp failures.
- [x] Verify normal startup and shutdown create no crash directory and do not
  alter logger, window, debugger, or process-exit behavior.

#### Acceptance Gate

- Synthetic tests cover every artifact operation failure without deadlock or
  unbounded retry. A real isolated access violation creates exactly one bounded
  artifact directory, preserves `0xC0000005`, includes the required context and
  a debugger-readable dump, and terminates within the measured timeout; a normal
  process creates none.

### Stage 3: Integrate complete lifecycle and logging context

Dependencies: Stage 2.

- [x] Publish the final Saved crash root after runtime storage initialization
  while preserving the early fallback for pre-initialization crashes.
- [x] Cover every selected `FEngineLoop` phase transition, including first and
  second object collection as distinguishable breadcrumbs without obscuring the
  direct shutdown protocol.
- [x] Publish active log path and logger counters at their existing authority
  points; prove crash capture never locks or flushes the logger.
- [x] Add selected object-drain breadcrumbs sufficient to distinguish CDO
  release, root retirement, first collection, render flush, second collection,
  deferred-destroy audit, and module unload without recording object names or
  project data in the fixed ring.
- [x] Add tests that crash with accepted-but-unprocessed log records and verify
  context reports the sequence gap honestly while already durable log content
  remains readable.
- [x] Verify crashes on the GameThread and one non-GameThread identify the exact
  faulting thread and retain a coherent process-phase snapshot.

#### Acceptance Gate

- Fault injection at startup, running, object collection, and a worker thread
  produces the correct phase/thread/reason and no logger wait. The motivating
  Editor shutdown class is attributable to object collection before opening the
  dump, and the dump supplies the exact source stack when matching PDBs exist.

### Stage 4: Add DurinDevTool discovery and offline analysis

Dependencies: Stages 2-3.

- [x] Extend runtime process results with unsigned Windows native status
  formatting and stable names for recognized terminal exception codes.
- [x] Discover only a crash directory matching the current launch interval,
  runtime variant, and process id where available; distinguish complete,
  partial, stale, and absent artifacts.
- [x] Parse the versioned context with forward-compatible unknown-key handling
  and actionable malformed/incomplete diagnostics.
- [x] Add local CDB discovery through explicit configuration and bounded Windows
  SDK probing; absence remains non-fatal and prints a manual analysis command.
- [x] Run CDB after process termination with local runtime/PDB symbols, capture
  complete analysis output, extract a bounded exception-context stack excerpt,
  and diagnose missing/mismatched PDBs.
- [x] Keep ordinary nonzero exits, assertions without dumps, timeouts, user
  interruption, and successful runs on their existing paths; do not label every
  failure as a crash.
- [x] Add Python tests using fake runtime artifacts and a fake debugger for
  matching, stale rejection, partial artifacts, malformed versions, debugger
  absence/failure, PDB mismatch text, excerpt bounds, spaces/non-ASCII paths,
  and successful-run silence.

#### Acceptance Gate

- One `DevTool run` of the isolated crash fixture ends with the original native
  exception status, named exception, process phase, faulting thread, top Durin
  source frames, and clickable artifact/analysis paths when CDB is available.
  The same result remains actionable without CDB and cannot attach an older
  crash to the current run.

### Stage 5: Qualify crash classes and hostile failure paths

Dependencies: Stage 4.

- [x] Register a separately isolated characterization target with the required
  runtime-stack rationale; its parent launches one child per intentional fault
  and owns cleanup only after artifacts are verified.
- [x] Qualify access violation for read, write, and execute metadata where the
  OS provides it; `std::terminate`; a faulting worker thread; two near-simultaneous
  faults; recursive artifact failure; unwritable primary crash root; name
  collision; missing DbgHelp; missing dump; and missing/mismatched PDBs.
- [x] Characterize stack overflow separately and label it supported only if it
  repeatedly writes a valid context and debugger-readable dump without relying
  on undefined remaining stack space.
- [x] Verify dumps stay within the measured size/latency bounds, retention runs
  only on later healthy startup, partial directories follow the frozen policy,
  and no test follows or removes an unrecognized directory link.
- [x] Run repeated crash children to detect handler-state leakage, nondeterministic
  artifact attribution, debugger hangs, and checkout-lock retention.
- [x] Verify Release behavior and the frozen Shipping policy without requiring
  CDB or PDB availability at runtime.

#### Acceptance Gate

- Every supported crash class passes repeated isolated characterization with
  one complete artifact set and bounded termination. Best-effort and deferred
  classes are reported honestly. Artifact failures retain the original crash
  status, never hang, never recurse indefinitely, and never delete outside the
  validated crash root.

### Stage 6: Publish contracts and complete end-to-end qualification

Dependencies: Stages 1-5.

- [x] Publish the lasting native crash contract under Runtime/Core, including
  ownership, crash-time safety, artifact format, lifecycle phase, breadcrumb,
  logger relationship, privacy, and supported-platform matrix.
- [x] Update Runtime Lifecycle with handler installation/removal and phase
  publication without duplicating the crash contract.
- [x] Update Build and Run with artifact discovery, CDB configuration/manual
  commands, PDB matching, retention, privacy, and DevTool output behavior.
- [x] Update Native Tests with the separately isolated intentional-crash target
  and retained-sandbox expectations.
- [x] Run changed-document validation, focused Core/Launch tests, DurinDevTool
  Python tests, the crash characterization target, and the smallest affected
  runtime targets during development.
- [x] Complete a final full Debug Editor `all` build, normal and inline-RHI
  bounded clean-exit smokes that create no artifacts, and one post-build
  threaded Editor crash qualification whose CDB stack resolves against the
  exact adjacent PDBs.
- [x] Record measured artifact sizes/latencies, supported crash matrix, focused
  results, full-build result, clean-exit evidence, and symbolized crash evidence
  in Current Status before completing the plan.

#### Acceptance Gate

- Documentation and implementation agree; clean execution is unchanged;
  supported crashes create bounded private local artifacts without logger or
  Engine teardown; DevTool provides a correct first-crash summary and exact-PDB
  stack; every validation row and Definition of Done item is satisfied.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Phase state | Stable enum values, correct startup/shutdown transitions, atomic inert reads, and no hidden lifecycle registry. |
| Breadcrumb ring | Fixed storage, multi-writer publication, wraparound, torn-slot rejection, monotonic committed sequences, and bounded snapshot time. |
| Logger relationship | Active path and accepted/processed/durable counters are accurate; a crash never waits for dispatch or flush. |
| Handler installation | Installed before subsystem startup, restored on success, exactly one writer, preserved native status, and no normal-run artifact. |
| Context format | Required v1 keys, explicit unavailable values, access metadata, build identity, phase, breadcrumbs, log counters, and operation errors. |
| Dump | Selected privacy-bounded flags, valid exception context, faulting thread, module list, debugger readability, measured size/latency, and failure fallback. |
| Filesystem safety | Validated root, collision handling, complete marker ordering, no overwrite, bounded healthy-start retention, and no directory-link traversal. |
| Crash classes | Access violation, terminate, worker fault, simultaneous fault, recursive failure, and stack overflow according to the frozen support labels. |
| DevTool attribution | Current process/time/variant match, stale rejection, complete/partial distinction, native status naming, and bounded artifact links. |
| Symbolization | Optional local CDB, no implicit network symbols, exact adjacent PDB success, missing/mismatched PDB diagnostics, and complete analysis log. |
| Privacy/configurations | No upload or full-memory default, documented sensitive-data warning, Debug/Release behavior, and frozen Shipping policy. |
| End to end | Focused native/Python tests, isolated crash characterization, full Debug Editor build, clean threaded/inline runs, and one exact-PDB symbolized Editor crash. |

## Definition of Done

- The first supported native crash produces one immutable, bounded local
  artifact directory containing a versioned context, debugger-readable dump,
  and final completion marker without relying on normal Engine shutdown.
- Crash capture does not call, lock, drain, or flush the asynchronous logger and
  reports queued-tail uncertainty through atomic logger sequence counters.
- Current process phase and committed lifecycle breadcrumbs remain readable
  without allocation, locks, waits, mutable object traversal, or platform types
  in Core's public interface.
- The original native exception status, exact faulting thread, exception address,
  and access metadata survive capture.
- `DevTool run` associates only the current process's artifact, names recognized
  Windows crash codes, links the dump/context, and prints a source-symbolized
  Durin stack when compatible local CDB and PDBs are available.
- Missing debugger, dump-write failure, unwritable directories, simultaneous or
  recursive crashes, and mismatched PDBs remain bounded and actionable without
  hiding or replacing the original crash status.
- Normal threaded and inline runtime exits create no crash artifacts and retain
  current logger and shutdown behavior.
- Artifact privacy, retention, configuration policy, manual analysis, and the
  supported crash/platform matrix are documented in their lasting domains.
- All stage acceptance gates and validation-matrix rows pass with evidence
  recorded in Current Status.

## Deferred Follow-ups

- An out-of-process crash writer may be planned if repeated characterization
  shows in-process DbgHelp cannot meet reliability requirements for heap
  corruption, stack overflow, loader-lock faults, or simultaneous failures.
- User consent, packaging, redaction, compression, upload, server-side symbol
  storage, issue correlation, and crash-rate telemetry require a separate
  product/privacy plan.
- Editor recovery, autosave restoration, safe-mode relaunch, and culprit-plugin
  isolation require stable crash artifacts first and remain separate workflows.
- Linux signal/core-dump integration, macOS CrashReporter integration, ARM64,
  and console platform adapters follow only after the platform-neutral context
  and tooling contracts are stable.
- A richer per-subsystem breadcrumb vocabulary requires evidence from real
  crashes that the process phase, faulting stack, existing log, and initial
  lifecycle breadcrumbs are insufficient.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Profiling](../Development/Build/Profiling.md)
- [Engine Loop Orchestration Cleanup Plan](EngineLoopOrchestrationCleanup.md)

## Related Code

- `Engine/Source/Runtime/Launch/Private/Launch.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/Launch/CMakeLists.txt`
- `Engine/Source/Runtime/Core/Public/Logging/Logger.h`
- `Engine/Source/Runtime/Core/Private/Logging/Logger.cpp`
- `Engine/Source/Runtime/Core/Public/HAL/PlatformProcess.h`
- `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp`
- `Engine/Source/Runtime/Core/CMakeLists.txt`
- `Engine/Tests/Native/CoreTests/`
- `Engine/Tests/Native/EngineTests/Private/Launch/`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
- `CMake/Project/ProjectTargets.cmake`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/durin_dev_tool/build/process.py`
- `Tools/DurinDevTool/durin_dev_tool/build/output.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
- `Tools/DurinDevTool/tests/test_build_output.py`
