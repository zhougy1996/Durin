# Python Toolset Integration Plan

Summary: Embed an editor-owned Python runtime and expose existing Toolsets through the detached EditorToolService endpoint, with bounded values, explicit host sessions, and cooperative cancellation.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

All stages are complete for the qualified Windows x64 first release. Embedded
Python executes existing Toolsets through the unchanged editor endpoint, and
MainFrame provides file execution, status, output and cancellation. Twenty Python
integration cases pass together and in isolation; all nine selected regression
targets, workspace builds and real-editor acceptance gates pass. Python is enabled
in the final local build. No required gate remains; other platform runtimes and
hard termination of non-cooperative scripts remain explicitly deferred.

The selected first release embeds CPython in an optional editor module and runs
one script job at a time on a dedicated worker. Scripts call existing native tools
through the detached endpoint. They do not access editor objects, invoke provider
handlers directly, or use MCP for in-process calls. Runtime packaging and
supported shutdown behavior are recorded below.

### Stage 0 decisions and evidence

- Pin CPython 3.13.15, official x64 Windows NuGet SDK, release CRT/GIL ABI,
  PSF license; archive SHA-256
  `05357887df50d3153efc681bdf432c321d3e2f9ce5788f99f4515b27e8fda0ac`.
  Package headers/import library privately and deploy the runtime DLL, license
  and pure-Python standard-library zip. Native extension/package installation is
  outside supported first-release scripts. Windows is the first qualified host;
  other hosts compile an unavailable-runtime path until a native SDK is qualified.
- Initialize and finalize on the one script worker, serialize jobs, and retain
  the runtime DLL for process lifetime. A process-wide owner prevents concurrent
  interpreters. Isolated PyConfig uses only explicit packaged library paths,
  disables site/user environment configuration and bytecode writes.
- Host closure cancels jobs and invocations before waiting; tool result waits
  observe cancellation without needing another editor tick. Line/opcode tracing
  provides Python cancellation checkpoints. Arbitrary native blocking imports
  and scripts suppressing cancellation are unsupported, not forcibly terminated;
  module unload waits for their actual return. No safe bounded unload is claimed
  for unsupported scripts. This limitation is acceptable for trusted opt-in work.
- Initial limits: 8 sessions, 16 outstanding jobs, 64 retained jobs, 1 MiB source,
  64 KiB captured output/error per job, 256 owned invocations per job; default and
  maximum tool wait/job deadline 30 seconds. Paths are explicit absolute UTF-8
  script paths; fresh globals do not change process working directory or sys.path.
  Completed jobs are evicted oldest-first; unknown/evicted IDs return errors.
- Public runtime/session/job operations and Python APIs follow the proposed table
  below. External is the default source and cannot be changed from Python. Output
  overflow fails the script; cancellation and timeout never imply rollback.
- Probe: `Build/Python/sdk/tools/python.exe -I Build/Python/lifecycle_probe.py`
  passed pure-Python loop cancellation, tool-wait cancellation without a host tick,
  and finite native blocking behavior. The prototype is exploratory; stages 1-4
  must verify these properties with embedded CPython and the real editor service.


### Stages 1-3 acceptance

- Embedded runtime, native value bindings, editor sessions and invocation handles
  are implemented. Twenty PythonToolIntegrationTests cases pass in one process
  and independently with four isolated slots on windows-msvc-x64 /
  Win64-Debug-DurinEditor using packaged CPython 3.13.15.
- Coverage includes real PIE calls, exact numbers, defaults/null, bad input/output,
  private tools, provider retirement, timeout, cancellation during queued/running
  work, host isolation, source policy, expiry, bounded retention and restart.
- Raw results: `Build/.agent-state/logs/20261016-162829-069136-40964-PythonToolIntegrationTests.log`
  and `Build/NativeTestResults/Win64-Debug-DurinEditor/PythonToolIntegrationTests.xml`.
- Windows rejects delay-loading CPython because its API includes data imports.
  The implementation instead uses a private explicit symbol table, retaining the
  DLL for process lifetime. DLL dependency inspection confirms no Python loader
  dependency in DurinPython, preserving editor startup without runtime files.
- The all workspace build and the eight-target tools/editor-shell/MCP regression
  set pass. MainFrame application smoke has completed PIE status/play/pause/stop;
  Stage 4/5 acceptance remains open until all close/error/disabled checks finish.


### Stage 4 acceptance

- MainFrame offers Window > Python Scripts with lazy startup, absolute file
  execution, status, cancellation, output and traceback presentation. Explicit
  DURIN_PYTHON_SCRIPT startup submission is supported on the qualified host.
- Sandbox application smoke passed PIE status/play/pause/stop, output and normal
  closure (`Build/.agent-state/logs/20261016-163228-609337-7184-DurinEditor.log`).
- Application traceback/output passed with the expected Failed job and filename
  (`Build/.agent-state/logs/20261016-163738-534050-34476-DurinEditor.log`). A script
  continuously invoking tools survived until editor exit and closed normally
  (`Build/.agent-state/logs/20261016-163803-579089-40160-DurinEditor.log`, plus
  `Build/Python/close.started` proving the tool loop started).
- Repeated host destruction no longer queries inactive adapters. Runtime/session
  shutdown remains idempotent; unsupported native blocking work is not forcefully
  terminated. CPU tests cover cancellation and actual host routing separately.
- DURIN_ENABLE_PYTHON=OFF passed the all workspace build and a real editor startup
  with a requested script reporting Python unavailable and exiting normally
  (`Build/.agent-state/logs/20261016-164023-064805-37392-DurinEditor.log`). Restore
  ON and final regression confirmation are tracked by Stage 5.


### Stage 5 final qualification

- Tested baseline: `10e069262` plus the final tool-facade binding correction and
  this acceptance update. Profile windows-msvc-x64, preset Win64-Debug-DurinEditor,
  packaged CPython 3.13.15; final DURIN_ENABLE_PYTHON=ON.
- Final review tightened Stage 2: every Python tools facade is bound to its
  creating job, and stale facades fail instead of using the next job's host.
  The existing cross-job handle case now verifies both facade and handle rejection.
- All 20 Python integration cases pass independently with four slots, including
  the final correction. Receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/PythonToolIntegrationTests.xml`;
  raw isolation log: `Build/.agent-state/logs/20261016-164403-097855-37116-ctest.log`.
- Final domain selection `@domain=python+tools+editor-shell+mcp` passed all nine
  targets, including all 20 Python cases together and existing Core/Console/MCP
  regressions. Raw log: `Build/.agent-state/logs/20261016-164518-391555-29736-ctest.log`;
  receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/domain-python-tools-editor-shell-mcp.xml`.
  Affected analysis selected all because project/test registration changed; this
  bounded domain selection covers the changed behavior without unrelated GPU tests.
- Final workspace all build passed after the binding correction, covering Engine,
  Sandbox and RoadWeaver targets. Log:
  `Build/.agent-state/logs/20261016-164407-289789-40108-cmake.log`.
  OFF configuration also passed all and real-editor unavailable-runtime startup;
  ON was restored and rebuilt before final execution. No shared Engine API changed.
- Final Sandbox application smoke passed discovery, PIE status/play/pause/stop,
  invocation cancellation, output and normal exit:
  `Build/.agent-state/logs/20261016-164524-768145-31964-DurinEditor.log`.
  Stage 4 logs additionally prove traceback presentation and active-script closure.
  Scripts and marker files remain under ignored `Build/Python/`.
- Lasting behavior is published in the [Python adapter](../../../Editor/Architecture/PythonTools.md)
  and [script guide](../../../Editor/Guides/PythonScripts.md); module ownership and task
  routing are updated. Changed-document, all-plan and diff validation pass.
- Limitations: trusted scripts, one worker, shared import state, packaged Windows
  runtime only, no native extension package support, no automatic script-wide Undo,
  no forceful termination or hard shutdown bound for unsupported non-cooperative
  work. These are the selected first-release boundaries, not outstanding gates.

## Goal

Allow an explicitly submitted Python string or UTF-8 script file to discover and
invoke Toolsets belonging to one selected editor, receive final native results,
and report Python exceptions and tool diagnostics without blocking GameThread.

Use the existing [Core tool contracts](../../../Runtime/Core/Tools.md),
[editor tool service](../../../Editor/Architecture/EditorTools.md), and
[MCP adapter](../../../Editor/Architecture/MCPTools.md) as authorities for implemented
behavior. This document describes proposed additions, not current APIs.

## Scope and Selected Design

### Module ownership

| Owner | Proposed additions or responsibilities |
| --- | --- |
| `DurinPython` (new optional editor module) | CPython lifetime, one script worker, per-editor sessions, native Python bindings, bounded value conversion, script jobs, output capture, cancellation and teardown |
| `MainFrame` | Explicit editor-session creation and closure; a minimal run-file entry point and job/result presentation |
| `DurinEd` | Reuse `DEditorToolSubsystem::GetService()` and existing endpoint methods; retain ownership of tool dispatch and host-state checks |
| `Core` | Reuse `FToolRequest`, `FToolValue`, schemas, diagnostics and source context; no Python dependency |
| Native Toolset providers | Continue owning business checks, domain transactions and output contracts; no Python dependency |
| `DurinMCP` | Keep existing external tool transport; no Python dependency or new script execution tool in this release |

`DurinPython` publicly depends on Core contracts and privately on DurinEd; the packaged CPython ABI stays private.
Use the repository dependency mechanism; do not use an installed system Python as
the editor runtime. CPython headers and Python object types stay private. The
initial binding implementation uses the CPython C API; adding a second binding
framework is outside this plan unless Stage 0 records a concrete need.

MainFrame's opt-in integration must permit ordinary editor startup when Python is
disabled or initialization fails. No runtime, game, or provider module acquires
a dependency on DurinPython. Confirm actual target membership in Stage 0.

### Execution and host identity

1. On GameThread, bind a script session to an explicitly selected editor and
   acquire its detached `FEditorToolService` endpoint.
2. Associate every job and Python tools facade with that immutable session;
   retain no `DEditorEngine`, subsystem, provider, or DObject pointer on the worker.
3. Execute Python on the dedicated worker. Convert arguments to owned native
   values and submit `FToolRequest` through the endpoint.
4. Existing editor `TickTools` dispatches native handlers on GameThread at its
   existing safe point. Python does not extend or bypass host admission.
5. The worker observes receipts and converts final results back to Python.

One process-wide interpreter and worker serve bounded, serialized script jobs.
Each job gets fresh execution globals and a session-bound `durin` facade. Imported
module state is shared; this is not per-editor interpreter isolation. Prevent
user-created background Python work in the supported execution contract; do not
claim enforcement or sandboxing. No Python-backed Toolset providers or callbacks
from GameThread into Python are introduced.

### Proposed interfaces

Names below are design targets to finalize in Stage 0, not existing symbols.
Public C++ signatures use owned native data, opaque IDs, and structured errors.

| Surface | Proposed operations | Contract |
| --- | --- | --- |
| Module/runtime | `Start`, `Stop` | One owner; explicit initialization, admission closure and interpreter finalization ordering |
| Editor session | `CreateSession(Editor)`, `CloseSession(SessionId)` | Creation on GameThread; permanently bind one endpoint; no retargeting after editor replacement |
| Script jobs | `RunString`, `RunFile`, `GetJob`, `CancelJob` | Submit without waiting on GameThread; bounded queue, source, retained output and results |
| `durin.tools` | `list`, `describe` | Owned native catalog snapshot; preserve separate toolset/tool names and contract version |
| `durin.tools` | `submit(toolset, tool, arguments, *, version=None)` | Return a session-bound invocation handle; pin discovered version or use explicit caller version |
| `durin.tools` | `call(toolset, tool, arguments, *, version=None, timeout=...)` | Worker-only convenience over submit and final-result waiting |
| Invocation handle | `state`, `result(timeout=...)`, `cancel` | Preserve queued/running/final distinctions; reject cross-session handles |
| Python errors | `ToolError`, job-level Python exception report | Preserve native code/path/message and Python filename/traceback; no automatic retry |

Example target API using a real existing tool:

```python
import durin

description = durin.tools.describe("editor.pie", "pause")
result = durin.tools.call("editor.pie", "pause", {"paused": True}, timeout=5.0)
print(result["state"])
```

For agent-originated or externally supplied jobs, the host fixes
`EToolSource::External` and filters discovery by `bExposeExternally`; submission
rechecks exposure through the existing service. A trusted host may explicitly
choose Native for its own scripts. Python arguments cannot choose or elevate the
source, invocation identity, cancellation context, or host. Do not add a Script
enum merely to distinguish the language. Tool exposure is not a Python sandbox:
the first release executes explicitly authorized trusted scripts only and does
not expose arbitrary Python execution through MCP.

### Values, waiting and lifecycle

Map None/bool/int/float/str/list/dict to the existing value tree. Object keys must
be strings. Handle bool before int; use schema-directed signed/unsigned/Number
conversion, reject overflow and non-finite numbers, preserve 64-bit integers, and
bound conversion before copying large graphs. Reject cycles and unsupported
objects. Do not stringify values or round-trip through JSON. Core remains the
authority for defaults, schema validation and native output validation.

Waiting releases the GIL after all Python input has become owned native data;
reacquire it before creating results or exceptions. Do not hold service/session
locks while waiting or acquiring the GIL. Initially use interruptible bounded
receipt polling; a new service completion-notification API requires separate
evidence of need. GameThread never waits synchronously for a script that may
depend on a future editor tick. Script jobs are not `FToolResult::OperationId`.

Job cancellation stops further submissions and cancels all invocations owned by
that job, including fire-and-forget handles. Tool cancellation is cooperative;
requesting it does not prove a handler stopped or that effects were rolled back.
Timeouts have the same limitation. Bound job and invocation retention separately.

Closing a session prevents new calls, wakes result waiters, and requests job and
invocation cancellation. Release endpoints only after worker access ceases.
Split GameThread admission closure from worker draining/finalization; never join
a worker on GameThread while it awaits editor progress. Finalize CPython only
after Python work and Python references are quiescent, under the recorded
interpreter ownership rules. Retain module code while jobs can still execute.

Arbitrary Python loops and blocking native extensions cannot be promised a hard
deadline or safe forced termination. Stage 0 must select supported imports and
cooperative Python cancellation checkpoints, document remaining non-cooperative
cases, and prove a safe editor-close/module-unload policy. If bounded forced
termination is required, stop and revise this plan toward a process boundary;
do not solve it with thread termination or interpreter finalization during use.

Each native tool continues to own its transaction. No whole-script automatic
Undo, rollback, atomic batch, or transaction kept open across queued calls is
promised. The current dispatcher requires an idle transaction owner; compound
atomic operations need a separately designed native tool.

## Implementation Stages

### Stage 0: Resolve runtime and lifecycle contracts

Dependencies: existing Core/editor tool contracts and repository dependency,
module, build and testing guidance.

- [x] Select and pin a supported CPython version, ABI/build options, platform
  artifacts, licensing, standard-library deployment and module search paths.
  Do not inherit arbitrary user site packages or environment search paths.
- [x] Confirm optional module/target integration and define initialization,
  interpreter thread ownership, GIL handling, and finalization order.
- [x] Finalize native/Python API names, source policy, script file path semantics,
  queue/value/output budgets, timeout defaults and receipt-expiry behavior.
- [x] Define serialized job scheduling, fresh globals versus shared imports,
  supported imports, cooperative cancellation, and editor-close/unload behavior.
- [x] Prototype a script waiting for a tool while its host closes, a pure Python
  loop cancellation, and the declared handling of non-cooperative extension work.

Completion: record concrete decisions and prototype evidence here. No unresolved
shutdown path may depend on GameThread waiting for its own tool execution; the
supported cancellation limitations must be explicit before Stage 1.

### Stage 1: Embed the optional runtime and implement script jobs

Dependencies: Stage 0 accepted runtime and lifecycle decisions.

- [x] Add DurinPython module/dependency metadata, packaged interpreter bootstrap,
  dedicated worker, bounded job queue, IDs, status queries and cancellation.
- [x] Implement RunString/RunFile with explicit UTF-8/file-size/path policy,
  fresh execution globals, filename-aware tracebacks and bounded stdout/stderr.
- [x] Implement partial-start failure cleanup and ordered Stop; support disabling
  Python without affecting normal editor creation.
- [x] Verify sequential scripts, syntax/runtime exceptions, missing/oversized
  files, output overflow, queue saturation, start failure and clean teardown.

Completion: CPU-only runtime tests execute scripts and capture failures without
editor object access; the selected shutdown contract passes deterministic tests.

### Stage 2: Bind sessions, catalog discovery and owned values

Dependencies: Stage 1 and the existing editor endpoint.

- [x] Implement GameThread session creation, detached endpoint ownership and
  session-bound durin.tools discovery; filter External catalogs consistently.
- [x] Implement Python/native value and diagnostic conversion using existing
  schemas and limits; preserve discovered contract versions without silent retry.
- [x] Verify integer boundaries, bool/int distinction, numeric schema conversion,
  UTF-8 byte limits, null versus absent, arrays, string-only keys, cycles,
  excessive depth/size, invalid values and unsupported Python objects.
- [x] Verify catalog refresh behavior, version mismatch, hidden tools, closed
  sessions and two editor hosts without cross-routing.

Completion: discovery and conversion tests pass with detached owned data and no
Python or editor pointers crossing the endpoint boundary.

### Stage 3: Execute tools and return final results

Dependencies: Stage 2; retain the current native tool service API unless a
documented requirement proves an extension necessary.

- [x] Implement submit, invocation handles, result waiting, call and ToolError
  through Submit/GetInvocation/Cancel; release the GIL during native waiting.
- [x] Preserve native admission errors, domain failures, invalid outputs and
  receipt expiry. Distinguish successful data containing null from no result.
- [x] Track every invocation by job/session, including handles not awaited;
  implement timeout and cancellation without retries or false rollback claims.
- [x] Verify no handler executes on the Python worker, no GameThread blocking
  wait is possible through this API, and no service lock spans GIL acquisition.
- [x] Exercise real PIE status/play/pause/stop; verify pause without Play fails
  with its native diagnostic and success returns final message/state data.

Completion: a script drives real CPU editor-host PIE tools through existing
GameThread dispatch; queued, running, completed and failed states remain distinct.

### Stage 4: Integrate editor entry points and close lifecycle gaps

Dependencies: Stage 3.

- [x] Integrate optional session creation/closure into MainFrame's explicit host
  lifecycle; add a minimal run-file action and job status/cancel/result display.
- [x] Handle editor closure during queued work, active result waiting, Python
  computation, provider execution and output capture under the Stage 0 policy.
- [x] Verify closed sessions cannot reach a replacement host, old handles cannot
  resolve new jobs, and provider retirement does not redirect accepted calls.
- [x] Verify cancelled jobs stop submitting, native completion/cancel races retain
  actual outcomes, and Python/module teardown cannot race active worker code.
- [x] Verify unavailable Python leaves the editor usable; exercise the declared
  non-cooperative-work limitation without unsafe thread termination.

Completion: real editor run-file workflow passes; deterministic lifecycle tests
prove endpoint, interpreter and module ownership for all supported close paths.

### Stage 5: Qualify the integration and publish implemented contracts

Dependencies: Stages 0-4 complete.

- [x] Follow [agent testing](../../../Agents/Testing.md) and
  [build/run guidance](../../../Agents/BuildAndRun.md); select runtime/codec, CPU editor
  integration and existing Toolset/Console/MCP regression targets affected by the
  implementation. Do not require GPU startup for CPU-only correctness checks.
- [x] Validate affected workspace project targets. If a shared Engine API changes,
  migrate every consumer declared in Durin.dworkspace and complete an all build.
- [x] Run an application-hosted script smoke check for discovery, PIE calls,
  traceback/output presentation, cancellation and editor closure.
- [x] Publish lasting Python adapter/lifecycle contracts under Editor/Architecture
  and script usage/setup under Editor/Guides; update module ownership and routing
  only when implemented behavior creates a distinct task trigger.
- [x] Record revision, platform/runtime version, exact test selection, outcomes,
  remaining limitations and reproducible acceptance settings here; keep raw logs
  and scripts generated for qualification under ignored Build/.

Completion: required regressions and project builds pass, implemented contracts
are published, and every supported lifecycle/acceptance gate has evidence. Follow
the [plan lifecycle rules](../../AGENTS.md) before marking this plan completed.

## Deferred Work

Direct engine-object Python bindings, Python-authored Toolset providers, arbitrary
Python execution over MCP, parallel interpreters, hot interpreter restart,
third-party package installation, process sandboxing, hard script termination,
and script-wide atomic transactions are outside this first release. Add them only
through an explicit design revision with their own lifecycle and validation gates.
